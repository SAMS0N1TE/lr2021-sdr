# Raw IQ from the LR2021

> **Credit:** the LR2021's hidden capture engine was originally discovered by
> **[SAMS0N1TE/lr2021-sdr](https://github.com/SAMS0N1TE/lr2021-sdr)** (MIT). That covers
> command 0x0290 and its raw routes, the RAM bank mapping through aux 0x80000008, FIFO IQ
> streaming, and TX through 0x0293. They built it into a full SDR: SoapySDR,
> 100 MHz–2.74 GHz, 500 kS/s 16-bit over a 26.67 MHz SPI on an ESP32-P4.
>
> The FIFO streaming recipe below comes from their documentation (`docs/capture-engine.md`,
> `docs/custom-firmware.md`). The ROM analysis in this repository reached the same code paths
> and agrees with their description: the same bank map and aux mapping, bytes 1–4 of 0x0290
> as the output rate, and the last byte as FIFO enable. What this page adds:
>
> - the ROM-side view;
> - hidden packet type 0x0B, and its interaction with Semtech's PRAM patch;
> - hardware AGC behavior in raw mode;
> - a checked continuous transport on an nRF52840.

Tags: **[HW]** = verified on hardware, **[RE]** = read from code.

## Hidden packet type 0x0B [HW]

The datasheet lists SetPacketType value 0x0B as "RFU". It is a raw/capture mode:

- 0x0290 only fully starts the capture DMA when the packet type (RAM 0x80002C) is 0x0B.
  In LoRa (0) it arms but writes nothing, and the LoRa demodulator stops receiving while it's
  armed.
- **With Semtech's PRAM patch loaded, the FIFO route only produces data in packet type 0x0B.**
  In LoRa the FIFO level stays 0. lr2021-sdr runs without the patch and stays in LoRa.
- **Hardware AGC still works in 0x0B.** Sampling 0xF30154 during a stream showed G13 when
  quiet, G1 during a nearby transmitter, then G13 again.

## One-shot capture into RAM [HW]

While a capture is armed, its RAM bank belongs to the DMA (aux 0x80000008 bank mapping).
**A CPU (or host) read of that bank stalls the bus and hangs the chip.** Stop the capture first
(0x0290 with mode 0), then read the RAM.

Recipe for 8 KB at 0x804000 (cfg 0xD0 = size 6, mode 2, source 0):

```
SetStandby(0)
SetPacketType(0x0B)
0x0290  FF 00000000 D0 00 00 08 00
SetRx(FFFFFF)
poll 0x0291 until 0x2000            (~17 ms, one-shot)
0x0290  FF 00 00 00 00 00           (mode 0: stop, give the bank back)
SetStandby(0)
ReadRegMem32 0x804000 ...
```

Source 0 is **interleaved int8 I, int8 Q at 250 kS/s** (4 × the 62.5 kHz LoRa bandwidth),
so 8 KB = 4096 samples = 16.4 ms. The channel filter edge is visible at about ±50 kHz.
This was verified with LoRa preamble upchirps: a -31..+31 kHz sweep with a 2.05 ms period
(SF7 at 62.5 kHz = 2.048 ms). Sources 1–6 have 2,2,4,4,4,4,2 bytes per sample (ROM table
0x13948) and haven't been explored. Source 2 hung the chip once in LoRa packet type.

Debug console: `cap <cfg> <words> <ms>` and `capwait <cfg> <ratio> <ms>`
([firmware/README.md](../firmware/README.md)).

## Streaming through the RX FIFO [HW]

Route (from lr2021-sdr):

```
SetStandby(0)
SetPacketType(0x0B)                                    (needed with the PRAM patch)
0x0290  FF rate(4) 08|src 00 00 00 00 01               (mode 1 + FIFO enable)
WriteRegMem32 0xF30028 = 0x804400, 0xF30030 = 0x3E8    (RX FIFO at the documented 1 KB region)
ClearFifoIrqFlags(0x3F, 0x3F)
SetRx(FFFFFF)
ClearRxFifo
loop: GetRxFifoLevel (0x011C) + ReadRxFifo (0x0001)
      GetFifoIrqFlags (0x011B) bits 0x30 = data lost
```

Source 0 at 250 kS/s is 500 KB/s across SPI. RadioLib's 2 MHz SPI is too slow, so the
firmware uses its own SPI transactions. The output-rate field had no effect for source 0
with byte 0 = 0xFF. lr2021-sdr sets a filter code in byte 0 together with an explicit rate.

Measured on an nRF52840:

| Setup | Result |
|---|---|
| 8 MHz SPI, triggered snapshots | lossless; FIFO peak 136–152 of 1024 bytes; ~67 µs per level poll |
| 16 MHz SPI, continuous, 3× MCU averaging (83.333 kS/s out) | **60 s: 4,999,818 IQ pairs, 9,766 frames, no FIFO or queue loss**. A 250 ms reader pause was absorbed; a 2 s stall ended with an explicit queue-full error |
| 8 MHz SPI, continuous | completed 5 s, but built 116 KB of queue backlog |
| 32 MHz SPI (beyond the 16 MHz spec) | **failed**, including all eight MISO sampling delays. Impossible status bits and wrong sample counts (`analysis/spi32_delay_sweep.json`) |
| Native IQ16, source 2, LoRa packet type, BW 31.25 kHz (125 kpair/s), 2× averaging | 60 s: 7,499,749 raw pairs, no reported loss |
| Native IQ16, BW 62.5 kHz (1 MB/s on SPI), 4× averaging | FIFO overflow within 5 ms. MCU averaging can't reduce what crosses SPI first |

Source 2 ("route byte 0x0A") is native little-endian signed int16 I, int16 Q, at four complex
pairs per configured LoRa bandwidth Hz. A 16-bit container doesn't establish 16 effective
ADC bits. Design notes and USB/SPI budgets are in `analysis/continuous_streaming.md`.

### Snapshot trigger and AGC

The snapshot streamer (`stream`) triggers when a 4 KB block's power exceeds a multiple of
the quiet baseline. With hardware AGC on, a strong packet only raised output power to about
1.6× baseline, because the AGC was doing its job. The firmware therefore also triggers when the
automatic gain step (0xF30154[23:20], sampled once per block) drops below the highest quiet
step it has seen.

## LRIQ wire format (continuous stream)

`iqstream` sends framed binary over USB CDC. All fields are little-endian, with a 48-byte
header, magic `LRIQ`. Version 1 = IQ8, version 2 = IQ16.

| Offset | Field |
|---:|---|
| 0 | magic (4), version u8, type u8, header size u16 |
| 8 | generation u32, sequence u32 |
| 16 | first output sample index u64 |
| 24 | sample count u32, nominal raw rate u32 |
| 32 | decimation u16, flags u16 |
| 36 | payload bytes u32, payload CRC32 u32, header CRC32 u32 |

- The header CRC32 covers the first 44 bytes. CRC32 is the standard reflected 0xEDB88320
  (same as Python `zlib.crc32`).
- Frame types:
  - 1 = START, with a 64-byte status payload;
  - 2 = DATA, with ≤ 1024 IQ bytes;
  - 3 = END, with a 64-byte status payload.
- Every frame advances the sequence number. DATA advances the sample index without gaps.
- Status payload:
  - four u64 values: raw samples, produced output pairs, sent pairs, elapsed µs;
  - u32 values: max FIFO level, sticky FIFO flags, max service gap µs, max queue bytes,
    queue overflows, BUSY timeouts, SPI Hz;
  - an i32 configuration error.
- Flags:
  - 0x01 FIFO loss;
  - 0x02 queue full;
  - 0x04 clean host stop;
  - 0x08 duration complete;
  - 0x10 BUSY timeout;
  - 0x20 reserved;
  - 0x40 configuration error.

  A missing END is a failed run. Footer totals must agree with the host and with
  floor(raw samples / decimation).
- The host stops the stream with ETX (0x03).

`tools/iq_stream.py` parses and validates the format, and `tools/continuous_capture.py`
records `iq.bin` + `frames.jsonl` + `summary.json`.

Two firmware details mattered:

- One 64-byte USB write per FIFO service was too slow; up to four bounded writes per service
  works.
- Status-read SPI transfers must clock out zero bytes. The nRF SPIM's default over-read byte
  is 0xFF, which the radio later rejected with -706.

## TX from RAM

0x0293 / 0x0294 switch the modulator input (0xF30904 bits 4/5) and packet-engine source
(0xF30800 bits 10/11), with a length and start time [RE]. lr2021-sdr uses this path to
transmit. It was not tested here.
