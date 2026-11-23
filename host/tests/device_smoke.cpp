// device_smoke.cpp -- 联调用命令行工具 / hardware smoke-test utility
//
// 用法 / Usage:
//   device-smoke.exe --port COM10
//   device-smoke.exe --port COM10 --read-only
//   device-smoke.exe --port COM10 --freq 166666
//
// 它直接调用 TimingDevice（和 GUI 完全相同的设备层），逐条打印命令结果，
// 用来在联调时区分"串口链路问题 / 协议问题 / 固件问题"。
// It drives the very same TimingDevice facade as the GUI and prints each
// command result, so an integration test can tell link / protocol / firmware
// problems apart.

#include "core/device.h"
#include "core/protocol.h"

#include <QCoreApplication>
#include <QStringList>

#include <cstdio>
#include <string>

namespace {

int g_pass = 0;
int g_fail = 0;

// 把字节数组打印成 "AA 55 07 ..."，方便和 PROTOCOL.md 的向量对照。
// Print bytes as "AA 55 07 ..." for direct comparison with PROTOCOL.md vectors.
std::string hexDump(const std::vector<std::uint8_t>& bytes)
{
    static const char digits[] = "0123456789ABCDEF";
    std::string out;
    out.reserve(bytes.size() * 3);
    for (std::size_t i = 0; i < bytes.size(); ++i) {
        if (i) out += ' ';
        out += digits[bytes[i] >> 4];
        out += digits[bytes[i] & 0x0F];
    }
    return out;
}

void report(bool ok, const std::string& what, const std::string& detail)
{
    std::printf("[%s] %-28s %s\n", ok ? "OK  " : "FAIL", what.c_str(), detail.c_str());
    ok ? ++g_pass : ++g_fail;
}

}  // namespace

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    const QStringList args = app.arguments();

    // 极简参数解析 / Minimal argument parsing
    QString port;
    int     baud = 115200;
    int     freq = 166666;
    bool    readOnly = false;
    bool    skipSelfTest = false;

    for (int i = 1; i < args.size(); ++i) {
        const QString a = args[i];
        if (a == "--port" && i + 1 < args.size()) port = args[++i];
        else if (a == "--baud" && i + 1 < args.size()) baud = args[++i].toInt();
        else if (a == "--freq" && i + 1 < args.size()) freq = args[++i].toInt();
        else if (a == "--read-only") readOnly = true;
        else if (a == "--skip-self-test") skipSelfTest = true;
        else if (a == "--help" || a == "-h") {
            std::printf("Usage: device-smoke --port COM10 [--baud 115200] "
                        "[--freq 166666] [--read-only] [--skip-self-test]\n");
            return 0;
        } else {
            std::printf("Unknown argument: %s\n", qPrintable(a));
            return 2;
        }
    }

    if (port.isEmpty()) {
        std::printf("ERROR: 必须用 --port 指定串口，例如 --port COM10\n");
        return 2;
    }

    std::printf("=== 联调：%s @ %d 8N1 ===\n", qPrintable(port), baud);

    TimingDevice dev;
    try {
        dev.connectPort(port, baud);
        report(true, "连接串口", port.toStdString());
    } catch (const DeviceError& e) {
        report(false, "连接串口", e.what());
        std::printf("\n结果：%d 通过 / %d 失败 —— 先检查端口是否被占用或线缆是否插好。\n",
                    g_pass, g_fail);
        return 1;
    }

    // 1) GET_STATUS —— 只读，最适合作为第一个探针。
    //    同时记下"现场"，测试结束后还原，避免留下状态改动。
    //    GET_STATUS is read-only, making it the safest first probe.
    //    Also snapshot the initial state so the tool can restore it at the end.
    bool initialRunning = false;
    try {
        const Status st = dev.getStatus();
        initialRunning = st.running;
        report(true, "GET_STATUS",
               "run=" + std::string(st.running ? "1" : "0") +
               " freq=" + std::to_string(st.freq_hz) +
               "Hz err=" + std::to_string(st.err_permille));
    } catch (const DeviceError& e) {
        report(false, "GET_STATUS", e.what());
    }

    // 2) GET_PARAM(FREQ) —— 回读当前配置 / Read back current configuration
    int freqBefore = 0;
    try {
        freqBefore = dev.getFreq();
        report(true, "GET_PARAM(FREQ)", std::to_string(freqBefore) + " Hz");
    } catch (const DeviceError& e) {
        report(false, "GET_PARAM(FREQ)", e.what());
    }

    if (!readOnly) {
        // 3) SET_PARAM(FREQ) —— 写入目标频率 / Write the target frequency
        try {
            dev.setFreq(freq);
            report(true, "SET_PARAM(FREQ)", std::to_string(freq) + " Hz");
        } catch (const DeviceError& e) {
            report(false, "SET_PARAM(FREQ)", e.what());
        }

        // 4) 回读校验：确认固件真的接受并应用了新频率。
        //    Verify the device actually accepted and applied the new value.
        int freqAfter = 0;
        try {
            freqAfter = dev.getFreq();
            report(freqAfter == freq, "回读频率校验",
                   "期望 " + std::to_string(freq) + " Hz, 实际 " +
                   std::to_string(freqAfter) + " Hz");
        } catch (const DeviceError& e) {
            report(false, "回读频率校验", e.what());
        }

        // 5) START -> GET_STATUS(run=1) -> SELF_TEST -> STOP -> GET_STATUS(run=0)
        try {
            dev.start();
            report(true, "START", "");
        } catch (const DeviceError& e) {
            report(false, "START", e.what());
        }

        try {
            const Status st = dev.getStatus();
            report(st.running, "启动后状态",
                   st.running ? "已在输出" : "仍在停止状态");
        } catch (const DeviceError& e) {
            report(false, "启动后状态", e.what());
        }

        if (!skipSelfTest) {
            try {
                const int err = dev.selfTest();
                report(true, "SELF_TEST",
                       std::to_string(err) + " permille" +
                       (err == 0 ? "  (注意：0 也可能表示 PA1->PA6 跳线未接)"
                                 : ""));
            } catch (const DeviceError& e) {
                report(false, "SELF_TEST", e.what());
            }
        }

        try {
            dev.stop();
            report(true, "STOP", "");
        } catch (const DeviceError& e) {
            report(false, "STOP", e.what());
        }

        try {
            const Status st = dev.getStatus();
            report(!st.running, "停止后状态",
                   st.running ? "仍在输出" : "已停止");
        } catch (const DeviceError& e) {
            report(false, "停止后状态", e.what());
        }
    }

    // 还原现场：频率和运行态都恢复到联调前的快照。
    // Restore the scene: put both frequency and run state back to the snapshot
    // taken at the start, so the test leaves no lasting device-state changes.
    if (!readOnly) {
        try {
            if (freqBefore > 0 && freqBefore != freq) {
                dev.setFreq(freqBefore);
                report(true, "还原频率", std::to_string(freqBefore) + " Hz");
            }
            const Status now = dev.getStatus();
            if (now.running != initialRunning) {
                if (initialRunning) dev.start();
                else                dev.stop();
                report(true, "还原运行态", initialRunning ? "运行中" : "已停止");
            }
        } catch (const DeviceError& e) {
            report(false, "还原现场", e.what());
        }
    }

    dev.disconnectPort();
    std::printf("\n结果：%d 通过 / %d 失败\n", g_pass, g_fail);
    if (g_fail == 0)
        std::printf("设备层与固件链路均正常，可以打开 GUI 做人工验收。\n");
    else
        std::printf("排查顺序：TX/RX 交叉接 -> 共地 -> 波特率 115200 -> 固件是否烧录 -> "
                    "python tools/uart_probe.py probe --port %s --cmd GET_STATUS\n",
                    qPrintable(port));
    return g_fail == 0 ? 0 : 1;
}
