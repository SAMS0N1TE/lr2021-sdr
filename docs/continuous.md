# Checked capture sessions

LakeShark's separate live task integration has run for 391 seconds without
reported FIFO loss or queue rejection. See [live measurements](live-streaming.md).
The portable API below retains its 60-second session limit.

`lr2021_iq_stream_checked()` keeps one receive session open for up to 60 seconds.
It checks FIFO flags before publishing each block, then resets and restores the
radio once at exit. It stops on FIFO loss, an I/O error, or a rejected block.
The portable API has host tests; the matching LakeShark adapter has also run
on ROM 1.24. See [extended FIFO results](extended-fifo.md) for measured limits.

The original `lr2021_iq_stream()` retains its 250 ms limit and provisional-data
contract. The checked API has a different contract: returning `false` from its
sink rejects that block. `validated_bytes` counts accepted blocks; `bytes` also
includes the unpublished tail. Previously accepted blocks remain qualified if
the session later fails. Recovery errors still require attention before reuse.

Use a dedicated producer and a separate consumer. The producer's callback must
only copy into a bounded queue. Do not decode, log, allocate, sleep, or call radio
functions from it. `lr2021_iq_queue` supplies eight 1280-byte slots, lock-free
indices on supported platforms, and explicit rejection when full. Initialize it
before starting either owner. A consumer holds a block until `queue_release()`.

Give each capture session a new generation number. Reset the downstream decoder
when generation changes or sample indices stop being consecutive. A restart is
a gap; joining samples across it produces a false timeline.

At 125000 IQ16 pairs/s the radio produces 500000 bytes/s. The currently assumed 256-byte FIFO fills
from empty in 512 us. A host queue decouples decoding, but cannot protect against
a stalled SPI producer. Measure worst-case BUSY, SPI and scheduling latency,
including the extra flag checks. Check task watchdog behavior before long runs.

`lr2021_iq_stream_checked_fifo(..., 1024, ...)` selects Semtech's nominal
1024-byte extended FIFO. Ordinary APIs keep their original 256-byte default.
Reads remain at most 256 bytes; the observed backlog is drained before another
level query. See [datasheet research](datasheet-research.md) for calibration
restrictions and [extended FIFO results](extended-fifo.md) for hardware evidence.

## Offline tools

```sh
build/lr2021_lab model
build/lr2021_lab banks
build/lr2021_lab commands rom.bin
build/lr2021_lab refs rom.bin 0xF30044
build/lr2021_lab diff before.bin after.bin 0x800560
build/lr2021_lab route FF0000000092000000FF00
build/lr2021_lab continuity capture.iq16 125000 5000
```

The tool opens local files only. `commands` checks the observed ROM table layout;
`refs` finds byte patterns in normal and ARC halfword order. Matches need manual
disassembly review. `diff` compares equal-sized memory snapshots. `route` decodes
candidate fields in an existing 11-byte `0x0290` payload, without sending it.
SRAM bank sizes are inferred from ROM; they do not prove safe memory ownership,
circular buffering, or a gap-free sample route. ROM dumps are not distributed.

`continuity` checks phase steps against a known single CW tone. It reports
outliers over 30 degrees after DC removal. Noise, fading, clipping or an
incorrect tone frequency can also cause outliers. Losses spanning whole tone
cycles are invisible. Do not use it as a continuity verdict on modulated P25.

The timing model assumes instantaneous FIFO drains and omits SPI/BUSY overhead.
Failure rules out that schedule; success does not establish hardware throughput.

[TheClams' driver](https://github.com/TheClams/lr2021) and
[examples](https://github.com/TheClams/lr2021-apps) provide useful FIFO and IRQ
references. Their packet receiver examples do not establish continuous raw I/Q.
No code from those projects is included here.

See [Capture engine investigation](capture-engine.md) for the SRAM bank map,
patch-loader mapping evidence, start/stop lifecycle, and transport budgets.
`banks` models inferred finite-buffer sizes; it does not establish circular
capture or safe bank ownership.

[Custom firmware research](custom-firmware.md) covers the proven RAM execution
entry point, vendor JLI hooks, patch memory budget and a C development path.
