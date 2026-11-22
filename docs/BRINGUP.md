# Bring-up log: UART host link

A record of taking the USART1 command interface from "compiles and flashes" to
"every command verified on hardware". The value here is the failure modes: four
of them looked identical from the PC side (silence on the serial port) but had
four unrelated causes.

Environment: STM32F103C8T6 blue pill, ST-Link V2 (SWD), CH340 USB-serial,
Keil MDK 5.43 / armclang V6.24, Windows 11. Host side: `tools/uart_probe.py`
(pyserial).

## Wiring

| CH340 | Board | Note |
|---|---|---|
| TXD | PA10 (USART1_RX) | crossed |
| RXD | PA9 (USART1_TX) | crossed |
| GND | GND | **required** — no common ground, no link |
| VCC | not connected | board powered from ST-Link / USB |

ST-Link SWD (PA13/PA14/GND/3V3) is independent and can stay connected.

Serial parameters: **115200, 8 data bits, 1 stop bit, no parity, no flow control**.

## Result

All six commands verified end-to-end, including a set-then-read-back round trip.

| Command | Response | Decoded |
|---|---|---|
| `GET_STATUS` | `AA 55 07 85 00 01 02 8B 0A 00 9F D7` | run=1, freq=166666, err=0 |
| `GET_PARAM` | `AA 55 05 82 00 02 8B 0A 3F 67` | freq=166666 |
| `SET_PARAM 100000` | `AA 55 02 81 00 B0 50` | OK |
| `GET_PARAM` (after set) | `AA 55 05 82 00 01 86 A0 4B 88` | freq=100000 — write took effect |
| `SELF_TEST` | `AA 55 04 86 00 00 00 FC 88` | err=0 (placeholder) |
| `STOP` | `AA 55 02 84 00 B3 00` | OK |
| `GET_STATUS` (stopped) | `AA 55 07 85 00 00 01 86 A0 00 4D 30` | run=0 — state tracked |
| `START` | `AA 55 02 83 00 B1 30` | OK |
| `GET_STATUS` (restarted) | `AA 55 07 85 00 01 02 8B 0A 00 9F D7` | run=1 |

The `GET_STATUS` response is byte-identical to the reference vector in
`docs/PROTOCOL.md`.

## Failure modes encountered

### 1. `Connection refused due to device mismatch`

Keil's pack validation compares the chip's IDCODE against the selected device.
A clone or a different revision trips it even though the part works. Disabling
pack validation (Options → Debug → Settings → Pack → uncheck Enable) lets the
debugger connect. Not a firmware issue.

### 2. `Cannot access Memory (@ 0x08000000, Write)` / `*** error 57`

Two different operations produce this:

- **Start Debug (Ctrl+F5)** with "Load Application at Startup" but no flash
  algorithm in the debug session → Keil writes the .axf straight to the flash
  address as if it were RAM, which fails. Fix: use **Download (F8)**, or enable
  "Download to Flash" in Debug → Settings → Flash Download.
- A successful `Load` followed by `Erase Done / Programming Done / Verify OK`
  is the *download* path and is normal.

The tell is whether `Erase Done` appears after `Load`.

### 3. Serial link completely silent — the real bug (`CRL` vs `CRH`)

Symptom: flash reported `Verify OK`, the PC sent frames, and **zero bytes came
back at any baud rate**. Loopback on the CH340 (short TXD-RXD) passed, and an
unrelated echo firmware ran fine, which isolated the problem to this firmware.

Root cause in `usart1_init()`:

```c
GPIOA->CRL &= ~(0xFU << 4);  GPIOA->CRL |= (0xAU << 4);   /* intended PA9  */
GPIOA->CRL &= ~(0xFU << 8);  GPIOA->CRL |= (0x4U << 8);   /* intended PA10 */
```

`CRL` configures pins 0–7; pins 8–15 live in `CRH`. So this actually rewrote:

- `CRL[7:4]` = **PA1** (still AF, harmless)
- `CRL[11:8]` = **PA2** → **forced to floating input**, destroying the PULSE output
- **PA9 / PA10 were never configured** and stayed at reset default (floating
  input), so USART1's TX pin never drove the line

One wrong register name broke two things at once. Fix:

```c
GPIOA->CRH &= ~(0xFU << 4);  GPIOA->CRH |= (0xAU << 4);   /* PA9  TX  */
GPIOA->CRH &= ~(0xFU << 8);  GPIOA->CRH |= (0x4U << 8);   /* PA10 RX  */
```

