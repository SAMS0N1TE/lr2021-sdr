# Chip AGC readback

[Research index](../../docs/research.md) / Chip AGC readback

Which gain step the LR2021's AGC actually chooses, read straight from the
chip, and what the step means in decibels. The register map and the
per-packet gain byte come from
[beala/lr2021-re](https://github.com/beala/lr2021-re) by Alex Beal (MIT),
mainly its [AGC gain notes](https://github.com/beala/lr2021-re/blob/main/docs/agc-gain.md).
That project credits this repository for the 0x0290 to 0x0293 capture
commands; this report returns the favour for the gain registers.

Tags follow lr2021-re: **[HW]** verified on this repository's board,
**[RE]** read from ROM or taken from lr2021-re and not yet run here,
**[guess]** unverified. lr2021-re marks several of these findings [HW] on its
own hardware. They stay [RE] here until they run on the LilyGO T-Display-P4
LR2021 (ROM 1.24) used for every other report in this repository.

## Status

| Experiment | Question | Status |
| --- | --- | --- |
| 1. Manual step sweep | Do 0xF30154 and 0xF3012C read back each forced step and the lr2021-re gain values? | Not run yet |
| 2. AGC against signal level | Which steps does the chip AGC pick for weak, medium and strong carriers, and which does host gain control pick? | Not run yet |
| 3. Gain units | Is `gain_value` in 0.5 dB units? | Not run yet |
| 4. Source 6 cross-check | Do the IQ6G gain bits equal the register readback? | Not run yet |
| 5. RSSI field | Is 0xF301A8 bits 26:18 the value GetRssiInst returns? | Not run yet |

The software side is done and tested against the simulator: the library call
`lr2021_iq_read_gain`, session sampling, bridge request `GET_GAIN` (cap 256),
the host, Rust and SoapySDR wrappers, and the tools below. The simulator only
checks the plumbing. It models the registers from the lr2021-re table, so its
results say nothing about the chip.

## Registers [RE]

| Register | Bits | Meaning (lr2021-re) |
| --- | --- | --- |
| 0xF30154 | 23:20 | gain step in use, 1 to 13; 13 in AGC mode with no signal |
| 0xF30154 | 19:16 | SetAgcGainManual setting, 0 = AGC |
| 0xF3012C | 26:18 | gain value of the step: 16, 28, 38, 49, 57, 64, 73, 85, 96, 108, 120, 127, 138 for steps 1 to 13, 139 idle in AGC mode |
| 0xF301A8 | 26:18 | instantaneous RSSI; GetRssiInst returns this field |

lr2021-re suggests the gain value is total gain in 0.5 dB units [guess]. If
that holds, the table spans 61 dB, about 5 dB per step.

## Setup

- LilyGO T-Display-P4 with LR2021, ROM 1.24, bridge firmware with cap 256
  (this branch), VHF whip on the external antenna port.
- HackRF carrier at 433.92 MHz from
  [`cw.py`](../rx-range-and-gain/tools/cw.py) at the weak, medium and strong
  levels of the [receive range report](../rx-range-and-gain/README.md#gain-and-boost).
  The LR2021 tunes 20 kHz below the carrier, as in that report.
- Host: Python 3 with numpy, and pyserial for the USB link.

`agc_readback.py` talks the bridge protocol over Wi-Fi (`tcp:IP`) or USB
serial (`COM5`, `/dev/ttyACM0`). On the 3 Mbaud serial link add `--cs12`.
Every command writes CSV; `analyze_agc.py` summarizes it. Commit the raw CSV
files under `logs/` with the results.

## Experiment 1: manual step sweep

Force steps 1 to 13 and read the registers after each one, idle and during a
short stream.

```bash
python agc_readback.py tcp:192.168.4.1 --freq 433900000 --out logs/sweep_quiet.csv sweep
python analyze_agc.py sweep logs/sweep_quiet.csv
```

Pass: for every step, `step` and `manual` equal the forced step and
`gain_value` equals the lr2021-re table, both idle and while streaming.
Record any difference here.

Result: not run yet.

## Experiment 2: AGC against signal level

Stream source 2 in AGC mode and log the chip step every 100 ms, once per
carrier level. Then repeat with host gain control choosing the step.

```bash
python agc_readback.py tcp:192.168.4.1 --freq 433900000 --out logs/agc_strong.csv log --seconds 30
python agc_readback.py tcp:192.168.4.1 --freq 433900000 --boost 7 --out logs/host_strong.csv log --step 13 --host-agc --seconds 30
python analyze_agc.py log logs/agc_strong.csv
```

`--host-agc` applies the rule LakeShark documents in the receive range report
(start at step 13, drop one step when samples come within 2 dB of full scale,
climb back after 300 ms below -15 dBFS) through live `SET_GAIN`, and logs the
step it chose next to the chip readback. It reimplements the documented rule;
it is not LakeShark's code. Running LakeShark itself with a readback log is
the alternative.

What to look for: the steps the chip AGC settles on at each level, against
the step host gain control settles on. That explains the measured 9 to 18 dB
gap between the two on medium and strong carriers.

Result: not run yet.

## Experiment 3: gain units

With a carrier strong enough to stand well above the floor at step 1 but
below clipping at step 13, sweep the steps and compare the tone level change
with the `gain_value` change.

```bash
python agc_readback.py tcp:192.168.4.1 --freq 433900000 --out logs/sweep_tone.csv sweep --tone-offset 20000
python analyze_agc.py sweep logs/sweep_tone.csv
```

The analysis fits tone level against `gain_value` over the usable steps
(peak below -1 dBFS, tone at least 20 dB over the floor) and prints dB per
unit and per step. 0.5 dB per unit confirms the lr2021-re reading. If it
holds, add a dB-per-step table to
[the receive range report](../rx-range-and-gain/README.md#gain-and-boost).
Host gain control could later use that table in place of its fixed 2 dB
threshold; that change is outside this work.

Result: not run yet.

## Experiment 4: source 6 cross-check

Source 6 (IQ6G) carries the step in the low bits of every pair. Stream it and
compare the most common step in each 100 ms interval with the readback.

```bash
python agc_readback.py tcp:192.168.4.1 --freq 433900000 --out logs/iq6.csv iq6 --seconds 20
python analyze_agc.py iq6 logs/iq6.csv
```

They should agree in every interval, in AGC mode and with a forced step
(`--step 9`).

Result: not run yet.

## Experiment 5: RSSI field

GetRssiInst is not a bridge request, so this runs from the firmware console.
Apply [`packet_status_probe/firmware-integration.patch`](packet_status_probe/firmware-integration.patch)
to the firmware, build, flash, and run:

```
agcrssi 200 50
```

Each line has GetRssiInst's two bytes, its 9-bit value
(`b0 << 1 | b1 >> 7`, -value/2 dBm), the 0xF301A8 field and whether they
match. Run it with no carrier and with each carrier level. The command
starts RX if needed and reapplies the saved receive configuration at the end.

Result: not run yet.

## Tools

| File | Purpose |
| --- | --- |
| [`bridge.py`](bridge.py) | Minimal Python bridge client: frames, HELLO, SET_RX, SET_GAIN, RX_START/STOP, GET_GAIN, raw RX_DATA |
| [`agc_readback.py`](agc_readback.py) | Experiments 1 to 4: `sweep`, `log`, `iq6` |
| [`analyze_agc.py`](analyze_agc.py) | Pass/fail table, dB per gain unit, step distribution, source 6 agreement |
| [`packet_status_probe/lr2021_agc_probe.inc`](packet_status_probe/lr2021_agc_probe.inc) | Firmware console command `agcrssi` (experiment 5) |
| [`packet_status_probe/firmware-integration.patch`](packet_status_probe/firmware-integration.patch) | Adds the command to `firmware/esp32p4/main/main.c` |

`tests/test_agc_tools.py` (CTest `agc_readback_tools`) runs `sweep`, `log`,
`log --host-agc` and `iq6` against `tools/lr2021_bridge_sim` with a normal and
a strong simulated tone.

## Limits

- One board, one antenna, one location, as in the other receive reports.
- Carrier levels are relative; the HackRF output is uncalibrated.
- The session samples the registers between FIFO reads, so a readback lags
  the samples by up to one sampling interval (64 service passes on the
  bridge).
- Reading the registers costs about six SPI frames per sample. The 500000
  samples/s figure in [host-sdr](../../docs/host-sdr.md) was measured without
  sampling; rerun it with sampling on before relying on both together.
