#!/usr/bin/env python3
"""LogX 离线解码工具(参见 spec §2 / §10)。

功能:
  - 读 prefix_YYYYMMDD.log(或 .1/.2 …),逐块解析
  - 解码每块的 payload(压缩 + 加密);加密块需要 key_provider 提供密钥
  - 解码后的 LGXE 序列按记录边界重组(支持跨块分片重组)
  - 把"内部用户载荷"还原为可读行(ts/pid/tid/level/tag/msg)
  - 跳过块内 LGXP PAD 与块尾 <6B 零残留(spec §3.1/§3.2)

错误处理策略(D-01~D-03,与 C++ 导出路径 decode_one_file /
parse_records_stream 现行语义对齐):
  - 块头结构损坏(bad magic / 版本 / 保留位 / seqno 回退 / 长度越界 / 截断):
    终止该文件扫描 —— 块边界自描述于块头,结构损坏后无法定位下一块,
    盲目继续会以错误 block_size 滑动导致整文件错位(对齐 C++ F-05
    block_header_plausible:宁丢不脏);
  - 块级解码失败(payload CRC / GCM / 解压 / 缺 key):输出与 C++ 导出一致的
    占位行 "[seq=N crc fail]" / "[seq=N decode fail]" 后跳过该块继续扫描,
    绝不截断文件后半部分(对齐 C++ 导出路径);
  - 块内记录损坏(cksum 失败 / 长度越界 / magic 失效含零段):跳过损坏记录或
    滑动重同步后继续解析,并丢弃跨块分片残片(对齐 C++ FM-02/FM-04),
    绝不因单条记录损坏丢弃整块。
  与 C++ 的差异仅在事件通道:C++ 经 emit_event 上报进程内监控,本工具打印
  stderr 并置退出码 2。

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
from dataclasses import dataclass
from typing import Optional, Dict, List, Tuple

MAGIC_FILE = b'LGXF'
MAGIC_BLOCK = b'LGXB'
MAGIC_ENTRY = b'LG'
MAGIC_PAD   = b'GP'

# 压缩算法编号,与 C++ core/include/logx_codec.h 的 kAlgoNone/kAlgoZstd/kAlgoZlib
# 严格一致。块头 flag 字节布局: bit0=加密(FLAG_ENCRYPTED),
# bit1..2=算法编号左移 FLAG_COMPRESS_SHIFT 位(掩码 COMP_MASK)。
ALGO_NONE = 0
ALGO_ZSTD = 1
ALGO_ZLIB = 2

FLAG_ENCRYPTED      = 0x01
COMP_MASK           = 0x06
FLAG_COMPRESS_SHIFT = 1

# 与 C++ core/include/logx_staging.h 的块格式常量严格一致 (spec §3.2):
# 单块解压输出上限 = kBlockMaxRaw (单条独占一块上限,含 6B 记录头)。
# (D-05) C++ 写入端单块 raw ≤ 20KB+6,超限块头在 F-05 严检即被拒绝;
# 解码端按同一契约收紧,防畸形帧触发过度内存分配 (原 64MB 过宽)。
# (R26 B-9 方案A) 上限放宽至 kBlockMaxRawWithSeam = 20KB+6+5 = 20491:
# 留零缝(≤5B)并入块尾后的合法块上限,与 logx_staging.h 派生式严格对齐。
MAX_BLOCK_RAW = 20 * 1024 + 6 + 5

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
        # (D-05) 输出上限对齐块格式契约 MAX_BLOCK_RAW (20KB+6);
        # 合法块解压结果恒 ≤ 上限,超限即畸形帧,拒绝过度分配。
        return zstandard.ZstdDecompressor().decompress(
            payload, max_output_size=MAX_BLOCK_RAW)
    if algo == ALGO_ZLIB:
        import zlib
        # zlib.decompress 无输出上限参数:用 decompressobj 限长解压,
        # 解出超过 MAX_BLOCK_RAW 即判畸形 (防 zip bomb 过度分配)。
        d = zlib.decompressobj()
        out = d.decompress(payload, MAX_BLOCK_RAW + 1)
        if len(out) > MAX_BLOCK_RAW:
            raise RuntimeError(f"decompressed output exceeds {MAX_BLOCK_RAW}B")
        if not d.eof:
            out += d.flush()
            if not d.eof:
                raise RuntimeError("zlib stream did not reach EOF")
        if len(out) > MAX_BLOCK_RAW:
            raise RuntimeError(f"decompressed output exceeds {MAX_BLOCK_RAW}B")
        return out
    raise RuntimeError(f"unknown algo {algo}")

def decrypt_gcm(key: bytes, nonce: bytes, ct_with_tag: bytes) -> bytes:
    """AES-256-GCM;返回明文;cipher+tag 拼在一起传入。

    C++ 侧 encrypt_gcm 无 AAD (gcm_starts 的 add_len=0),此处必须显式传
    associated_data=None —— cryptography 库该参数无默认值 (D 系列验证时
    发现的解码器潜在缺陷,缺失即 TypeError)。
    """
    try:
        from cryptography.hazmat.primitives.ciphers.aead import AESGCM
    except ImportError:
        raise RuntimeError("pip install cryptography for AES-GCM blocks")
    return AESGCM(key).decrypt(nonce, ct_with_tag, None)

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

    def drop_carry(self) -> None:
        """(FM-02 对齐) 字节流断裂后,未闭合的跨块分片残片必须整体丢弃 ——
        重同步点之后找到的首/中片属于新流,与旧残片拼接只会产生错乱输出。"""
        self.cur_slices.clear()
        self.in_split = False

def parse_lgxe_segments(raw: bytes) -> Tuple[List[List[LogSlice]], bool]:
    """从 raw 字节流中抽取 LGXE 分片,按"流断裂点"切分为段。

    段边界 (每次边界 = 一次 carry 丢弃,对齐 C++ parse_records_stream 现行语义):
      - cksum 失败: 跳过整条损坏记录 (6B 头 + len,FM-04: 记录按头起始对齐,
        损坏记录内不存在合法子记录,逐字节重扫既找不回数据又 O(n²)),不丢块内
        后续健康记录 —— 仅丢弃跨片残片;
      - 记录头声明长度越界: 头长不可信,滑动 6B 重扫 (FM-04),丢弃残片;
      - magic 失效 (含零段): 字节滑动重同步到下一个合法 magic (要求 6B 头完整),
        重同步失败则停止本块扫描;残片丢弃 (FM-02)。
    返回 (segments, carry_ok):carry_ok=False 表示流末尾存在断裂事件 (撕裂尾/
    重同步失败),调用方须在喂完本块后丢弃重组器 carry;段间残片由调用方在
    段边界处丢弃。块末尾干净收束时 carry 合法保留,跨块分片照常衔接。
    """
    segments: List[List[LogSlice]] = []
    cur: List[LogSlice] = []
    pos = 0
    carry_ok = True
    while pos + 6 <= len(raw):
        magic = raw[pos+2:pos+4]
        if magic != MAGIC_ENTRY and magic != MAGIC_PAD:
            # 零段 / 未知 magic:字节滑动重同步 (对齐 C++ FM-02 路径)
            carry_ok = False
            found = -1
            for s in range(1, len(raw) - pos - 5):
                nxt = raw[pos+s+2:pos+s+4]
                if nxt == MAGIC_ENTRY or nxt == MAGIC_PAD:
                    found = s
                    break
            if cur:
                segments.append(cur)
                cur = []
            if found < 0:
                break  # 重同步失败:停止本块扫描
            pos += found
            continue
        payload_len = get_u16(raw, pos)
        if pos + 6 + payload_len > len(raw):
            # 长度越界 (撕裂尾或头长不可信):FM-04 对齐 —— 丢弃残片、
            # 滑动 6B 继续扫,而非整块放弃 (后续可能仍有健康记录)
            carry_ok = False
            if cur:
                segments.append(cur)
                cur = []
            pos += 6
            continue
        if magic == MAGIC_PAD:
            pos += 6 + payload_len
            continue
        flags = raw[pos+4]
        cksum = raw[pos+5]
        payload = raw[pos+6:pos+6+payload_len]
        if cksum1(payload) != cksum:
            # (D-01) 对齐 C++ FM-04 现行语义:跳过整条损坏记录重新对齐,
            # 单条损坏不丢整块;残片经段边界丢弃。
            carry_ok = False
            if cur:
                segments.append(cur)
                cur = []
            pos += 6 + payload_len
            continue
        cur.append(LogSlice(flags, payload))
        carry_ok = True
        pos += 6 + payload_len
    if cur:
        segments.append(cur)
    return segments, carry_ok

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
                     kid_in_file: int, file_salt: bytes) -> DecodedBlockResult:
    """解码单块。错误时 error 字段以固定前缀分类 (D-03):
    'crc32 mismatch' / 'gcm fail' / 'decompress fail' / 'missing key' ——
    上层据此生成与 C++ 导出一致的占位行 ([seq=N crc fail] / [seq=N decode fail])。

    GCM 校验失败策略差异 (D-03,与 C++ 现行行为一致,此处显式归档):
      C++ 导出路径 (logx_c.cpp:decode_one_file) = 输出 "[seq=N decode fail]"
      占位行 + emit_event 进程内监控,继续下一块;
      本工具 = errors 收集 + stderr 打印 + 退出码 2,同样继续下一块。
      差异仅在事件通道,C++ 走 emit_event、本工具走 stderr。
    """
    seqno = get_u32(bhdr, 6)
    expected_crc = get_u32(bhdr, 18)
    actual_crc = crc32_update(0, payload)
    if expected_crc != actual_crc:
        return DecodedBlockResult(seqno, raw_len, error=f"crc32 mismatch: expected {expected_crc:#x} != {actual_crc:#x}")

    algo = (flag & COMP_MASK) >> FLAG_COMPRESS_SHIFT
    enc = bool(flag & FLAG_ENCRYPTED)
    if enc:
        key = kid_keys.get(kid_in_file)
        if key is None:
            return DecodedBlockResult(seqno, raw_len, error=f"missing key for kid={kid_in_file}")
        # R19 F-15: Nonce 由文件头 salt[8] + seqno_be[4] 派生, 对齐 C++ make_nonce
        nonce = file_salt + struct.pack('>I', seqno)
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
    # R19 F-15: raw_len 与解压输出长度交叉校验, 对齐 C++ decode_block
    if len(raw) != raw_len:
        return DecodedBlockResult(seqno, raw_len, error=f"raw_len mismatch: expected {raw_len} != actual {len(raw)}")
    return DecodedBlockResult(seqno, raw_len, raw_bytes=raw)

# 块头严检常量 (对齐 C++ F-05 block_header_plausible)
BLOCK_HEADER_SIZE = 34
GCM_TAG_SIZE = 16
BLOCK_VER = 1
# raw_len 合法区间 [6, 16MB];payload_len ∈ (0, 16MB]
BLOCK_RAW_MIN = 6
BLOCK_LEN_MAX = 16 * 1024 * 1024

def block_header_plausible(bhdr: bytes, file_remaining: int,
                           prev_seqno: int = 0, has_prev_seqno: bool = False) -> Optional[Tuple[int, int, int, int]]:
    """(D-02) 块头结构严检,对齐 C++ F-05 block_header_plausible。

    校验项与 C++ 逐一对应: magic / 版本 / flag 保留位 / 算法编号 /
    seqno > 0 且严格单调递增 / raw_len ∈ [6, 16MB] / payload_len ∈ (0, 16MB] /
    未压缩时 payload_len == raw_len / 块大小不越过文件尾。
    合法返回 (seqno, raw_len, payload_len, block_size);
    结构损坏返回 None —— 块边界自描述于块头,此时无法定位下一块,必须终止
    该文件扫描 (宁丢不脏,盲目 continue 会以错误 block_size 滑动整文件错位)。
    """
    if len(bhdr) < BLOCK_HEADER_SIZE:
        return None
    if bhdr[:4] != MAGIC_BLOCK:
        return None
    if bhdr[4] != BLOCK_VER:
        return None
    flag = bhdr[5]
    if (flag & ~0x07) != 0:
        return None
    algo = (flag & COMP_MASK) >> FLAG_COMPRESS_SHIFT
    if algo not in (ALGO_NONE, ALGO_ZSTD, ALGO_ZLIB):
        return None
    seqno = get_u32(bhdr, 6)
    if seqno == 0:
        return None
    # R19 F-15: seqno 严格单调递增校验
    if has_prev_seqno and seqno <= prev_seqno:
        return None
    raw_len = get_u32(bhdr, 10)
    if raw_len < BLOCK_RAW_MIN or raw_len > BLOCK_LEN_MAX:
        return None
    payload_len = get_u32(bhdr, 14)
    if payload_len == 0 or payload_len > BLOCK_LEN_MAX:
        return None
    if algo == ALGO_NONE and payload_len != raw_len:
        return None
    enc = bool(flag & FLAG_ENCRYPTED)
    block_size = BLOCK_HEADER_SIZE + payload_len + (GCM_TAG_SIZE if enc else 0)
    if block_size > file_remaining:
        return None
    return seqno, raw_len, payload_len, block_size

def decode_file_records(path: str, kid_keys: Dict[int, bytes]) -> Tuple[List[bytes], List[str]]:
    """解码整个文件，返回 (用户载荷列表, 警告与错误列表)。

    错误处理分两层 (D-02,对齐 C++ decode_one_file 现行语义):
      - 块头结构损坏 → errors 记录后终止本文件扫描 (宁丢不脏);
      - 块级解码失败 (payload CRC / GCM / 解压 / 缺 key) → errors 记录后
        跳过该块继续,文件后半部分照常解码,绝不截断。
    """
    try:
        with open(path, 'rb') as f:
            data = f.read()
    except OSError as e:
        return [], [f"{path}: open/read failed: {e}"]

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
    prev_seqno = 0
    has_prev_seqno = False
    while off + BLOCK_HEADER_SIZE <= len(data):
        bhdr = data[off:off+BLOCK_HEADER_SIZE]
        hdr = block_header_plausible(bhdr, len(data) - off, prev_seqno, has_prev_seqno)
        if hdr is None:
            # 结构损坏:与 C++ 一致终止扫描 (块边界已不可信)
            errors.append(f"{path} off={off}: bad block header; stop scanning file")
            break
        seqno, raw_len, payload_len, block_size = hdr
        prev_seqno = seqno
        has_prev_seqno = True
        flag = bhdr[5]

        payload = data[off+BLOCK_HEADER_SIZE:off+BLOCK_HEADER_SIZE+payload_len]
        tag = data[off+BLOCK_HEADER_SIZE+payload_len:
                   off+BLOCK_HEADER_SIZE+payload_len+GCM_TAG_SIZE] \
               if (flag & FLAG_ENCRYPTED) else None

        blk_res = decode_block_raw(bhdr, payload, tag, flag, raw_len,
                                   kid_keys, fh.kid, fh.salt)
        if blk_res.error:
            # 块级解码失败:跳过该块继续 (对齐 C++ 导出路径占位行行为)
            kind = blk_res.error.split(':', 1)[0]
            errors.append(f"{path} seq={seqno}: {blk_res.error}")
            if kind == 'crc32 mismatch':
                user_records.append(f"[seq={seqno} crc fail]".encode('ascii'))
            else:
                user_records.append(f"[seq={seqno} decode fail]".encode('ascii'))
            off += block_size
            continue
        if blk_res.raw_bytes:
            # 分段喂入:段边界 = 流断裂事件 (cksum 失败/长度越界/重同步),
            # 段间丢弃重组器 carry (FM-02);块末干净收束则 carry 跨块保留
            segments, carry_ok = parse_lgxe_segments(blk_res.raw_bytes)
            for i, seg in enumerate(segments):
                if i > 0:
                    reassembler.drop_carry()
                for s in seg:
                    for full_payload in reassembler.feed(s):
                        user_records.append(full_payload)
            if not carry_ok:
                reassembler.drop_carry()
        off += block_size

    trailing_len = len(data) - off
    if trailing_len > 0:
        # R19 F-15: 块尾 <34B 撕裂残片输出警告
        errors.append(f"{path}: trailing torn tail ({trailing_len}B < {BLOCK_HEADER_SIZE}B)")
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

def log_file_sort_key(filename: str) -> Tuple[int, int]:
    base = os.path.basename(filename)
    m = re.match(r'^(\d{8})\.log(?:\.(\d+))?$', base)
    if not m:
        return (0, 0)
    date = int(m.group(1))
    split = int(m.group(2)) if m.group(2) else 0
    return (date, split)

def main(argv: List[str]) -> int:
    ap = argparse.ArgumentParser(description="LogX 离线日志解码工具")
    ap.add_argument('log_dir', help="日志存储目录")
    ap.add_argument('--from', dest='from_date', default=None, help="起始日期 YYYYMMDD")
    ap.add_argument('--to',   dest='to_date',   default=None, help="结束日期 YYYYMMDD")
    ap.add_argument('--kid-map', default='', help='KID与16进制密钥映射: kid=hexkey,kid=hexkey,...')
    ap.add_argument('--out', default=None, help='输出目录;省略则输出至 stdout')
    args = ap.parse_args(argv)

    kid_keys = parse_kid_map(args.kid_map) if args.kid_map else {}

    # R19 F-9: 校验目录合法性, 区分普通文件与不存在
    if not os.path.isdir(args.log_dir):
        print(f"[error] log_dir not a valid directory: {args.log_dir}", file=sys.stderr)
        return 1

    try:
        dir_entries = os.listdir(args.log_dir)
    except OSError as e:
        print(f"[error] failed to list log_dir: {e}", file=sys.stderr)
        return 1

    matched_names = [n for n in dir_entries if re.match(r'^\d{8}\.log(\.\d+)?$', n)]
    # R19 F-8: 按 (date, split) 数值排序, 避免 .log.10 排在 .log.2 之前
    matched_names.sort(key=log_file_sort_key)
    files = [os.path.join(args.log_dir, n) for n in matched_names]

    if args.from_date: files = [p for p in files if os.path.basename(p)[:8] >= args.from_date]
    if args.to_date:   files = [p for p in files if os.path.basename(p)[:8] <= args.to_date]

    if not files:
        print("[no matching files]", file=sys.stderr)
        return 1

    out_dir = args.out
    if out_dir: os.makedirs(out_dir, exist_ok=True)
    out_fh = None
    out_path = None
    current_date = None

    rc = 0
    for path in files:
        date = os.path.basename(path)[:8]
        if out_dir:
            if date != current_date:
                if out_fh:
                    out_fh.close()
                    print(f"[wrote {out_path}]", file=sys.stderr)
                current_date = date
                out_path = os.path.join(out_dir, f"plain_{date}.log")
                out_fh = open(out_path, 'w', encoding='utf-8')
        else:
            out_fh = sys.stdout
            out_path = None

        # R19 F-9: 捕获单文件解码异常(如删除竞争/权限/不可读), 避免整轮脚本崩溃中断
        try:
            user_payloads, errors = decode_file_records(path, kid_keys)
        except Exception as e:
            print(f"[{date}] {path}: unhandled decode error: {e}", file=sys.stderr)
            rc = 2
            continue

        for err in errors:
            print(f"[{date}] {err}", file=sys.stderr)
            rc = 2

        for payload in user_payloads:
            # 块级解码失败的占位行 ([seq=N crc fail] / [seq=N decode fail]):
            # 与 C++ 导出路径一致,占位行本身出现在输出流中标记数据空洞,
            # 不进入用户载荷解析。
            if payload.startswith(b'[seq=') and (payload.rstrip().endswith(b' crc fail]') or payload.rstrip().endswith(b' decode fail]')):
                out_fh.write(payload.decode('ascii') + '\n')
                continue
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