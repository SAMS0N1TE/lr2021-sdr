# Research and measurements

[Guides](README.md) / Research

Follow the measurements behind the features, or reproduce an experiment.

| Topic | Report |
| --- | --- |
| Capabilities, rates, and implementation overview | [Technical overview](technical-overview.md) |
| Raw receive commands and sample layout | [RX protocol](protocol.md) |
| Sustained reception | [Live-streaming results](live-streaming.md) |
| Tuning range, sample-rate ceiling, gain table, Bluetooth LE engine | [Receive range and gain](../research/rx-range-and-gain/README.md) |
| sfowlr's W12 findings and our raw-IQ follow-up on LakeShark | [ROM 1.24 raw-IQ follow-up](../research/community-2026-10/README.md) |
| Finite source-4 SRAM capture, repeatable pattern, and bandwidth-dependent suppression | [Source-4 SRAM measurements](../research/source4-sram/README.md) |
| Wider source-0 passband at the same 576k-pair/s output rate | [Source-0 wider profile](../research/source0-wide-profile/README.md) |
| Source-6 IQ packing, gain tags, and FM/FSK classification | [Source-6 classifier](../research/source6-classifier/README.md) |
| GPS L1 and 2.4 GHz receive checks | [GNSS and 2.4 GHz](../research/gnss-wifi/README.md) |
| Binary protocol checks on LakeShark's separate GPS module | [L76K](../research/l76k/README.md) |
| The same tests on the SX1262, for comparison | [SX1262 range](../research/sx1262-range/README.md) |
| Captured P25 call and replay | [Voice capture](voice-capture.md) |
| Finding the amplitude-preserving transmit path | [TX discovery](tx-discovery.md) |
| Using the working TX path | [TX playback](tx-playback.md) |
| Custom amplitude-and-phase payloads | [8-APSK experiment](../research/tx-payload/README.md) |
| Temperature packets decoded by rtl_433 | [Sensor-packet experiment](../research/tx-temperature/README.md) |
| P25 frames and reconstructed audio | [P25 TX experiment](../research/tx-p25/README.md) |

The transmit reports include independent receiver results and reproduction tools. The [remaining TX work](tx-playback.md#remaining-scope) tracks what comes next.
