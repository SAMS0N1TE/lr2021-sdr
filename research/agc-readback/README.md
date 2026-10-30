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
| 1. Manual step sweep | Do 0xF30154 and 0xF3012C read back each forced step and the lr2021-re gain values? | **Step and manual: yes, all 13 steps [HW].** Gain value: within -2 to +4 of the table, varies between reads |
| 2. AGC against signal level | Which steps does the chip AGC pick for weak, medium and strong carriers, and which does host gain control pick? | Quiet band only: AGC stays at step 13 [HW]. Carrier runs pending (no HackRF tools on the test PC) |
| 3. Gain units | Is `gain_value` in 0.5 dB units? | Pending, needs a carrier |
| 4. Source 6 cross-check | Do the IQ6G gain bits equal the register readback? | **Yes**, in AGC (13) and at forced step 9, in every interval while streaming [HW] |
| 5. RSSI field | Is 0xF301A8 bits 26:18 the value GetRssiInst returns? | **Yes** [HW]: 94 of 100 back-to-back reads equal, the rest within 4 |
| Per-packet gain | Is GetLoraPacketStatus byte 9 the step latched during the packet? | Pending: no packets heard in 5 minutes; needs a known transmitter |
| GET_GAIN over the bridge | Does it work idle and while streaming? | USB serial: **yes** [HW]. Wi-Fi: pending (the board has no Wi-Fi credentials yet) |

The runs on 2026-10-30 used the LilyGO T-Display-P4 LR2021 (ROM 1.24)
on its USB serial link, with this branch's bridge
firmware plus the two research patches. Raw logs are in [`logs/`](logs/).

The software side is also tested against the simulator: the library call
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

On this board [HW]:

- The fields only mean something while the radio receives. In standby,
  0xF30154 bits 23:20 read 10 for every setting and both 9-bit fields read
  0x1FF; bits 19:16 still hold the manual setting. `lr2021_iq_read_gain`
  therefore returns `LR2021_IQ_STATE` outside RX, with the fields filled in.
- In LoRa RX, bits 23:20 and 19:16 equal the forced step for all 13 steps.
- `gain_value` is a live reading, not a constant. At step 7 it read 74, 75,
  76 and 77 in different runs, and it stays within -2 to +4 of the lr2021-re
  table. In the FSK setup of the raw routes it reads about 153 at step 9,
  against 95 to 99 in LoRa RX, so the table only describes LoRa reception.
- 0xF301A8 bits 26:18 is the GetRssiInst value. GetRssiInst returns two
  bytes, and byte 1 only ever reads 0 or 1 here, so the value is
  `b0 << 1 | (b1 & 1)`, and -value/2 is the level in dBm.

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

Result [HW], no carrier, LoRa BW 31.25 kHz at 433.9 MHz, 1.5 s per step
([`logs/sweep_quiet_usb.csv`](logs/sweep_quiet_usb.csv)): step and manual
equal the forced step on all 13 steps while streaming, so the step fields
pass. The gain values read 16, 27, 38, 50, 58, 67, 76, 86, 95, 108, 119,
129, 139 for steps 1 to 13, against the lr2021-re table 16, 28, 38, 49, 57,
64, 73, 85, 96, 108, 120, 127, 138: the same shape, up to 3 units higher in
the middle, and slightly different in every run (an earlier run read 40, 59,
66, 77 and 99 at steps 3, 5, 6, 7 and 9). Idle, in standby, every request
answered `STATE` with the manual setting.

## Experiment 2: AGC against signal level

Stream source 2 in AGC mode and log the chip step every 100 ms, once per
carrier level. Then repeat with host gain control choosing the step.

```bash
python agc_readback.py tcp:192.168.4.1 --freq 433900000 --out logs/agc_strong.csv log --seconds 30
python agc_readback.py tcp:192.168.4.1 --freq 433900000 --boost 7 --out logs/host_strong.csv log --step 13 --host-agc --seconds 30
python analyze_agc.py log logs/agc_strong.csv
```

