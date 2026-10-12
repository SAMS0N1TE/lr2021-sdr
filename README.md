# LR2021-SDR

**P25 voice, signal recording, and custom transmissions on a LoRa radio chip.**

[Start here](docs/getting-started.md) · [Explore the guides](docs/README.md) · [P25 in action](docs/p25.md) · [LakeShark](https://github.com/SAMS0N1TE/LakeShark)

[![LakeShark handheld radio workbench on the bench alongside other radio hardware](https://github.com/user-attachments/assets/bdd8069b-3c1a-4dd6-a821-1526e724c950)](https://github.com/SAMS0N1TE/LakeShark)

*The LakeShark radio workbench. LR2021-SDR brings access to the signal inside its LR2021 radio.*

The LR2021 is sold as a LoRa transceiver. This project opens up its raw signal path so software can receive and create other kinds of radio signals. It already powers **live P25 Phase 1 voice reception in LakeShark**, and the experimental transmit path now sends **P25 test-tone bursts, temperature packets, and custom waveforms**.

## What you can do

| Listen | Record | Transmit |
| --- | --- | --- |
| Hear P25 Phase 1 voice through the onboard LR2021 in LakeShark. | Save the radio signal itself, then replay it through a decoder or study it on a computer. | Upload a waveform and send it over the air, controlling both its strength and phase. |
| [Receive guide →](docs/receive.md) | [Recording guide →](docs/voice-capture.md) | [Transmit guide →](docs/transmit.md) |

## P25 is the proof

**A LoRa chip receiving digital voice—and now transmitting P25 voice-data bursts.**

Live reception produced audible P25 calls. A saved signal recording played back into voice again. On transmit, an independent receiver recovered **72 voice frames** and reconstructed the expected test tone.

[Follow the P25 story](docs/p25.md) · [Listen to the recovered TX tone](research/tx-p25/evidence/v13-p25-voice19200/recovered.wav)

## Make it yours

**Want a handheld?** Start with [LakeShark](https://github.com/SAMS0N1TE/LakeShark) on the LR2021 version of the LilyGO T-Display P4.

**Want to build with it?** This repo includes a portable receive library, USB capture firmware, waveform tools, and a separate LakeShark transmit integration. [Choose your starting point →](docs/getting-started.md)

Experimental, tested on LR2021 ROM 1.24. [Hardware and setup](docs/hardware.md) · [Current TX capabilities](docs/tx-playback.md#remaining-scope)

[**All guides**](docs/README.md) · [Developer guide](docs/developer-guide.md) · [Research and measurements](docs/research.md)

---

Inspired by [ESP-SDR](https://github.com/ESPARGOS/esp-sdr) and the ESPARGOS team's work opening raw radio access. Original LR2021 capture and hardware integration by [LakeShark](https://github.com/SAMS0N1TE/LakeShark). No ESP-SDR code is included. [GPL-3.0](LICENSE).
