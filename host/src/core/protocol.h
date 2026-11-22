#pragma once

// 协议层：帧格式见 docs/PROTOCOL.md（唯一权威）。
// 本文件是纯 C++，不依赖 Qt —— 你可以脱离 GUI 单独编译/测试。
//
// 帧格式： AA 55 LEN CMD PAYLOAD... CRClo CRChi
//   LEN   = CMD + PAYLOAD 的字节数（不含帧头/CRC）
//   CRC16 = Modbus（初值 0xFFFF，多项式 0xA001），覆盖 [LEN .. PAYLOAD末]
//   CRC 低字节先发；多字节数值一律大端（CRC 除外）

#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

namespace proto {

constexpr std::uint8_t MAGIC0 = 0xAA;
constexpr std::uint8_t MAGIC1 = 0x55;

// ---- 请求命令码 ----
enum Cmd : std::uint8_t {
    CmdSetParam  = 0x01,  // payload: param_id(1) + value(3)
    CmdGetParam  = 0x02,  // payload: param_id(1)
    CmdStart     = 0x03,  // payload: 空
    CmdStop      = 0x04,  // payload: 空
    CmdGetStatus = 0x05,  // payload: 空
    CmdSelfTest  = 0x06,  // payload: 空
};

// ---- 参数 ID ----
constexpr std::uint8_t ParamFreq = 0x01;

// ---- 响应结果码（PAYLOAD[0]）----
enum Result : std::uint8_t {
    ResOk       = 0x00,
    ResErrCrc   = 0x01,
    ResErrUnk   = 0x02,
    ResErrRange = 0x03,
};

// 响应帧的 CMD 是 "请求码 | 0x80"
constexpr std::uint8_t RespBit = 0x80;

// 一个解析出来的帧（已通过 CRC 校验）
struct Frame {
    std::uint8_t         cmd = 0;   // 原始 CMD（响应帧含 bit7）
    std::vector<std::uint8_t> payload;
};

// CRC16/Modbus。覆盖范围由调用者给出（本协议是 [LEN .. PAYLOAD末]）。
std::uint16_t crc16(const std::uint8_t* data, std::size_t len);

// 组一帧请求。payload 为空表示无参数命令。
std::vector<std::uint8_t> buildFrame(std::uint8_t cmd,
                                     const std::vector<std::uint8_t>& payload = {});

// 从累积缓冲区里取出一个"完整且 CRC 正确"的帧。
// 返回 std::nullopt 表示当前还凑不出一帧。必须处理三种情况：
//   1. 噪声前缀（不是 AA 55）——跳过，继续找帧头
//   2. 粘包（一次收到多帧）——只取出第一帧，剩余留在 buf 里给下次用
//   3. CRC 失败——丢弃该帧头，从后一个字节继续重新同步
// 成功时要把已消费的字节从 buf 头部删掉。
std::optional<Frame> parseFrame(std::vector<std::uint8_t>& buf);

}  // namespace proto
