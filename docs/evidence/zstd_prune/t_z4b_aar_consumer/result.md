# T-Z4b G2 AAR 真实产物验证结果（2026-10-17）

**状态：PASS**（APK 安装受 MIUI 限制，改用 app_process 运行同一 AAR 原生库）

## 验证对象

- AAR：`android/logx-aar/logx/build/outputs/aar/logx-release.aar`
  （668,819 B，SHA-256 `c7ab3696…c7c6bc`）
- 打包库：`jni/arm64-v8a/liblogx.so` 371,544 B（SHA-256 `5c4b056c…7217db`）、
  `libc++_shared.so` 1,374,336 B（SHA-256 `c4c2fe5c…d93b`）
- 消费端：`android/logx-aar-consumer/`，`implementation(files(...aar))` 直接依赖真实 AAR

## 执行路径

1. **AAR 消费编译**：`:app:assembleDebug` 成功，产物
   `app/build/outputs/apk/debug/app-debug.apk`，证明 AAR 结构、classes.jar、
   manifest 与 native 打包可被标准 AGP 消费。
2. **安装受限**：MIUI V140 返回 `INSTALL_FAILED_USER_RESTRICTED`；`pm install`、
   `adb_install_need_confirm=0`、`secure adb_install_need_confirm=0` 均无效，设备无 root。
3. **等价回退**：用 d8 打包 app_process Java harness
   （`shell/com/zzlfeng/logx/LogX.java`），在设备上加载 AAR 解包出的
   `liblogx.so` / `libc++_shared.so`；两库 SHA-256 与 AAR 内文件逐一相同，
   因此验证的确实是发布原生产物。

## 设备运行结果

```text
EXPORT date=20261017 size=135030
EXPORT date=20261017 size=138392
EXPORT date=20261017 size=62910
CONSUMER_STATS flushRc=0 writeFails=0 stats=0,0,0,1,0,0
CONSUMER_FILES 20261017.log:66001 20261017.log.1:68530 20261017.log.2:31139
CONSUMER_SMOKE_OK exportRc=0 exportFiles=3
```

- 3 个分卷正常滚动；`writeFails=0`；
- stats 六字段依次为 `drop_no_space/drop_too_large/truncated_blocks/pad_records/block_write_errors/truncated_logs`
  = `0,0,0,1,0,0`：零丢弃、零 IO 错误，仅 1 条 PAD 记录；
- 导出 3 个明文分卷（135,030 / 138,392 / 62,910 B）。

## 外部解码对账

| 分卷 | 导出条数 | 解码条数 | 逐字节一致 |
|---|---:|---:|---|
| 20261017.log | 45 | 45 | ✅ |
| 20261017.log.1 | 66 | 66 | ✅ |
| 20261017.log.2 | 30 | 30 | ✅ |
| **合计** | **141** | **141** | **✅ 141/141** |

```text
tags             = {'Main': 20, 'Big': 1, 'Split': 120}
big_msg_len      = 40960
decoder_errors   = []（三个分卷均为空）
concatenated_equal = True
```

对账使用独立解码器的二进制记录 API（`decoder/logx_decode.py` +
Python `zstandard 0.25.0`），详见 `reconcile.txt`；文本解码输出在
`decoded/plain_20261017.log`（exit=0）。注意：随机二进制载荷中的非法 UTF-8
字节在文本输出中会被替换为 U+FFFD，因此字节级结论以二进制 API 为准。

## 结论

AAR 结构、真实 G2 原生库、真机 zstd Level 3 + AES-256-GCM、分卷导出与
独立解码全链路闭环；唯一未覆盖项是“通过 `am start` 安装 APK”这一步，
原因是设备 MIUI 安装策略而非产物缺陷，已在计划 T-Z4b 节记录。
