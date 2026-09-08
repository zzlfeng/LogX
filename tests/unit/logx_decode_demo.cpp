// logx_decode_demo.cpp — 真机解码联调用引擎驱动器 (M5 / T3.4 验收)。
//
// 流程: logx_open(生产配置: zstd + AES-256-GCM + KeyProvider)
//   → 写入跨日多分卷日志 (含 4KB 长记录触发分片)
//   → logx_export 按日期范围导出
//   → 退出。导出期间 key_provider 同时服务导出解密路径 (kid 轮换覆盖)。
//
// 产物在 /data/local/tmp/logx_dcd/{YYYYMMDD.log,*.log.N},由 adb pull 拉回
// 后用 decoder/logx_decode.py 做离线解码联调 (kid 密钥见下方固定表)。
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "logx_c.h"
#include "logx_codec.h"
#include "logx_pal.h"  // default_pal(): Pal / file_* 直连 (与单测同一路径)

using namespace logx;

// 固定测试密钥 (联调专用, 非生产密钥): kid 1 与 kid 2
static uint8_t kKey1[32] = {
    0x11,0x11,0x11,0x11,0x22,0x22,0x22,0x22,0x33,0x33,0x33,0x33,0x44,0x44,0x44,0x44,
    0x55,0x55,0x55,0x55,0x66,0x66,0x66,0x66,0x77,0x77,0x77,0x77,0x88,0x88,0x88,0x88};
static uint8_t kKey2[32] = {
    0xAA,0xAA,0xAA,0xAA,0xBB,0xBB,0xBB,0xBB,0xCC,0xCC,0xCC,0xCC,0xDD,0xDD,0xDD,0xDD,
    0xEE,0xEE,0xEE,0xEE,0xFF,0xFF,0xFF,0xFF,0x00,0x00,0x00,0x00,0x12,0x34,0x56,0x78};

static const uint8_t* key_provider(uint16_t kid, void* /*user*/) {
    switch (kid) {
        case 1: return kKey1;
        case 2: return kKey2;
        default: return nullptr;  // kid=3 块故意无密钥 → 验证降级占位
    }
}

int main() {
    Pal& pal = default_pal();
    std::string dir = "/data/local/tmp/logx_dcd";
    pal.file_mkdirs(dir.c_str());
    std::string staging = dir + "/staging.mmap";

    logx_config_t cfg{};
    cfg.log_dir = dir.c_str();
    cfg.staging_path = staging.c_str();
    cfg.max_log_length = 64 * 1024;
    cfg.retention_days = 7;
    cfg.max_file_size = 64 * 1024;          // 64KB 快速切分 → 产生 .log.1/.log.2
    cfg.flush_threshold = 4 * 1024;
    cfg.compress_algo = kAlgoZstd;          // 真实 zstd 压缩链路
    cfg.encrypt_enable = 1;
    cfg.kid = 1;
    cfg.key_provider = key_provider;

    logx_instance_t* inst = nullptr;
    logx_status_e rc = logx_open(&cfg, &inst);
    if (rc != LOGX_OK) { std::printf("open fail rc=%d\n", (int)rc); return 1; }

    // 1) 普通整条记录 ×20 (中文 UTF-8 消息覆盖非 ASCII)
    char buf[256];
    for (int i = 0; i < 20; ++i) {
        std::snprintf(buf, sizeof(buf), "hello-%02d 中文消息 payload", i);
        logx_write(inst, 2, "Main", reinterpret_cast<const uint8_t*>(buf),
                   static_cast<uint32_t>(std::strlen(buf)));
    }

    // 2) 单条 40KB 长记录 → 超 20KB 单片上限 → 多分片 + 跨块
    {
        std::string big;
        big.reserve(40 * 1024);
        big.append("BIG:");
        big.append(40 * 1024 - 4, 'B');
        logx_write(inst, 3, "Big", reinterpret_cast<const uint8_t*>(big.data()),
                   static_cast<uint32_t>(big.size()));
    }

    // 3) 64KB 文件切分: 再写 120 条 ×1KB → 应滚动出 .log.1 (seqno 单调衔接)
    for (int i = 0; i < 120; ++i) {
        std::snprintf(buf, sizeof(buf), "split-fill-%03d xx", i);
        std::string msg(buf);
        msg.append(1000, 'x');
        logx_write(inst, 2, "Split", reinterpret_cast<const uint8_t*>(msg.data()),
                   static_cast<uint32_t>(msg.size()));
    }

    logx_flush(inst);

    // 4) 导出: 回调把每个日期的临时明文文件落到 export/ (保留而非删除,
    //    供 adb pull); 覆盖当前日期区间
    uint32_t ymd = 0, hms = 0;
    pal.wallclock_date(&ymd, &hms);
    char from[16], to[16];
    std::snprintf(from, sizeof(from), "%08u", ymd);
    std::snprintf(to, sizeof(to), "%08u", ymd);

    std::string export_dir = dir + "/export";
    pal.file_mkdirs(export_dir.c_str());

    static std::string g_saved_path;   // 回调可见
    auto export_cb = [](void*, const char* /*date*/, const char* path, uint64_t size) -> int {
        // 导出临时文件默认会被 core 删除;此处读出内容另存一份到 export/
        Pal& p = default_pal();
        int src = p.file_open(path, false, false);
        if (src < 0) return 1;
        std::vector<char> data(size);
        p.file_pread(src, data.data(), size, 0);
        p.file_close(src);
        static int seq = 0;
        char name[128];
        std::snprintf(name, sizeof(name), "/data/local/tmp/logx_dcd/export/plain_%02d.txt", seq++);
        int dst = p.file_open(name, true, false);
        if (dst < 0) return 1;
        p.file_write(dst, data.data(), size);
        p.file_close(dst);
        g_saved_path = name;
        return 0;
    };
    rc = logx_export(inst, from, to, export_cb, nullptr);
    std::printf("export rc=%d\n", (int)rc);

    logx_close(inst);

    // 5) 列产物清单供 adb pull
    auto files = pal.file_list(dir.c_str());
    for (const auto& f : files) std::printf("ARTIFACT %s\n", f.c_str());
    return rc == LOGX_OK ? 0 : 2;
}
