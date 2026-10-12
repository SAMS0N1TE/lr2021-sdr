# Existing sensor packet format decoded by stock rtl_433

Measured 2026-10-12 on LR2021 ROM 1.24, with the existing v13 LakeShark TX
firmware. **Three different synthetic temperature readings were transmitted
as uploaded I8/Q8 and decoded from independent HackRF recordings by unmodified
rtl_433 25.12.** No firmware rebuild, flash or decoder modification was needed.

This moves beyond the earlier custom 8-APSK frame: the packet layout and
decoder already existed outside this project. OOK pulse-position modulation
also depends on the amplitude-preserving TX route; constant-envelope playback
would erase its on/off information.

## Results

| Capture | TX sample rate | I amplitude | Repeated rows | Stock decoder result |
| --- | ---: | ---: | ---: | --- |
| `v13-temperature23` | 9,600 | 48 | 6 | ID 90, channel 1, battery OK, **23.4 C**, checksum valid |
| `v13-freezer-neg12` | 4,800 | 48 | 6 | ID 167, channel 3, low battery, **-12.3 C**, checksum valid |
| `v13-temperature37-a48` | 19,200 | 48 | 3 | ID 49, channel 2, battery OK, **37.8 C**, checksum valid |
| `v13-temperature-badcrc` | 9,600 | 48 | 6 | Deliberately incorrect checksum: **no accepted reading** |
| `v13-temperature37` | 19,200 | 24 | 3 | Half-amplitude acquisition failure: **no accepted reading** |

Each successful burst produces one decoded reading from repeated packet rows;
the result is **three accepted readings**.
All five transmissions completed with zero radio errors, no FIFO overflow or
underflow, empty final TX FIFO, and successful stop/recovery. Bursts lasted
approximately 0.38-0.79 seconds. The historical `badcrc` filename refers to the
sensor's LFSR checksum. Independent generic
pulse slicing recovered all six deliberately invalid rows as `5a80ea8f0`
(the valid checksum byte is `8e`). Thus the rejected control contained the
intended bits rather than simply failing to produce a detectable packet.
See [raw pulse output](evidence/bad-checksum-pulses.txt).

The lower-amplitude case failed acquisition. With a
global gain of four applied offline, pulse analysis recovered two complete
rows and an incomplete first row; protocol 55 requires at least three matching
rows. The same packet at amplitude 48 decoded with default receiver settings.


## What this demonstrates

LakeShark can act as a short-burst sensor-packet test source: choose an ID,
temperature, channel and battery flag, generate the waveform, and transmit it
through raw I/Q. A separate software receiver reads the intended fields and
checks integrity without knowing the expected payload.

The test readings were generated in software and transmitted at **915 MHz,
configured -9 dBm**, with HackRF receiving. This OOK result complements the
earlier amplitude-and-phase 8-APSK evidence.

## Generate and transmit

Use the TX-enabled LakeShark integration and console preflight in
[TX playback](../../docs/tx-playback.md). From the repository root:

```text
python tools/lr2021_make_temperature.py temperature.iq8 --temperature 23.4 --id 90
python tools/lr2021_tx_upload.py temperature.iq8 --serial YOUR_USB_SERIAL --send 9600
```

The generator defaults to six repeated rows at 9,600 complex samples/s,
with I alternating between 0 and 48 and Q remaining zero. It writes a JSON
sidecar with the exact packet bytes, sample rate and duration. Use the same
rate for generation and playback. A 19,200-sample/s burst needs `--rows 3`
to fit the 16 KiB upload buffer. `--corrupt-checksum` produces the rejection
control. RF is emitted only by the uploader's explicit `--send` option.

## Independent decoder and replay

Install NumPy and obtain the unmodified Windows executable from the official
[rtl_433 25.12 release](https://github.com/merbanan/rtl_433/releases/tag/25.12).
The tested executable is `rtl_433.exe` from `rtl_433-win-msvc-x64-25.12.zip`.
Its SHA-256 is
`7a72a4b0b282e52e1444686c7ff379e5ce4bdeeeaa5183e686f3f5d0d4516bd6`.

```text
python research/tx-temperature/replay.py --rtl433 PATH_TO/rtl_433.exe
```

For a single recording:

```text
python tools/lr2021_decode_temperature_capture.py research/tx-temperature/samples/v13-temperature23.iq8.gz --rtl433 PATH_TO/rtl_433.exe
```

The archived recordings are complete three-second HackRF captures, losslessly
gzip-compressed. Each contains signed I8/Q8 at 2 Msamples/s, receiver LO
914.750 MHz, LNA gain 16 dB, VGA gain 32 dB, RF amplifier off. The replay tool
performs a fixed -250 kHz frequency shift and eight-sample mean/decimation to
250 ksample/s, then feeds complex float32 to stock `rtl_433 -R 55 -F json`.
There is no amplitude normalization, pulse repair, payload fitting or custom
packet decoder. The replay assertion compares fields only after rtl_433 has
returned its own checksum-validated output.

[manifest.json](manifest.json) records capture hashes and expected outcomes.
[inputs](inputs/) contains the exact transmitted bytes and generator metadata.
[evidence](evidence/) contains decoder output and radio completion reports.

The packet field layout, pulse timing and integrity algorithm are documented
in upstream [acurite.c at 25.12](https://github.com/merbanan/rtl_433/blob/25.12/src/devices/acurite.c),
with the checksum primitive in
[bit_util.c](https://github.com/merbanan/rtl_433/blob/25.12/src/bit_util.c).
The independent decoder requires repeated rows and validates the LFSR checksum.
