#include "core/protocol.h"

// ===========================================================================
//  待实现（你写）。完成后运行 protocol-selftest 对照 PROTOCOL.md 的全部向量。
// ===========================================================================

namespace proto {

std::uint16_t crc16(const std::uint8_t* data, std::size_t len)
{
    // 步骤：
    //   std::uint16_t crc = 0xFFFF;
    //   for 每个字节 b:
    //       crc ^= b;
    //       for (8 次):
    //           若 (crc & 1)  crc = (crc >> 1) ^ 0xA001;
    //           否则          crc >>= 1;
    //   返回 crc
    (void)data;
    (void)len;
    return 0;
}

std::vector<std::uint8_t> buildFrame(std::uint8_t cmd,
                                     const std::vector<std::uint8_t>& payload)
{
    // 步骤：
    //   len = 1 + payload.size()               // LEN = CMD + PAYLOAD
    //   body = { cmd } + payload
    //   把 len 和 body 一起算 CRC（即 CRC 覆盖 LEN 开始的部分）
    //   帧 = { 0xAA, 0x55, len, body..., crc & 0xFF, crc >> 8 }
    (void)cmd;
    (void)payload;
    return {};
}

std::optional<Frame> parseFrame(std::vector<std::uint8_t>& buf)
{
    // 见头文件里的三种情况。一个可用的思路：
    //   1. 在 buf 里找 0xAA 0x55，找不到就清空/跳过噪声
    //   2. 找到后读 LEN，若 buf 剩余不足 LEN+5 就 return nullopt（等更多数据）
    //   3. 校验 LEN 合法性（1..27）和 CRC
    //   4. CRC 对：构造 Frame，erase 掉已消费的字节，返回
    //   5. CRC 错：跳过这个帧头（前进 2 字节）继续找，不要整段丢弃
    (void)buf;
    return std::nullopt;
}

}  // namespace proto
