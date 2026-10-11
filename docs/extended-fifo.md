# Extended FIFO hardware results

LR2021 ROM 1.24, ESP32-P4, 154.785 MHz, external VHF antenna.
Receive only. These are real board tests, not a timing simulation.

The 1024-byte FIFO is used by LakeShark's continuous raw-I/Q receiver, which
has decoded live P25 Phase 1 C4FM voice. These bounded diagnostics explain the
FIFO change; see [live streaming](live-streaming.md) and the
[decoded voice recording](voice-capture.md) for the integrated receiver results.

## Applied changes

LakeShark raw-I/Q capture now defaults to the nominal 1024-byte RX FIFO.
`lora lrfifo-size 256` selects the old configuration for comparison;
`lora lrfifo-size 1024` selects the new one. The selection is session-local
and defaults to 1024 on reboot. Ordinary packet/FSK reception is unchanged.

The driver writes RX base 0x804400 and size encoding 0x3E8, and checks them
before and after starting reception. This ROM reads the base back as 0x4400;
the verifier accepts that observed offset or the full alias, not arbitrary
addresses. After capture, reset/reconfiguration restores the normal mapping.
The final hardware readback was base 0x6140, size 0xE4, with no radio errors.

Each SPI read remains at most 256 bytes. The loop drains the observed backlog
in several bounded reads before paying for another FIFO-level query. Sticky
FIFO loss still rejects data. Capture buffers use internal heap memory to
avoid overflowing the console task stack. Allocation happens before the
radio lock and buffers are freed after recovery.

The C library exposes `lr2021_iq_stream_checked_fifo(hal, duration_ms, 1024,
sink, context, result)`. Existing APIs retain their 256-byte default. It checks
the mapping and drains bounded chunks in the same way. Its caller must provide
adequate stack for the local staging buffers and HAL calls.

## Final comparison

The checked diagnostic ran at the live receiver's task priority with UI and
wireless services enabled. Each 1024-byte run requested 3000 ms. Each 256-byte
run requested 1000 ms but stopped early on loss. All six recoveries succeeded.

| FIFO | Elapsed ms | Read / accepted bytes | Peak bytes | Largest service gap us | Result |
| --- | --- | --- | --- | --- | --- |
| 1024 | 3000.130 | 1499976 / 1499976 | 224 | 447 | ESP_OK |
| 1024 | 3000.239 | 1500024 / 1500024 | 216 | 424 | ESP_OK |
| 1024 | 3000.083 | 1499968 / 1499968 | 216 | 486 | ESP_OK |
| 256 | 21.463 | 10604 / 9472 | 188 | 376 | ESP_ERR_INVALID_RESPONSE |
| 256 | 149.498 | 74612 / 73540 | 212 | 383 | ESP_ERR_INVALID_RESPONSE |
| 1024 | 3000.241 | 1499992 / 1499992 | 236 | 442 | ESP_OK |

The four extended-FIFO runs accepted about 6 MB over 12 seconds of aggregate
capture, each with one uninterrupted three-second receive session. They are
four separate sessions, not a continuous 12-second recording. Their FIFO flags
were 0x04, with no bits in the 0x30 loss mask. Both 256-byte runs latched 0x1C.

An earlier lower-priority run reached 308-byte occupancy and completed three
seconds successfully, establishing use beyond the original 256-byte limit.
Other early runs failed: extending storage alone did not remove scheduling
and service-cost problems. A full/lost state reported 1028 bytes; the driver
conservatively rejects levels at or above the documented nominal 1024 limit.
That observation does not establish four extra usable bytes.

## Verification

All four portable host test suites pass. Added cases exercise oversized levels,
bounded multi-read drainage, both base-address encodings, changed mapping after
RX, partial configuration failure, overflow, rejected sinks, and restoration.
The LakeShark firmware builds and the programmed image passed flash verification.
Existing P25 I/Q conversion, frontend and queue host tests also pass.

The extended FIFO completed all four three-second comparison runs. The
256-byte configuration stopped on loss in both comparison runs.

The live P25 option now uses a continuously running producer and consumer,
removing the former reset/decode gaps between 250 ms windows. Separate live
sessions reached 391 seconds without reported FIFO loss or queue rejection.
Live P25 voice has been decoded through this continuous I/Q path. The
[saved voice capture](voice-capture.md) also replayed to 27 IMBE frames with
none muted.

The original datasheet investigation is [here](datasheet-research.md).

## Earlier windowed P25 check

After the final diagnostic, the board was left on LR2021 raw I/Q at
154.785 MHz, external MMCX1 antenna, volume 70, unmuted. The console reported
137 accepted windows, zero rejected windows and zero FIFO-loss events; the
following screen snapshot had advanced to 150 accepted / zero rejected, with
zero read errors and zero audio-buffer drops. These counters validate the
older windowed capture path. It has been replaced in live reception by the
continuous producer/consumer implementation described above.
