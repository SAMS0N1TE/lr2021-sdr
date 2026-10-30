# Bridge protocol, version 1

[Guide hub](README.md) / Bridge protocol

The bridge protocol carries LR2021 receive samples, controls and transmit
bursts between a host and a board that owns the radio. It runs over any byte
stream: a USB serial port (the LilyGO CH343 USB-UART, or ESP32-P4 USB
Serial/JTAG), a raw UART, or TCP. It was designed for this repository; the C
codec is [`include/lr2021_bridge.h`](../include/lr2021_bridge.h) and the
portable device side is
[`include/lr2021_bridge_server.h`](../include/lr2021_bridge_server.h).

The same server code runs on the simulator (`tools/lr2021_bridge_sim.c`) and
behind the local Linux backend, so host tests exercise the device logic.

## Frames

All integers are little endian.

| Offset | Size | Field | Notes |
| ---: | ---: | --- | --- |
| 0 | 2 | sync | `4C 52` (`"LR"`) |
| 2 | 1 | version | 1 |
| 3 | 1 | type | see below |
| 4 | 2 | sequence | requests: host-chosen; responses echo it; device messages count per type |
| 6 | 2 | length | payload bytes, 0..2048 |
| 8 | length | payload | |
| 8+length | 4 | CRC-32 | IEEE: reflected `0xEDB88320`, initial and final XOR `0xFFFFFFFF`, over bytes 2..7+length |

A receiver scans for `4C 52 01`, reads the length, waits for the whole frame
and checks the CRC. On a bad version, an oversized length or a CRC mismatch it
discards one byte and scans again. Nothing else is needed to resynchronize a
serial line after noise, a reset banner or console text.

## Requests and responses

The response type is the request type with bit 7 set (`0x81` for `HELLO`).
Every response payload starts with an `i32 status`.

| Type | Request | Payload | Response after status | Context |
| ---: | --- | --- | --- | --- |
| `01` | HELLO | `u16 host_version` | rest of the hello record | reader |
| `02` | SET_RX | RX record (17 bytes, or 11 for source 2) | `u16 radio_errors, u32 rate_hz` | worker |
| `03` | RX_START | `u16 fifo_capacity, u32 duration_ms`, optional `u8 format` | `u32 generation, u32 rate_hz` | worker |
| `04` | RX_STOP | empty | none | reader |
| `05` | SET_GAIN | `u8 step, u8 boost` | `u16 radio_errors, u32 rate_hz` | reader while streaming, else worker; while streaming only the step may change (EBUSY otherwise) |
| `06` | TX_BEGIN | TX record (26 bytes) | `u32 next_offset` (0) | reader |
| `07` | TX_DATA | `u32 offset`, 2..1024 even bytes | `u32 next_offset` | reader |
| `08` | TX_COMMIT | empty | none; the burst result follows as TX_END | worker |
| `09` | GET_TIME | empty | `u64 device_time_us` | reader |
| `0A` | BYE | empty | none; the device then leaves bridge mode | reader |
| `0B` | GET_GAIN | empty | gain record (14 bytes) | worker when idle; reader while streaming, from the stream's latest sample (EBUSY before the first one) |

The device handles one worker request at a time. A worker request that arrives
while another runs, including any worker request during a receive stream, is
answered with `EBUSY` straight away. The host must stop a stream with
`RX_STOP` and wait for its `RX_END` before retuning or transmitting.

`duration_ms` 0 streams until `RX_STOP`; otherwise 1..60000. `fifo_capacity` is
256 or 1024; hosts should use 1024 (the extended FIFO, see
[extended FIFO results](extended-fifo.md)).

### Status codes

| Code | Name | Meaning |
| ---: | --- | --- |
| 0 | OK | |
| -1 | ARGUMENT | value outside the supported range |
| -2 | STATE | radio refused: wrong ROM, patch active, not configured |
| -3 | TIMEOUT | radio timeout |
| -4 | DATA | radio status, error flag or FIFO loss |
| -5 | IO | bus error |
| -16 | EBUSY | worker busy, or not allowed while streaming |
| -17 | EUNKNOWN | unknown request type |
| -18 | ELENGTH | wrong payload length for the type |
| -19 | ESTATE | wrong protocol state, such as TX_DATA without TX_BEGIN |
| -20 | EUNSUPPORTED | this build has no TX buffer |
| -21 | ECRC | TX upload CRC mismatch |
| -22 | ERANGE | TX length, offset, rate, repeats or sample magnitude outside the contract |

-1..-5 are passed through from the C radio library.

### Records

HELLO response, 84 bytes:

| Offset | Field |
| ---: | --- |
| 0 | `i32 status` |
| 4 | `u16 protocol_version` (1) |
| 6 | `u16 caps`: 1 RX, 2 TX, 4 timed TX, 8 live gain, 16 source 2 above 125 ksample/s, 32 CS12 format, 64 checked-TX flag, 128 raw routes, 256 gain readback |
| 8 | `u8 rom_major, u8 rom_minor` (1, 24) |
| 10 | `u16 max_payload` (2048) |
| 12 | `u32 clock_hz`, unit of device time (1000000: microseconds) |
| 16 | `u32 tx_capacity`, upload bytes (65534 = 32767 samples) |
| 20 | `char name[32]`, NUL padded firmware name and version |
| 52 | `char id[32]`, NUL padded board identifier |

