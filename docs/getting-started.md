# Getting started

[Guides](README.md) / Getting started

LR2021-SDR gives software access to the radio signal before it becomes decoded packets or audio. That signal is stored as I/Q samples: a recording of its changing amplitude and phase. Software can then interpret it as P25, inspect its spectrum, or generate a waveform for transmission.

## Use it in a handheld

[LakeShark](https://github.com/SAMS0N1TE/LakeShark) is the starting point for listening on the LR2021 version of the LilyGO T-Display P4. Its onboard radio can receive P25 Phase 1 voice, with the screen and speaker providing the interface.

Follow [LakeShark's setup instructions](https://github.com/SAMS0N1TE/LakeShark#start-here), then the [receive guide](receive.md). Fit an antenna appropriate for the frequency you want to receive.

## Capture signals over USB

The [standalone ESP32-P4 firmware](../firmware/esp32p4/README.md) exposes capture commands over USB. It includes the LilyGO board adapter. Build the [host tools](developer-guide.md) to save captures and inspect samples on your computer.

## Try transmission

Use the separate [LakeShark TX integration](tx-playback.md#integrating-the-source). The [transmit guide](transmit.md) introduces the P25, temperature-packet, and custom-waveform examples; the [playback reference](tx-playback.md) supplies installation steps and commands.

## Build your own application

Start with the [developer guide](developer-guide.md). The portable C11 library provides raw reception, with board-specific hooks for SPI, reset, and timing. The TX integration and host tools are provided separately.

Next: [Hardware](hardware.md) · [The P25 story](p25.md)
