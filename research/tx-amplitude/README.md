# TX magnitude path found through OOK setup

Measured on LR2021 ROM 1.24, 2026-10-12. The earlier constant-envelope
result was specific to GFSK setup. **OOK setup followed by raw selector 0
transmits both coordinate magnitude and phase**, with distortion and a
limited useful magnitude range. This is finite, experimental IQ playback;
continuous streaming and full waveform fidelity remain unverified.

## Why revisit setup?

The RX discovery succeeded by tracing the ROM calibration source routing,
then validating actual signed I/Q with known signals. We applied the same
method to TX: trace documented magnitude controls into ROM, compare source
configuration, and measure known samples without normalizing away amplitude.
No PRAM patch is needed for either discovery.

The OOK `0x0281` handler at `F4F4` calls `B8C0`, which calls `B7E4` for
the documented magnitude-depth argument. That routine changes the low and
third bytes of `F30910`. `0x0293` changes sample routing without overwriting
those fields. This suggested the experiment; it did not prove the mechanism.

An isolation test disproved the simplest explanation: changing only those
fields from `000040C0` to `00004000` after GFSK setup still gives a constant
envelope. **Use the full OOK setup below; do not treat F30910 as a proven IQ
enable switch.** Other setup state or TX-start behavior matters.

## Measured results

All tests: 915 MHz, configured -9 dBm, 64 complex samples. HackRF received at
914.750 MHz, 2 Msps, LNA 16, VGA 32, amplifier off, fixed gain. All 13 captures
completed with TX FIFO flags `03`, zero radio errors, and no receiver ADC
clipping. Packet control lasted about 57 ms; raw bursts about 7 or 14 ms.

| Setup / input | Result |
| --- | --- |
| GFSK, I=16..112, Q=0 | Amplitude max/min 1.005 for a 7:1 input range |
| OOK full depth, same input, 9,600 samples/s | Amplitude rises through I=64, then saturates |
| OOK limited depth, same input | Similar magnitude response and saturation |
| OOK full depth, same input, 4,800 samples/s | Reproduces magnitude response |
| GFSK plus F30910 byte-field change | Still flat: max/min 1.010 |
| OOK radius-64 circle, 45-degree steps | Relative phase within 1.29 degrees of expected |
| OOK simultaneous magnitude/phase staircase, 4,800 samples/s | 5.60% settled-plateau EVM |
| Same staircase, 9,600 samples/s | 7.70%; independent repeat 7.14% |
| GFSK plus F30910 change, same staircase, 4,800 | 53.76% plateau EVM; amplitude remains flat |

For the OOK positive-I staircase at 9,600 samples/s:

| Input I | 16 | 32 | 48 | 64 | 80 | 96 | 112 | 32 |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| Receiver RMS | .01833 | .03421 | .05037 | .06627 | .06599 | .06644 | .06715 | .03417 |

Receiver units are IQ8/128 after a fixed frequency translation and averaging
filter, with estimated noise power removed. They are not calibrated watts.
The ordinary OOK control has a 23.48 dB ratio between its envelope quartiles,
confirming that the receive/analysis chain distinguishes amplitude levels.

The combined staircase uses exact quantized I/Q pairs in
[v7-patterns.json](v7-patterns.json), radii approximately 8..56 and successive
45-degree phases, held eight samples each. Amplitude correlations are
0.9982..0.9989. Low-radius points have phase errors up to about 25 degrees
after a common gain/CFO correction. This is a real magnitude path, but not
yet calibrated linear IQ. Do not scale signed bytes to a radius of 127 and
expect proportional output: saturation starts around radius 64 in these tests.

## Working recipe

1. Stop competing sessions, use standby, and select OOK packet type `0x0A`.
2. Configure 915 MHz and documented OOK modulation `0x0281`: bitrate 4800
   or 9600, pulse shape 0, RX filter 100 kHz, magnitude depth 0. Depth 1 was
   also tested for the positive-I staircase.
3. Configure fixed OOK packet length 128 bytes, preamble 32, no CRC or
   Manchester coding; configure no sync word. Apply the existing board's
   DCDC workaround, LF PA -9 dBm and standby-RC fallback.
4. Send `0x0293` with `[00,40, rate_BE32, 00]` for 64 signed I8/Q8 pairs.
5. Clear TX FIFO, IRQ and FIFO flags; upload exactly 128 bytes.
6. Start `0x020D` with a 150 ms timeout. Check TX_DONE, FIFO flags and radio
   errors; stop and recover the radio after the bounded experiment.

The archived [diagnostic include](lr2021_tx_probe.inc) contains the actual
sequence and fixed test inputs. Examples on that LakeShark diagnostic:

```text
lora lrtx-probe ookpacket 9 9600
lora lrtx-probe raw0 4 9600
lora lrtx-probe ookraw0 4 9600
lora lrtx-probe ook20raw0 4 9600
lora lrtx-probe ookraw0 10 4800
lora lrtx-probe ookraw0 10 9600
lora lrtx-probe magraw0 4 9600
```

`magraw0` is the failed register-only isolation experiment, not the working
recipe. This include is not a standalone firmware or a portable library API.
The diagnostic retains the `sdr-txlab` settings isolation introduced in v5.
The v7 image remains installed; the board was left idle with zero errors.

## Evidence and analysis

[Evidence](evidence/) contains filtered console output, register snapshots
before configuration / after configuration / after TX start, and measurements.
[Samples](samples/) contains lossless interleaved signed-IQ8 RF excerpts.
The manifest records hashes of both complete original recordings and excerpts,
with exact original sample offsets. Only unrelated surrounding recording time
was removed. Private board settings, firmware backups and unrelated logs are
not published.

To replay with Python and NumPy, create `rf` beside these scripts and copy
the `samples/*.iq8` files into it, then run:

```text
python analyze_envelope.py v6-ook-amplitude9600 9600 4
python analyze_complex_envelope.py v7-ook-spiral9600 9600 10
python test_complex_envelope.py
```

The analyzer applies one common carrier-offset correction and one common
complex gain to all plateaus. It does not normalize individual samples or
blocks. Carrier offset is estimated within settled plateaus, weighted by
received amplitude, before comparing against the known input. The synthetic
check recovers linear IQ at 0.55% plateau EVM and detects amplitude removal
at 53.45%. Cropped replay changes the median noise estimate and therefore
the threshold-derived window position. In the exported replay checks, this
changed plateau EVM by up to 0.62 percentage points. Treat these measurements
as approximate rather than calibrated EVM. [Replay differences](evidence/replay-check.json)
are recorded explicitly. These figures exclude transitions and are
**not full-waveform EVM**.

## Next amplitude work

Trace the remaining OOK-specific TX-start state, characterize AM/AM and AM/PM
response below radius 64, and measure transitions with a smooth signal whose
amplitude and phase both vary. Calibrate only after separating repeatable
radio distortion from receiver noise and drift. Preserve absolute relative
amplitude throughout analysis, as in the RX investigation.
