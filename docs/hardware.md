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

## Waveshare Core2021-XF

The [Core2021-XF](https://docs.waveshare.com/Core2021-XF) is an LR2021 module with a **32 MHz crystal, not a TCXO**. Run it with TCXO off. Asked for TCXO mode, the chip reports HF_XOSC_START and PLL_LOCK (GetErrors bits 0 and 2). The firmware and host library then reset the radio and start it on the crystal, and `info` shows `clock=crystal (TCXO did not start)`.

A crystal is less accurate than a TCXO, so each module needs its own frequency correction. One unit measured 37.2 ppm slow: -34 kHz at 910.525 MHz. At LoRa BW 62.5 kHz every packet failed its header until a -37.2 ppm correction was set. The value is the reference error, + when it runs fast. Every receive and transmit frequency is programmed as `f / (1 + ppm / 1e6)`.

- Firmware: `board set ppm -37.2`, then `board save` and reboot, or `CONFIG_LRIQ_FREQ_PPB`.
- Host on SPI: `ppm=-37.2`.

The module has no RF switch. Each band has its own antenna pad (2.4 GHz and sub-GHz) behind a passive match, so no `rfsw` table is needed. DIO7, DIO8 and DIO11 only go to module pads.

Wiring on a Waveshare ESP32-P4-NANO, IRQ not wired (the firmware polls over SPI):

| Module pad | ESP32-P4 GPIO |
| --- | --- |
| RESET | 23 |
| BUSY | 5 |
| MISO | 4 |
| NSS | 20 |
| SCK | 21 |
| MOSI | 22 |
| 3V3, GND | 3V3, GND |
| DIO7, DIO8, DIO11 | not connected |

Set it on the console with `board preset core2021-xf` (keeps the saved ppm), then `board save` and reboot. To build it in, add [`sdkconfig.defaults.core2021-xf`](../firmware/esp32p4/sdkconfig.defaults.core2021-xf); the file has the command. Check the P4 revision first: the stock build targets revisions below 3.

The crystal mode and the correction were measured with LakeShark firmware on one unit. This repository's Core2021-XF profile has not run on hardware yet.

Next: [Receiving](receive.md) · [Transmitting](transmit.md) · [Getting started](getting-started.md)
