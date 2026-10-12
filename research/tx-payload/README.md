# Working uploaded I/Q playback and decoded payloads

2026-10-12, LR2021 ROM 1.24, LakeShark ESP32-P4 board. TX now accepts an
uploaded I8/Q8 buffer and streams it through the radio FIFO during a bounded
transmission. Independent HackRF reception decodes a modulation that uses
both amplitude and phase, with payload CRC verification.

**The final v13 build passed 235 frames / 15,040 payload bits with zero bit
errors and every CRC valid.** These tests cover all six supported sample
rates, repeated frames, long buffers, and the host upload-and-send command.

[Using the working interface](../../docs/tx-playback.md) gives commands,
buffer limits, build integration and remaining scope.

## Final-build RF tests

| Test | Complex samples/s | Complete decoded frames | Payload errors | Result |
| --- | ---: | ---: | ---: | --- |
| Built-in frame | 4,800 | 1 | 0 | CRC valid |
| Host upload-and-send tool | 9,600 | 16 | 0 | All CRCs valid |
| Built-in frame | 19,200 | 1 | 0 | CRC valid |
| Large uploaded buffer, four repeats | 38,400 | 92 | 0 | All CRCs valid |
| Built-in frame | 76,800 | 1 | 0 | CRC valid |
| Repeated built-in frame | 125,000 | 16 | 0 | All CRCs valid |
| Independent repeat | 125,000 | 16 | 0 | All CRCs valid |
| Large uploaded buffer, four repeats | 125,000 | 92 | 0 | All CRCs valid |

The large upload is 23 complete frames (15,824 bytes), repeated four times:
**31,648 complex samples / 63,296 FIFO bytes in one run**. At 125 ksample/s,
the run used 513 refill writes, minimum observed refill-time FIFO level 325
bytes, and maximum observed poll gap 1,816 us. All bytes were queued and the
radio ended with an empty FIFO and no errors. At 38.4 ksample/s the same
data lasted about 0.826 seconds including command overhead.

Earlier v9/v11/v12 successful tests bring the total to 383 complete frames
and 24,512 payload bits with valid CRCs. They include the original 256-byte
FIFO, a full 16 KiB upload, and short-buffer wraparound. There is also **one
failed v12 125 ksample/s run**, retained in the evidence, which is excluded
from these successful-run totals.

## Why the high-rate test initially failed

The normal LakeShark SPI path was 2 MHz: 250,000 wire bytes/s before command
overhead. Raw TX at 125,000 complex samples/s requires 250,000 payload
bytes/s alone. A larger FIFO cannot fix that sustained throughput deficit.
The failed capture reported underflow (`txflags=27`), incomplete queuing
and a roughly 5 ms service gap. Its later decoded noise is not useful BER data.

The fix reuses the RX implementation's owned bus, polling transactions and
**16 MHz SPI**, plus RX's service priority of 18. TX restores the original
task priority and normal SPI mode after the run. Sticky FIFO errors are now
checked during service, causing an early abort. No radio patch was required.
The fixed high-rate runs passed, including the 92-frame uploaded buffer.

## FIFO and sample routing

The working OOK setup and selector-0 route remain those in the earlier
[amplitude report](../tx-amplitude/). For payload/upload runs, after radio
calibration/setup and before filling:

```text
F3002C <- 00804000   # extended TX FIFO SRAM base
F30034 <- 000003FC   # 1024-byte capacity encoding
```

The base reads back as `00004000` on this board, the SRAM alias behavior also
encountered during RX work. Both base representations are accepted; size is
checked exactly. Before each subsequent setup the mapping is again the
default `6228/FC`, confirming the previous radio recovery reset it.

The sender prefills up to 896 bytes, polls byte level directly, and replenishes
toward 896 when level reaches 768 or below. Writes are at most 128 bytes and
split at every input-buffer boundary. Direct level polling avoids the
extended-FIFO threshold-IRQ workaround. Final flags `07` include the sticky
high-water flag and are not an error; overflow/underflow bits are `10/20`.

The radio's selector-0 handler doubles N then truncates to 16 bits. The sender
therefore rejects more than 32,767 complex samples, as well as sample durations
over 900 ms. A hardware timeout and a local timeout bound each run. This is
streaming **into the radio FIFO during a finite transmission**, not indefinite
host-to-radio streaming or gapless chaining of multiple transmissions.

## The decoded waveform

The 8-APSK constellation has radius 24 or 48 and phase 45, 135, 225 or 315
degrees. Each symbol carries three bits, including one amplitude bit. Its
eight samples use four interpolated amplitude/shortest-phase steps followed
by four held samples, avoiding the earlier abrupt-reversal problem.

Each frame has 16 known training symbols followed by eight payload bytes,
CRC-16/CCITT-FALSE (polynomial 1021, initial FFFF), and one padding bit.
The original built-in payload is `LR2021IQ` / CRC `3A9B`; the uploaded payload
is `LAKESHAR` / CRC `36F8`. Exact samples are in [v9-frame.json](v9-frame.json)
and [uploaded-apsk.json](uploaded-apsk.json). The generator in `tools/` can
make another eight-byte payload without rebuilding firmware.

At 125 ksample/s this waveform has 15,625 symbols/s and 46,875 gross mapped
bits/s. Training, CRC and padding reduce actual payload throughput. This is
not evidence of a flat 125 kHz TX bandwidth.

## Independent checks

[decode_apsk.py](decode_apsk.py) fits timing, carrier offset and one complex
gain using **training only**. It then chooses the nearest constellation point,
reconstructs payload bytes, and checks CRC before calculating bit errors
against the expected payload. There is no payload-aided fit or blockwise
amplitude normalization. A synthetic corrupted payload produces CRC failure.

Create `rf` beside these scripts and copy `samples/*.iq8` into it:

```text
python decode_apsk.py v13-fast125000-r16 125000 16
python decode_apsk.py v13-upload125000-92frames 125000 92 uploaded-apsk.json
python test_apsk.py
```

The excerpts retain the original signed IQ8 samples without resampling.
[The manifest](samples/manifest.json) includes original/excerpt SHA-256 hashes
and exact sample offsets. Every successful capture reproduces identical
decoded payloads and CRCs when replayed from the excerpt. Failed-run noise
decisions can change with crop-dependent detection; that does not affect its
underflow classification. [Summary](evidence/summary.json) and
[replay checks](evidence/replay-check.json) are machine-readable.

All measurements used 915 MHz, configured -9 dBm; HackRF received at
914.750 MHz, 2 Msps, LNA 16, VGA 32, amplifier off. No ADC clipping was found.
The decoder mixes down and averages to 62.5 ksample/s. For very high sample
rates this is a symbol decoder, not a complete waveform-fidelity measurement.

Seven host test suites pass, including existing RX tests, upload integrity,
FIFO wraparound and failed-write handling. Hardware also rejected a bad upload
CRC and refused to transmit the uncommitted buffer. The standalone host tool's
upload-and-send path was captured and decoded independently.

## Source and installed state

The three `.inc` files here are the final installed diagnostic implementation.
[The integration patch](lakeshark-integration.patch) adds console dispatch and
isolates the five diagnostic settings users in `sdr-txlab`. Firmware/NVS
backups and unrelated device logs stay private. The application remains
installed; the board was left idle, with zero radio errors and a valid
688-byte uploaded frame ready for another explicit send.

This delivers usable bounded I/Q playback on the tested board. Indefinite
streaming, a GUI, a portable TX HAL, calibrated linearity, other RF settings,
and a broader spectral characterization are not implemented or established.
