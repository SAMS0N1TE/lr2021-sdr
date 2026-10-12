# Smooth complex-envelope TX verified

2026-10-12, LR2021 ROM 1.24. Follow-up to the [magnitude-route discovery](../tx-amplitude/).
The OOK-initialized `0x0293` selector-0 route transmits a waveform with
amplitude and phase changing on every input sample. This goes beyond the
earlier held-plateau measurements, while remaining a bounded narrowband test.

## Measured results

| Setup and waveform | Samples/s | Interior waveform EVM | Amplitude correlation |
| --- | ---: | ---: | ---: |
| OOK, varying amplitude and phase | 9,600 | 7.93% | 0.9950 |
| Same, independent repeat | 9,600 | 8.42% | 0.9950 |
| OOK, varying amplitude and phase | 4,800 | 8.19% | 0.9945 |
| OOK, amplitude only | 9,600 | 6.16% | 0.9958 |
| GFSK control, varying amplitude and phase | 9,600 | 39.31% | -0.0153 |

The GFSK control preserves phase reasonably but flattens the intended
amplitude variation. The OOK output spans approximately 16..48 coordinate
units after one common complex gain correction, tracking the intended range.
The combined waveform has about 5 degrees RMS phase error.

A constant I=48,Q=0 control has 2.32% residual error after carrier correction.
That includes the transmitter, propagation and receiver; it does not isolate
receiver noise. It is also measured at a higher average amplitude than the
varying signal, so do not subtract that percentage directly from waveform EVM.

All six transmissions were at 915 MHz, configured -9 dBm, with 64 complex
samples per burst. HackRF settings remained 2 Msps, LO 914.750 MHz, LNA 16,
VGA 32, amplifier off. All completed with TX FIFO flags `03`, zero radio
errors and no receiver ADC clipping. No PRAM patch was used.

## Exact inputs and measurement scope

Pattern 12 uses, for n=0..63:

```text
A(n)   = 32 + 16*cos(2*pi*n/32)
phi(n) = 2*pi*n/32 + 0.35*sin(4*pi*n/32)
I(n)   = round(A(n)*cos(phi(n)))
Q(n)   = round(A(n)*sin(phi(n)))
```

Pattern 13 uses the same amplitude with phase zero. Pattern 14 is constant
I=48,Q=0. Exact quantized bytes are in [v8-patterns.json](v8-patterns.json).
These are low-bandwidth test signals: the fundamental is 150 or 300 Hz,
with modulation harmonics. They do not establish usable TX bandwidth near
the input sample-rate limit.

The analysis frequency-translates the receiver capture and averages 32 raw
samples, producing 62.5 ksample/s complex data. It fits one timing offset,
one carrier offset and one complex gain against linear interpolation of
the quantized input. It uses the continuously varying interior approximately
covering input samples 10..54. There is no equalizer, DC subtraction,
per-block normalization or amplitude/phase correction.

EVM here includes changing samples and their intervening waveform, unlike
the previous settled-plateau metric. However, it excludes burst startup/end
and assumes a linearly interpolated reference. It is an experimental
waveform comparison, not a calibrated modulation-compliance measurement.
Timing, carrier and gain are fitted on the same window being scored.

Synthetic validation recovers the same waveform with added noise at 3.73%
EVM and detects a deliberately flattened amplitude at 39.62%. Exported
capture replay reproduces all six reported metrics to floating-point precision.

## Reproduction

The working OOK configuration sequence remains the one in the
[amplitude report](../tx-amplitude/#working-recipe). The archived
[diagnostic include](lr2021_tx_probe.inc) adds the three patterns in one build:

```text
lora lrtx-probe ookraw0 12 9600
lora lrtx-probe ookraw0 12 4800
lora lrtx-probe raw0 12 9600
lora lrtx-probe ookraw0 13 9600
lora lrtx-probe ookraw0 14 9600
```

Create an `rf` directory beside the analysis scripts, copy the lossless
`samples/*.iq8` excerpts into it, and run with Python and NumPy:

```text
python analyze_smooth.py v8-ook-smooth9600 9600 12
python analyze_smooth.py v8-gfsk-smooth9600 9600 12
python analyze_carrier.py v8-ook-carrier9600
python test_smooth.py
```

[Evidence](evidence/) includes filtered radio logs and numeric results.
[The manifest](samples/manifest.json) records original/excerpt hashes and
sample offsets. The v8 diagnostic remains installed; the board was left
idle at 915 MHz with zero errors. Original application/settings backups stay
private, and the diagnostic continues using its isolated settings namespace.

## Significance for LakeShark

The original RX work exposes actual receive I/Q. This TX result supplies a
candidate complementary path for finite I/Q transmission, making LakeShark's
LR2021 more programmable than a fixed packet-modem interface alone.

- Software-generated narrowband modulation and pulse shaping become plausible.
- Custom ASK/PSK and, if linearity permits, QAM experiments can be built on it.
- Known test signals could help validate receivers and protocol implementations.
- Receive-and-replay becomes a research target, provided sample rates,
  amplitude scale, filtering and sustained TX data flow are handled correctly.

These are development opportunities, not implemented LakeShark features.
The portable library remains RX-only. Continuous FIFO refill, useful maximum
bandwidth, spectral quality, calibrated linearity and payload decoding on
this magnitude-preserving route remain open. Simultaneous RX/TX has not been
demonstrated. The result does not make the LR2021 a wideband HackRF replacement.

The next useful milestones are a decoded payload using shaped complex samples,
repeatable amplitude/phase calibration, and sustained FIFO-fed transmission.
Continue tracing the OOK-specific setup/start state: changing F30910 alone
was insufficient, and the exact internal magnitude enable remains unresolved.
