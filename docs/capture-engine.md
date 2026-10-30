# Capture engine investigation

The saved ROM contains a larger SRAM capture path, but no automatic ping-pong
buffer switch was found in the routines examined. SRAM bank switching is not implemented. The implemented streaming path uses the
1024-byte FIFO and P4 producer/consumer tasks; it has decoded
[live P25 voice](voice-capture.md). The SRAM findings below are static ROM
analysis, separate from that tested FIFO implementation.

Evidence is from ROM SHA256
`2bf5736c9b15719598096d077c090d2c39a53e2b27800cb2caab0c48d405d3dc`,
identified as LR2021 ROM 1.24. Addresses below are
ROM instruction addresses unless marked as registers. This is static analysis
of the saved image, not a specification of the silicon or the patched runtime.
ROM binaries are not distributed.

## SRAM capacity and ownership

The `0x0290` parser at `0xF9A0` calls `0xCF7C`. Mode 2 selects one of six
branches at `0xD02E..0xD0AE`. For source 2, extending the observed four-byte
stride gives these candidate ranges and durations:

| Bank | CPU address range, end exclusive | Maximum index | Inferred bytes | At 125k pairs/s |
| --- | --- | --- | --- | --- |
| 1 | 0x801000..0x802000 | 1023 | 4096 | 8.192 ms |
| 2 | 0x801000..0x804000 | 3071 | 12288 | 24.576 ms |
| 3 | 0x801000..0x806000 | 5119 | 20480 | 40.960 ms |
| 4 | 0x802000..0x804000 | 2047 | 8192 | 16.384 ms |
| 5 | 0x802000..0x806000 | 4095 | 16384 | 32.768 ms |
| 6 | 0x804000..0x806000 | 2047 | 8192 | 16.384 ms |

Only bank 4 with limit index 255 (1024 bytes) was tested previously. Maximum
sizes are inferred, not measured. A requested limit of zero selects the bank's
maximum, not zero samples. `0xD0B8..0xD0DE` clamps nonzero limits to that maximum
and writes the index into register `0xF30050` bits 2..14.

Bank selection changes fields in ARC auxiliary register `0x80000008`:

| Bank | Cleared mask | Selected bits |
| --- | --- | --- |
| 1 | 0x038 | 0x010 |
| 2 | 0x1F8 | 0x090 |
| 3 | 0xFF8 | 0x490 |
| 4 | 0x1C0 | 0x080 |
| 5 | 0xFC0 | 0x480 |
| 6 | 0xE00 | 0x400 |

The mapping is calculated and saved at `0x8060C4` during configuration, then
written to the auxiliary register during start (`0xD276`). This is not ordinary
host RAM allocation. The ROM patch loader at `0x24FC` also modifies this auxiliary
register: it selects bit 3 in mask 0x38 before checking a magic value at 0x101000,
then has another branch selecting bit 6 in mask 0x1C0 and checking 0x102000.
The separate entry at `0xCD8` selects bit 3 and reads a candidate entry pointer
at 0x101008. This links the mapping control to patch execution; it does not
fully establish the physical aliases or prove any candidate bank is free.

Banks 1..3 overlap the CPU addresses used to load the current patch at 0x801000.
Banks 4 and 6 have disjoint inferred ranges and disjoint mapping fields, making
them a better *pair to investigate*. They are not established ping-pong banks.
The larger bank 4 range has not been backed up or checked for live firmware use.

## Start, stop and count

- `0x3E9E` calls start helper `0xD234`. For SRAM it installs the saved mapping,
  sets register 0xF30048 bits 30 and 31, then bit 9. For FIFO it sets 0xF30044
  bit 14, then bit 9. Hardware meanings of individual bits remain inferred.
- `0xB226` calls stop helper `0xD294`. In the SRAM branch, if 0xF30054 bits
  24..25 equal 2, it temporarily clears a three-bit selector, sets 0xF30048 bit
  13 and spins until that status field equals 3. There is no timeout in this
  ROM loop. It then restores the selector, clears bit 31, sets bit 30 and
  clears the selected bank's auxiliary mapping fields.
- The callback `0xCF18` dispatches event bits 23 and 10 to shared receive
  completion helpers `0xB3DC` and `0xB454`. The callback itself contains no
  buffer address update. Those helpers include indirect callbacks and receive
  lifecycle work, so this is not proof that all possible rearming is absent.
- `0xCF54` reapplies saved routing/configuration. The inspected path has one
  base slot at 0x8060C0 and one mapping slot at 0x8060C4, not a descriptor ring.
- `0x0291` reaches count helper `0xCE98`: low 16 bits of
  `(F30054 - saved_base + source_width)`, with bit 15 forced when F30054 bit 31
  is set. Do not mistake the entire result for an unconditional byte count.
  The significance of the forced bit is not established.
- `0x0292` reaches `0xCED4`, which copies bytes from the selected base plus
  requested offset, rejecting an exclusive end above offset 0x6000. Its command
  parser at 0x5B60 supplies a 16-bit offset and an 8-bit length. The helper
  checks the global ceiling, not the selected bank's size or completed count.
  It has no local producer-pointer check or coherent-read handshake.

Together these observations fit finite capture with explicit lifecycle control.
They do not establish a hardware circular mode, safe reads during writes, or
lossless host-controlled rearming. Changing the bank through 0x0290 calls cleanup
first; it is not a demonstrated atomic switch.

## Other hidden controls

