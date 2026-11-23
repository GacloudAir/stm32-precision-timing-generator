#include "core/device.h"
#include "core/protocol.h"

#include <QByteArray>
#include <QElapsedTimer>
#include <QSerialPort>

// ===========================================================================
//  设备层：把"串口字节流 + 帧协议"包装成 6 个语义化命令。
//  GUI 只调用 TimingDevice 的公开方法，永远看不到帧/字节/CRC。
//
//  Device facade: wraps the serial byte stream and the frame protocol into
//  six semantic commands. The GUI only calls public TimingDevice methods and
//  never sees frames, raw bytes or CRCs.
// ===========================================================================

namespace {

// 把 RESULT 码变成可读文字，方便弹窗/日志定位问题。
// Turn a RESULT code into readable text for dialogs and logs.
std::string resultName(std::uint8_t r)
{
    switch (r) {
    case proto::ResOk:       return "OK";
    case proto::ResErrCrc:   return "ERR_CRC";
    case proto::ResErrUnk:   return "ERR_UNK";
    case proto::ResErrRange: return "ERR_RANGE";
    default:                 return "UNKNOWN(" + std::to_string(r) + ")";
    }
}

// 两字节大写十六进制，用于错误信息里的 CMD 码。
// Two-digit upper-case hex, used for CMD codes in error messages.
std::string hex2(std::uint8_t v)
{
    static const char digits[] = "0123456789ABCDEF";
    return std::string{digits[v >> 4], digits[v & 0x0F]};
}

// 防御性检查：固件版本不匹配时给出清晰错误，而不是让调用方越界读空 vector。
// Defensive check: on firmware/host version mismatch, fail with a clear
// message instead of letting callers index past the end of an empty vector.
void requirePayload(const proto::Frame& f, std::size_t min, const char* what)
{
    if (f.payload.size() < min) {
        throw DeviceError(std::string(what) + ": short payload (got " +
                          std::to_string(f.payload.size()) + ", need " +
                          std::to_string(min) + ")");
    }
}

}  // namespace

struct TimingDevice::Impl {
    QSerialPort port;                   // 串口对象 / serial port handle
    std::vector<std::uint8_t> rx;       // 跨调用累积的接收缓冲区 / persistent RX buffer
};

TimingDevice::TimingDevice(QObject* parent) : QObject(parent), d(new Impl)
{
}

TimingDevice::~TimingDevice()
{
    disconnectPort();
    delete d;
}

// ---------------------------------------------------------------------------
// 连接管理 / Connection management
// ---------------------------------------------------------------------------

void TimingDevice::connectPort(const QString& port, int baud)
{
    // 中文：重复连接时先断开旧的，避免串口被占用或状态残留。
    // EN: If already open, close it first so no stale state or handle remains.
    if (d->port.isOpen())
        d->port.close();

    // 115200 8N1，无流控 —— 与固件 USART1 初始化一致。
    // 115200 8N1, no flow control — must match the firmware's USART1 setup.
    d->port.setPortName(port);
    d->port.setBaudRate(baud);
    d->port.setDataBits(QSerialPort::Data8);
    d->port.setParity(QSerialPort::NoParity);
    d->port.setStopBits(QSerialPort::OneStop);
    d->port.setFlowControl(QSerialPort::NoFlowControl);

    if (!d->port.open(QIODevice::ReadWrite)) {
        throw DeviceError("cannot open " + port.toStdString() + ": " +
                          d->port.errorString().toStdString());
    }

    // 中文：清掉上一次超时可能遗留的半帧，防止污染第一条命令。
    // EN: Drop any half frame left over from an earlier timeout, so the
    //     first command after (re)connect starts from a clean buffer.
    d->rx.clear();
    (void)d->port.readAll();
}

void TimingDevice::disconnectPort()
{
    // 中文：关闭串口并清空接收缓冲；可重复调用。
    // EN: Close the port and clear the RX buffer; safe to call repeatedly.
    if (d->port.isOpen())
        d->port.close();
    d->rx.clear();
}

bool TimingDevice::isConnected() const
{
    // 中文：以"端口是否打开"为唯一连接判据。
    // EN: The port being open is the single source of truth for connection.
    return d->port.isOpen();
}

