# STM32 Precision Timing Generator

A three-channel, nanosecond-resolution timing waveform generator built on STM32F103C8T6. All three channels are produced by hardware timer output-compare on a single timer — no software bit-banging, no FPGA, no HAL library.

## Features

- **Three synchronized channels** from one timer (TIM2 CH2/CH3/CH4) — no inter-timer phase drift
- **13.9 ns edge resolution** at 72 MHz timer clock (PSC = 0)
- **PWM1 + PWM2 combination** produces a window that a single PWM mode cannot
- **Deterministic startup**: outputs are forced low before pins are handed to the timer, avoiding power-on glitches
- **Ringing-aware GPIO config**: 2 MHz drive strength instead of 50 MHz for clean edges on long wires
- Bare-metal register-level implementation (no HAL dependency), Keil MDK / armclang

## Hardware

| Item | Value |
|---|---|
| MCU | STM32F103C8T6 (Cortex-M3, 72 MHz) |
| Debugger | ST-Link V2 (SWD: PA13 / PA14) |
| Channels | PA1 = CH1, PA2 = CH2, PA3 = CH3 |
| Timer | TIM2, CH2 / CH3 / CH4 |
| Logic level | 3.3 V |

Wiring: connect PA1 / PA2 / PA3 to the scope probes and GND to the board ground. Trigger on PA1 rising edge, timebase 1 us/div.

## Timing specification

| Parameter | Value |
|---|---|
| Timer clock | 72 MHz, PSC = 0 |
| One tick | 2 us = 144 counts |
| Period | 3 ticks = 432 counts, ARR = 431 |
| CH1 (PA1) | high [0, 288) counts — PWM1 |
| CH2 (PA2) | high [0, 144) counts — PWM1 |
| CH3 (PA3) | high [288, 432) counts — PWM2 |

All three channels share one time base. The falling edge of CH1 and the rising edge of CH3 occur at the same counter value (288), so they are aligned by hardware rather than by software delay.

The firmware is fixed-frequency in this release. Runtime frequency / duty configuration over UART is on the roadmap below.

## Quick start

1. Open `firmware/target/timing-generator.uvprojx` in Keil MDK (armclang V6.24, STM32F1xx DFP 2.4.1)
2. Build — expect `0 Error, 0 Warning`, code size ~1.3 KB
3. Flash with ST-Link V2 (SWD)
4. Connect oscilloscope probes to PA1 / PA2 / PA3, common ground
5. Trigger on PA1 rising edge, timebase 1 us/div

Expected: PA1 high 4 us / low 2 us; PA2 high 2 us; PA3 high 2 us aligned with PA1 falling edge; period 6 us.

## Design decisions (the part worth talking about)

### Why one timer is enough

An earlier two-timer version placed the third channel's active window in the *middle* of the period. Standard PWM can only produce a window anchored at the start (PWM1) or at the end (PWM2) of a period — a middle window required a second timer with a phase preset, plus a tricky failure mode when the trigger signal was a long level.

In this version the third channel's window was moved to the **end of the period**, so PWM2 generates it directly. One timer, three channels, zero software phase correction.

> Q&A follow-up: to move the window back to the middle, use a second timer with a phase preset (as before) or a one-pulse mode with an update interrupt. Both cost an extra timer and introduce software jitter.

### Why 2 MHz drive strength, not 50 MHz

3.3 V logic at 166.7 kHz does not need 2–3 ns edges. The 50 MHz drive setting produced severe ringing and crosstalk on long jumper wires; dropping to the 2 MHz setting (~30–50 ns edges) removed most of it.

### Why outputs are forced low first

On reset, GPIOs default to floating inputs. If the timer is enabled before the pins are configured, the outputs may glitch high. The firmware writes all outputs low first, configures them as push-pull, and only then hands them to the alternate function.

> Note B in the source: with PWM2, the third channel's falling edge is the period end (ARR), not a compare value. Adding an idle gap by simply stretching the period stretches that channel high through the gap. See `main.c` for the three correct ways to add a gap.

## Verification

- [ ] Three-channel waveform capture with readable timebase/measurements (PA1 / PA2 / PA3)
- [ ] "Set vs measured" error table (target < 0.5%)

Captures are added to `assets/` once available.

## Project structure

```
firmware/
  target/   final, buildable firmware and Keil project (this project)
docs/       design notes
assets/     oscilloscope captures and diagrams
```

## Roadmap

- [x] Fixed-frequency three-channel waveform
- [ ] UART parameter configuration (frequency / pulse width / on-off / status readback)
- [ ] PC host tool in C++ (serial port + frame protocol + CSV export)
- [ ] Input-capture self-check with error report

## License

MIT — see [LICENSE](LICENSE).
