// 协议自检：不依赖 Qt、不接硬件。
// 用法：cmake --build build --target protocol-selftest && ./build/protocol-selftest
//
// 全部通过 = 你的 crc16 / buildFrame / parseFrame 与 docs/PROTOCOL.md 一致。

#include <cstdio>
#include <string>
#include <vector>

#include "core/protocol.h"

namespace {

int g_pass = 0;
int g_fail = 0;

std::string hex(const std::vector<std::uint8_t>& v)
{
    std::string s;
    char b[4];
    for (auto x : v) {
        std::snprintf(b, sizeof b, "%02X ", static_cast<unsigned>(x));
        s += b;
    }
    if (!s.empty()) s.pop_back();
    return s;
}

std::vector<std::uint8_t> fromHex(const std::string& s)
{
    std::vector<std::uint8_t> out;
    int hi = -1;
    for (char c : s) {
        int v = -1;
        if (c >= '0' && c <= '9') v = c - '0';
        else if (c >= 'A' && c <= 'F') v = c - 'A' + 10;
        else if (c >= 'a' && c <= 'f') v = c - 'a' + 10;
        else continue;
        if (hi < 0) hi = v;
        else { out.push_back(static_cast<std::uint8_t>((hi << 4) | v)); hi = -1; }
    }
    return out;
}

void checkFrame(const char* name, std::uint8_t cmd,
                const std::vector<std::uint8_t>& payload, const std::string& wantHex)
{
    auto got = proto::buildFrame(cmd, payload);
    auto want = fromHex(wantHex);
    bool ok = (got == want);
    std::printf("  [%s] %-22s want[%s] got[%s]\n", ok ? "OK" : "FAIL", name,
                hex(want).c_str(), hex(got).c_str());
    ok ? ++g_pass : ++g_fail;
}

void checkParse(const char* name, const std::string& rawHex,
                std::uint8_t cmd, const std::vector<std::uint8_t>& payload,
                std::size_t restAfter = 0)
{
    auto buf = fromHex(rawHex);
    auto f = proto::parseFrame(buf);
    bool ok = f && f->cmd == cmd && f->payload == payload && buf.size() == restAfter;
    std::printf("  [%s] %-22s cmd=0x%02X len=%zu rest=%zu\n", ok ? "OK" : "FAIL", name,
                f ? f->cmd : 0, f ? f->payload.size() : 0, buf.size());
    if (!ok && f) {
        std::printf("         payload=[%s]\n", hex(f->payload).c_str());
    }
    ok ? ++g_pass : ++g_fail;
}

}  // namespace

int main()
{
    using namespace proto;

    std::printf("=== 请求帧组帧（对照 PROTOCOL.md §4）===\n");
    checkFrame("SET_PARAM 166666", CmdSetParam, fromHex("01 02 8B 0A"),
               "AA 55 05 01 01 02 8B 0A 7B 45");
    checkFrame("GET_PARAM FREQ", CmdGetParam, fromHex("01"),
               "AA 55 02 02 01 10 A0");
    checkFrame("START", CmdStart, {},
               "AA 55 01 03 40 21");
    checkFrame("STOP", CmdStop, {},
               "AA 55 01 04 01 E3");
    checkFrame("GET_STATUS", CmdGetStatus, {},
               "AA 55 01 05 C0 23");
    checkFrame("SELF_TEST", CmdSelfTest, {},
               "AA 55 01 06 80 22");

    std::printf("\n=== 响应帧解析 ===\n");
    checkParse("SET_PARAM OK", "AA 55 02 81 00 B0 50",
               static_cast<std::uint8_t>(CmdSetParam | RespBit), fromHex("00"));
    checkParse("GET_PARAM 166666", "AA 55 05 82 00 02 8B 0A 3F 67",
               static_cast<std::uint8_t>(CmdGetParam | RespBit), fromHex("00 02 8B 0A"));
    checkParse("GET_STATUS run=1", "AA 55 07 85 00 01 02 8B 0A 00 9F D7",
               static_cast<std::uint8_t>(CmdGetStatus | RespBit), fromHex("00 01 02 8B 0A 00"));
    checkParse("SELF_TEST err=0", "AA 55 04 86 00 00 00 FC 88",
               static_cast<std::uint8_t>(CmdSelfTest | RespBit), fromHex("00 00 00"));

    std::printf("\n=== 噪声前缀 ===");
    checkParse("噪声+一帧", "00 FF 7E AA 55 01 05 C0 23",
               static_cast<std::uint8_t>(CmdGetStatus | RespBit), {});
    // 注意：上一条期望 cmd=0x85、payload 为空（LEN=1 时 payload 长度 0）

    std::printf("\n=== 粘包（两帧连发）===\n");
    {
        auto buf = fromHex("AA 55 01 05 C0 23 AA 55 01 03 40 21");
        auto f1 = proto::parseFrame(buf);
        auto f2 = proto::parseFrame(buf);
        bool ok = f1 && f2 && f1->cmd == 0x85 && f2->cmd == 0x83 && buf.empty();
        std::printf("  [%s] 两帧分离            f1=0x%02X f2=0x%02X rest=%zu\n",
                    ok ? "OK" : "FAIL", f1 ? f1->cmd : 0, f2 ? f2->cmd : 0, buf.size());
        ok ? ++g_pass : ++g_fail;
    }

    std::printf("\n=== CRC 错误应被丢弃 ===\n");
    {
        // GET_STATUS 的最后一字节故意改错
        auto buf = fromHex("AA 55 01 05 C0 FF");
        auto f = proto::parseFrame(buf);
        bool ok = !f;   // 不应返回帧
        std::printf("  [%s] 坏 CRC 丢弃          returned=%s\n",
                    ok ? "OK" : "FAIL", f ? "frame" : "nullopt");
        ok ? ++g_pass : ++g_fail;
    }

    std::printf("\n========================================\n");
    std::printf("通过 %d / 失败 %d\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
