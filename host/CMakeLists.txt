cmake_minimum_required(VERSION 3.21)
project(TimingGeneratorHost LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)

# ---------------------------------------------------------------------------
# 1) 协议自检：纯 C++，不依赖 Qt。
#    你写完 protocol.cpp 后先跑这个，对照 PROTOCOL.md 的全部向量。
# ---------------------------------------------------------------------------
add_executable(protocol-selftest
    tests/protocol_selftest.cpp
    src/core/protocol.cpp
)
target_include_directories(protocol-selftest PRIVATE src)

# ---------------------------------------------------------------------------
# 2) 图形界面：需要 Qt6 Widgets + SerialPort。
# ---------------------------------------------------------------------------
find_package(Qt6 QUIET COMPONENTS Widgets SerialPort)

if(Qt6_FOUND)
    add_executable(timing-host
        src/main.cpp
        src/core/protocol.cpp
        src/core/device.cpp
        src/ui/mainwindow.cpp
    )
    target_include_directories(timing-host PRIVATE src)
    target_link_libraries(timing-host PRIVATE Qt6::Widgets Qt6::SerialPort)

    # 2b) 无界面的联调工具：和 GUI 共用 TimingDevice，逐条打印命令结果。
    #     Headless smoke test: shares TimingDevice with the GUI and prints
    #     every command result, for hardware integration debugging.
    add_executable(device-smoke
        tests/device_smoke.cpp
        src/core/protocol.cpp
        src/core/device.cpp
    )
    target_include_directories(device-smoke PRIVATE src)
    target_link_libraries(device-smoke PRIVATE Qt6::Core Qt6::SerialPort)

    message(STATUS "Qt6 found - building timing-host (GUI) + device-smoke + protocol-selftest")
else()
    message(WARNING "Qt6 not found - only protocol-selftest will be built")
endif()
