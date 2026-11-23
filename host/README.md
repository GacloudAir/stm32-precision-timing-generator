# PC 上位机

三通道时序发生器的图形化上位机。GUI、协议层、设备层均已实现，通过 `core/device.h` 的接口对接；关键逻辑带中英双语注释。

## 目录结构

```
host/
  CMakeLists.txt
  src/
    main.cpp              入口（助手）
    core/
      models.h            Status 数据类（已定稿，双方共用）
      protocol.h          协议接口声明（契约）
      protocol.cpp        CRC16 / 组帧 / 解帧（双语注释）
      device.h            设备门面接口 + sendAndWait 私有声明（契约）
      device.cpp          QSerialPort 收发 + 命令封装（双语注释）
    ui/
      mainwindow.h/.cpp   GUI
  tests/
    protocol_selftest.cpp 协议自检，不依赖 Qt、不接硬件
    device_smoke.cpp      联调工具：和 GUI 共用设备层，逐条打印命令结果
```

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

**第 3 步：命令行联调（推荐顺序）**
不用打开 GUI，先用 `device-smoke` 逐条验证 6 个命令：

```powershell
.\build\device-smoke.exe --port COM10 --read-only   # 只读探针
.\build\device-smoke.exe --port COM10               # 完整流程
```

`device-smoke` 走的是和 GUI 完全相同的 `TimingDevice`，打印每条命令的结果。
若失败，先用 `python ../tools/uart_probe.py probe --port COM10 --cmd GET_STATUS`
区分是固件问题还是上位机问题。

**第 4 步：GUI**
直接运行 `.\build\timing-host.exe`，选择同一串口即可，无需改 GUI 代码。

## 编译（Qt6 + MinGW）

```bash
cmake -S host -B host/build -G "MinGW Makefiles" \
      -DCMAKE_PREFIX_PATH=C:/Qt/6.8.3/mingw_64 \
      -DCMAKE_BUILD_TYPE=Release
cmake --build host/build
./host/build/timing-host.exe
```

需要 `C:/Qt/Tools/mingw1310_64/bin` 和 `C:/Qt/6.8.3/mingw_64/bin` 在 PATH 中编译。

`build.ps1` 会自动调用 `windeployqt` 把 Qt / MinGW 运行库拷到 `build/`，
所以**编译后直接双击 `host/build/timing-host.exe` 即可运行**，不需要配 PATH。

GUI 里"实测误差"只显示 `SELF_TEST` 的返回值；`GET_STATUS` 的 `err` 字段是
固件预留位（恒 0），不会被轮询刷掉。改频率后误差会清空，需要重新点自检。

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
