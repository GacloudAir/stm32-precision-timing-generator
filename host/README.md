# PC 上位机

三通道时序发生器的图形化上位机。**协议层由你实现，GUI 由助手实现**，通过 `core/device.h` 的接口对接。

## 目录结构

```
host/
  CMakeLists.txt
  src/
    main.cpp              入口（助手）
    core/
      models.h            Status 数据类（已定稿，双方共用）
      protocol.h          协议接口声明（契约）
      protocol.cpp        ★ 你实现：CRC16 / 组帧 / 解帧
      device.h            设备门面接口（契约）
      device.cpp          ★ 你实现：QSerialPort 收发 + 命令封装
    ui/
      mainwindow.h/.cpp   GUI（助手）
  tests/
    protocol_selftest.cpp 协议自检，不依赖 Qt、不接硬件
```

★ 标记的两个文件是你要写的；其余已由助手完成。

## 接口契约（GUI 只认这些）

```cpp
class TimingDevice : public QObject {
public:
    void connectPort(const QString& port, int baud = 115200);
    void disconnectPort();
    bool isConnected() const;

    void   setFreq(int hz);
    int    getFreq();
    void   start();
    void   stop();
    Status getStatus();
    int    selfTest();      // 返回 err_permille
};
```

**三条铁律**：

1. 每个方法内部必须有超时（建议 500 ms），超时抛 `DeviceError`。
2. GUI 层不碰字节：帧、CRC、状态机全部封在 `device.cpp` 里。
3. 失败抛异常，不要返回魔法值或静默吞掉。

## 开发顺序（建议）

**第 1 步：协议层**
实现 `protocol.cpp` 的三个函数。对照 `docs/PROTOCOL.md`，也可以参考
`tools/uart_probe.py`（同逻辑的 Python 版）。

**第 2 步：跑自检（不需要硬件、不需要 Qt）**

```bash
cmake -S host -B host/build -G "MinGW Makefiles"
cmake --build host/build --target protocol-selftest
./host/build/protocol-selftest.exe
```

全部 OK 再往下走。这时你的 CRC/组帧/解帧已经可信了。

**第 3 步：串口层 + 门面**
实现 `device.cpp`。先在命令行验证，不用打开 GUI：

```bash
# 假设已有一个临时 main 或者用 Qt 的 qDebug 打印
```

**第 4 步：GUI**
把 `main.cpp` 里的 `MockDevice` 换成 `TimingDevice` 即可。

## 编译（Qt6 + MinGW）

```bash
cmake -S host -B host/build -G "MinGW Makefiles" \
      -DCMAKE_PREFIX_PATH=C:/Qt/6.8.3/mingw_64 \
      -DCMAKE_BUILD_TYPE=Release
cmake --build host/build
./host/build/timing-host.exe
```

需要 `C:/Qt/Tools/mingw1310_64/bin` 和 `C:/Qt/6.8.3/mingw_64/bin` 在 PATH 中
（运行时也依赖后者里的 Qt DLL）。

## 协议速查

| 命令 | 请求 payload | 响应 payload |
|---|---|---|
| SET_PARAM (0x01) | `param_id + freq(3, 大端)` | `RESULT` |
| GET_PARAM (0x02) | `param_id` | `RESULT + freq(3)` |
| START (0x03) | 空 | `RESULT` |
| STOP (0x04) | 空 | `RESULT` |
| GET_STATUS (0x05) | 空 | `RESULT + run + freq(3) + err` |
| SELF_TEST (0x06) | 空 | `RESULT + err_permille(2)` |

响应 CMD = 请求码 `| 0x80`。完整定义见 `docs/PROTOCOL.md`。