Verified in the disassembly: `MOVT r1,#0x4001` + `MOV r1,#0x804` = `0x40010804`
(CRH), confirming the fix reached the binary.

Lesson: "0 bytes, not even garbage" means the TX pin never drove the bus. A
baud-rate error would still produce garbage bytes on a scope or in a serial
terminal. Distinguishing "0 bytes" from "garbage bytes" halves the search space.

### 4. Flash succeeds but firmware does not run

`UV4 -f` (command-line download) performs erase/program/verify but does **not**
reset-and-run; the chip sits halted. The `-R0`/`-R1` flag in the `.uvoptx`
ST-Link entry is respected by the Keil GUI but not by command-line `-f`.

Two workarounds:

- Press the board's RST button after a command-line flash.
- Flash from the Keil GUI (Download), which resets and runs.

An onboard LED (`PC13`, active low) is lit at the end of `main()` init, so "LED
on" is an unambiguous "the main loop is running" signal.

## Protocol documentation bug found during bring-up

`docs/PROTOCOL.md` had a wrong SET_PARAM vector:
`AA 55 05 01 02 8B 0A AE CB` is 9 bytes and omits `param_id`, so it can never
match the firmware's `plen >= 4` check. Corrected to
`AA 55 05 01 01 02 8B 0A 7B 45` (10 bytes, CRC 0x457B). See erratum #4 in
`PROTOCOL.md`. The other nine vectors were re-checked with an independent CRC
script and are correct.

`tools/uart_probe.py selftest` now re-verifies all vectors and the resync logic
without any hardware — run it after touching the protocol.

## Input-capture self-check: the overflow bug that only measurement caught

`SELF_TEST` measures the real period on PA6 (TIM3_CH1) and returns the deviation
in permille. The first version averaged a **fixed 16 periods**. At the default
166666 Hz that is 16 × 432 = 6912 counts — fine. It also passed at 650000 and
700000 Hz. It looked correct.

Then 12345 Hz returned an error of **2359 ‰** (~236 %). Not noise, not a wiring
problem, not jitter:

```
12345 Hz  ->  period = 72e6 / 12345 = 5832 counts
16 periods ->  16 x 5832 = 93312 counts  >  65535 (16-bit counter)
```

The counter wrapped twice. A `uint16` subtraction only absorbs **one** wrap, so
the measured period came out as `(93312 mod 65536) / 16 = 1736`, implying
41475 Hz — a 236 % "error" that was purely an artefact of the measurement code.

Fix: make N adaptive so the window always fits in one counter pass.

```c
n = (uint8_t)(64000UL / per);   /* 64000 leaves margin for +/-1 count */
if (n > 16U) n = 16U;
if (n < 1U)  n = 1U;
```

After the fix, six setpoints from 5 kHz to 700 kHz all match theory:

| Set (Hz) | Period | N | Measured | Theory |
|---|---|---|---|---|
| 166666 | 432 | 16 | 0 ‰ | 0 ‰ |
| 100000 | 720 | 16 | 0 ‰ | 0 ‰ |
| 12345 | 5832 | 10 | 0 ‰ | 0 ‰ |
| 5000 | 14400 | 4 | 0 ‰ | 0 ‰ |
| 650000 | 110 | 16 | 6 ‰ | 6 ‰ |
| 700000 | 102 | 16 | 8 ‰ | 8 ‰ |

Two takeaways worth keeping:

1. **A self-check that only ever returns 0 teaches you nothing.** The bug was
   invisible at every frequency the developer happened to test first; it only
   appeared once a low frequency was tried. Test across the range, not at the
   nominal point.
2. **Residual error here is quantisation, not jitter.** 650000 Hz needs 110.77
   counts and rounds to 110 → 654545 Hz → 6 ‰. That is arithmetic, and it is
   reproducible, which is exactly what distinguishes it from a timing problem.

## Checklist for next time

1. `python tools/uart_probe.py selftest` — protocol logic, no hardware.
2. Loopback with TXD-RXD shorted — PC/CH340/cable chain.
3. Echo firmware — chip + clock + wiring.
4. Protocol firmware — press RST, confirm LED, then `probe --cmd GET_STATUS`.
5. If silent: check 0 bytes vs garbage, then cross-check PA9 with a scope.
