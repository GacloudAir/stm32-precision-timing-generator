#ifndef PROTOCOL_H_
#define PROTOCOL_H_

#include <stdint.h>//提供uint8_t,uint16_t,uint32_t等类型,确保上下位机能使用约定好的数值
/* ============================================================================
 *  公共约定：命令码、参数 ID、结果码
 *  这些数值上位机也要用同一份，建议以后抽成 shared/protocol.h 共用
 * ========================================================================== */
#define CMD_SET_PARAM   0x01U //设置参数，帧中携带参数 ID 和参数值
#define CMD_GET_PARAM   0x02U //读取参数
#define CMD_START       0x03U //启动定时输出
#define CMD_STOP        0x04U
#define CMD_GET_STATUS  0x05U
#define CMD_SELF_TEST   0x06U
#define CMD_RESP_BIT    0x80U      /* 响应帧：CMD | CMD_RESP_BIT */

#define RES_OK          0x00U
#define RES_ERR_CRC     0x01U
#define RES_ERR_UNK     0x02U
#define RES_ERR_RANGE   0x03U

#define PARAM_FREQ      0x01U      /* 频率：value 为 3 字节大端 Hz，166666 = 02 8B 0A */
#define PARAM_DUTY      0x02U      /* 占空比档位（预留） */

#define FRAME_MAX       32U        // 接收缓冲上限,(32字节)防止越界
#define MAGIC0          0xAAU
#define MAGIC1          0x55U

/* 帧开销 = MAGIC(2) + LEN(1) + CRC(2)；供组帧/解帧做边界检查 */
#define FRAME_OVERHEAD  5U
#define LEN_MAX         (FRAME_MAX - FRAME_OVERHEAD)
#define PAYLOAD_MAX     (LEN_MAX - 1U)     /* LEN 含 CMD(1) */
#if FRAME_MAX <= FRAME_OVERHEAD
#error "FRAME_MAX is too small for the protocol header and CRC"
#endif

#endif