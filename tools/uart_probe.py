#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
uart_probe.py -- STM32 USART1 串口联调小工具

用途：在 Keil 下载受阻、或刚烧完固件时，先用 PC 侧验证串口链路和协议收包是否正确。
只依赖 pyserial。所有帧格式严格遵循 docs/PROTOCOL.md（v1 定稿）。

帧格式:  AA 55 LEN CMD PAYLOAD... CRClo CRChi
  LEN      = CMD + PAYLOAD 的字节数（不含帧头/CRC）
  CRC16    = Modbus, 初值 0xFFFF, 多项式 0xA001, 覆盖 [LEN .. PAYLOAD末]
  CRC 低字节先发；多字节数值一律大端（CRC 除外）

三种模式：
  1) selftest : 不发串口，仅用协议里的标准向量自检 CRC 和组帧是否正确
  2) loopback : 自发自收，验证 PC <-> CH340 <-> 线缆 这段是否通（需短接 TXD-RXD）
  3) probe    : 真正和下位机对话，发一条命令并解析响应（需固件已烧录并接线）

用法示例：
  python uart_probe.py selftest
  python uart_probe.py loopback --port COM10
  python uart_probe.py probe --port COM10 --cmd GET_STATUS
  python uart_probe.py probe --port COM10 --cmd SET_PARAM --freq 166666
  python uart_probe.py probe --port COM10 --cmd RAW --hex "AA 55 01 05 C0 23"
  python uart_probe.py probe --port COM10 --cmd RAW --hex "AA 55 05 01 01 02 8B 0A 7B 45"
