# 串口帧协议 v1（定稿）

> 这是 Level 2 固件（下位机）与 C++ 上位机之间**唯一权威**的帧格式约定。
> 两边都必须逐字节遵守本文。旧仓库里的协议文档和脚手架代码**以本文为准**——
> 它们有两处已确认的 bug，见文末"勘误"。

## 1. 帧格式

```
[0xAA][0x55][LEN][CMD][PAYLOAD ...][CRC16lo][CRC16hi]
  帧头 2B         1B   1B     LEN-1 B        2B
```

| 字段 | 含义 |
|---|---|
| `0xAA 0x55` | 帧头（MAGIC），固定两字节 |
| `LEN` | `CMD + PAYLOAD` 的字节数，**不含**帧头和 CRC |
| `CMD` | 命令码；响应帧为请求码 `\| 0x80` |
| `PAYLOAD` | 长度 = `LEN - 1` |
| `CRC16` | Modbus（初值 0xFFFF，多项式 0xA001），覆盖 `[LEN .. PAYLOAD 末]`，即 `CRC16(&frame[2], LEN+1)`，**低字节先发** |

- 总帧长 = `LEN + 5`（2 帧头 + 1 LEN + LEN 内容 + 2 CRC）
- 所有多字节数值一律**大端**（高字节在前），唯一例外是 CRC 低字节在前
- 参数 `FREQ` 频率：**3 字节大端 Hz**，例如 166666 Hz = `02 8B 0A`

## 2. 请求命令

| CMD | 值 | PAYLOAD | 说明 |
|---|---|---|---|
| SET_PARAM | 0x01 | `param_id(1) + value(3)` | 目前只有 `param_id=0x01`（FREQ） |
| GET_PARAM | 0x02 | `param_id(1)` | 回读当前值 |
| START | 0x03 | 空 | 启动输出 |
| STOP | 0x04 | 空 | 停止输出 |
| GET_STATUS | 0x05 | 空 | 回读状态 |
| SELF_TEST | 0x06 | 空 | 自检（Level 3 填入实测误差） |

## 3. 响应帧

响应帧的 `PAYLOAD[0]` **永远是 RESULT**（这是旧脚手架漏掉、导致解帧错位的地方）：

| RESULT | 值 |
|---|---|
| OK | 0x00 |
| ERR_CRC | 0x01 |
| ERR_UNK | 0x02 |
| ERR_RANGE | 0x03 |

| 请求 | 响应 CMD | 响应 PAYLOAD（含 RESULT） |
|---|---|---|
| SET_PARAM | 0x81 | `RESULT(1)` |
| GET_PARAM | 0x82 | `RESULT(1) + value(3)` |
| START / STOP | 0x83 / 0x84 | `RESULT(1)` |
| GET_STATUS | 0x85 | `RESULT(1) + run(1) + freq(3) + err(1)` |
| SELF_TEST | 0x86 | `RESULT(1) + err_permille(2)` |

例：GET_STATUS 成功响应（run=1, freq=166666, err=0），LEN = 1(CMD) + 6(PAYLOAD) = 7：
```
AA 55 07 85 00 01 02 8B 0A 00 9F D7
```

## 4. 构造帧测试向量（CRC 已用独立脚本核算）

写解帧代码时，直接用这些帧喂进去，验证输出是否与注释一致。

**请求帧：**

| 名称 | 完整帧（十六进制） |
|---|---|
| SET_PARAM FREQ=166666 | `AA 55 05 01 02 8B 0A AE CB` |
| GET_PARAM FREQ | `AA 55 02 02 01 10 A0` |
| START | `AA 55 01 03 40 21` |
| STOP | `AA 55 01 04 01 E3` |
| GET_STATUS | `AA 55 01 05 C0 23` |
| SELF_TEST | `AA 55 01 06 80 22` |

**响应帧（验证上位机 parse）：**

| 名称 | 完整帧（十六进制） |
|---|---|
| SET_PARAM OK | `AA 55 02 81 00 B0 50` |
| GET_PARAM FREQ=166666 | `AA 55 05 82 00 02 8B 0A 3F 67` |
| GET_STATUS (run=1) | `AA 55 07 85 00 01 02 8B 0A 00 9F D7` |
| SELF_TEST (err=0) | `AA 55 04 86 00 00 00 FC 88` |

**负向测试**（应满足"不误执行"）：
- 把任一帧的 CRC 改一字节 → 固件回 `RES_ERR_CRC` 或静默丢弃，**不得执行**
- 发 `AA 55 01 99 ...`（非法 CMD）→ 回 `RES_ERR_UNK`
- 先发半帧垃圾 `AA 55 03 01` 再紧跟一个真帧 → 状态机能重新同步，不卡死

## 5. 勘误：旧脚手架与参考文件里的三处 bug

写你自己的固件/上位机时避开这三点：

1. **旧 `host/protocol.cpp` 的 `need` 算错（致命）**
   ```cpp
   uint8_t need = (uint8_t)(4 + len + 2);   // 错误：= LEN + 6
   ```
   正确总帧长是 `LEN + 5`，多算 1 字节会**拒绝所有正确帧**。应为 `need = len + 5`。
   而 CRC 读取位置 `frame[4+(len-1)]` 恰好是对的——所以现象很有迷惑性：
   长度检查先失败，你会以为是帧头没对上。

2. **频率编码三种写法互相冲突**
   - 旧 L2 文档：`freq_hz(3)` 3 字节大端 —— 本文采用这个
   - 旧 `host/protocol.h`：注释说 3 字节，但 `SET_PARAM` 的 payload 只留了 `value(2)`
   - 旧 `uart_protocol_ref.c`：用 2 字节、单位 100Hz
   
   2 字节裸 Hz 装不下 166666（上限 65535）；2 字节 100Hz 精度只有 100Hz。
   定稿统一为 **3 字节大端 Hz**，已同步修正参考文件。

3. **GET_STATUS 响应漏掉 RESULT**
   旧 `uart_protocol_ref.c` 把 `resp[0]` 直接写成 `running`，违反"PAYLOAD[0]=RESULT"约定，
   上位机解出来的状态整体错位一格。已修正为 `RESULT + run + freq + err`。

## 6. 状态机要点

串口是无边界字节流，帧靠状态机"造"出来：

```
WAIT_M0 --0xAA--> WAIT_M1 --0x55--> GET_LEN --合法LEN--> GET_BODY --收满LEN+2--> 校验
```

- 中断里**只**做"收一个字节 + 喂状态机"，命令处理放主循环（用 `frame_ready` 标志握手）
- 超时/非法长度时必须复位到 `WAIT_M0` 并把索引清零，否则下一帧接着旧偏移写
- 收满判断：`rx_index >= LEN + 5`（索引从 3 起算，共收 `LEN+2` 个 body 字节）
- 主循环用 `__WFI()` 时，USART1 的 RXNE 中断能唤醒 CPU，不会睡死