// ---------------------------------------------------------------------------
// 事务核心：发一帧 -> 等一帧 / Transaction core: send one frame, wait for one
// ---------------------------------------------------------------------------

proto::Frame TimingDevice::sendAndWait(std::uint8_t cmd,
                                       const std::vector<std::uint8_t>& payload,
                                       int timeoutMs)
{
    // 中文：前置检查。请求码 bit7 必须为 0（bit7 是响应方向位）。
    // EN: Preconditions. Request CMD must have bit7 clear; bit7 marks a response.
    if (!d->port.isOpen())
        throw DeviceError("port not open");
    if (cmd & proto::RespBit)
        throw DeviceError("request CMD 0x" + hex2(cmd) + " has bit7 set");

    // 中文：每次事务都从干净缓冲开始。上一次超时留下的字节属于旧响应，
    //       如果混进来会导致这一条命令解到错误的帧。
    // EN: Start every transaction with a clean buffer. Bytes left over from a
    //     previous timeout belong to the old response and could make this
    //     command parse the wrong frame.
    d->rx.clear();
    (void)d->port.readAll();

    // 中文：组帧并写出。write() 是缓冲写入，必须等 waitForBytesWritten
    //       确认真的发到驱动层，否则下面的等待可能早于实际发送。
    // EN: Build and write the frame. write() is buffered, so wait for
    //     waitForBytesWritten to confirm it reached the driver; otherwise the
    //     receive wait below could start before the bytes are actually sent.
    const auto bytes = proto::buildFrame(cmd, payload);
    const qint64 written = d->port.write(
        reinterpret_cast<const char*>(bytes.data()),
        static_cast<qint64>(bytes.size()));
    if (written != static_cast<qint64>(bytes.size()) ||
        !d->port.waitForBytesWritten(timeoutMs)) {
        throw DeviceError("serial write failed: " +
                          d->port.errorString().toStdString());
    }

    // 中文：期望的响应码 = 请求码 | 0x80。
    // EN: Expected response CMD = request CMD | 0x80.
    const auto wantCmd = static_cast<std::uint8_t>(cmd | proto::RespBit);

    QElapsedTimer timer;
    timer.start();

    // 中文：循环收字节，直到超时。一次 waitForReadyRead 只保证"有新数据到达"，
    //       不保证整帧到齐，所以每轮都要 append + parseFrame。
    // EN: Keep reading until the deadline. waitForReadyRead only guarantees
    //     "some new bytes arrived", not a whole frame, so append and re-parse
    //     on every iteration.
    while (timer.elapsed() < timeoutMs) {
        const int remaining = timeoutMs - static_cast<int>(timer.elapsed());
        const bool ready = d->port.waitForReadyRead(remaining > 0 ? remaining : 1);

        // 中文：即便 ready=false（这一轮等到超时），也把缓冲区里已有的字节取出来，
        //       避免刚好在最后一刻到达的响应被丢掉。
        // EN: Even when ready=false (this round timed out), drain whatever is
        //     already buffered so a response arriving at the last moment isn't lost.
        const QByteArray chunk = d->port.readAll();
        for (char c : chunk)
            d->rx.push_back(static_cast<std::uint8_t>(c));

        // 中文：parseFrame 会消费掉完整帧，粘包剩下的留给下一轮/下一次调用。
        // EN: parseFrame removes the complete frame it returns; any coalesced
        //     frames stay in the buffer for the next iteration/call.
        while (auto frame = proto::parseFrame(d->rx)) {
            // 中文：不是本命令的响应（例如未请求的广播帧）就丢掉，继续找。
            // EN: Skip frames that don't answer this command (e.g. unsolicited
            //     broadcasts) and keep looking.
            if (frame->cmd != wantCmd)
                continue;

            // 中文：PAYLOAD[0] 永远是 RESULT，必须先判成功再读后面的值。
            // EN: PAYLOAD[0] is always RESULT; check it before reading any
            //     following value bytes.
            if (frame->payload.empty())
                throw DeviceError("empty response payload for CMD 0x" + hex2(cmd));
            const auto result = static_cast<proto::Result>(frame->payload[0]);
            if (result != proto::ResOk) {
                throw DeviceError("device rejected CMD 0x" + hex2(cmd) +
                                  ": " + resultName(frame->payload[0]));
            }
            return *frame;
        }

        // 中文：串口在等待过程中被拔掉属于硬错误，不必再等满超时。
        // EN: If the cable is unplugged mid-wait, fail fast instead of burning
        //     the whole timeout.
        if (!d->port.isOpen())
            throw DeviceError("port closed while waiting for response");
        if (!ready)
            break;
    }

    // 中文：超时。当前 d->rx 里可能还有半帧，下一次事务开头会清掉。
    // EN: Timed out. d->rx may hold a partial frame; the next transaction
    //     clears it at entry.
    throw DeviceError("timeout (" + std::to_string(timeoutMs) +
                      " ms) waiting for response to CMD 0x" + hex2(cmd));
}

