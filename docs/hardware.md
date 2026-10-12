# Hardware and firmware

[Guides](README.md) / Hardware

The tested platform is the **LilyGO T-Display P4 with an LR2021 running ROM 1.24**. Check the radio variant: the SX1262 version uses a different chip.

| Your goal | Use |
| --- | --- |
| Listen through a handheld interface | [LakeShark](https://github.com/SAMS0N1TE/LakeShark) |
| Capture samples over USB | [Standalone ESP32-P4 firmware](../firmware/esp32p4/README.md) |
| Play uploaded TX waveforms | [LakeShark TX integration](tx-playback.md#integrating-the-source) |
| Connect another host or board | [Portable library and board hooks](porting.md) |

Choose the antenna for the operating band. The live P25 reception work used an external VHF antenna; the TX experiments used 915 MHz. The [TX reference](tx-playback.md) records the frequency, power, and sample limits used by that integration.

Independent TX measurements used a HackRF receiver. It records the transmitted RF for decoding and comparison with the source waveform.

Next: [Receiving](receive.md) · [Transmitting](transmit.md) · [Getting started](getting-started.md)
