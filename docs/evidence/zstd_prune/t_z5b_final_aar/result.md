# T-Z5b 最终 AAR 产物与配置证明结果（2026-10-17）

**状态：PASS**

## 门禁结果

| 检查项 | 实测 | 判定 |
|---|---|---|
| AAR 内 `jni/arm64-v8a/liblogx.so`（strip 后） | **371,544 B** | ✅ < 512,000 B，余量 **140,456 B** |
| AAR 内 `libc++_shared.so` | 1,374,336 B | 单列披露，不参与门禁 |
| 总 native 载荷 | **1,745,880 B** | 单列披露（shared STL 口径） |
| AAR zip | 668,819 B | 仅信息，不作为体积指标 |
| AAR SHA-256 | `c7ab3696a1f79277bedea4d5a7e19c6feef41089f26914afa4d5df3f30c7c6bc` | — |
| liblogx.so SHA-256 | `5c4b056cff5dae1eaf2a0d29bb57e6f17960e63f3301323077af3e5e2f7217db` | — |

## 配置证明

`cmake_proof.txt`：

```text
CMAKE_BUILD_TYPE:STRING=MinSizeRel
CMAKE_CXX_FLAGS_MINSIZEREL:STRING=-Oz -DNDEBUG -g
CMAKE_C_FLAGS_MINSIZEREL:STRING=-Oz -DNDEBUG -g
```

- AAR release 的有效 native 构建类型确为 **MinSizeRel**，编译 flags 为
  `-Oz -DNDEBUG -g`（D4-A：对象保留符号，AGP 打包阶段 strip）；
- `aar_listing.txt` 确认 AAR 包含 `classes.jar`、`proguard.txt`、
  `META-INF/.../aar-metadata.properties` 与两个 arm64 `.so`。

## 符号与段

- `readelf_sections.txt`：打包 SO 无 `.debug_*`、无 `.symtab`（已被 AGP strip）；
- `nm_defined.txt`：447 个已定义符号，其中 252 个 `ZSTD_*`；
- `ZSTD_compressBlock_(greedy|lazy|lazy2|btlazy2|btopt|btultra*)` 计数为 **0**，
  与 B0 的 64 个重型符号形成正向对照；
- `size.txt`：text 363,634 B、data 4,744 B、bss 6,192 B。

## 结论

发布 AAR 结构完整、有效配置为 MinSizeRel + `-Oz -DNDEBUG -g`、打包 SO 已 strip
且重型匹配器符号全部消失；G2 体积门禁通过。未 strip 的 MinSizeRel + `-g`
归档产物为 2,810,704 B（D4-A，保留 native 崩溃栈可读性）。
