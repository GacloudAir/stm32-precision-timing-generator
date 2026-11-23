#include "core/protocol.h"

// ===========================================================================
//  待实现（你写）。完成后运行 protocol-selftest 对照 PROTOCOL.md 的全部向量。
// ===========================================================================

namespace proto {

std::uint16_t crc16(const std::uint8_t* data, std::size_t len)
{
    std::uint16_t crc = 0xFFFF;
    for (std::size_t i = 0; i < len; ++i) {
        crc ^= data[i];
        for (int j = 0; j < 8; ++j) {
            if (crc & 1) {
                crc = (crc >> 1) ^ 0xA001;
            } else {
                crc >>= 1;
            }
        }
    }
    return crc;
}

std::vector<std::uint8_t> buildFrame(std::uint8_t cmd,const std::vector<std::uint8_t>& payload)
{
    const auto len = static_cast<std::uint8_t>(1 + payload.size());
    std::vector<std::uint8_t> frame{0xAA, 0x55, len, cmd};
    frame.insert(frame.end(), payload.begin(), payload.end());

    const auto crc = crc16(frame.data() + 2, static_cast<std::size_t>(len) + 1);
    frame.push_back(static_cast<std::uint8_t>(crc & 0xFF));
    frame.push_back(static_cast<std::uint8_t>(crc >> 8));
    return frame;
}

std::optional<Frame> parseFrame(std::vector<std::uint8_t>& buf)
{
    while (true) {
        std::size_t i = 0;
        while (i + 1 < buf.size() &&
               (buf[i] != 0xAA || buf[i + 1] != 0x55)) {
            ++i;
        }

        if (i + 1 >= buf.size()) {
            const bool keepTrailingHeaderByte = !buf.empty() && buf.back() == 0xAA;
            buf.clear();
            if (keepTrailingHeaderByte) {
                buf.push_back(0xAA);
            }
            return std::nullopt;
        }
        if (i > 0) {
            buf.erase(buf.begin(), buf.begin() + i);
        }
        if (buf.size() < 3) {
            return std::nullopt;
        }

        const auto len = buf[2];
        if (len < 1 || len > 27) {
            buf.erase(buf.begin(), buf.begin() + 2);
            continue;
        }
        const auto frameSize = static_cast<std::size_t>(len) + 5;
        if (buf.size() < frameSize) {
            return std::nullopt;
        }

        const auto crc = crc16(buf.data() + 2, static_cast<std::size_t>(len) + 1);
        if (buf[3 + len] != static_cast<std::uint8_t>(crc & 0xFF) ||
            buf[4 + len] != static_cast<std::uint8_t>(crc >> 8)) {
            buf.erase(buf.begin(), buf.begin() + 2);
            continue;
        }

        Frame frame{buf[3],
                    std::vector<std::uint8_t>(buf.begin() + 4,
                                              buf.begin() + 3 + len)};
        buf.erase(buf.begin(), buf.begin() + frameSize);
        return frame;
    }
}
}  // namespace proto
