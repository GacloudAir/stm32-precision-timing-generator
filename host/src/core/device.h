#pragma once

// 设备门面：GUI 只和这个类打交道。
// 你负责实现它（用 QSerialPort + 你自己的 protocol 层），
// 对外只暴露下面这些"语义化"的方法 —— GUI 里不会出现任何帧/字节/CRC。

#include <QObject>
#include <QString>
#include <stdexcept>

#include "models.h"

// 统一异常类型：超时、CRC 错、参数非法、串口断开……全都抛这个。
class DeviceError : public std::runtime_error {
public:
    explicit DeviceError(const std::string& msg) : std::runtime_error(msg) {}
};

class TimingDevice : public QObject {
    Q_OBJECT
public:
    explicit TimingDevice(QObject* parent = nullptr);
    ~TimingDevice() override;

    // ---- 连接管理 ----
    // 打开失败（端口不存在/被占用）抛 DeviceError
    void connectPort(const QString& port, int baud = 115200);
    void disconnectPort();
    bool isConnected() const;

    // ---- 命令（同步、内部带超时，失败抛 DeviceError）----
    // 建议超时 500 ms：GUI 主线程调用最多阻塞这么久。
    void   setFreq(int hz);        // -> SET_PARAM(FREQ)
    int    getFreq();              // -> GET_PARAM(FREQ)
    void   start();                // -> START
    void   stop();                 // -> STOP
    Status getStatus();            // -> GET_STATUS
    int    selfTest();             // -> SELF_TEST，返回 err_permille

signals:
    // 可选：异步错误通知（例如串口被拔出）。没有也能工作。
    void errorOccurred(const QString& message);

private:
    struct Impl;   // 内部实现（QSerialPort、接收缓冲区、状态机……）
    Impl* d;
};
