# Live I/Q streaming

LakeShark's ESP32-P4 integration keeps the LR2021 receiver running while a
separate task processes I/Q and feeds the P25 decoder. The previous 250 ms
capture/reset/decode cycle is removed from this live path. Live P25 Phase 1
C4FM voice is confirmed at 154.7850 MHz. A recording from the live sample
stream also replayed to PCM audio: [27 IMBE frames played, none muted](voice-capture.md).

![Measured session duration and FIFO occupancy](images/live-streaming-results.png)

LR2021 ROM 1.24, 154.785 MHz, external VHF antenna, nominal 125 ksample/s,
1024-byte FIFO. UI and wireless services remained enabled. Receive only.

| Session | Duration s | Accepted KiB | Peak FIFO bytes | Largest service gap us |
| --- | --- | --- | --- | --- |
| A | 226.956 | 110816 | 820 | 1628 |
| B | 192.214 | 93852 | 792 | 1527 |
| C | 391.148 | 190987 | 808 | 1537 |

All three sessions ended on request with FIFO flags 0x04, zero queue
rejections and zero consumer errors. The loss mask is 0x30. Accepted KiB are
rounded down. These are separate runs, not one uninterrupted recording.
During run C, observed block counts advanced from 131987 to 167678 with
zero reported gaps, rejected blocks or FIFO-loss events.

## Scheduling

- A dedicated capture task drains the observed FIFO backlog in SPI reads of
  at most 256 bytes and publishes checked blocks into an eight-slot queue.
- The capture task yields after draining a small backlog so the consumer and
  system tasks can run. Extending the FIFO alone was insufficient.
- The consumer preserves resampler, DSP and decoder state across blocks.
- Overflow, queue rejection or retuning ends the session and resets acquisition.
  Missing samples are not concealed.

The portable C library provides checked sessions up to 60 seconds and the
bounded queue. The unbounded live task integration is in LakeShark; it is
not a drop-in board adapter included in this repository.

## Voice observation

LakeShark decoded Phase 1 C4FM voice from continuous LR2021 raw I/Q at
154.785 MHz.

The observed snapshot showed 101 sync detections, 78 voice frames, 105 passing
BCH checks, 515 failed BCH checks, zero reported capture gaps, zero FIFO loss,
zero read errors and zero audio drops. The totals include idle-channel activity.
One 11-second call reported 60 passing and 2 failed BCH checks, 450 clean IMBE
frames, zero audio drops and 38 audio underruns. A separate 1.5-second call
reported 63 clean IMBE frames, zero drops and 19 underruns.

The [voice-triggered capture](voice-capture.md) records the same checked I/Q
blocks supplied to the live decoder. Its 500,000-byte export passed CRC32
verification and replayed through the frontend, P25 decoder and vocoder to
PCM audio: 3 voice LDUs, 27 IMBE frames played, none muted or repeated.
The post-capture live snapshot reported zero capture gaps and FIFO loss.

[Measurement data](data/live-streaming.json) contains only the session metrics.
Recreate the figure with Python and matplotlib:

```sh
python tools/plot_stream_results.py
```

See [portable API](continuous.md) and [short FIFO comparisons](extended-fifo.md).
