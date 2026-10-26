# Additional ROM 1.24 findings from another board

[Research index](../../docs/research.md)

Updated 2026-10-26. This incorporates the report in
[LakeShark discussion #16](https://github.com/SAMS0N1TE/LakeShark/discussions/16)
by **sfowlr**, dated 2026-10-24. The first table is contributor-reported;
our separate reproduction results follow below. No supporting command
transcript or raw capture was attached to the discussion when reviewed.

## What was reported

The test platform was a Meshnology W12 running LR2021 ROM 1.24.

| Area | Contributor's result |
| --- | --- |
| FSK rate selection | With routing byte zero at `FF`, the FSK `rx_bw` setting controls export rate; the reported relationship is `32 MHz / (mode * decimation)` |
| Export formats | Source 1 carries signed 8-bit components; source 2 carries signed 16-bit components. Both taps follow the channel filter |
| Throughput | Source 1 sustained approximately 664.8 thousand complex pairs/s using 16 MHz SPI, with no loss reported |
| Dynamic range | The 8-bit container saturated near -32/+31; source 2 showed at least 50 dB additional range in that setup |
| Manual gain | Apply the manual setting after entering RX; starting RX returned it to step 13 |
| TX selector 1 | Observed frequency offset followed `sample * fdev / 64`, across tested rates from 31.25 to 250 thousand pairs/s; repeated RX/TX transitions needed no reset |
| Native four-level FSK | Command `023B`, argument `01`, enabled a mode that transmitted 32 DMR bursts decoded without FEC errors |

The maintainer requested command sequences, implementation and capture evidence
in the discussion. Credit for these extensions belongs to sfowlr.

## What we reproduced on the LakeShark board

Receive-only tests used a LilyGO T-Display-P4 with LR2021 ROM 1.24 on COM14,
experimental LakeShark `2.18.0-g4c9d49e9954b`, and the Wi-Fi antenna already
on the LR2021 path. The `lora lrlab` command enters FSK RX, sets gain after
`SetRx`, validates FIFO status, and restores normal LoRa RX. The follow-up
board status showed no chip errors. All raw IQ and console logs remain local.

| Discussion claim | Our evidence and current limit |
| --- | --- |
| `rx_bw` controls source-1 export rate | Supported for selected codes: 163 delivered ~200k candidate pairs/s, 162 ~400k, and 154 and 26 ~500k. Codes 154 and 26 have different listed FSK bandwidths (166,667 and 192,308 Hz) yet the same observed export rate. The full `32 MHz/(mode × decimation)` mapping is not established. |
| Source 1 is IQ8, source 2 IQ16 | Supported by their two versus four bytes per candidate complex pair and by the same moving RF-associated tone in both, using interleaved signed 8-bit versus little-endian 16-bit interpretation. Source-1 component values stayed within −32..+31. Pair order and sign on the frequency axis are supported; absolute amplitude scale and the claimed ≥50 dB dynamic-range difference need calibrated input levels. |
| 664.8k pairs/s without loss at 16 MHz SPI | **Not reproduced on this board.** A code-225 high-rate candidate overflowed in all five 20 ms count-only runs. Removing the PSRAM sample copy did not fix it. The precise code-to-rate mapping of sfowlr's setup has not been supplied, so this does not refute their W12 result. |
| `SetRx` resets manual gain | **Not observed in our tested command sequence.** Bounded pre-only versus post-RX gain experiments in FSK and LoRa showed the same low/high marker levels for steps 1/13. There is no direct gain-register readback, and the W12 command sequence may differ. |
| Selector-1 TX scaling and reset-free transitions | Not tested in this receive-only pass. |
| Native 4-FSK and DMR delivery | Not tested; no 4-FSK TX or DMR decode claimed here. |

The SPI capture path is **configured** for 16 MHz: it primes the device with
`override_freq_hz=16000000` and then uses the P4 CPU SPI registers with that
timing. SCLK was not measured at a pin. The 16 MHz ideal bit rate allows at
most 2 MB/s before opcode/status bytes, BUSY polling and scheduling. Our
source-1 path drains up to 62 payload bytes per CPU SPI transaction from a
1,024-byte FIFO, so a ~500k-pair/s IQ8 stream (~1 MB/s) fills that FIFO in
about 1.0 ms without service; a 665k-pair/s IQ8 stream (~1.33 MB/s) fills it
in about 0.77 ms.

### Rate and loss pass

At 2,437 MHz, code 154 ran without reported FIFO loss for 80 ms while storing
79,876 bytes. A count-only mode then removed the large sample copy and extended
the window to 250 ms. For codes 154 and 26, **five of ten** 250 ms trials
completed and delivered about 249,900–250,060 bytes, consistent with ~500k
candidate complex pairs/s. Five trials overflowed between 12 and 141 ms.
Successful runs had maximum service gaps at most 0.82 ms; failed runs had
gaps of 1.16–1.85 ms. Code 225 overflowed in all five 20 ms count-only
trials, usually within about 2–3 ms. All trials recovered normal LoRa RX.
The rejected captures are not counted as continuous samples. Even a clean
FIFO flag does not independently prove every sample index was preserved.
With codes 154 and 26, the same marker moved from about +4 to −16 kHz under
a +20 kHz retune when interpreted at 500k pairs/s. This supports their
shared export rate independently of the byte counts. Their different
listed FSK bandwidths are **not** yet independently measured filter shapes:
the strong internal marker remained visible even beyond those nominal
half-bandwidths, where leakage may bypass the external analog path.

### Gain command order

The research firmware's `hexpre` variant sends `SetAgcGainManual` before
`SetRx` and deliberately omits the usual post-RX gain write. At 920.010 MHz
with FSK source 1, we alternated pre-only and post-RX step 1 and step 13 in
twelve 10 ms captures, using the same board-associated marker. Each capture
had valid serial framing, no reported FIFO overflow and successful recovery.

| Requested gain | Write order | Captures | Median marker above spectrum median | Median IQ component RMS |
| --- | --- | ---: | ---: | ---: |
| Step 1 | Before RX only | 3 | 30.44 dB | 1.16 counts |
| Step 1 | After RX | 3 | 31.10 dB | 1.16 counts |
| Step 13 | Before RX only | 3 | 59.85 dB | 11.06 counts |
| Step 13 | After RX | 3 | 60.14 dB | 11.06 counts |

A LoRa source-2 pilot at the same marker gave about 25 dB above median for
step 1 in both orders and about 58 dB for step 13 in both orders. Thus our
`SetRx` path did **not visibly replace pre-set step 1 with step 13** in either
modem. This tests received levels, not an internal gain register; it cannot
explain why the contributor saw a reset on the Meshnology W12. The firmware
keeps the conservative post-RX write in its normal lab path.

```text
python tools/lr2021_lab.py --port COM14 --khz 920010 --source 1 --bw 163 --ms 10 --gain 1 --mode hexpre --assumed-rate 200000 --out local-gain-pre
python tools/lr2021_lab.py --port COM14 --khz 920010 --source 1 --bw 163 --ms 10 --gain 1 --mode hex --assumed-rate 200000 --out local-gain-post
```

### Frequency-axis pass beyond the specified bands

The board's clock-related leakage supplies a stationary marker without an RF
transmitter. At each test frequency we retuned the LR2021 upward by 20 kHz,
then compared 10 ms FSK captures at code 163 from sources 1 and 2. Both
sources showed the **same peak moving downward by about 20 kHz** under the
interleaved IQ8/IQ16 interpretations and an approximately 200k-pair/s axis.
All paired captures below had complete serial transfers, no reported FIFO
overflow, and successful LoRa recovery.

| Tune pair (MHz) | Source-1 marker, first → second (kHz) | Source-2 corroboration |
| --- | --- | --- |
| 120.010 → 120.030 | −10.42 → −30.39 | Corresponding peak shifted ~−20 kHz |
| 920.010 → 920.030 | −4.41 → −24.40 | Corresponding peak shifted ~−20 kHz |
| 2320.010 → 2320.030 | +4.11 → −15.88 | Corresponding peak shifted ~−20 kHz |
| 2600.010 → 2600.030 | −9.50 → −29.49 | Corresponding peak shifted ~−20 kHz |
| 2720.010 → 2720.030 | −10.02 → −30.03 | Corresponding peak shifted ~−20 kHz |

At 1560 MHz a strong ~+16.7 kHz component stayed fixed when tuned, so it is
not counted as the moving marker. These tests validate the digital tuning and
sample frequency axis on this board, including source 1 well outside the
LR2021's nominal bands. Internal leakage is **not** a measure of antenna-port
sensitivity, analog passband, external signal identity or sample-sequence
continuity. The same limitation applies to our marker at 2.6 and 2.72 GHz.

To repeat a bounded capture with the research firmware, store raw data in a
private local directory and compare the peaks after a 20 kHz retune:

```text
python tools/lr2021_lab.py --port COM14 --khz 2320010 --source 1 --bw 163 --ms 10 --mode hex --assumed-rate 200000 --out local-marker-a
python tools/lr2021_lab.py --port COM14 --khz 2320030 --source 1 --bw 163 --ms 10 --mode hex --assumed-rate 200000 --out local-marker-b
python tools/lr2021_lab.py --port COM14 --khz 2437000 --source 1 --bw 154 --ms 250 --mode discard --out local-rate-a
```

The first two commands make a spectral check, not an RF-calibrated gain test.
`discard` counts delivered bytes without saving IQ; it cannot check sequence
continuity. The host parser rejects missing hex blocks, reported FIFO loss,
transport errors and failed recovery. The capture command itself is RX-only.

### RadioFoundry as a future 4-FSK reference

[RadioFoundry](https://github.com/RadioFoundry/RadioFoundry) does not contain
LR2021 drivers or the reported `0x023B` command. It does provide a useful
independent DMR reference: its [modem specification](https://github.com/RadioFoundry/RadioFoundry/blob/main/docs/spec/dmr-modem.md)
states the 4800-symbol/s, 12.5 kHz-channel four-level waveform and nominal
deviations of ±648 and ±1944 Hz, and its [SDR bench monitor](https://github.com/RadioFoundry/RadioFoundry/blob/main/tools/bench/sdrmon.py)
describes burst timing, sync/FEC and level checks from captured IQ. Its
board-specific HR_C6000/HR_C7000 commands do not transfer to LR2021. If we
later test native LR2021 4-FSK, an independent receiver should measure those
four deviation levels and decode complete DMR bursts; a successful `0x023B`
command response alone would not demonstrate modulation or interoperability.

## What changes in our interpretation

Our **250 kpair/s clean source-2 result** remains a measurement of our IQ16
transport. It is not a universal LR2021 export ceiling. The new source-1
captures show ~500k candidate pairs/s intermittently clean for 250 ms on this
board. At 664800 complex pairs/s,
two bytes per pair require 1,329,600 B/s, or 66.48% of a 16 MHz SPI link's ideal
2,000,000 B/s. Four bytes require 2,659,200 B/s and cannot fit. Commands, BUSY,
polling and scheduling consume the remaining budget. This is calculated capacity,
not a reproduced loss-free stream.

The rate formula alone does not identify which `rx_bw` encodings select each
mode/decimation pair. The local source-1 route byte `0x09` and moving-marker
captures support one interpretation on ROM 1.24, but do not establish the
entire table, analog filter, amplitude scale or sample-sequence continuity.
Verify those against positive/negative offset signals and a nonrepeating
sequence before changing the portable API or file format.

The TX result concerns **selector 1's frequency-deviation interpretation**. The
existing IQ playback/replay measurements used their own selectors, initialization
and waveforms. They are separate experiments. Do not replace the IQ replay rate
recommendation or call native 4-FSK an amplitude/phase I/Q transmitter.

## Reproduction order

1. Preserve board/module identity, ROM, cold/warm state, modem and exact command
   sequence. Keep portable source-2 behavior unchanged until comparison passes.
2. Run the same RF tone at matched analog bandwidth, gain and input level through
   both sources. Establish pair order, sign, scale, saturation and filter shape.
3. Resolve the disagreement on gain reset with the W12's exact RX command
   sequence, and read back gain if a matching register/API exists. Our
   pre-only/post-RX comparison did not show a reset on this LakeShark board.
4. Increase the FSK export rate in steps. Record raw byte counts, FIFO flags,
   occupancy and longest service gap. Verify sample ordering with a known
   sequence, including under UI/audio load and through FIFO wrap.
5. Compare reset-based recovery with warm reconfiguration. Require successful
   post-test reception and unchanged radio ownership after each trial.
6. Treat selector-1 scaling and native 4-FSK as separate bench experiments with
   an independent receiver. This change adds no TX command or automatic test.

The immediate opportunities are [GNSS and 2.4 GHz receive research](../gnss-wifi/README.md).
