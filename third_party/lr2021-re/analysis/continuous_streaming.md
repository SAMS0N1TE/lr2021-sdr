# Continuous IQ feasibility review — 2026-10-28

This review led to the implemented continuous prototype: `firmware/LR2021IQStream.h`,
`tools/continuous_capture.py`, `tools/decode_continuous.py`, and `tools/continuous_compare.py`.
A 60-second live run delivered 4,999,818 IQ pairs with
matching counters and checksums, no FIFO/queue loss, and three packets matching the RAK.
See [docs/raw-iq.md](../docs/raw-iq.md) for usage and results.
Subsequent SPI32 timing sweeps failed on this board; native IQ16 experiments and their
limits are recorded in [docs/raw-iq.md](../docs/raw-iq.md) and `spi32_delay_sweep.json`.

The first implementation uses a cooperative FIFO-first loop and bounded nonblocking USB
writes, reusing the existing MCU ring. It does not require a separate RTOS producer task,
CPU replacement, or PRAM changes. The planning and isolated benchmarks below are retained
as background; simultaneous SPI+USB operation has now been tested at 16 MHz / decimation 3.

Reviewed [SAMS0N1TE/lr2021-sdr](https://github.com/SAMS0N1TE/lr2021-sdr), commit
`0a44fc69beb34274d6e86f29c1c064b3d720c48d`. No upstream implementation was copied into our firmware.

## Current hardware budgets

KB/MB rates below are decimal; KiB buffer sizes are binary.

| Stage | Budget |
| --- | --- |
| Radio source 0 | 250,000 complex int8 I/Q pairs/s = 500,000 B/s crossing SPI |
| Existing 3x MCU decimation | 83,333.33 pairs/s = 166,666.67 B/s exported |
| Nominal 1024-byte RX FIFO | Fills in 2.048 ms from empty at the raw rate |
| Existing 128 KiB MCU ring | 786.432 ms of decimated samples |
| SPI at 8 MHz | 1 MB/s ideal payload ceiling; raw data consumes 50% before overhead |
| SPI at 16 MHz | 2 MB/s ideal payload ceiling; raw data consumes 25% before overhead |

Three saved-buffer USB exports of 131,072 bytes measured **504,519–510,303 B/s** including
command/header time. Payload time was about 255 ms. See `usb_dump_benchmark.json`.
The line-coding setting was 115200: native USB CDC is not limited to a 115200-baud UART's
throughput. These tests did **not** run SPI capture concurrently. They show USB capacity,
not a proven continuous pipeline. Retain decimation: raw 500 KB/s export has little margin.

Short receive-only tests of the existing capture path:

| Requested SPI | Duration | Peak FIFO level | FIFO loss bits 0x30 |
| --- | ---: | ---: | --- |
| 8 MHz | 1 second | 224 bytes | clear |
| 16 MHz | 1 second | 160 bytes | clear |

Both returned approximately 166.6 KB/s after decimation. See `spi_capture_benchmark.json`.
These were capture-then-export tests, without an RF-quality comparison. Clock speed was
requested through the SDK, not scope-measured. The installed nRF52 SPI implementation
defaults to SPIM3 and supports 16/32 MHz. The LR20xx Rev 2.2 datasheet section 3.7.2 limits
SPI to **16 MHz**; upstream's 26.67 MHz is above that specification. Start within the limit.

## Useful upstream findings

The [live results](https://github.com/SAMS0N1TE/lr2021-sdr/blob/main/docs/live-streaming.md)
describe separate capture/consumer tasks using the nominal 1 KiB FIFO, including a
391-second session without reported FIFO loss or queue rejection. Their
[current ESP32-P4 bridge](https://github.com/SAMS0N1TE/lr2021-sdr/blob/main/firmware/esp32p4/README.md)
uses large SPI reads and an 8 MiB PSRAM output queue. Those results validate the architecture
on their board; they do not establish the nRF52840's scheduling margin.

## First implementation

1. Add a binary streaming mode alongside snapshots. Enter RX once; remain there until
   explicit stop or a detected error. Suspend ordinary mesh/UI/sensor work during streaming.
2. Give FIFO service highest priority, using bulk SPIM/EasyDMA transfers and preallocated
   buffers. Arduino bulk SPI already uses EasyDMA but waits synchronously; asynchronous
   completion or a dedicated task can overlap USB. Measure worst-case BUSY and service
   latency, with a target comfortably below the roughly 2 ms FIFO deadline.
3. A lower-priority consumer exports completed blocks. USB writes must never block the
   FIFO producer. Reuse the existing 128 KiB allocation as a queue to absorb temporary
   USB/host stalls; a longer frozen snapshot would still have readout gaps.
4. Frame blocks with format/rate, generation, first-sample index, sample count, known queue
   drops and integrity checks. FIFO overflow can lose an unknown number of samples:
   report a discontinuity and new generation instead of fabricating an exact loss count.
5. Use separate PC reader, disk and decoder queues. Initially record continuously and
   decode offline. Then add persistent preamble/header acquisition and filter state across
   chunk boundaries; restarting the current batch decoder on each USB block loses packets.

Acceptance requires sustained **simultaneous** SPI and USB operation, no FIFO/queue loss,
consecutive sample indices, and bounded maximum service latency. A known sequence provides
an extra end-to-end check; a tone alone cannot reveal every dropped whole cycle.

## Other avenues

**Hardware rate control:** the upstream
[raw-route setup](https://github.com/SAMS0N1TE/lr2021-sdr/blob/main/src/lr2021_iq.c)
supplies an input/filter code in byte 0 of 0x0290 and an explicit output rate. Our code uses
0xFF there; earlier rate-field tests had no effect. Their setup also changes packet/filter
configuration. A native 125 kpair/s IQ8 route could halve SPI traffic, but needs validation
with our active Semtech patch and hidden 0x0B mode. Preserve the LoRa channel bandwidth
and check filtering, actual rate, IQ orientation and payload CRC before relying on it.

**Radio SRAM:** the upstream
[bank investigation](https://github.com/SAMS0N1TE/lr2021-sdr/blob/main/docs/capture-engine.md)
infers finite banks up to 20 KiB without establishing a safe circular/ping-pong route.
At our raw rate, 8 KiB buys 16.384 ms and 20 KiB buys 40.960 ms. Some banks overlap patch
or working memory; our own active-bank reads stalled the chip. A larger bank helps only
after safe concurrent access or gap-free switching is established.

**PRAM:** our 4 KiB region is essentially occupied: 2240-byte patch plus 1840-byte generated
JLI table = 4080 bytes. Upstream's
[execution proof](https://github.com/SAMS0N1TE/lr2021-sdr/blob/main/docs/custom-firmware.md)
does not establish coexistence with the active vendor patch or sustained DSP service.
On-chip filtered decimation or a RAM-ring service would be interesting research, but is
not required to try continuous FIFO streaming first.

**Faster CPU/larger RAM:** first try a minimal bridge on this MCU. Upgrade if measured
scheduling, filtering or wider bandwidth exceeds its budget. More RAM absorbs temporary
stalls; it cannot fix an output link slower than the input indefinitely. PC RAM is the
natural place for long recordings.
