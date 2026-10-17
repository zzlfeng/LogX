# T-Z3 外部解码对账结果（2026-10-17）

**状态：PASS**

## 验证对象

裁剪后的 zstd（dfast/fast/decompress 保留，Level 5+ 重型匹配器剔除）在真实
Android 加密日志上的互操作性。样本由 `tests/unit/logx_decode_demo.cpp` 以生产配置
生成：zstd Level 3 + AES-256-GCM、kid 1、64KB 分卷、4KB flush 阈值。

## 产物与方法

| 项目 | 值 |
|---|---|
| 设备 | Redmi K30 Pro，Android 12，arm64-v8a，NDK 26.1 |
| 原始日志 | `device_pull/20261017.log`（1,637 B） |
| 产品侧导出 | `device_pull/plain_00.txt`（260,254 B，104 条 `seq/raw/hex` 记录） |
| 独立解码 | `decoded/plain_20261017.log`（131,753 B），`decoder/logx_decode.py` + Python `zstandard 0.25.0` |
| 对账脚本输出 | `reconcile.txt` |
| 解码日志 | `decode_stdout.log` / `decode_stderr.log` |

对账使用独立解码器的二进制记录 API（`decode_file_records` + `parse_user_payload`），
逐条比较 `(seq, 时间戳, pid, tid, level, tag, payload)`，不做文本再格式化。

## 结果

```text
export_records   = 104
decoder_records  = 104
decoder_errors   = []
byte_equal       = True
tags             = {'Main': 20, 'Big': 1, 'Split': 83}
big_records      = 1, msg_len = 40960
```

- 104/104 条记录逐字节一致，包含跨块分片的 40,960 B 长记录；
- 解码器无错误、无损坏块、无占位记录。

## 说明

1. **104/141 的差异不是裁剪回归**：demo 写满 128KB staging 后以
   `staging_wait_ms=0` 立即丢弃（Demo 未检查 `logx_write` 返回值），计划写入
   141 条、实际落盘 104 条。本次验收对象是“所有落盘记录均可被独立解码器逐字节还原”，
   104/104 已满足。
2. **D2 级联 smoke 已闭环**：host 与真机的 `logx_unit_tests` 均包含
   `ZstdCascadeSmoke.AllLevelsOnLogxBlockSize`（level 0–22 × 20,491 B）与
   `ZstdCascadeSmoke.BoundarySizesAtKeyLevels`（0/1/1024/16384/20491/131072 ×
   level 0/3/4/19/22），两套件均 PASS（197/197）。越界 level 全部由上游
   `ZSTD_adjustCParams_internal()` 级联降级，无崩溃、无错误返回。
3. 文本解码输出使用 CRLF；二进制载荷中的非法 UTF-8 字节会被替换为 U+FFFD，
   因此字节级对账一律以二进制记录 API 为准（本样本全为合法 UTF-8 文本）。
