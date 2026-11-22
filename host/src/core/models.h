#pragma once

// 上位机与 GUI 之间传递的"干净"数据。
// GUI 只认识这个结构，不碰任何字节/帧/CRC 细节。

struct Status {
    bool running      = false;  // 是否正在输出波形
    int  freq_hz      = 0;      // 当前频率（Hz）
    int  err_permille = 0;      // SELF_TEST 实测误差（千分比，0..N）
};
