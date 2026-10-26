# Receive range, sample rate and gain

[Research index](../../docs/research.md) / Receive range and gain

How far the LR2021 receiver reaches outside its datasheet bands, how fast the
source-2 I/Q stream can run, and which gain settings give the best signal to
noise ratio. Also a first look at one of the chip's unused built-in modems:
Bluetooth LE.

All results come from one LilyGO T-Display-P4 LR2021 board on ROM 1.24, with a
VHF whip on its external antenna port. Tone levels are relative. Nothing here
is a calibrated sensitivity figure.

## Summary

| Question | Result |
| --- | --- |
| Tuning range that receives | **100 MHz to 2740 MHz**, continuous |
| Below 100 MHz | Image calibration fails (`IMG_CAL`), no signal reaches the samples |
| Above about 2750 MHz | The synthesizer reports `PLL_LOCK`; nothing streams |
| LF input above 960 MHz | Works up to at least 1480 MHz; 10 to 25 dB stronger than the HF input from 960 to 1440 MHz |
| Highest clean I/Q rate | **250 ksample/s** (LoRa BW 62.5 kHz), no loss in 40 ms captures |
| 500 ksample/s and 1 Msample/s | FIFO overflows within about 2 ms: the 16 MHz SPI bus cannot carry 2 or 4 MB/s |
| Best gain for weak signals | Fixed step 12 or 13; boost 7 adds 0.5 to 1.4 dB |
| Chip AGC | Matches step 13 on weak signals, about 20 dB worse SNR than a fixed step on strong ones |
| Host-side gain control | Equal to the best fixed step at every level tested, 9 to 18 dB ahead of the chip AGC |
| Image rejection | About 41 dB at 433.9 MHz |
| Bluetooth LE 1M advertising | Received on channels 37, 38 and 39 through the 2.4 GHz input |

![Tone over noise floor across the tuning range](../../docs/images/rx-tuning-range.svg)

## Tuning range

The datasheet specifies 150 to 960 MHz on the low-frequency (LF) input and
1.5 to 2.5 GHz on the high-frequency (HF) input. The test ignored both limits
and asked a simpler question at each frequency: does a real signal move when
the receiver is retuned?

**Method.** The board's own 40 MHz clock leaks harmonics at every multiple of
40 MHz, so every test frequency has a marker without any transmitter. At each
multiple N, the receiver was tuned to N + 10 kHz and then N + 30 kHz, with a
125 ksample/s capture each time. A received signal shows up as a tone that
moves by exactly -20 kHz between the two captures. Fixed internal spurs do not
move, and neither does a synthesizer that has not locked.

**Result.** Every 40 MHz step from 120 MHz to 2720 MHz tracked, on whichever
input the frequency selects (LF below 1.5 GHz, HF above). The one gap was
2440 MHz, where Wi-Fi traffic buried the marker. A finer pass placed the edges:

| Frequency | Result |
| --- | --- |
| 84 to 96 MHz | `IMG_CAL` error, flat noise, nothing tracks |
| 100 MHz | Tracks, with `IMG_CAL` still flagged |
| 104 to 116 MHz | Tracks, no errors |
| 2740 MHz | Tracks |
| 2756 MHz | `PLL_LOCK` error |
| 2760 to 3760 MHz | `PLL_LOCK` and `IMG_CAL`; the capture is refused |

This opens the top of the FM broadcast band, the whole 108 to 137 MHz airband,
the 1 to 1.5 GHz gap and the 2.5 to 2.7 GHz band. The markers are leakage from
the board, so these runs prove the receive chain works at each frequency, not
how sensitive it is there.

From 960 to 1480 MHz both inputs work. The LF input showed the markers 10 to
25 dB stronger than the HF input over most of that span.

### Shielded tone check

A check with a known outside signal instead of the board's own leakage. Both
radios sat in a shielded enclosure. A HackRF One, tuned 200 kHz below each test
frequency F, sent one tone at F + 30 kHz for a few seconds. The LR2021 took a
60 ms capture at F (`lora lrprobe <kHz> 62500 14 -1 auto 60 hex`, 250
ksample/s) before the tone and during it. The figure is the strongest bin within
1.5 kHz of +30 kHz over the capture's median, tone minus quiet. The HackRF's own
carrier leak sits 200 kHz below F, outside the capture.

