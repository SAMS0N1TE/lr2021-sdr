# Phase-ramp experiment — 2026-10-12

A paired experiment on LR2021 ROM 1.24 at 915 MHz, configured -9 dBm.
HackRF: LO 914.750 MHz, 2 MS/s, amp/antenna-power off, LNA 16, VGA 32.
64 IQ8 coordinate pairs were supplied through command `0x0293`, selector 0.

Pattern 6 jumps directly between phase targets 0, 170, 0, 175, 0, 179, 0,
180 degrees, holding each for eight samples. Pattern 7 reaches the same
endpoint through four nominal equal angular increments, then holds it for
four more samples. Coordinate magnitude is nominally 64, rounded to signed
integer I/Q; `v5-patterns.json` contains every exact pair.

## Measured endpoint changes

| Capture | Sample rate | Largest absolute error across seven endpoint changes |
| --- | ---: | ---: |
| v5-direct19200 | 19200 | 88.98 degrees |
| v5-direct4800 | 4800 | 88.21 degrees |
| v5-direct9600 | 9600 | 87.04 degrees |
| v5-ramped19200 | 19200 | 3.26 degrees |
| v5-ramped4800-repeat | 4800 | 1.45 degrees |
| v5-ramped4800 | 4800 | 1.59 degrees |
| v5-ramped9600 | 9600 | 3.09 degrees |

Ramps removed the large reversal errors in these short captures. No FIFO
underflow, radio errors or receiver clipping occurred. This supports smooth
phase paths as a practical workaround; it does not identify the internal
cause or prove arbitrary IQ fidelity. Exact 180-degree behavior was not
monotonic with nearby reversal angles, so a simple universal angle threshold
is not established. Later [OOK setup measurements](../tx-amplitude/) established amplitude control.

The analyzer chooses timing from plateau flatness without using desired phase.
It estimates common carrier offset on the final two samples of each eight-sample
block, then compares adjacent plateau phases with the exact quantized input
endpoints. Results are endpoint differences, not calibrated EVM, BER, transition
shape, or spectral-mask measurements. Carrier-fit bias can shift errors by a
few degrees; inspect the saved fit RMS and carrier estimate.

## Reproduce from actual RF samples

```sh
mkdir rf
cp samples/v5-ramped4800.iq8 rf/
python analyze_reversals.py v5-ramped4800 4800 7
```

Requires Python and NumPy. Excerpts are lossless, include the full burst and
noise margins, and have boundaries aligned to eight receiver samples.
The manifest records full-recording and excerpt hashes plus original offsets.
Add the offset / 2000000 to cropped timestamps to compare with saved evidence.

## Diagnostic build

`lr2021_tx_probe.inc` is a LakeShark driver include, not standalone firmware.
Pattern 6 is direct; pattern 7 is ramped. Rate arguments: 4800/9600/19200.
The enclosing driver provides SPI, ownership, power setup and reset recovery.

`settings-isolation.patch` redirects all five `sdr-tool` namespace users in
the local LakeShark diagnostic build to `sdr-txlab`. This avoids running the
older diagnostic schema migration on the original namespace. NVS was backed
up before installation; read-only before/after comparison verified the three
active original keys were unchanged. The public check summary contains no NVS
values. Raw NVS backups must remain private. This is a lab-build patch, not a
normal firmware migration. Diagnostic firmware remains installed for further
research; the radio was left idle at 915 MHz and -9 dBm with mesh stopped.