`0x0294` reaches `0xD134`. It writes two three-bit fields at bits 6..8 and
10..12 of both 0xF30044 and 0xF30048, plus two nine-bit fields in 0xF30130.
The stop helper temporarily replaces the bits 10..12 field while requesting
the state transition. This makes a trigger/stop-selector interpretation worth
testing. There is no evidence yet that 0x0294 controls decimation or wrapping.

The source-width table at 0x13948 is `02 02 04 04 04 04 02`. FIFO setup helper
0x6944 passes that width into 0x10CBC and programs 0xF2A0A0. The matching normal
setup helper 0x643C uses stored format fields. This supports a packing/transfer
width role, not a lower sample rate. Sources 0, 1 and 6 remain unidentified.
Sources 0 and 6 also have a special DIO-related configuration branch at 0xD0F4;
they should not be advertised as alternate low-bandwidth I/Q streams.

TheClams' [applications](https://github.com/TheClams/lr2021-apps/tree/cf3b05c7cbd74dc7b97595284f4131ceb96a2517)
and [driver](https://github.com/TheClams/lr2021/tree/402a12ba350c7a5419acf595ca6ea529403d2dbc)
provide packet FIFO and IRQ examples. The examined ADS-B application
drains decoded packets on RX_DONE. It does not implement these hidden SRAM
routes or establish continuous raw I/Q. No code was copied.

## Offline transport budget

```sh
lr2021_lab banks
lr2021_lab banks 2000000 50 200
```

Arguments are payload bytes/second, additional overhead microseconds per read,
and rearm microseconds. The first command assumes a 16 MHz SPI payload-only
ceiling of 2 MB/s, zero command overhead and zero rearm time. Reads are modeled
as at most 252 payload bytes, keeping IQ16 pairs intact within an 8-bit length.
Include all command/status wire time, BUSY and host cost in the overhead argument.
The second command is an illustrative budget, not measured timing.

At that ideal 2 MB/s rate, a stop/read/restart design spends at most 80% of
its time capturing, regardless of bank size: incoming data is 0.5 MB/s and
reading the completed bank takes one quarter of its fill time. Larger banks
amortize fixed rearm costs but cannot remove read-time gaps.

For an 8 KiB bank the ideal fill/read times are 16.384/4.096 ms. With the
illustrative 50 us per read and 200 us rearm, read time is 5.746 ms and duty
cycle about 73.4%. A hypothetical overlapping design has 10.438 ms of margin
under those assumptions. Positive margin proves only sufficient transport
budget; the required simultaneous access and atomic switch are unverified.

## Findings from lr2021-re

[beala/lr2021-re](https://github.com/beala/lr2021-re/blob/main/docs/raw-iq.md)
(MIT, Alex Beal) followed the same 0x0290 code paths from the ROM side and
agrees with the bank map and mapping register above. It adds, all [RE] here
until run on the LilyGO board:

- **Packet type 0x0B.** SetPacketType value 0x0B, "RFU" in the datasheet, is
  a raw capture mode. The capture DMA only fully starts in 0x0B; in LoRa it
  arms but writes nothing, and the LoRa demodulator stops while it is armed.
  With Semtech's PRAM patch loaded, the FIFO route only produces data in
  0x0B. The chip AGC keeps working in 0x0B. This repository offers it as
  receive route `raw0b` ([raw routes](host-sdr.md#raw-receive-routes)).
- **Armed banks stall reads.** While a capture is armed, its RAM bank belongs
  to the DMA, and a host read of the bank hangs the chip. Stop the capture
  with 0x0290 mode 0 before reading it.
- **One-shot 8 KiB capture.** In packet type 0x0B, `0x0290 FF 00000000 D0 00
  00 08 00` (size 6, mode 2, source 0) then SetRx fills 0x804000 with 4096
  int8 I/Q pairs at 250000 pairs/s (BW 62.5 kHz) in about 17 ms; poll 0x0291
  until 0x2000, stop with mode 0, then read. The debug console's `cap` and
  `capwait` do this ([debug console](debug-console.md)).
- **Source 0** is interleaved int8 I, Q at 4 x the LoRa bandwidth in 0x0B; the
  output-rate field had no effect with byte 0 = 0xFF. Source 2 hung the chip
  once in LoRa packet type during one-shot experiments.
- **SPI clock.** On an nRF52840, 16 MHz streamed source 0 for 60 s without
  loss; 32 MHz failed with every MISO sampling delay (impossible status bits,
  wrong counts). The ESP32-P4 bus here runs at 26.67 MHz
  ([receive](host-sdr.md#receive)).

## Further SRAM experiments

The documented 1024-byte FIFO relocation has been
[validated with raw I/Q](extended-fifo.md) and is used by the live receiver.
Its memory region overlaps some inferred SRAM banks listed above. The following
experiments concern an alternative SRAM capture engine, not the working FIFO path.

1. Use the [measured FIFO implementation](live-streaming.md) as the baseline
   when comparing service intervals and capture load for an SRAM alternative.
2. Before increasing SRAM length, establish ownership of the full candidate
   region and mapping state, including the active patch. Require full-range
   restoration checks, not only the previously tested 1 KiB.
3. With a known sequence, observe pointer/status progression and compare
   prefix reads during capture against the stopped snapshot. Determine whether
   reads are accepted, coherent and non-stalling. A single tone is insufficient
   to detect all omissions or duplicates.
4. Establish whether base/limit can change while running without reset or loss.
   If no supported mechanism exists, an ARC-side patch might be necessary;
   that remains a separate experiment with no working implementation.
5. Validate an SRAM alternative with long sequence/count checks for missing,
   repeated or reordered samples under realistic load.
