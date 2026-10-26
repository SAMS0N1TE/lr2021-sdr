# Additional ROM 1.24 findings from another board

[Research index](../../docs/research.md)

Recorded 2026-10-26. This incorporates the report in
[LakeShark discussion #16](https://github.com/SAMS0N1TE/LakeShark/discussions/16)
by **sfowlr**, dated 2026-10-24. These are contributor-reported measurements,
not reproductions by this repository. No supporting command transcript or raw
capture was attached to that discussion when reviewed.

## What was reported

The test platform was a Meshnology W12 running LR2021 ROM 1.24.

| Area | Contributor's result |
| --- | --- |
| FSK rate selection | With routing byte zero at `FF`, the FSK `rx_bw` setting controls export rate; the reported relationship is `32 MHz / (mode * decimation)` |
| Export formats | Source 1 carries signed 8-bit components; source 2 carries signed 16-bit components. Both taps follow the channel filter |
| Throughput | Source 1 sustained approximately 664.8 thousand complex pairs/s using 16 MHz SPI, with no loss reported |
| Dynamic range | The 8-bit container saturated near -32/+31; source 2 showed at least 50 dB additional range in that setup |
| Manual gain | Apply the manual setting after entering RX; starting RX returned it to step 13 |
| TX selector 1 | Observed frequency offset followed `sample * fdev / 64`, across tested rates from 31.25 to 250 thousand pairs/s; repeated RX/TX transitions needed no reset |
| Native four-level FSK | Command `023B`, argument `01`, enabled a mode that transmitted 32 DMR bursts decoded without FEC errors |

The maintainer requested command sequences, implementation and capture evidence
in the discussion. Credit for these extensions belongs to sfowlr.

## What changes in our interpretation

Our **250 kpair/s clean source-2 result** remains a measurement of our IQ16
transport. It is not a universal LR2021 export ceiling. At 664800 complex pairs/s,
two bytes per pair require 1,329,600 B/s, or 66.48% of a 16 MHz SPI link's ideal
2,000,000 B/s. Four bytes require 2,659,200 B/s and cannot fit. Commands, BUSY,
polling and scheduling consume the remaining budget. This is calculated capacity,
not a reproduced loss-free stream.

The rate formula alone does not identify which `rx_bw` encodings select each
mode/decimation pair. Do not invent that table, source-1 route bytes or sample
order from the summary. Obtain them, then verify with positive/negative offset
tones and a nonrepeating sequence before changing an API or file format.

The TX result concerns **selector 1's frequency-deviation interpretation**. The
existing IQ playback/replay measurements used their own selectors, initialization
and waveforms. They are separate experiments. Do not replace the IQ replay rate
recommendation or call native 4-FSK an amplitude/phase I/Q transmitter.

## Reproduction order

1. Preserve board/module identity, ROM, cold/warm state, modem and exact command
   sequence. Keep portable source-2 behavior unchanged until comparison passes.
2. Run the same RF tone at matched analog bandwidth, gain and input level through
   both sources. Establish pair order, sign, scale, saturation and filter shape.
3. Test gain immediately before and after `SetRx`, then repeat after RX restart.
   Read back gain where supported and retain the actual command order.
4. Increase the FSK export rate in steps. Record raw byte counts, FIFO flags,
   occupancy and longest service gap. Verify sample ordering with a known
   sequence, including under UI/audio load and through FIFO wrap.
5. Compare reset-based recovery with warm reconfiguration. Require successful
   post-test reception and unchanged radio ownership after each trial.
6. Treat selector-1 scaling and native 4-FSK as separate bench experiments with
   an independent receiver. This change adds no TX command or automatic test.

The immediate opportunities are [GNSS and 2.4 GHz receive research](../gnss-wifi/README.md).