| F | Input | Antenna on the LR2021 | HackRF TX gain | Tone minus quiet |
| --- | --- | --- | --- | --- |
| 96 MHz | LF | VHF whip | 20 | none (`IMG_CAL` flagged) |
| 98 MHz | LF | VHF whip | 20 | none: a fixed spur near +30 kHz, unchanged by the tone |
| 100 MHz | LF | VHF whip | 20 | 16.3 dB, 14.5 dB on a repeat (`IMG_CAL` flagged) |
| 103 MHz | LF | VHF whip | 20 | 11.5 dB (`IMG_CAL` flagged) |
| 103 MHz | LF | 433 MHz | 20 | 7.1 dB |
| 106 MHz | LF | VHF whip | 20 | 15.7 dB |
| 106 MHz | LF | 433 MHz | 20 | 11.1 dB |
| 1300 MHz | LF | VHF whip | 20 | 37.6 dB |
| 1300 MHz | LF | 433 MHz | 20 | 16.9 dB |
| 2600 MHz | HF | 433 MHz | 0 | 35 dB |
| 2740 MHz | HF | 433 MHz | 0 | 42 dB |
| 2760 MHz | HF | 433 MHz | 0 | `PLL_LOCK`, no samples |

The low edge is between 98 and 100 MHz: the same tone that stands 14 to 16 dB
clear at 100 MHz is gone at 98. At TX gain 0 the low-band tones were too weak
to show at 103 and 106 MHz, and 1300 MHz still showed 8.7 dB. Every tone that
showed landed within 400 Hz of the expected offset and was absent from the
quiet capture.

## Sample rate

The I/Q rate is four times the LoRa bandwidth. Each rate was captured for
40 ms at 920 MHz, 433 MHz and 2461 MHz:

| LoRa bandwidth | Rate | Result | Peak FIFO fill of 1024 bytes |
| --- | --- | --- | --- |
| 31.25 kHz | 125 ksample/s | Clean | 36 to 156 |
| 62.5 kHz | 250 ksample/s | Clean | 160 to 668 |
| 125 kHz | 500 ksample/s | FIFO overflow after 1.2 to 2.3 ms | 1028 |
| 250 kHz | 1 Msample/s | FIFO overflow after 0.7 ms | 1028 |

The same 920 MHz marker appears at the same offset at 125 and 250 ksample/s,
which confirms the four-times-bandwidth rate. At 500 ksample/s the stream needs
2 MB/s, the whole capacity of a 16 MHz SPI bus before any command overhead, so
250 ksample/s is the practical ceiling for this interface. Long sessions at
250 ksample/s have not been tested yet. The ceiling belongs to the link, not
necessarily the chip: the datasheet limits SPI to 16 MHz, this board drains the
FIFO with CPU transfers of up to 64 bytes, and samples arrive as 16-bit pairs.
A host with a tighter read loop, or an 8-bit or decimated export if the ROM
offers one, could move it.

## Gain and boost

**Reproduction note, 2026-10-26:** [sfowlr reports](../community-2026-10/README.md) that `SetRx` resets manual gain and the setting must follow RX entry. The tables below remain historical measurements; audit actual command order before interpreting a requested gain as effective. The reported source-1/FSK export also makes the source-2 throughput ceiling above specific to that transport.

