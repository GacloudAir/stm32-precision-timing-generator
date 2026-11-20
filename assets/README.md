# Assets

Oscilloscope captures of the actual output, taken with a two-channel
GWInstek scope (only two channels could be captured at once, so the three
channels are verified pairwise — together the two captures cover all
pairwise timing relations).

## oe_damp_2us_per_div.png — CH1 (PA1) + CH3 (PA3), 2 us/div

Verifies the single-timer hardware alignment:

- PA1 (OE) high 4 us / low 2 us, PA3 (DAMP) high 2 us, period 6 us
- PA1 falling edge and PA3 rising edge occur at the same instant (CNT = 288)
- PA3 falling edge and the next PA1 rising edge occur at the same instant
- The two channels are perfectly complementary — no software delay visible

## pulse_damp_1us_per_div.png — CH2 (PA2) + CH3 (PA3), 1 us/div

Verifies the pulse / damp relationship:

- PA2 (PULSE) high 2 us, PA3 (DAMP) high 2 us, period 6 us
- 2 us all-low gap between PULSE falling edge and DAMP rising edge
- DAMP falling edge coincides with the next period's PULSE rising edge

## Capture notes

- Probe attenuation was left at 10X while the channel menu was at 1X, so the
  on-screen vertical readings were wrong and have been cropped out. These
  captures verify **timing only**; the outputs are 3.3 V logic.
- Timebase is encoded in the file names (`2us_per_div`, `1us_per_div`).
- Still pending: a single three-channel capture (needs a 4-channel scope),
  and a before/after ringing comparison for the 2 MHz drive-strength setting.
