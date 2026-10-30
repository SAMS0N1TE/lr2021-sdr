# LR2021 AGC gain readout

The LR2021 receiver has 13 gain steps (G1..G13, plus boost levels on G12/G13). The datasheet
documents `SetAgcGainManual` (0x021A) for forcing a step, but there is no documented way to
read which step the AGC chose, except RTToF results type 2 (ranging only). Knowing the step
for each packet helps diagnose deployments: it shows whether a node is being desensed by a
strong nearby signal.

## Registers [HW]

| Register | Field | Meaning |
|---|---|---|
| 0xF30154 | [23:20] | **current gain step** G1..G13. Reads 13 in auto mode with no signal |
| 0xF30154 | [19:16] | manual setting written by SetAgcGainManual; 0 = automatic |
| 0xF3012C | [26:18] | gain value per step: 16, 28, 38, 49, 57, 64, 73, 85, 96, 108, 120, 127, 138 for G1..G13 (139 idle in auto). Monotonic; probably total gain in 0.5 dB units |
| 0xF301A8 | [26:18] | instantaneous RSSI. GetRssiInst (ROM 0x5C6C) just returns this field, so RSSI gain compensation happens in hardware |

`SetAgcGainManual` (handler 0xE4A8 → `set_agc_gain_manual` 0xE47C) writes g to RAM 0x8007BD
and to 0xF30154[19:16].

How it was found: in continuous LoRa RX, force g = 1..13, dump the 0xF301xx block after each
step, and diff the dumps (`logs/gain_sweep.txt`). The [23:20] field followed g exactly.

## Per-packet gain: GetLoraPacketStatus byte 9 [HW]

The ROM already latches the gain for every packet. At the RX event (ROM 0x8074, IRQ status
bit 7) it copies 0xF30154[23:20] into the LoRa packet status struct at RAM 0x8003C0 + 0xC.

**GetLoraPacketStatus (0x022A) returns 10 data bytes**; RadioLib reads only the first 6.

| Byte | Meaning |
|---|---|
| 0–5 | documented fields (see the datasheet; byte 1 is the payload length) |
| 6–8 | freq_offset, signed 24-bit. Documented in Rev 2.2, but RadioLib doesn't read it |
| **9** | **AGC gain step G1..G13 latched during the packet** (undocumented) |

Semtech's PRAM patch doesn't override 0x022A, so this works with or without the patch.

```
TX: 02 2A
RX: status(2)  b0 b1 b2 b3 b4 b5 b6 b7 b8 b9
```

## Live verification

All on 910.525 MHz / BW 62.5 kHz / SF7 / CR 4/5.

- **Auto gain** (`logs/rx_auto.txt`): 15 packets from one neighbor at -93.5 dBm, SNR ~13.5 dB.
  Byte 9 was 13 every time (weak signal → maximum gain).
- **Forced G5** (`logs/rx_g5.txt`): byte 9 was 5 on all 9 packets. SNR fell to about -6 dB,
  and reported RSSI rose to -86.5/-87.5 dBm, 6 dB higher than the same node at G13. So
  per-step RSSI calibration is not exact at low gain.
- **Close transmitter**: in raw IQ capture (see [raw-iq.md](raw-iq.md)), automatic gain went
  from G13 during quiet periods to G1 during a nearby RAK3401's transmissions, then back to
  G13.

Each line in the logs shows the length, status, RSSI and SNR, then the raw 10 status bytes,
then `gain`.

## Using it

- MeshCore: branch `beala/lr2021-rx-gain-step` on
  [beala/MeshCore](https://github.com/beala/MeshCore) reads byte 9 after each packet and reports it
  as `last_gain` in the radio stats.
- Any driver: issue 0x022A with a 10-byte read instead of 6.
- To watch the current gain without a packet, read 0xF30154 with ReadRegMem32 and take
  bits 23:20.

## Open questions

- Characterize the intermediate auto gain steps and the boost levels on G12/G13.
- Meaning of 0xF30154[14:10], and of the status structs at 0x8002E8 / 0x8003F0 (FSK/FLRC?).
