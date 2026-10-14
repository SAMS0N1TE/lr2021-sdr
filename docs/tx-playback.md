# Using experimental I/Q TX in LakeShark

The tested LakeShark build can upload and transmit signed interleaved I8/Q8
buffers on LR2021 ROM 1.24. It uses OOK initialization, raw command `0x0293`,
a 1 KiB TX FIFO and the same owned 16 MHz SPI path used by raw RX.
The portable RX library and standalone ESP32-P4 RX firmware do not provide
these commands; use the LakeShark integration described below.

## Send a generated frame

With the TX-enabled LakeShark firmware running, stop competing sessions in
its console:

```text
tui off
mesh stop
lora config 915 7 62.5 5 -9
```

Install Python's `pyserial` package, then run from this repository:

```text
python tools/lr2021_make_apsk.py LAKESHAR frame.iq8
python tools/lr2021_tx_upload.py frame.iq8 --serial YOUR_USB_SERIAL --send 9600 --repeats 16
```

The first command creates an eight-byte payload with CRC-16 and a shaped
8-APSK waveform. The second uploads it, verifies a whole-buffer CRC-32,
then transmits. Omit `--send` to upload without transmitting. `--port COM19`
or a device path can replace serial-number selection. The uploader keeps
DTR/RTS low and does not reset the board.

After upload, the console can send the same buffer again:

```text
lora lrtx-send 125000 4
lora lrtx-upload status
lora status
```

RF is fixed at **915 MHz, configured -9 dBm** in this experimental interface.
Successful transmission reports `result=ESP_OK`, successful stop/recovery,
zero radio errors and `txlevel=0`. Final FIFO flags `03` or `07` are normal;
bits `10`/`20` mean overflow/underflow and invalidate the transmission.

## P25 Phase 1 test-tone example

P25 voice-data transmission is now RF-verified using the existing firmware:

```text
python tools/lr2021_make_p25.py research/tx-p25/inputs/voice-tone.p25 p25.iq8
python tools/lr2021_tx_upload.py p25.iq8 --serial YOUR_USB_SERIAL --send 19200 --repeats 2
```

Requires NumPy. The example transmits pre-encoded LDU1/LDU2/TDU test-tone
frames at the fixed 915 MHz test frequency. Two RF captures recovered 72 IMBE
frames matching the reference; stock DSD-neo file playback reconstructed the
1,011 Hz tone. [Evidence, decoded WAV and offline replay](../research/tx-p25/).

## Existing sensor packet example

A second generator produces synthetic Acurite-606TX temperature packets,
independently decoded from HackRF recordings by stock rtl_433 25.12:

```text
python tools/lr2021_make_temperature.py temperature.iq8 --temperature 23.4 --id 90
python tools/lr2021_tx_upload.py temperature.iq8 --serial YOUR_USB_SERIAL --send 9600
```

Use the same console preflight above.
[Three verified readings, negative controls and replay](../research/tx-temperature/).

## Supported buffer contract

| Property | Current bound |
| --- | --- |
| Format | Interleaved signed 8-bit I, Q; no header |
| Upload size | 2..16,384 bytes, even length |
| Coordinate magnitude | `I*I + Q*Q <= 56*56` |
| Complex sample rate | 4,800; 9,600; 19,200; 38,400; 76,800; 125,000 samples/s |
| Repetition | 1..16 copies of the uploaded buffer |
| Total complex samples | At most 32,767, including repetitions |
| Sample duration | At most 900 ms; hardware/local timeout adds 50 ms |

The magnitude limit stays below the observed saturation near radius 64.
Do not normalize each sample to a constant radius: that destroys the
amplitude information this path now preserves. Quantize/resample complete
waveforms with one appropriate scale. RX IQ16 cannot simply be copied into
this I8/Q8 interface.

The ROM doubles the sample count and stores a 16-bit byte count for selector
0; the 32,767-sample limit prevents wraparound. The upload can be repeated
without allocating a second full buffer. FIFO writes wrap at the exact
buffer boundary, including during initial prefill.

A run holds exclusive radio ownership, stops on FIFO error or timeout, then
resets/reconfigures the radio. Fast SPI ownership and task priority are
released/restored. The firmware application stays installed. Sending again
starts a separate burst; it is not a gapless continuation.

## Sample rate and frequency findings

