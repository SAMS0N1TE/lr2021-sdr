<div align="center">

<h1 align="center">LR2021-SDR</h1>

### P25 voice, signal recording, and custom transmissions on a LoRa radio chip

<br>

[![License: GPL-3.0](https://img.shields.io/badge/license-GPL--3.0-blue?style=for-the-badge)](LICENSE)
[![Status: Experimental](https://img.shields.io/badge/status-experimental-orange?style=for-the-badge)](docs/tx-playback.md#remaining-scope)
[![LR2021 ROM 1.24](https://img.shields.io/badge/LR2021-ROM%201.24-informational?style=for-the-badge)](docs/hardware.md)
[![P25 Phase 1](https://img.shields.io/badge/P25-Phase%201%20voice-success?style=for-the-badge)](docs/p25.md)

<br>

**[Start here](docs/getting-started.md)** &nbsp;•&nbsp;
**[Explore the guides](docs/README.md)** &nbsp;•&nbsp;
**[P25 in action](docs/p25.md)** &nbsp;•&nbsp;
**[LakeShark](https://github.com/SAMS0N1TE/LakeShark)**

<br>

[![LakeShark handheld radio workbench on the bench alongside other radio hardware](https://github.com/user-attachments/assets/bdd8069b-3c1a-4dd6-a821-1526e724c950)](https://github.com/SAMS0N1TE/LakeShark)

<sub><i>The LakeShark radio workbench. LR2021-SDR brings access to the signal inside its LR2021 radio.</i></sub>

</div>

<br>

<div align="center">

The [Semtech LR2021](https://www.semtech.com/products/wireless-rf/lora-plus/lr2021) is sold as a LoRa transceiver.<br>
This project opens up its **raw signal path** so software can receive and create other kinds of radio signals.

It already powers **live P25 Phase 1 voice reception in LakeShark**,<br>
and the experimental transmit path now sends **P25 test-tone bursts, temperature packets, and custom waveforms**.

</div>

<br>

<h1 align="center">What you can do</h1>

<div align="center">

| Listen | Record | Transmit |
| :---: | :---: | :---: |
| Hear P25 Phase 1 voice through the onboard LR2021 in LakeShark. | Save the radio signal itself, then replay it through a decoder or study it on a computer. | Upload a waveform and send it over the air, controlling both its strength and phase. |
| **[Receive guide →](docs/receive.md)** | **[Recording guide →](docs/voice-capture.md)** | **[Transmit guide →](docs/transmit.md)** |

</div>

<br>

<h1 align="center">P25</h1>

<div align="center">

### A LoRa chip receiving digital voice, and now transmitting P25 voice-data bursts.

Live reception produced audible P25 calls.<br>
A saved signal recording played back into voice again.<br>
On transmit, an independent receiver recovered **72 voice frames** and reconstructed the expected test tone.

<br>

[![Received P25 call showing I/Q waveform, signal envelope, spectrum and waterfall](docs/images/voice-four-panel-purple.png)](docs/voice-capture.md)

<sub>A one-second P25 call recording at 154.785 MHz. These same samples replayed into <b>27 IMBE voice frames</b>.</sub>

<br>

**[Follow the P25 story →](docs/p25.md)** &nbsp;•&nbsp; **[Listen to the recovered TX tone](research/tx-p25/evidence/v13-p25-voice19200/recovered.wav)**

</div>

<br>

<h1 align="center">Receiver measurements</h1>

<div align="center">

[![Three LR2021 receive sessions showing duration and peak FIFO occupancy](docs/images/live-streaming-results.png)](docs/live-streaming.md)

<sub>Three separate receive sessions, up to <b>391 seconds</b>, with no reported FIFO loss or queue rejection.<br>These runs measure capture continuity while decoder input processing stays active.</sub>

<br>

**[Live-streaming results →](docs/live-streaming.md)**

</div>

<details>
<summary><b>Sample rates and known-tone checks</b></summary>

<br>

<div align="center">

### Sample rate versus LoRa bandwidth

[![Measured complex sample rates follow four times the configured LoRa bandwidth](docs/images/sample-rates.png)](docs/technical-overview.md#hardware)

<sub>Known-tone estimates for four tested bandwidths, from about 125,000 to 1,000,000 complex samples per second.<br>These are short-window measurements, not continuous-streaming rate limits.</sub>

<br>

### I/Q orientation with known RF tones

[![Captured I/Q samples and spectra of positive and negative RF-offset test tones](docs/images/tone-capture.png)](docs/technical-overview.md#known-tone-checks)

<sub>Separate +25 kHz and −15 kHz test tones verify frequency direction in the captured I/Q.<br>Spectra are normalized separately; amplitude is uncalibrated.</sub>

<br>

**[Measurements and setup →](docs/technical-overview.md#known-tone-checks)**

</div>

</details>

<br>

<h1 align="center">Make it yours</h1>

<div align="center">

| Want a handheld? | Want to build with it? |
| :---: | :---: |
| Start with **[LakeShark](https://github.com/SAMS0N1TE/LakeShark)** on the LR2021 version of the LilyGO T-Display P4. | This repo includes a portable receive library, USB capture firmware, waveform tools, and a separate LakeShark transmit integration. |
| | **[Choose your starting point →](docs/getting-started.md)** |

</div>

<br>

<div align="center">

*Experimental, tested on LR2021 ROM 1.24.*<br>
[Hardware and setup](docs/hardware.md) &nbsp;•&nbsp; [Current TX capabilities](docs/tx-playback.md#remaining-scope)

<br>

**[All guides](docs/README.md)** &nbsp;•&nbsp; **[Developer guide](docs/developer-guide.md)** &nbsp;•&nbsp; **[Research and measurements](docs/research.md)**

<br>

---

<sub>Inspired by [ESP-SDR](https://github.com/ESPARGOS/esp-sdr) and the ESPARGOS team's work opening raw radio access.<br>
Original LR2021 capture and hardware integration by [LakeShark](https://github.com/SAMS0N1TE/LakeShark).<br>
No ESP-SDR code is included. [GPL-3.0](LICENSE).</sub>

</div>
