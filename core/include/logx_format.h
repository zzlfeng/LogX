// logx_format.h - 序列化/分片/校验和公共声明。
//
// 该头仅对 core 内部使用;不在 C ABI 中暴露。

#ifndef LOGX_FORMAT_H
#define LOGX_FORMAT_H

#include <cstdint>
#include <vector>

namespace logx {

// 单条日志 staging 总上限(格式化后,含记录头与全部分片);由 Config.maxLogLength 注入。
constexpr uint32_t kMaxRecordTotal = 64 * 1024;

// 单条日志格式化"内部用户载荷"上限(扣 6B 头约 60KB);超过此值整条丢弃。
constexpr uint32_t kMaxUserPayload = 60 * 1024;

// 单片 LGXE payload 上限(20KB);超过即分片。
constexpr uint32_t kMaxSlicePayload = 20 * 1024;

// staging 记录头 6B;len = payload 字节数。
// magic 存为小端 2B;磁盘字节顺序为字面 'L','G'(条目) / 'G','P'(PAD)。
//   kMagicEntry 低字节 'L'=0x4c, 高字节 'G'=0x47 -> 写入 [0x4c,0x47] -> 'L','G'。
//   kMagicPad   低字节 'G'=0x47, 高字节 'P'=0x50 -> 写入 [0x47,0x50] -> 'G','P'。
constexpr uint32_t kHeaderLen = 6;
constexpr uint16_t kMagicEntry = 0x474c;   // 高字节 0x47='G', 低字节 0x4c='L'
constexpr uint16_t kMagicPad   = 0x5047;   // 高字节 0x50='P', 低字节 0x47='G'

// 写入环形缓冲区的多片 LGXE 记录(同条日志的所有分片,连续)。
// 失败原因:level 非法 / tag 过长 / payload 总长 > 60KB / 输出缓冲不足(仅内部防御)。
bool format_record(uint8_t level, const char* tag,
                   const uint8_t* payload, uint32_t payload_len,
                   uint64_t ts_ms, uint32_t pid, uint64_t tid,
                   uint8_t* out_buf, uint32_t out_buf_capacity,
                   uint32_t* out_total_bytes);

// 写一条 LGXP PAD(单条,≤ 20KB);调用方负责循环调用以填满回绕缝。
// 返回写入 out_buf 的字节数(== 6 + pad_len)。
uint32_t format_pad(uint8_t* out_buf, uint32_t pad_len);

// 解析一段 LGXE 序列(单条记录的所有分片,跨 PAD 仍连续)。
// out_user_payload = 拼接后的"内部用户载荷"(hdr_size+tag+msg)。
// 返回 false = 分片不完整、magic 错、cksum 失败。
bool parse_record(const uint8_t* recs, uint32_t recs_len,
                  std::vector<uint8_t>* out_user_payload);

// 流式解析一段 LGXE 序列(单块内可能包含多条独立整记录与/或连续分片)。
// out_user_payloads = 成功还原的完整"内部用户载荷"列表。
// 返回 false = 格式严重损坏(magic 错 / 校验和失败 / 结构截断)。
bool parse_records_stream(const uint8_t* recs, uint32_t recs_len,
                          std::vector<std::vector<uint8_t>>* out_user_payloads,
                          std::vector<uint8_t>* carry_slice_buf = nullptr);

}  // namespace logx
#endif /* LOGX_FORMAT_H */