"""

import argparse
import sys
import time

try:
    import serial
    from serial.tools import list_ports
except ImportError:
    sys.exit("缺少 pyserial，请先执行: python -m pip install pyserial")

# ---------------- 协议常量（与 docs/PROTOCOL.md 一致）----------------
MAGIC0, MAGIC1 = 0xAA, 0x55
CRC_INIT = 0xFFFF
CRC_POLY = 0xA001

CMD = {
    "SET_PARAM":  0x01,
    "GET_PARAM":  0x02,
    "START":      0x03,
    "STOP":       0x04,
    "GET_STATUS": 0x05,
    "SELF_TEST":  0x06,
}
CMD_NAME = {v: k for k, v in CMD.items()}

RESULT = {
    0x00: "OK",
    0x01: "ERR_CRC",
    0x02: "ERR_UNK",
    0x03: "ERR_RANGE",
}

PARAM_FREQ = 0x01


# ---------------- 核心算法 ----------------
def crc16_modbus(data: bytes) -> int:
    """Modbus CRC16：初值 0xFFFF，多项式 0xA001，返回 16 位整数。"""
    crc = CRC_INIT
    for byte in data:
        crc ^= byte
        for _ in range(8):
            if crc & 0x1:
                crc = (crc >> 1) ^ CRC_POLY
            else:
                crc >>= 1
            crc &= 0xFFFF
    return crc


def build_frame(cmd: int, payload: bytes = b"") -> bytes:
    """按协议组一帧请求。payload 为空表示无参数命令。"""
    if not 0x00 <= cmd <= 0x7F:
        raise ValueError("请求 CMD 必须 < 0x80（bit7 留给响应方向位）")
    body = bytes([cmd]) + bytes(payload)   # [CMD][PAYLOAD...]
    length = len(body)                      # LEN = CMD + PAYLOAD
    crc = crc16_modbus(bytes([length]) + body)
    return bytes([MAGIC0, MAGIC1, length]) + body + bytes([crc & 0xFF, crc >> 8])


def parse_frame(raw: bytes):
    """
    从字节流里找出一帧并校验。返回 (frame, consumed) 或 None。
    consumed 是这帧吞掉的字节数，便于处理粘包/噪声前缀。
    """
    i = 0
    n = len(raw)
    while i < n:
        if raw[i] != MAGIC0:
            i += 1
            continue
        if i + 1 >= n or raw[i + 1] != MAGIC1:
            i += 1
            continue
        if i + 3 >= n:            # 帧头 + LEN 还没收全
            return None
        length = raw[i + 2]
        total = length + 5        # 总帧长 = LEN + 5
        if length < 1 or total > 32:
            i += 2
            continue
        if i + total > n:         # 没收满
            return None
        frame = raw[i:i + total]
        covered = frame[2:2 + length + 1]          # [LEN .. PAYLOAD末]
        crc_recv = frame[-2] | (frame[-1] << 8)
        if crc16_modbus(covered) == crc_recv:
            return frame, i + total
        i += 2                    # CRC 不符，跳过帧头继续找
    return None


def hexstr(data: bytes) -> str:
    return " ".join(f"{b:02X}" for b in data)


def decode_response(frame: bytes):
    """把一帧响应翻译成人话。返回多行字符串列表。"""
    cmd = frame[3]
    req = cmd & 0x7F
    payload = frame[4:-2]
    out = []
    out.append(f"  响应 CMD = 0x{cmd:02X}  (对应请求 {CMD_NAME.get(req, '未知')})")
    if not payload:
        out.append("  PAYLOAD 为空")
        return out
    result = payload[0]
    out.append(f"  RESULT   = 0x{result:02X} ({RESULT.get(result, '未知')})")
    if req == CMD["SET_PARAM"] or req == CMD["START"] or req == CMD["STOP"]:
        pass
    elif req == CMD["GET_PARAM"] and len(payload) >= 4:
        freq = int.from_bytes(payload[1:4], "big")
        out.append(f"  FREQ     = {freq} Hz  ({hexstr(payload[1:4])})")
    elif req == CMD["GET_STATUS"] and len(payload) >= 6:
        run = payload[1]
        freq = int.from_bytes(payload[2:5], "big")
        err = payload[5]
        out.append(f"  RUN      = {run} ({'运行中' if run else '已停止'})")
        out.append(f"  FREQ     = {freq} Hz")
        out.append(f"  ERR      = 0x{err:02X}")
    elif req == CMD["SELF_TEST"] and len(payload) >= 3:
        permille = int.from_bytes(payload[1:3], "big")
        out.append(f"  误差     = {permille} ‰")
    else:
        out.append(f"  原始 PAYLOAD = {hexstr(payload)}")
    return out


# ---------------- 模式 1：自检 ----------------
def mode_selftest():
    print("=== 协议算法自检（不接串口）===")
    vectors = [
        ("SET_PARAM 166666", 0x01, bytes([0x01, 0x02, 0x8B, 0x0A]), "AA 55 05 01 01 02 8B 0A 7B 45"),
        ("GET_PARAM FREQ",   0x02, bytes([0x01]),                    "AA 55 02 02 01 10 A0"),
        ("START",            0x03, b"",                              "AA 55 01 03 40 21"),
        ("STOP",             0x04, b"",                              "AA 55 01 04 01 E3"),
        ("GET_STATUS",       0x05, b"",                              "AA 55 01 05 C0 23"),
        ("SELF_TEST",        0x06, b"",                              "AA 55 01 06 80 22"),
    ]
    ok = True
    for name, cmd, payload, expect in vectors:
        frame = build_frame(cmd, payload)
        got = hexstr(frame)
        match = got == expect
        ok &= match
        print(f"  [{'OK ' if match else 'FAIL'}] {name:18s} 期望 {expect}  实际 {got}")

    # 再验证解析 + 解码标准响应
    resp = bytes.fromhex("AA 55 07 85 00 01 02 8B 0A 00 9F D7")
    parsed = parse_frame(resp)
    if parsed:
        dec = decode_response(parsed[0])
        print("  [OK ] 标准 GET_STATUS 响应解析：")
        for line in dec:
            print("       " + line)
    else:
        print("  [FAIL] 标准 GET_STATUS 响应解析失败")
        ok = False

    # 抗干扰：前面塞垃圾字节 + 后面粘一帧
    noisy = bytes.fromhex("00 FF 7E") + resp + build_frame(CMD["START"])
    p1 = parse_frame(noisy)
    p2 = parse_frame(noisy, ) if False else None
    if p1 and p1[1] == 3 + len(resp):
        rest = noisy[p1[1]:]
        p2 = parse_frame(rest)
        if p2 and p2[0] == build_frame(CMD["START"]):
            print("  [OK ] 噪声前缀 + 粘包分离正确")
        else:
            print("  [FAIL] 粘包分离失败")
            ok = False
    else:
        print("  [FAIL] 噪声前缀跳过失败")
        ok = False

    print(f"\n自检结果: {'全部通过' if ok else '存在失败，请勿用于联调'}")
    return 0 if ok else 1


# ---------------- 模式 2：回环 ----------------
def mode_loopback(port: str, baud: int):
    print(f"=== 回环测试 {port} @ {baud} ===")
    print("请把 CH340 的 TXD 与 RXD 短接（GND 保持不变），然后按回车开始...")
    input()
    try:
        ser = serial.Serial(port, baudrate=baud, bytesize=8, parity="N",
                            stopbits=1, timeout=1)
    except Exception as e:
        print(f"打开串口失败: {e}")
        return 1
    payload = bytes(range(0x00, 0x40))
    frame = build_frame(CMD["GET_STATUS"]) + payload   # 用合法帧 + 一段递增字节
    print(f"发送 {len(frame)} 字节: {hexstr(frame[:16])} ...")
    ser.reset_input_buffer()
    ser.write(frame)
    ser.flush()
    time.sleep(0.2)
    got = ser.read(len(frame))
    ser.close()
    if got == frame:
        print(f"[PASS] 收到 {len(got)} 字节，逐字节一致 -> PC/CH340/线缆链路正常")
        return 0
    else:
        print(f"[FAIL] 期望 {len(frame)} 字节，实收 {len(got)} 字节")
        print(f"  期望: {hexstr(frame)}")
        print(f"  实收: {hexstr(got)}")
        print("排查: TXD/RXD 是否短接、COM 口是否选对、波特率是否一致")
        return 1


# ---------------- 模式 3：协议探测 ----------------
def mode_probe(port: str, baud: int, cmd_name: str, freq: int | None, raw_hex: str | None):
    print(f"=== 协议探测 {port} @ {baud} ===")
    if raw_hex:
        frame = bytes.fromhex(raw_hex.replace(" ", ""))
        label = "RAW"
    elif cmd_name == "SET_PARAM":
        if freq is None:
            print("SET_PARAM 需要 --freq，例如 --freq 166666")
            return 1
        frame = build_frame(CMD["SET_PARAM"], bytes([PARAM_FREQ]) + freq.to_bytes(3, "big"))
        label = f"SET_PARAM {freq} Hz"
    else:
        frame = build_frame(CMD[cmd_name])
        label = cmd_name
    print(f"发送 [{label}]: {hexstr(frame)}")

    try:
        ser = serial.Serial(port, baudrate=baud, bytesize=8, parity="N",
                            stopbits=1, timeout=0.15)
    except Exception as e:
        print(f"打开串口失败: {e}")
        return 1

    ser.reset_input_buffer()
    ser.write(frame)
    ser.flush()

    buf = bytearray()
    deadline = time.time() + 2.0
    result = None
    while time.time() < deadline:
        chunk = ser.read(64)
        if chunk:
            buf.extend(chunk)
            parsed = parse_frame(buf)
            if parsed:
                result = parsed[0]
                break
    ser.close()

    if result is None:
        print("[无响应] 2 秒内未收到合法帧")
        if buf:
            print(f"  收到 {len(buf)} 字节但无法成帧: {hexstr(bytes(buf))}")
        print("排查顺序: TX/RX 是否交叉接(CH340.TXD->PA10, RXD->PA9)、GND 是否共地、")
        print("          固件是否已烧录且 USART1 已初始化、波特率是否 115200、是否开了'发送新行'")
        return 1

    print(f"收到响应: {hexstr(result)}")
    for line in decode_response(result):
        print(line)
    return 0


def build_parser():
    p = argparse.ArgumentParser(description="STM32 USART1 串口联调小工具")
    sub = p.add_subparsers(dest="mode", required=True)

    sub.add_parser("selftest", help="自检 CRC/组帧/解帧（不接串口）")

    lp = sub.add_parser("loopback", help="自发自收，验证 PC-CH340-线缆链路")
    lp.add_argument("--port", required=True, help="串口号，如 COM10")
    lp.add_argument("--baud", type=int, default=115200, help="波特率，默认 115200")

    pp = sub.add_parser("probe", help="与下位机对话，发命令并解析响应")
    pp.add_argument("--port", required=True, help="串口号，如 COM10")
    pp.add_argument("--baud", type=int, default=115200, help="波特率，默认 115200")
    pp.add_argument("--cmd", choices=list(CMD.keys()) + ["RAW"], default="GET_STATUS")
    pp.add_argument("--freq", type=int, help="SET_PARAM 的频率(Hz)")
    pp.add_argument("--hex", dest="raw_hex", help="RAW 模式的原始帧，如 'AA 55 01 05 C0 23'")
    return p


def main():
    args = build_parser().parse_args()
    if args.mode == "selftest":
        return mode_selftest()
    if args.mode == "loopback":
        return mode_loopback(args.port, args.baud)
    if args.mode == "probe":
        return mode_probe(args.port, args.baud, args.cmd, args.freq, args.raw_hex)
    return 1


if __name__ == "__main__":
    sys.exit(main())
