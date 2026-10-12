# TX discovery: FIFO-fed phase and frequency modulation

Measured 2026-10-12. [Evidence and diagnostic source](../research/tx-2026-10-12/).

## Result

The undocumented 0x0293 command provides working FIFO-fed TX sample input
on this LR2021 ROM 1.24 board. Actual 915 MHz transmissions were independently
captured by HackRF. HackRF only received; MMX was not used.

Selector 0 consumes two signed bytes per sample, consistent with I8 then Q8.
RF phase follows the coordinate angle, but amplitude did NOT follow input
magnitude in the tested setup. Selector 1 consumes signed frequency samples.
Selector 2 consumes signed samples with a frequency scale proportional to
sample rate, consistent with phase increments.

This establishes finite raw modulation, not amplitude-preserving SDR-style
IQ playback or continuous streaming. All diagnostic emissions were short bursts at
915 MHz, configured -9 dBm. No PRAM patch was needed.

## Reproducible Command Format

0x0293 arguments are seven bytes:
  [N_hi,N_lo,rate_31:24,rate_23:16,rate_15:8,rate_7:0,selector]
N and rate are big-endian. Tested N=64 and rates 9600 and 4800 samples/s.

Starting from ordinary GFSK setup, configure 915 MHz, pulse shape 0, GFSK
deviation 10000 Hz, RX BW 100 kHz, LF PA -9 dBm, fallback standby RC.
The diagnostic establishes fixed packet/sync settings before 0x0293, clears
TX FIFO and IRQs, and uploads 128 bytes for selector 0 or 64 for selector 1/2.
Start TX with 0x020D arguments 00 12 C0 (150 ms hardware timeout).
Poll TX_DONE; explicitly stop, reset/recover LR, and restore LoRa settings.

Implementation: lr2021_tx_probe.inc (LakeShark integration, not standalone firmware). The source
is also present in LakeShark components/lakeshark/board/.
CLI: lora lrtx-probe raw0|raw1|raw2 [pattern] [4800|9600|19200]
config0/config1/config2 do configuration only. cw and packet are controls.
19200 is accepted by the diagnostic but was NOT RF-tested.
Patterns: 0=random xorshift, 1=inverted bytes, 2=held signed levels,
3=I/Q circle (raw0 only), 4=I-only amplitude levels (raw0 only).
Inspect FIFO flags as well as result=ESP_OK: command success alone does not
rule out underflow. CW's nominal local 150 ms stop measured 151061 us.

## Rf Measurements

Controls:
- Original-firmware LoRa packet: about 33 dB above measured band floor;
  console TX complete. rf/baseline-lora.*
- CW: approximately 151 ms RF; all 64 FIFO bytes remained, no radio errors.
- FSK packet: 512/512 payload bits recovered with zero errors, about 61 ms
  RF matching 576 bits including preamble/sync at 9600 bit/s. TX_DONE,
  FIFO empty, no underflow/errors. rf/probe-packet0.analysis.json.
- Decoder passed synthetic IQ payload, frequency-offset, and timing checks.

Selector 1:
Held values -96,-64,-32,0,32,64,96,0, eight samples per plateau.
9600 samples/s: 156.2868 Hz/count, correlation 0.99999965.
4800 samples/s: 156.2714 Hz/count, correlation 0.99999997.
Consistent with signed_sample * deviation/64 at deviation=10000 Hz.
Changing deviation was not tested, so that formula remains an inference.
Independent random and inverted payloads correlate with signed bytes at
approximately 0.9976 or better. See rf/v2-raw1-steps*.steps.json and
the console and measurement summaries in the evidence directory.

Selector 2:
9600 samples/s: 75.0091 Hz/count, correlation 0.99999933.
4800 samples/s: 37.6179 Hz/count, correlation 0.99999921.
Consistent with signed_sample * sample_rate/128 Hz, equivalently a phase
increment signed_sample * 2*pi/128 radians/sample. This is experimentally
supported interpretation, not an official documented format.
See rf/v2-raw2-steps*.steps.json.

Selector 0:
Sent (64,0),(45,45),(0,64),(-45,45),(-64,0),(-45,-45),(0,-64),(45,-45),
each repeated eight times. After carrier-offset correction, measured phase
relative to the first plateau was:
  0.00,45.00,90.01,134.63,179.08,-135.91,-91.88,-47.58 degrees.
This strongly supports signed interleaved I8,Q8 coordinate input for phase.
Absolute RF phase is not established by this receiver-relative measurement.
See rf/v3-raw0-circle.phase.json.

Amplitude test: I=16,32,48,64,80,96,112,32 and Q=0. Received amplitude stayed
about 0.0671..0.0676 full scale despite the 7:1 input range. The tested output
is effectively constant envelope. Do not claim general linear complex-IQ
transmission. See rf/v3-raw0-amplitude.phase.json.

Random I/Q pairs fit less cleanly: phase-model coherence 0.723 at 9600 and
0.874 at 4800, phase RMS 44.6 and 29.6 degrees with a simple midpoint model.
Filtering, transitions and timing need further characterization. Conjugated
IQ and direct 16-bit phase models fit substantially worse. See
rf/iq-input-models.json and rf/v3-raw0-random4800.models.json.

## Fifo / Length

First selector-0 trial loaded only 64 bytes for N=64: FIFO flags 23 included
UNDERFLOW bit5. This is retained as negative evidence, not a complete success.
Loading 128 bytes removed underflow. Selectors 1/2 consumed 64 bytes.
All complete subsequent trials reported TX_DONE, level0, flags03 (empty/low
threshold), errors0000, final_errors0000, and successful stop/recovery.
64-sample bursts measured about 6.9 ms at 9600 and 13.6 ms at 4800 including
envelope edges. Console elapsed times include SPI/status work and are longer.

## Rom Cross-Check

0x0293 -> FA78 -> D1A8. N/rate/selector saved at 800578/57C/580; helper1AE8
programs rate. Config-only reads match the ROM trace and persist through TX:
 selector0 F30800=00100801 F30838=00020080 F30904=00103200
 selector1 F30800=00100401 F30838=00020040 F30904=00103210
 selector2 F30800=00100401 F30838=00020040 F30904=00103220
F30838 contains 2*N bytes for selector0 and N for selector1/2.
F30930=63682AAA at9600; 63D05555 at4800.
Eight live64-byte TX-relevant ROM blocks matched the saved128KiB ROM dump.
The original ROM trace is retained in the local investigation archive.

## Capture Setup

LR2021 ROM 1.24 on ESP32-P4; HackRF One receiver, LO914.750MHz, 2MS/s,
amp and antenna power off, LNA16dB. Initial VGA16dB; v2/v3 VGA32dB.
Over-air US-band antennas. No calibrated output-power claim. Original board
firmware was restored and flash hash verified after testing.

## Limits / Next Work

Continuous FIFO refill, sustained TX, maximum rate, filter behavior, abrupt
phase fidelity, zero-IQ semantics, alternative amplitude controls, packet-type
dependencies, other ROM versions and other bands remain uncharacterized.
The verified path supports developing finite custom constant-envelope
waveforms, with explicit timeout/FIFO checking/stop/reset recovery retained.
