#include "core/device.h"
#include "core/protocol.h"

// ===========================================================================
//  待实现（你写）。核心是"发一帧 → 等一帧 → 解析"，带超时。
//  可参考 tools/uart_probe.py 里 probe/probe 的收发逻辑（Python 版，逻辑一致）。
// ===========================================================================

struct TimingDevice::Impl {
    // 你需要的成员大致是：
    //   QSerialPort port;
    //   std::vector<std::uint8_t> rx;   // 接收累积缓冲
};

TimingDevice::TimingDevice(QObject* parent) : QObject(parent), d(new Impl)
{
}

TimingDevice::~TimingDevice()
{
    disconnectPort();
    delete d;
}

void TimingDevice::connectPort(const QString& port, int baud)
{
    // TODO:
    //   d->port.setPortName(port);
    //   d->port.setBaudRate(baud);
    //   d->port.setDataBits(QSerialPort::Data8);
    //   d->port.setParity(QSerialPort::NoParity);
    //   d->port.setStopBits(QSerialPort::OneStop);
    //   d->port.setFlowControl(QSerialPort::NoFlowControl);
    //   打开失败 -> throw DeviceError(d->port.errorString().toStdString());
    //   打开成功后清空 d->rx
    (void)port;
    (void)baud;
    throw DeviceError("connectPort() 尚未实现");
}

void TimingDevice::disconnectPort()
{
    // TODO: 若已打开则 close()，并清空 d->rx
}

bool TimingDevice::isConnected() const
{
    // TODO: return d->port.isOpen();
    return false;
}

// ---------------------------------------------------------------------------
// 建议先写一个内部辅助（不必是成员，可以是文件内的 static 函数）：
//
//   proto::Frame sendAndWait(std::uint8_t cmd,
//                            const std::vector<std::uint8_t>& payload,
//                            int timeoutMs = 500);
//
// 它的职责：
//   1. buildFrame() 组帧，port.write()
//   2. 循环直到超时：port.waitForReadyRead(剩余时间)
//                     把读到的新字节 append 到 d->rx
//                     parseFrame(d->rx) 若返回帧，校验 cmd == (请求码|0x80)
//   3. 超时 -> throw DeviceError("timeout")
//   4. 响应 PAYLOAD[0] != ResOk -> throw DeviceError(对应的错误含义)
//
// 注意：GUI 主线程调用会阻塞最多 timeoutMs，所以超时不要设太大（500 ms 足够）。
// ---------------------------------------------------------------------------

void TimingDevice::setFreq(int hz)
{
    // TODO: payload = { ParamFreq, hz>>16, (hz>>8)&0xFF, hz&0xFF }（大端 3 字节）
    //       sendAndWait(CmdSetParam, payload)
    (void)hz;
    throw DeviceError("setFreq() 尚未实现");
}

int TimingDevice::getFreq()
{
    // TODO: sendAndWait(CmdGetParam, {ParamFreq})
    //       payload = [RESULT, b2, b1, b0] -> 拼成 int 返回
    throw DeviceError("getFreq() 尚未实现");
}

void TimingDevice::start()
{
    // TODO: sendAndWait(CmdStart, {})
    throw DeviceError("start() 尚未实现");
}

void TimingDevice::stop()
{
    // TODO: sendAndWait(CmdStop, {})
    throw DeviceError("stop() 尚未实现");
}

Status TimingDevice::getStatus()
{
    // TODO: sendAndWait(CmdGetStatus, {})
    //       payload = [RESULT, run, b2, b1, b0, err]
    //       Status{ run != 0, freq, err }
    throw DeviceError("getStatus() 尚未实现");
}

int TimingDevice::selfTest()
{
    // TODO: sendAndWait(CmdSelfTest, {})
    //       payload = [RESULT, errHi, errLo] -> (errHi<<8)|errLo
    throw DeviceError("selfTest() 尚未实现");
}
