#!/usr/bin/env python3
"""LogX 离线解码工具(参见 spec §2 / §10)。

功能:
  - 读 prefix_YYYYMMDD.log(或 .1/.2 …),逐块解析
  - 解码每块的 payload(压缩 + 加密);加密块需要 key_provider 提供密钥
  - 解码后的 LGXE 序列按记录边界重组(支持跨块分片重组)
  - 把"内部用户载荷"还原为可读行(ts/pid/tid/level/tag/msg)
  - 跳过块内 LGXP PAD 与块尾 <6B 零残留(spec §3.1/§3.2)

用法:
  python logx_decode.py <log_dir> [--from YYYYMMDD] [--to YYYYMMDD] \\
                          [--kid-map kid=hex,key ...] [--out out_dir]

依赖(可选):
  pip install cryptography  # 用于 AES-256-GCM 解密
  pip install zstandard     # 用于 zstd 解压
"""
from __future__ import annotations

import argparse
import os
import re
import struct
import sys
from dataclasses import dataclass, field
from typing import Callable, Optional, Dict, List, Tuple

MAGIC_FILE = b'LGXF'
MAGIC_BLOCK = b'LGXB'
MAGIC_ENTRY = b'LG'
MAGIC_PAD   = b'GP'

ALGO_NONE = 0
ALGO_ZSTD = 1
ALGO_ZLIB = 2

FLAG_ENCRYPTED = 0x01
COMP_MASK      = 0x06

# ---------------- 文件格式 helpers ----------------
def crc32_update(crc: int, data: bytes) -> int:
    crc = (~crc) & 0xffffffff
    for b in data:
        crc = (crc >> 8) ^ CRC_TABLE[(crc ^ b) & 0xff]
    return (~crc) & 0xffffffff

CRC_TABLE = []
for i in range(256):
    c = i
    for _ in range(8):
        c = ((c >> 1) ^ 0xedb88320) if (c & 1) else (c >> 1)
    CRC_TABLE.append(c)

def put_u32(v: int) -> bytes:
    return struct.pack('<I', v & 0xffffffff)

def get_u32(b: bytes, off: int) -> int:
    return struct.unpack('<I', b[off:off+4])[0]

def get_u16(b: bytes, off: int) -> int:
    return struct.unpack('<H', b[off:off+2])[0]

# ---------------- codec helpers ----------------
def decompress(algo: int, payload: bytes) -> bytes:
    if algo == ALGO_NONE:
        return payload
    if algo == ALGO_ZSTD:
        try:
            import zstandard
        except ImportError:
            raise RuntimeError("pip install zstandard for zstd blocks")
        return zstandard.ZstdDecompressor().decompress(payload, max_output_size=64 * 1024 * 1024)
    if algo == ALGO_ZLIB:
        import zlib
        return zlib.decompress(payload)
    raise RuntimeError(f"unknown algo {algo}")

def decrypt_gcm(key: bytes, nonce: bytes, ct_with_tag: bytes) -> bytes:
    """AES-256-GCM;返回明文;cipher+tag 拼在一起传入。"""
    try:
        from cryptography.hazmat.primitives.ciphers.aead import AESGCM
    except ImportError:
        raise RuntimeError("pip install cryptography for AES-GCM blocks")
    return AESGCM(key).decrypt(nonce, ct_with_tag)

# ---------------- LGXE 重组 ----------------
def cksum1(payload: bytes) -> int:
    c = 0
    for b in payload:
        c ^= b
    return c & 0xff

@dataclass
class LogSlice:
    flags: int
    payload: bytes
    @property
    def is_first(self) -> bool: return (self.flags & 0x03) == 0x01
    @property
    def is_mid(self)   -> bool: return (self.flags & 0x03) == 0x02
    @property
    def is_last(self)  -> bool: return (self.flags & 0x03) == 0x03
    @property
    def is_whole(self) -> bool: return (self.flags & 0x03) == 0x00

class RecordReassembler:
    """流式分片重组器：支持单块内多条日志与跨块连续分片重组。"""
    def __init__(self):
        self.cur_slices: List[bytes] = []
        self.in_split = False

    def feed(self, s: LogSlice) -> List[bytes]:
        results = []
        if s.is_whole:
            if self.in_split:
                self.cur_slices.clear()
                self.in_split = False
            results.append(s.payload)
        elif s.is_first:
            self.cur_slices = [s.payload]
            self.in_split = True
        elif s.is_mid:
            if self.in_split:
                self.cur_slices.append(s.payload)
        elif s.is_last:
            if self.in_split:
                self.cur_slices.append(s.payload)
                results.append(b"".join(self.cur_slices))
                self.cur_slices = []
                self.in_split = False
        return results