`--host-agc` applies the host gain rule described in the
[receive range report](../rx-range-and-gain/README.md#gain-and-boost)
(start at step 13, drop one step when samples come within 2 dB of full scale,
climb back after 300 ms below -15 dBFS) through live `SET_GAIN`, and logs the
step it chose next to the chip readback.

What to look for: the steps the chip AGC settles on at each level, against
the step host gain control settles on. That explains the measured 9 to 18 dB
gap between the two on medium and strong carriers.

Result: the quiet band only, so far. Over 30 s of AGC reception at 125000
samples/s with no carrier, the chip stayed at step 13 in all 150 readbacks
([`logs/agc_quiet_usb_30s.csv`](logs/agc_quiet_usb_30s.csv)). The weak,
medium and strong carrier runs are pending: the HackRF is connected to the
test PC, but `hackrf_transfer`, which `cw.py` drives, is not installed there.

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

Result: pending, needs the carrier. The quiet sweep shows the analysis
guard working: with no single carrier present, it declines to report a
unit.

## Experiment 4: source 6 cross-check

Source 6 (IQ6G) carries the step in the low bits of every pair. Stream it and
compare the most common step in each 100 ms interval with the readback.

```bash
python agc_readback.py tcp:192.168.4.1 --freq 433900000 --out logs/iq6.csv iq6 --seconds 20
python analyze_agc.py iq6 logs/iq6.csv
```

They should agree in every interval, in AGC mode and with a forced step
(`--step 9`).

Result [HW]: in AGC with no carrier, the IQ6G bits read 13 in every pair
and the readback read 13; at forced step 9 both read 9
([`logs/iq6_agc_usb.csv`](logs/iq6_agc_usb.csv),
[`logs/iq6_step9_usb.csv`](logs/iq6_step9_usb.csv)). Each log has one
mismatched last row with a readback of 10: that request arrived after the
device's bounded stream had ended, with the radio back in standby.

Source 6 ran at 333332 pairs/s whatever `rate_hz` asked for (50000 and
125000 both gave 333332 with input code 17), while the RX_DATA headers
reported the requested rate. That is a defect in route 3, outside this
work, and needs its own fix. It also means source 6 outruns the 3 Mbaud
serial link, so these runs used bounded 3 s streams that the device spooled.

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

Result [HW], LoRa RX at 433.9 MHz, AGC, no carrier, 100 reads 20 ms apart
([`logs/agcrssi_lora_quiet.txt`](logs/agcrssi_lora_quiet.txt)): with the
value decoded as `b0 << 1 | (b1 & 1)`, 94 reads equal the 0xF301A8 field
exactly and the rest differ by at most 4, as expected for two reads taken
microseconds apart. Mean and spread are identical (249.7, standard
deviation 26.3; the first read after SetRx is 511 in both). Decoded as
`b0 << 1 | b1 >> 7`, the half-dB bit is always 0, because byte 1 never has
bit 7 set here. An earlier run in the FSK setup
([`logs/agcrssi_fsk_step9.txt`](logs/agcrssi_fsk_step9.txt)) shows the same
range; it ends early because the first probe build hung in its restore
step, which is fixed.

## Per-packet gain from GetLoraPacketStatus

lr2021-re reports that GetLoraPacketStatus (0x022A) returns 10 bytes, not
the 6 RadioLib reads [RE]:

| Byte | Meaning |
| --- | --- |
| 0 to 5 | documented fields; byte 1 is the payload length |
| 6 to 8 | frequency offset, signed 24 bits (in datasheet Rev 2.2) |
| 9 | gain step latched during the packet (undocumented) |

Semtech's PRAM patch does not override 0x022A, so this repository's
unpatched chip should behave the same [RE].

This repository has no LoRa packet receive path and this work does not add
one. The console command `pktstatus` in the same probe include receives LoRa
packets on its own and prints all 10 status bytes per packet, plus the live
0xF30154 step for comparison:

```
pktstatus 120 910525000 62500 7 0x12 0
```

Arguments: seconds, frequency (150 to 960 MHz), bandwidth, spreading factor,
sync word, gain (0 for AGC), preamble length. It never transmits. Run it in
three conditions:

| Condition | Command | Expected byte 9 |
| --- | --- | --- |
| AGC, normal traffic | `pktstatus 300 910525000 62500 7 0x12 0` | 13 for weak packets |
| Forced step 5 | `pktstatus 300 910525000 62500 7 0x12 5` | 5 on every packet |
| Strong nearby transmitter, AGC | as the first, transmitter within a few metres | below 13 |

Result: no packets heard. Two minutes on MeshCore's US settings
(910.525 MHz, BW 62.5 kHz, SF7, sync 0x12) and three on Meshtastic LongFast
(906.875 MHz, BW 250 kHz, SF11, sync 0x2B), both in AGC, received nothing
([`logs/pktstatus_meshcore_agc.txt`](logs/pktstatus_meshcore_agc.txt),
[`logs/pktstatus_meshtastic_agc.txt`](logs/pktstatus_meshtastic_agc.txt)).
Either nothing was transmitting in range or the probe has a gap; byte 9
needs a known transmitter before it can be checked. The probe ran and
restored the radio cleanly both times.

### For MeshCore and Meshtastic driver authors

Read 10 bytes from GetLoraPacketStatus instead of 6. Byte 9 then gives the
gain step the chip used for that packet, at no extra cost: it shows whether a
node is being desensitized by a strong neighbour. Bytes 6 to 8 are the
frequency offset. lr2021-re has a MeshCore branch that reports byte 9 as `last_gain`. This stays [RE]
here until `pktstatus` confirms it on this board.

## Tools

| File | Purpose |
| --- | --- |
| [`tools/lr2021_bridge.py`](../../tools/lr2021_bridge.py) | Minimal Python bridge client: frames, HELLO, SET_RX, SET_GAIN, RX_START/STOP, GET_GAIN, raw RX_DATA |
| [`agc_readback.py`](agc_readback.py) | Experiments 1 to 4: `sweep`, `log`, `iq6` |
| [`analyze_agc.py`](analyze_agc.py) | Pass/fail table, dB per gain unit, step distribution, source 6 agreement |
| [`packet_status_probe/lr2021_agc_probe.inc`](packet_status_probe/lr2021_agc_probe.inc) | Firmware console commands `agcrssi` (experiment 5) and `pktstatus` |
| [`packet_status_probe/firmware-integration.patch`](packet_status_probe/firmware-integration.patch) | Adds both commands to `firmware/esp32p4/main/main.c` |

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
  samples/s figure in [host-sdr](../../docs/host-sdr.md) was measured over
  Wi-Fi without sampling; rerun it with sampling on before relying on both
  together. Over USB, 30 s at 125000 samples/s (CS12, 4 Mbaud) with
  sampling and 150 GET_GAIN polls delivered all 3750426 pairs.
- The bridge firmware sends every frame, replies included, through one
  spool in order. When the samples outrun the link (source 6 over serial,
  or CS12 at 125000 samples/s at 3 Mbaud), a GET_GAIN reply waits behind
  the backlog for seconds. Size the link for the stream, or stop the stream
  first.