RX record, 17 bytes: `u32 frequency_hz` (100000000..2740000000),
`u32 bandwidth_hz` (LoRa bandwidth 31250, 62500, 125000 or 250000 for routes 0 and 4),
`u8 gain_step` (0 chip AGC, 1..13), `u8 boost` (0..7, acts at steps 12 and 13),
`u8 path` (0 LF, 1 HF, 2 automatic: LF below 1.5 GHz), `u8 route` (0 source 2,
1 source 0, 2 source 1, 3 source 6, 4 source 0 in packet type 0x0B),
`u8 input_code` (FSK RX filter code for
routes 1..3), `u32 rate_hz` (output rate for routes 1..3, 1000..4000000). The
first 11 bytes alone select source 2. The source-2 rate is 4 times the
bandwidth, 125000 to 1000000 complex pairs/s
([receive range report](../research/rx-range-and-gain/README.md)); routes 1..3
run at `rate_hz` ([raw routes](host-sdr.md#raw-receive-routes)). Route 4 streams
IQ8 at 4 times the bandwidth, like source 2.
`radio_errors` is GetErrors after configuration; IMG_CAL (`0x0080`) is reported
without failing, as it stays flagged near 100 MHz although reception works.

TX record, 28 bytes: `u32 bytes`, `u32 crc32` of the uploaded bytes,
`u32 rate_hz`, `u32 frequency_hz`, `i8 power_half_db` (SetTxParams units,
-18 = -9 dBm), `u8 repeats`, `u64 start_time` (device time, 0 = now),
`u8 pa` (0 LF, 1 HF, 2 by frequency: HF from 1.5 GHz), `u8 flags` (bit 0:
checked, see [Transmit](#transmit); bit 1: run the burst with SetRegMode
SIMO_NORMAL, the DC-DC converter for the PA supply, instead of the LDO the
receive setup uses; the reset after the burst returns to LDO).

Gain record, 18 bytes: `i32 status`, `u8 step`, `u8 manual`,
`u16 gain_value`, `u16 rssi_raw`, `u64 device_time`. The fields are the
chip registers that `lr2021_iq_read_gain` reads
([chip gain readback](host-sdr.md#chip-gain-readback)): the gain step in use
(1 to 13), the SetAgcGainManual setting (0 for AGC), the gain value of the
step and the instantaneous RSSI field, all unscaled. `device_time` is when
they were read. When idle, the worker reads the chip on request. The chip
only reports the step while it receives, so with the radio in standby the
status is -2 (STATE) and only `manual` is meaningful (the step field reads 10
there). A status of -4 (DATA) means the step read back outside 1 to 13. With
either error the fields still carry what was read. While a stream runs, the device samples the registers inside
the stream session every 64 service passes and answers from the latest
sample without touching the radio; before the first sample of a stream it
answers EBUSY. During a TX burst the worker is busy and GET_GAIN gets EBUSY.
Replies share the device's output queue with RX_DATA, so on a link slower
than the stream a reply arrives after the backlog ahead of it.
The register map is from
[beala/lr2021-re](https://github.com/beala/lr2021-re/blob/main/docs/agc-gain.md)
[RE]; [the AGC readback research](../research/agc-readback/README.md) tracks
its verification on this repository's board.

## Device messages

### RX_DATA (`C0`)

32-byte header, then up to 1920 bytes of packed pairs (480 CS16, 640 CS12 or
960 two-byte pairs):

| Offset | Field |
| ---: | --- |
| 0 | `u32 generation`, increments on every RX_START |
| 4 | `u32 rate_hz` |
| 8 | `u64 first_pair`, index of the first pair since the stream started |
| 16 | `u64 start_time`, device time when pair 0 was retained |
| 24 | `u32 dropped`, pairs the device queue has dropped so far |
| 28 | `u16 pairs` |
| 30 | `u8 format`, `u8 shift` |

Formats (I first, positive frequency is RF above the tuned frequency):

| Format | Bytes per pair | Encoding |
| ---: | ---: | --- |
| 0 CS16 (default) | 4 | int16 I, int16 Q, little endian, exactly as read from the FIFO |
| 1 CS12 | 3 | source 2 packed for slower links: every value of the frame is shifted right by `shift` (0..4, the smallest that fits -2048..2047, rounding toward minus infinity); the 24-bit word `I12 | Q12 << 12` is stored little endian; the host multiplies by `2^shift` |
| 2 IQ8 | 2 | sources 0 and 1: int8 I, int8 Q as read from the FIFO |
| 3 IQ6G | 2 | source 6 as read from the FIFO: `I = signed(byte0) >> 2`, `Q = signed(byte1) >> 2`, gain step `(byte0 & 3) | (byte1 & 3) << 2` |

CS16 and CS12 carry source 2; RX_START's format byte chooses between them.
Routes 1..3 always travel as IQ8 or IQ6G. CS12 is exact while a frame stays
within ±2047 counts and otherwise keeps the top 12 bits; the frame's shift is
visible to the host.

The radio side only publishes blocks after checking the sticky FIFO loss flags,
so every pair the device sends is part of a continuous capture. If the link is
slower than the radio, the device drops pairs from its queue instead of
stopping the radio, adds them to `dropped`, and the next frame's `first_pair`
jumps. A host must treat any `first_pair` other than the expected value, or a
new generation, as a discontinuity and report it (SoapySDR: overflow). The
time of pair `k` is `start_time + k / rate_hz` on the device clock. That is the
nominal rate (measured within about 0.5%), not a hardware-latched timestamp.

### RX_END (`C1`), 56 bytes

`u32 generation, i32 capture_error, i32 recovery_error, u8 reason (0 request
or duration, 1 error), u8 fifo_flags, u16 max_fifo_level, u32 dropped,
u32 gain_changes, u64 pairs, u64 start_time, u64 elapsed_us,
u64 max_service_gap_us`. Sent after the last RX_DATA of the generation.
`pairs` counts all checked pairs, including any the device queue dropped.

### TX_END (`C2`), 53 bytes

`i32 error, i32 stop_error, i32 recovery_error, u8 tx_flags, u16 final_level,
u16 min_level, u32 irq, u32 samples, u64 start_time, u64 elapsed_us,
u64 max_service_gap_us, u32 untested`. `tx_flags` bits `0x10`/`0x20` mean TX
FIFO overflow/underflow and invalidate the burst. `start_time` is the device
time SetTx was issued. `untested` lists the settings outside the measured
TX contract: 1 rate, 2 sample radius, 4 duration, 8 frequency, 16 power,
32 HF PA, 64 repeats.

## Transmit

1. `TX_BEGIN` with the size, CRC-32, rate, frequency, power, repeats and start
   time. A new TX_BEGIN discards the previous upload.
2. `TX_DATA` chunks in order; each response returns the next expected offset.
3. `TX_COMMIT` checks the CRC and the full contract, answers, transmits, then
   sends TX_END. The upload stays ready: a later TX_COMMIT sends it again.

A burst may use: at most 32767 samples including repeats (the ROM's 16-bit
byte count), 1..255 repeats, 1..1000000 samples/s, 100..2740 MHz, the
datasheet SetTxParams range of the PA in use (LF -19..44, HF -39..24) and a
SetTx timeout below 512 s. The measured TX contract
([TX playback](tx-playback.md)) is signed I8, Q8 pairs with
`I*I + Q*Q <= 56*56`; rates 4800, 9600, 19200, 38400, 76800, 125000, 250000, 500000 and
1000000; 1..16
repeats; up to 900 ms; LF PA, 150..960 MHz; power -19..20. TX_END's `untested`
field reports every setting outside it, and with the checked flag the device
refuses such a burst with ERANGE instead. PA settings follow the datasheet
tables; set the RF switch table's TX bits for the PA in use.

The device holds the radio for the burst,
then resets it and restores the last receive configuration. Upload requests
never emit RF. Timed bursts wait for `start_time` after prefilling the FIFO;
the start is checked against the device clock in a polling loop, so expect
about 1 ms of jitter on the ESP32-P4 and more on a loaded Linux host. Start
times more than 10 s ahead are refused.

## Transports

**TCP.** The device listens on port 51021 and serves one connection at a
time. A disconnect stops any stream.

**Serial.** The firmware console runs at 115200 baud. The console command
`bridge [baud]` replies `lrbridge: baud=<baud>` and, about 50 ms later,
switches the port to binary bridge mode at that rate. Hosts first try HELLO at
the bridge rate, in case the board is already in bridge mode; if nothing
answers they send `\r\nbridge <baud>\r\n` at 115200, wait 100 ms, change rate
and retry HELLO. BYE returns the board to its console at 115200.

Link budget with full frames (44 bytes of framing each):

| Rate | CS16 | CS12 | IQ8 / IQ6G |
| ---: | ---: | ---: | ---: |
| 125000 pairs/s | 511 kB/s | 386 kB/s | 256 kB/s |
| 250000 pairs/s | 1023 kB/s | 773 kB/s | 512 kB/s |
| 576000 pairs/s | | | 1179 kB/s |
| 1000000 pairs/s | 4092 kB/s | 3092 kB/s | 2046 kB/s |

A UART with 8N1 carries baud/10 bytes/s. On the LilyGO T-Display-P4 the CH343
link runs at 3 and 4 Mbaud (400 kB/s), which carries source 2 at 125000
pairs/s as CS12 and two-byte routes up to about 190000 pairs/s; the board's
Wi-Fi TCP link carried CS16 at 250000 pairs/s for 10 minutes with every pair
delivered.

Opening a USB serial port can reset some boards. The host client deasserts
DTR and RTS after opening and waits for the boot banner before talking.

## Sequence rules

- Hosts number requests and match responses by type and sequence. Device
  messages are not responses; their sequence is a per-type counter.
- Unknown device message types must be ignored.
- A new protocol version changes byte 2. A device answers HELLO from a newer
  host with its own version; the host decides whether to continue.
