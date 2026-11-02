# Debug console

[Guide hub](README.md) / Debug console

The bridge firmware's serial console (115200 baud, `lriq>` prompt) carries
the raw chip access and capture commands of
[beala/lr2021-re](https://github.com/beala/lr2021-re/blob/main/firmware/README.md)
(MIT, Alex Beal), ported to the ESP32-P4 firmware in
[`firmware/esp32p4/main/debug.c`](../firmware/esp32p4/main/debug.c), plus the
gain readback, RSSI and packet status probes from
[the AGC readback research](../research/agc-readback/README.md). The command
names, arguments and output lines follow lr2021-re, so its notes and
examples apply here.

These are debug commands. `mw` and `rc` can overwrite PRAM, live RAM or
registers, or send an opcode that hangs the chip until reset; `mr` of an
unmapped address, or of the PRAM load window at 0x801000, stalls it. Run
`rxrestore` afterwards to reset the radio and reapply the saved receive
configuration. The capture and probe commands do that themselves.

## Commands

| Command | What it does |
| --- | --- |
| `mr <hex addr> [words]` | ReadRegMem32, 32 words per command, printed 8 per line (`AAAAAA: w0 ... w7`), then `mr done` |
| `mw <hex addr> <hex value>` | WriteRegMem32 of one word; prints `mw st=<status>` |
| `rc <hex op> <reply bytes> [hex params...]` | Any command: prints `rc st=<status> stat=<status word> <reply bytes>` |
| `err` | Print and clear the chip error register |
| `romprobe` | Read a few words at candidate ROM bases (how lr2021-re located the ROM) |
| `rxrestore` | Reset the radio and reapply the saved receive configuration |
| `gain` | Gain step, manual setting, gain value and RSSI field ([chip gain readback](host-sdr.md#chip-gain-readback)) |
| `cap <hex cfg> <words> <ms> [hex trig]` | One-shot 0x0290 capture into RAM 0x804000: fills it with A5A5A5A5, arms the capture, follows the write pointer (0x0291), stops it with mode 0, then dumps the words that changed. `cfg` = size[7:5] \| mode[4:3] \| src[2:0], for example `D0` |
| `capwait <hex cfg> <ratio> <ms>` | Repeats one-shot 8 KiB captures until one has more than `ratio` times the first one's power, then dumps it. Set packet type 0x0B first (`rc 207 0 0B`) |
| `stream <src> <ratio> <ms> <pre KiB> [-] [dec]` | Packet type 0x0B, then the 0x0290 FIFO route for `src` into a 128 KiB PSRAM ring. Triggers when a 4 KiB block's power exceeds `ratio` times the quiet baseline or the chip AGC step drops below its quiet value, keeping `pre` KiB before the trigger; `ratio` 0 is a timed capture. `dec` averages that many pairs |
| `streamdump` | The last stream's summary line, `STREAMBIN <n>`, then n raw bytes |
| `streamstats` | Per-4 KiB block power, gain step, manual setting and rail count of the last stream |
| `agcrssi [count] [interval_ms]` | GetRssiInst against the 0xF301A8 field and the AGC registers |
| `pktstatus [s] [Hz] [bw] [sf] [sync] [gain] [preamble]` | Receives LoRa packets and prints all 10 GetLoraPacketStatus bytes per packet |
| `undocprobe [all\|aux\|param\|getters]` | Read-only probes of 0x0103, 0x0213, 0x023E and 0x023F ([undocumented commands](undocumented-commands.md)) |

Examples from lr2021-re:

```
mr 0 32768                 ROM and zeros up to 0x1FFFF
mr F30154 1                AGC register; bits 23:20 are the gain step
rc 21A 0 05                SetAgcGainManual(5)
rc 22A 10                  GetLoraPacketStatus, all 10 bytes (byte 9 = gain step)
rc 103 4 00 00 00 04       aux IDENTITY
stream 0 4 2000 16         source 0, trigger at 4x, 2 s, 16 KiB before the trigger
```

`stream` holds the radio for its whole duration, so the console answers
nothing until it ends; fetch the ring afterwards with `streamdump`.

## Programs on the radio's own CPU

The LR2021 has an undocumented 32 MHz ARCv2 CPU that runs code from its 4 KB
Patch RAM. lr2021-re's `pram.bin` (source and notes in
[`third_party/lr2021-re/firmware/pram`](../third_party/lr2021-re/firmware/pram),
write-up in
[pram-custom-commands.md](https://github.com/beala/lr2021-re/blob/main/docs/pram-custom-commands.md))
is built into the firmware and adds ActivatePram sub-commands: a spectrum
sweep and a capture + 256-point FFT + dB that run entirely on the chip. This
firmware never loads Semtech's own patch, so all of PRAM is free.

| Command | What it does |
| --- | --- |
| `pram load` | Upload the program to 0x801000, ActivatePram, read it back at 0x101000 and check `hello` |
| `pram hello` | `pram hello st=0` when the program is loaded (a radio reset clears it) |
| `pram sweep <start Hz> <stop Hz> <step Hz> [dwell] [avg]` | The chip retunes in continuous receive and sums instant RSSI per point; prints `freq_hz,rssi_dbm` lines, then `pram sweep done` |
| `wfrun <nseg> [frames] [overlap] [baud] [bw]` | Stream the spectrum the chip computes. `nseg` 1..10 segments averaged; `overlap` 1 (with `nseg` 1) starts the next capture while the chip computes; `baud` switches the UART for the frames; `bw` 31250 or 62500 (I/Q at 4x). `x` stops it |

`pram sweep` and `wfrun` load the program themselves when it is missing, and
reset the radio afterwards like the other probes. The ESP32-P4 only reads
256 bytes per frame back: lr2021-re measured 317 frames/s with one segment
and 464 frames/s overlapped on its nRF52 board. At the console's 115200 baud
a frame takes 23 ms on the wire, so use `baud`:

```bash
python tools/lr2021_waterfall.py COM14 waterfall --seconds 10 --overlap --bw 62500 --plot wf.png
python tools/lr2021_waterfall.py COM14 sweep 902e6 928e6 25e3 --out band.csv --plot band.png
```

`wfrun` frames are lr2021-re's: `WFBIN <nseg> <rate> <freq>`, then per frame
A5 5A, sequence (u16 LE), board time in µs (u32 LE), chip time in µs (u32 LE)
and 256 half-dB bytes in FFT order (bin 0 = DC, 128..255 negative).

## Differences from lr2021-re

- The commands run on the ESP32-P4 bridge firmware, not on MeshCore, so
  there is no `hold`: nothing else touches the radio while the console runs.
- lr2021-re's `iqstream` (its LRIQ continuous transport over USB) is not
  ported. The bridge protocol carries continuous checked IQ instead, and
  [`tools/lr2021_lora.py`](../tools/lr2021_lora.py) records it in
  lr2021-re's file layout ([LoRa software receiver](lora-software-receiver.md)).
- `stream`'s rate field stays 0 as in lr2021-re; the fifth argument, the SPI
  clock there, is ignored, because the firmware owns the bus at its tuned
  clock (`spiclk`).
- Every capture and probe command resets the radio and reapplies the saved
  receive configuration when it ends.

## Scripting

[`tools/lr2021_console.py`](../tools/lr2021_console.py) sends commands and
waits for each one's last line, like lr2021-re's `sc.py`, but opens the port
without asserting DTR or RTS, so the LilyGO board does not reset:

```bash
python tools/lr2021_console.py COM14 "mr F30154 1" "rc 22A 10"
python tools/lr2021_console.py COM14 -t 5 "stream 0 4 3000 16" --streamdump shot.iq8
python third_party/lr2021-re/tools/lora_decode.py shot.iq8 250000
```

A console session and the bridge share the port: leave bridge mode (BYE, or
reset) before using the console.
