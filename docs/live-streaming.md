# Live I/Q streaming

LakeShark's ESP32-P4 integration keeps the LR2021 receiver running while a
separate task processes I/Q and feeds the P25 decoder. The previous 250 ms
capture/reset/decode cycle is removed from this live path.

![Measured session duration and FIFO occupancy](images/live-streaming.png)

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

## Limits

These measurements establish sustained capture and decoder-input processing
without reported loss under the tested load. FIFO flags and software counters
do not independently prove sample-perfect RF continuity. A known nonrepeating
RF sequence is still needed. No valid voice call arrived during these runs;
matched P25 recordings and audio-quality comparisons remain unverified.

[Measurement data](data/live-streaming.json) contains only the session metrics.
Recreate the figure with Python and matplotlib:

```sh
python tools/plot_stream_results.py
```

See [portable API](continuous.md) and [short FIFO comparisons](extended-fifo.md).