def reassemble(slices: List[LogSlice]) -> Optional[bytes]:
    """把单条长日志切片重组为内部用户载荷。失败返回 None。"""
    asm = RecordReassembler()
    out = []
    for s in slices:
        out.extend(asm.feed(s))
    return out[0] if out else None

def parse_lgxe(raw: bytes) -> List[LogSlice]:
    """从 raw 字节流中抽取 LGXE 序列(跳过 LGXP 与 < 6B 零残留)。"""
    out = []
    pos = 0
    while pos + 6 <= len(raw):
        if raw[pos:pos+6] == b'\x00' * 6:
            pos += 6
            continue
        magic = raw[pos+2:pos+4]
        if magic == MAGIC_PAD:
            pad_len = get_u16(raw, pos)
            pos += 6 + pad_len
            continue
        if magic == MAGIC_ENTRY:
            payload_len = get_u16(raw, pos)
            flags = raw[pos+4]
            cksum = raw[pos+5]
            if pos + 6 + payload_len > len(raw):
                break  # 撕裂尾巴
            actual = cksum1(raw[pos+6:pos+6+payload_len])
            if actual != cksum:
                break  # cksum 失败
            out.append(LogSlice(flags, raw[pos+6:pos+6+payload_len]))
            pos += 6 + payload_len
            continue
        break  # magic 不识别:停止扫描
    return out

# ---------------- 文件级解码 ----------------
@dataclass
class FileHeader:
    date: int
    kid: int
    salt: bytes
    enc: bool

def parse_file_header(b: bytes) -> FileHeader:
    assert b[:4] == MAGIC_FILE, f"bad file magic: {b[:4]!r}"
    crc_expected = get_u32(b, 28)
    crc_actual = crc32_update(0, b[:28])
    if crc_expected != crc_actual:
        raise RuntimeError(f"file header CRC fail: expected {crc_expected:#x} != {crc_actual:#x}")
    return FileHeader(
        date=get_u32(b, 6),
        kid=get_u16(b, 10),
        salt=bytes(b[12:20]),
        enc=(b[5] & FLAG_ENCRYPTED) != 0,
    )

@dataclass
class DecodedBlockResult:
    seqno: int
    raw_len: int
    raw_bytes: Optional[bytes] = None
    error: Optional[str] = None

def decode_block_raw(bhdr: bytes, payload: bytes, tag: Optional[bytes],
                     flag: int, raw_len: int, kid_keys: Dict[int, bytes],
                     kid_in_file: int) -> DecodedBlockResult:
    seqno = get_u32(bhdr, 6)
    expected_crc = get_u32(bhdr, 18)
    actual_crc = crc32_update(0, payload)
    if expected_crc != actual_crc:
        return DecodedBlockResult(seqno, raw_len, error=f"crc32 mismatch: expected {expected_crc:#x} != {actual_crc:#x}")

    algo = (flag & COMP_MASK) >> 1
    enc = bool(flag & FLAG_ENCRYPTED)
    if enc:
        key = kid_keys.get(kid_in_file)
        if key is None:
            return DecodedBlockResult(seqno, raw_len, error=f"missing key for kid={kid_in_file}")
        nonce = bhdr[22:34]  # salt[8] + seqno_be[4]
        try:
            ct_with_tag = payload + (tag or b'')
            plain = decrypt_gcm(key, bytes(nonce), bytes(ct_with_tag))
        except Exception as e:
            return DecodedBlockResult(seqno, raw_len, error=f"gcm fail: {e}")
    else:
        plain = bytes(payload)
    try:
        raw = decompress(algo, plain)
    except Exception as e:
        return DecodedBlockResult(seqno, raw_len, error=f"decompress fail: {e}")
    return DecodedBlockResult(seqno, raw_len, raw_bytes=raw)

def decode_file_records(path: str, kid_keys: Dict[int, bytes]) -> Tuple[List[bytes], List[str]]:
    """解码整个文件，返回 (用户载荷列表, 警告与错误列表)。"""
    with open(path, 'rb') as f:
        data = f.read()
    if len(data) < 32:
        return [], [f"{path}: file too short (<32B)"]
    try:
        fh = parse_file_header(data[:32])
    except Exception as e:
        return [], [f"{path}: bad file header: {e}"]

    user_records: List[bytes] = []
    errors: List[str] = []
    reassembler = RecordReassembler()

    off = 32
    while off + 34 <= len(data):
        bhdr = data[off:off+34]
        if bhdr[:4] != MAGIC_BLOCK:
            errors.append(f"{path} off={off}: bad block magic {bhdr[:4]!r}")
            break
        payload_len = get_u32(bhdr, 14)
        flag = bhdr[5]
        block_size = 34 + payload_len + (16 if (flag & FLAG_ENCRYPTED) else 0)
        if off + block_size > len(data):
            errors.append(f"{path} off={off}: truncated block")
            break
        raw_len = get_u32(bhdr, 10)
        payload = data[off+34:off+34+payload_len]
        tag = data[off+34+payload_len:off+34+payload_len+16] if (flag & FLAG_ENCRYPTED) else None

        blk_res = decode_block_raw(bhdr, payload, tag, flag, raw_len, kid_keys, fh.kid)
        if blk_res.error:
            errors.append(f"{path} seq={blk_res.seqno}: {blk_res.error}")
            break
        if blk_res.raw_bytes:
            slices = parse_lgxe(blk_res.raw_bytes)
            for s in slices:
                for full_payload in reassembler.feed(s):
                    user_records.append(full_payload)
        off += block_size
    return user_records, errors