Measured at 433.92 MHz on the LF path, configured -9 dBm, with a HackRF
receiving. Earlier sections were measured at 915 MHz; the LF path at 433.92 MHz
behaves the same for OOK and works for FSK at the rates below.

| Rate (samples/s) | 2-FSK, 4,800 baud, +/-20 kHz | OOK, 400/1,200 us pulses |
| --- | --- | --- |
| 125,000 | Correct: 20.1 kHz deviation, frames decode | On/off kept |
| 76,800 | Half deviation (about 10.5 kHz), frames still decode | Not tested |
| 38,400 | Frequency sense inverted (bits flip) | Not tested |
| 19,200 | Not tested | Correct: 418/1,199 us, frames decode |
| 9,600 | Not tested | Long pulses short by about one sample period (1,148 us) |

- Send frequency-modulated waveforms at 125,000 samples/s.
- Send OOK at 19,200 samples/s with a constant on level (about 48) and exact zeros
  when off.

## Replaying received captures

Raw IQ16 captures from the same chip at 125,000 samples/s can be sent back
through this path after conversion:

- OOK: slice the envelope, drop glitches shorter than 100 us or below half the
  median pulse level, then rebuild clean on/off samples at 19,200 samples/s. A
  CC1101 receiver (Flipper Zero, stock Princeton decoder) decoded the replayed
  keys.
- FSK: center the carrier on the midpoint of the two tone clusters. A median or
  power-weighted estimate is biased when one tone is stronger (a 9 dB imbalance
  was seen), and the low-pass then cuts the far tone. Keep 125,000 samples/s.
- Telling them apart: count tone changes inside bursts, at least 100 us away
  from envelope edges. FSK changes tone mid-burst; OOK does not. Transmitters
  such as the CC1101 add a short frequency transient at each turn-on, which
  looks like a second tone near the edges.

## Measuring TX with an SDR

Tune the measuring SDR away from the carrier, for example 200 kHz below. With a
HackRF tuned exactly on the carrier, OOK plateaus faded and only pulse edges
remained, because of its baseband DC high-pass. The same transmission decoded
cleanly with the offset.

## Integrity and decoding

Uploads are sequential, acknowledged 64-byte chunks. An incomplete upload,
CRC mismatch, or out-of-range coordinate cannot be committed for playback.
No RF is emitted by upload commands. A new upload invalidates the previous
ready state; reboot loses the RAM buffer. A completed transmission retains
the ready buffer for reuse.

The demo's two amplitude rings and four phase positions carry three bits
per symbol. Four intermediate phase/amplitude steps precede four held
samples. The frame includes 16 training symbols, eight payload bytes and
CRC-16/CCITT-FALSE. A receiver calibrates timing, carrier offset and one
complex gain from training, slices payload symbols, and checks CRC before
comparing against expected bytes. It does not use the payload to fit the RF.

[RF evidence, decoder and exact inputs](../research/tx-payload/) include both
the working runs and the earlier underflow that motivated fast SPI.

## Integrating the source

Copy these three files from `research/tx-payload/` into LakeShark's
`components/lakeshark/board/`:

- `lr2021_tx_probe.inc`
- `lr2021_tx_upload.inc`
- `lr2021_tx_fifo.inc`

Apply [lakeshark-integration.patch](../research/tx-payload/lakeshark-integration.patch)
to the matching LakeShark checkout. It adds the include and console dispatch
and isolates the diagnostic's five settings-namespace users as `sdr-txlab`.
Check its contexts against your checkout; this is a board integration,
not a drop-in generic HAL. The tested ESP32-P4 build uses ESP-IDF 5.5.4 and
the existing RX fast-SPI implementation. `ESP_IDF_VERSION` for its WiFi-remote
Kconfig environment is `5.5`, not the three-part version string.

Host transaction tests cover upload integrity, malformed/out-of-order
chunks, amplitude bounds, repeated-buffer FIFO boundaries, and failed writes.
The existing RX tests remain enabled.

## Remaining scope

**Bounded I/Q playback and FIFO-fed transmission are working.** Remaining
work includes continuous host streaming, gapless burst chaining, microphone
voice encoding, P25 HDU interoperability, and simultaneous RX/TX testing.
Characterization work covers TX bandwidth, linearity, spectral emissions,
other frequencies/powers and other boards. The earlier research records
distortion at abrupt phase reversals and very small amplitudes.
