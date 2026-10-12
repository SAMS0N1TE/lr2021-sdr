# Create and transmit waveforms

[Guides](README.md) / Transmitting

**Upload a signal, then play it through the LR2021 transmitter.** The working path preserves changes in both amplitude and phase, giving software control over the shape of the transmitted signal.

## Try an example

| Example | What happened | Explore |
| --- | --- | --- |
| P25 digital voice | An independent receiver recovered 72 voice frames and reconstructed the test tone. | [P25 walkthrough](p25.md) |
| Temperature packets | rtl_433 decoded readings of 23.4, -12.3, and 37.8 °C, plus sensor IDs and status. | [Generator and received packets](../research/tx-temperature/README.md) |
| Custom amplitude and phase | A two-ring waveform delivered 235 valid frames containing 15,040 payload bits with zero bit errors. | [Payload experiment](../research/tx-payload/README.md) |

These examples give you a starting point for packet generators, decoder test signals, and experiments with modulation that the chip's normal packet interface does not provide.

## Use it in LakeShark

Install the [TX integration](tx-playback.md#integrating-the-source), generate an I/Q file, and use the upload tool to send it to the board. The [playback guide](tx-playback.md) contains the full commands and example recipes.

The current integration plays finite bursts at six sample rates, up to 125,000 complex samples per second. It uses a 915 MHz test configuration and supports bursts up to 900 ms, subject to buffer limits. Continuous host streaming and microphone voice encoding are [next development steps](tx-playback.md#remaining-scope).

Next: [P25](p25.md) · [Hardware](hardware.md) · [TX discoveries](tx-discovery.md)