`SetAgcGainManual` takes steps 1 to 13 (0 is the chip's AGC). `SetRxPath`
boost takes 0 to 7 and, per the datasheet, only acts at steps 12 and 13.

**Weak signal.** A HackRF radiated a carrier at 433.92 MHz at its lowest
useful level, with the LR2021 tuned 20 kHz below it:

| Gain step | Tone over floor |
| --- | --- |
| 1 to 5 | Not detected |
| 6 | 13.6 dB |
| 7 | 17.9 dB |
| 8 | 24.0 dB |
| 9 | 25.1 dB |
| 10 | 30.1 dB |
| 11 | 33.6 dB |
| 12 | 35.9 dB (boost 7: 37.3 dB) |
| 13 | 37.0 dB (boost 7: 37.5 dB) |
| AGC | 36.7 dB |

**Strong signal.** With the carrier about 30 dB stronger, AGC reached only
47.0 dB, against 66.5 dB at step 7 and 68.4 dB at step 13 with boost 7. That
last setting pushed the samples to -1.8 dBFS, close to clipping.

So the chip AGC is as good as maximum gain for weak signals and clearly worse
for strong ones. A host-side AGC that picks a fixed step from the I/Q level
would beat it in both cases. Gain settings alone do not move weak-signal SNR
by more than about 1 dB once the step is 12 or 13.

At step 13 with boost 7 the noise floor sits near -58 dBFS. With AGC it sits
near -65 dBFS, about 6 to 7 bits of the 16-bit samples.

**Host-side gain control.** LakeShark now picks the gain itself while
streaming: it starts at step 13 with boost 7, drops one step whenever samples
come within about 2 dB of full scale, and climbs back after 300 ms below
-15 dBFS. Against the chip AGC, on the same 433.92 MHz carrier:

| Carrier level | Chip AGC | Host gain control | Best fixed step |
| --- | --- | --- | --- |
| Strong | 49.9 dB | 67.9 dB (settled at step 12) | 67.9 dB |
| Medium | 58.3 dB | 67.6 dB | 67.7 dB |
| Weak | 37.4 dB | 38.1 dB | 38.1 dB |

It matches the best fixed step at every level, 9 to 18 dB ahead of the chip
AGC on medium and strong signals. Changing the gain mid-stream does not
interrupt the samples: a 60 second P25 session and FM sessions at 121.5, 162.55
and 1200 MHz ran with no FIFO loss.

**DC offset.** At step 13 the mean of I and Q sat about 33 dB below the noise
RMS, so no DC correction is applied. That also matters for narrowband FM,
where a centred carrier sits at 0 Hz and a DC blocker would remove it.

**Image rejection.** The mirror of the strong carrier appeared about 41 dB
below it at both step 7 and step 13.

**Spurs.** A few fixed-offset internal tones appear at some frequencies and
gain steps, for example -20874 Hz and -41503 Hz at 433.9 MHz. They do not move
when the receiver is retuned, which is the quickest way to tell them from RF.

## Bluetooth LE advertising

The LR2021 has a Bluetooth LE packet engine (packet type 3, datasheet section
14). Configured for LE 1M on the advertising channels, with the
channel-dependent whitening seed, CRC seed 0x555555 and access address
0x8E89BED6, it received ordinary advertising from nearby devices through the
2.4 GHz input:

| Channel | Time | Valid advertisements | Distinct addresses |
| --- | --- | --- | --- |
| 37 (2402 MHz) | 12 s | 807 | 31 |
| 38 (2426 MHz) | 10 s | 497 | 24 |
| 39 (2480 MHz) | 10 s | 742 | 32 |

Received levels ranged from -58 to -95.5 dBm. Packets with a CRC error are
counted and dropped by the chip. LE Coded (S2, S8) and LE 2M heard nothing in
the same place: 2M is never used on the advertising channels and no
long-range advertisers were nearby, so those modes are untested rather than
failed.

## Built-in modems not yet used

Besides LoRa, FSK and OOK, the LR2021 datasheet lists these receive engines.
Each could become a receiver without the I/Q stream:

| Engine | Receive capability |
| --- | --- |
| Bluetooth LE | 1M, 2M, Coded S2 and S8 (1M verified above) |
| O-QPSK 802.15.4 | 2.4 GHz, 250 kbit/s DSSS: Zigbee and Thread frames |
| FLRC | Up to 2.6 Mbit/s, sub-GHz and 2.4 GHz |
| Wireless M-Bus | Meter collector modes |
| RTToF | LoRa round-trip ranging between two LR20xx radios |

## Reproduce

The measurements used two console commands in a LakeShark development build:

- `lora lrprobe <kHz> [bw] [gain] [boost] [lf|hf|auto] [ms] [hex]` runs one
  receive-only I/Q capture at any frequency, bandwidth, gain step, boost and
  input, then prints the chip error flags, FIFO statistics and the three
  strongest tones of an averaged 1024-point spectrum.
- `lora lrble <37|38|39> [seconds] [1m|2m|s2|s8]` receives Bluetooth LE
  advertising and prints one line per packet.

Tools in [`tools/`](tools/):

| File | Purpose |
| --- | --- |
| `lrprobe.py` | Sends a list of `lrprobe` argument lines to the board and writes a CSV |
| `track.py` | Pairs the +10 kHz and +30 kHz captures and reports which frequencies track |
| `plot_tuning.py` | Draws the tuning-range chart from the CSV files |
| `cw.py` | Sends a HackRF carrier for a fixed time, ISM frequencies only |

Raw results are in [`data/`](data/). Advertising logs are not included
because they contain nearby devices' addresses and names.

## Limits of these results

- One board, one antenna, one location. The tuning edges depend on this
  module's front end and matching and may differ on other module builds.
- Tone levels are relative: the markers are board leakage and the HackRF
  output is uncalibrated.
- The 433 and 915 MHz carriers came in at different levels with the same
  source settings. That depends on the antenna, the source and the module's
  matching network, and does not identify the module variant on its own.
