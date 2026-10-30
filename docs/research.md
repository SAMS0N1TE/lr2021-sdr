# Research and measurements

[Guides](README.md) / Research

Follow the measurements behind the features, or reproduce an experiment.

| Topic | Report |
| --- | --- |
| Capabilities, rates, and implementation overview | [Technical overview](technical-overview.md) |
| Raw receive commands and sample layout | [RX protocol](protocol.md) |
| Sustained reception | [Live-streaming results](live-streaming.md) |
| Tuning range, sample-rate ceiling, gain table, Bluetooth LE engine | [Receive range and gain](../research/rx-range-and-gain/README.md) |
| GPS L1 and 2.4 GHz receive checks | [GNSS and 2.4 GHz](../research/gnss-wifi/README.md) |
| Binary protocol checks on LakeShark's separate GPS module | [L76K](../research/l76k/README.md) |
| The same tests on the SX1262, for comparison | [SX1262 range](../research/sx1262-range/README.md) |
| Captured P25 call and replay | [Voice capture](voice-capture.md) |
| Finding the amplitude-preserving transmit path | [TX discovery](tx-discovery.md) |
| Using the working TX path | [TX playback](tx-playback.md) |
| Custom amplitude-and-phase payloads | [8-APSK experiment](../research/tx-payload/README.md) |
| Temperature packets decoded by rtl_433 | [Sensor-packet experiment](../research/tx-temperature/README.md) |
| P25 frames and reconstructed audio | [P25 TX experiment](../research/tx-p25/README.md) |
| Undocumented host opcodes, read-only probes, commands not to call | [Undocumented commands](undocumented-commands.md) |
| Debug console commands and the raw0b route on the board | [Debug console check](../research/debug-console-check/README.md) |

Related work: [beala/lr2021-re](https://github.com/beala/lr2021-re) maps the
LR2021 ROM 1.24 command router, the AGC registers, the PRAM patch and a
software receiver built on this repository's 0x0290 capture route. Its tools
and data are included in [`third_party/lr2021-re`](../third_party/lr2021-re),
with the [LoRa software receiver](lora-software-receiver.md),
[debug console](debug-console.md), [ROM and memory map](rom-and-memory.md)
and [undocumented commands](undocumented-commands.md) guides built on them.

The transmit reports include independent receiver results and reproduction tools. The [remaining TX work](tx-playback.md#remaining-scope) tracks what comes next.
