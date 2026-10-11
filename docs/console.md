# Console interface

The host tool accepts the original capture console format. A board adapter
must implement these commands; the portable library does not install a shell.
These are finite-capture export commands. LakeShark's
[continuous P25 receiver](live-streaming.md) uses a separate live task path.
The included [ESP32-P4 console firmware](../firmware/esp32p4/README.md) uses
`capture` and `dump` for its FIFO exports.

## SRAM burst

`lora lrcap` emits optional metadata, 256 ordered words, then a result:

```text
lrcap-meta: bw=31250 sample_rate=125000 iq=1
lrcap: 000 0x12345678
...
lrcap: 255 0x12345678
lrcap: source=2 hz=910525000 status=0x03000000 bytes=1024 words=256 phase=readback result=ESP_OK restore=ESP_OK recovery=ESP_OK
```

The example omits rows 1..254. Set rate and iq to 0 for an unvalidated profile.
Words are formatted as host-order hexadecimal; the parser writes little-endian
bytes. All three result fields must be `ESP_OK`. These are protocol labels,
not an ESP-IDF dependency. Missing, duplicated or reordered words are rejected.

## FIFO window

`lora lrfifo 250 hex` captures up to 250 ms, then prints up to 64 bytes per row.
Offsets are decimal bytes. Preserve byte order in the hexadecimal payload.

```text
lrfifo-hex: 000000 01020304
lrfifo-crc32: B63CFBCD bytes=4 encoding=iq16le
lrfifo: bytes=4 max_level=4 polls=1 elapsed_us=10 flags=0x00 result=ESP_OK recovery=ESP_OK
```

This four-byte example is a format test, not a radio recording. CRC is IEEE
CRC-32: reflected polynomial `0xEDB88320`, initial and final XOR `0xFFFFFFFF`.
Length, CRC, contiguous offsets and successful radio results are required.
Keep frequency and sample rate in separate metadata.

The original firmware also accepts `lora lrfifo 250 hex -110 60000`: poll RSSI
for up to 60 seconds, then capture when it reaches -110 dBm. Threshold range is
-130..-20 dBm; wait range is 1..60000 ms. Configure RX first. This is a signal
level trigger and can miss the start of a transmission. It is outside the
portable capture API.