# ---------------- 内部用户载荷 → 可读行 ----------------
def parse_user_payload(b: bytes) -> Tuple[int, int, int, int, str, bytes]:
    """返回 (ts_ms, pid, tid, level, tag, msg)。"""
    if len(b) < 27:
        raise RuntimeError("user payload too short (<27B)")
    ts_ms = struct.unpack('<Q', b[0:8])[0]
    pid   = struct.unpack('<I', b[8:12])[0]
    tid   = struct.unpack('<Q', b[12:20])[0]
    level = b[20]
    tag_len = struct.unpack('<H', b[21:23])[0]
    tag = b[23:23+tag_len].decode('utf-8', errors='replace')
    msg_len = struct.unpack('<I', b[23+tag_len:23+tag_len+4])[0]
    msg = b[23+tag_len+4:23+tag_len+4+msg_len]
    return ts_ms, pid, tid, level, tag, msg

# ---------------- main ----------------
def parse_kid_map(s: str) -> Dict[int, bytes]:
    out = {}
    for kv in s.split(','):
        kv = kv.strip()
        if not kv: continue
        kid_s, key_hex = kv.split('=', 1)
        kid = int(kid_s)
        key = bytes.fromhex(key_hex)
        if len(key) != 32:
            raise RuntimeError(f"kid {kid}: key must be 32 bytes (got {len(key)})")
        out[kid] = key
    return out

def main(argv: List[str]) -> int:
    ap = argparse.ArgumentParser(description="LogX 离线日志解码工具")
    ap.add_argument('log_dir', help="日志存储目录")
    ap.add_argument('--from', dest='from_date', default=None, help="起始日期 YYYYMMDD")
    ap.add_argument('--to',   dest='to_date',   default=None, help="结束日期 YYYYMMDD")
    ap.add_argument('--kid-map', default='', help='KID与16进制密钥映射: kid=hexkey,kid=hexkey,...')
    ap.add_argument('--out', default=None, help='输出目录;省略则输出至 stdout')
    args = ap.parse_args(argv)

    kid_keys = parse_kid_map(args.kid_map) if args.kid_map else {}

    if not os.path.exists(args.log_dir):
        print(f"[error] log_dir not found: {args.log_dir}", file=sys.stderr)
        return 1

    files = sorted(
        os.path.join(args.log_dir, n) for n in os.listdir(args.log_dir)
        if re.match(r'^\d{8}\.log(\.\d+)?$', n)
    )
    if args.from_date: files = [p for p in files if os.path.basename(p)[:8] >= args.from_date]
    if args.to_date:   files = [p for p in files if os.path.basename(p)[:8] <= args.to_date]

    if not files:
        print("[no matching files]", file=sys.stderr)
        return 1

    out_dir = args.out
    if out_dir: os.makedirs(out_dir, exist_ok=True)
    out_fh = None
    out_path = None

    rc = 0
    for path in files:
        date = os.path.basename(path)[:8]
        if out_dir:
            if out_fh: out_fh.close()
            out_path = os.path.join(out_dir, f"plain_{date}.log")
            out_fh = open(out_path, 'w', encoding='utf-8')
        else:
            out_fh = sys.stdout
            out_path = None

        user_payloads, errors = decode_file_records(path, kid_keys)
        for err in errors:
            print(f"[{date}] {err}", file=sys.stderr)
            rc = 2

        for payload in user_payloads:
            try:
                ts_ms, pid, tid, level, tag, msg = parse_user_payload(payload)
            except Exception as e:
                print(f"[{date}] bad user payload: {e}", file=sys.stderr)
                rc = 2
                continue
            try:
                msg_text = msg.decode('utf-8', errors='replace')
            except Exception:
                msg_text = repr(msg)
            level_names = {0: "V", 1: "D", 2: "I", 3: "W", 4: "E", 5: "F"}
            lvl_str = level_names.get(level, str(level))
            line = f"{date} {ts_ms} pid={pid} tid={tid} [{lvl_str}] {tag}: {msg_text}\n"
            out_fh.write(line)
    if out_dir and out_fh:
        out_fh.close()
        print(f"[wrote {out_path}]", file=sys.stderr)
    return rc

if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))