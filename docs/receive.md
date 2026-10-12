# Listen, record, and replay

[Guides](README.md) / Receiving

**LakeShark decodes live P25 Phase 1 voice using the LR2021's raw signal output.** The radio supplies samples, software decodes the call, and the handheld plays the audio.

![Measured spectrum and waterfall of a P25 voice recording received through the LR2021](images/voice-iq-hero-purple.png)

## Save more than audio

An I/Q recording preserves the received signal so you can run it through a decoder again. That makes it useful for investigating an unfamiliar signal, improving a decoder, or comparing processing changes against exactly the same recording.

The voice-capture experiment saved one second of signal while reception continued. Replaying that recording recovered 27 voice frames, with none muted. [See the recording and replay results](voice-capture.md).

## Keep receiving

LakeShark's live integration ran for 391 seconds without reported FIFO loss or queue rejection. The [live-streaming report](live-streaming.md) explains the checked capture path and measured results.

For USB capture on a computer, use the [standalone firmware](../firmware/esp32p4/README.md) and [host tools](developer-guide.md). For your own board, see [porting](porting.md).

Next: [P25](p25.md) · [Console commands](console.md) · [Continuous capture](continuous.md)
