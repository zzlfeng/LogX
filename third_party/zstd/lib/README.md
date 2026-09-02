# Vendored zstd 1.5.7 (lib/ only)

Upstream: https://github.com/facebook/zstd/releases/tag/v1.5.7
仅保留 `lib/` 压缩/解压所需源码(common / compress / decompress + 3 个头:
zstd.h / zstd_errors.h / 本 README 之外的均未改动),剔除 deprecated /
dictBuilder / legacy / dll / tests / programs 等。未做任何源码改动。

接入方式:core/CMakeLists.txt 在 LOGX_WITH_ZSTD=ON 时把以下源文件编译进 logx_core:
common/{debug.c,entropy_common.c,error_private.c,fse_decompress.c,pool.c,threading.c,xxhash.c,zstd_common.c}
compress/{fse_compress.c,hist.c,huf_compress.c,zstd_compress.c,zstd_compress_literals.c,zstd_compress_sequences.c,zstd_compress_superblock.c,zstd_double_fast.c,zstd_fast.c,zstd_lazy.c,zstd_ldm.c,zstd_opt.c,zstd_preSplit.c}
decompress/{huf_decompress.c,zstd_ddict.c,zstd_decompress.c,zstd_decompress_block.c}

注:
- 单线程配置(ZSTD_MULTITHREAD 未定义,zstdmt_compress.c 不参与编译)。
- zstd_ddict.c 必须带上:zstd_decompress.c 引用其符号,即使不使用字典。
- 源文件清单若与上游发布不一致,以 upstream Release tarball 的 lib/ 为准。