// ---------------------------------------------------------------------------
// 六个语义化命令 / The six semantic commands
// ---------------------------------------------------------------------------

void TimingDevice::setFreq(int hz)
{
    // 中文：频率是 3 字节大端 Hz；先做范围检查，越界直接给本地错误。
    // EN: Frequency is 3 big-endian bytes in Hz; range-check locally first
    //     so an invalid value never reaches the device.
    if (hz < 1 || hz > 0xFFFFFF)
        throw DeviceError("frequency out of range 1..16777215 Hz: " +
                          std::to_string(hz));

    // payload = { param_id, b2, b1, b0 }
    const std::vector<std::uint8_t> payload{
        proto::ParamFreq,
        static_cast<std::uint8_t>(static_cast<unsigned>(hz) >> 16),
        static_cast<std::uint8_t>((static_cast<unsigned>(hz) >> 8) & 0xFF),
        static_cast<std::uint8_t>(static_cast<unsigned>(hz) & 0xFF)
    };
    (void)sendAndWait(proto::CmdSetParam, payload);
}

int TimingDevice::getFreq()
{
    // 中文：请求 GET_PARAM(FREQ)，响应 payload = RESULT + freq(3 字节大端)。
    // EN: Request GET_PARAM(FREQ); response payload = RESULT + freq (3 bytes,
    //     big-endian).
    const auto frame = sendAndWait(proto::CmdGetParam, {proto::ParamFreq});
    requirePayload(frame, 4, "GET_PARAM(FREQ)");

    const auto b = frame.payload;
    return (static_cast<int>(b[1]) << 16) |
           (static_cast<int>(b[2]) << 8) |
           static_cast<int>(b[3]);
}

void TimingDevice::start()
{
    // 中文：START 无 payload；sendAndWait 已校验 RESULT。
    // EN: START has no payload; sendAndWait has already validated RESULT.
    (void)sendAndWait(proto::CmdStart, {});
}

void TimingDevice::stop()
{
    // 中文：STOP 无 payload；sendAndWait 已校验 RESULT。
    // EN: STOP has no payload; sendAndWait has already validated RESULT.
    (void)sendAndWait(proto::CmdStop, {});
}

Status TimingDevice::getStatus()
{
    // 中文：响应 payload = RESULT + run(1) + freq(3) + err(1)，共 6 字节。
    // EN: Response payload = RESULT + run(1) + freq(3) + err(1), six bytes total.
    const auto frame = sendAndWait(proto::CmdGetStatus, {});
    requirePayload(frame, 6, "GET_STATUS");

    const auto b = frame.payload;
    Status st;
    st.running      = b[1] != 0;
    st.freq_hz      = (static_cast<int>(b[2]) << 16) |
                      (static_cast<int>(b[3]) << 8) |
                      static_cast<int>(b[4]);
    st.err_permille = b[5];
    return st;
}

int TimingDevice::selfTest()
{
    // 中文：响应 payload = RESULT + err_permille(2 字节大端)。
    //       注意：PA1->PA6 未接跳线时固件也返回 0，别误判成"精度完美"。
    // EN: Response payload = RESULT + err_permille (2 bytes, big-endian).
    //     Caveat: with no PA1->PA6 jumper the firmware also returns 0; don't
    //     mistake that for a perfect measurement.
    const auto frame = sendAndWait(proto::CmdSelfTest, {});
    requirePayload(frame, 3, "SELF_TEST");

    return (static_cast<int>(frame.payload[1]) << 8) |
           static_cast<int>(frame.payload[2]);
}
