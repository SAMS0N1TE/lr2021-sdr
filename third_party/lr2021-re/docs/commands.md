# LR2021 host commands: routing and undocumented opcodes

Compared against LR20xx datasheet Rev 2.2. Tags: **[HW]** = verified on hardware,
**[RE]** = read from ROM code, **[guess]** = plausible, unverified.

## Routing [RE]

- The host command router (ROM 0x23F4) looks at the opcode's high byte:
  - **0x01** goes to the system table at 0x13BEC (55 slots, 0x0100–0x0136);
  - **0x02** goes to the radio table at 0x13CD0 (168 slots, 0x0200–0x02A7);
  - anything else is rejected.

  There are no other command groups. The FIFO opcodes 0x0001/0x0002 are handled
  outside this router.
- Radio commands pass through the dispatcher at 0x24E0, which does
  `ld.as r12,[0x13CD0, op & 0xFF]`. Semtech's PRAM patch wraps this dispatcher (JLI index 61).
- Invalid-opcode stubs: 0xD56C (system) and 0xD568 (radio). 9 system slots and 38 radio slots
  point at them.
- `analysis/opcode_table.txt` lists every slot as DOC / UNDOC / STUB, with its handler address,
  datasheet name and RadioLib name. **38 opcodes have real handlers but aren't documented.**
- `analysis/undoc_fingerprints.txt` and `analysis/register_fingerprints.txt` list, for each
  handler, the length check, the registers and RAM it touches, and its callees.

## Handler ABI [RE]

```
r0 = parameter byte count
r1 = pointer to parameter bytes
r2 = response struct: +0 response length (bytes), +4 response buffer pointer
return 0/1 = error (CMD_FAIL / CMD_PERR), 2 = OK, 3 = OK with response data
```

Multi-byte parameters are big-endian. Handlers read them with `read_be24` (0x23B0) and
`read_be32` (0x23C0). The PRAM patch's own handlers use the same ABI.

## System group (0x01xx), undocumented

| Opcode | Handler | What it does | Status |
|---|---|---|---|
| 0x0102 | 0x12058 | **Write CPU auxiliary register.** Params `aux(u32) value(u32)` → `sr value,[aux]` through `guarded_sr` (0xD80C) | [RE] |
| 0x0103 | 0xD388 | **Read CPU auxiliary register.** Params `aux(u32)`, returns 4 bytes, through `guarded_lr` (0xD798). A bad aux returns -707 cleanly | [HW] |
| 0x012B | 0x11564 | 12-way debug/test multiplexer. The first param byte selects the case: force IRQ bits (0xF2A008, masked by RAM 0x80060C), set that mask, DIO bits through 0xF2A004, and others | [RE] |
| 0x012C | 0xF800 | **Program hardware actionpoints** (aux 0x220/0x221/0x222 + 3n = match/mask/control, n = 0..1), with exception handler 0x236C. This is a breakpoint / ROM-patch mechanism | [RE] |
| 0x012D | 0x10AC4 | ActivatePram. RadioLib uses it; the datasheet doesn't list it. With extra params it **dispatches sub-commands into a PRAM-provided table** (pointer at RAM 0x8007F0, count at [0x8007F4]+4), so a PRAM blob can add its own commands | [RE] |
| 0x012F | 0xE958 | Writes 0xF2F020 with a selector 0x00–0x5F. Selector 0xFF reconfigures pads (0xF24008 / 0xF02008). Looks like an internal test bus / signal mux, probably routable to DIOs | [guess] |
| 0x0134 / 0x0135 | 0x2D2C / 0x2D04 | Use the same frequency-list parser as CalibFE (`parse_freq_list`, 0x5B88), then run calibration 0x7114 with stored params (RAM 0x80064A / 0x800648). Touch 0xF30154 (AGC), 0xF40140/0xF40148 (synth) and 0xF40024. Probably other per-frequency calibrations | [RE] |
| 0x0136 | 0x3284 | CalibFE (0x0123) with a leading **block-mask byte** (default 0x3F = all blocks) | [RE] |

## Radio group (0x02xx), undocumented

### Capture and raw data path

This command family was originally discovered by
[SAMS0N1TE/lr2021-sdr](https://github.com/SAMS0N1TE/lr2021-sdr); see [raw-iq.md](raw-iq.md).

| Opcode | Handler | What it does | Status |
|---|---|---|---|
| 0x0290 | 0xF9A0 | **Reconfigure the RX data DMA**: what gets written to RAM, and where. Params `trig(1, FF = none) rate/start(u32) cfg(1) 0 0 len(u16) [fifo_enable(1)]`. `cfg` = size[7:5] \| mode[4:3] \| src[2:0]. Mode 0 = stop (gives the RAM bank back to the CPU), mode 1 = route to the RX FIFO, mode 2 = one-shot to RAM. Size 1–3 → base 0x801000 (overwrites PRAM!), 4–5 → 0x802000 (overwrites live data), 6 → 0x804000. Sources 0–6 have 2,2,4,4,4,4,2 bytes/sample (table 0x13948). Boot default: mode 2, source 7 (packet data), base 0x804000 | [HW] partly |
| 0x0291 | 0x5B44 | Capture write pointer (u16, relative to the base) | [HW] |
| 0x0292 | 0x5B60 | Read captured bytes: `offset(u16) len(1)` from RAM 0x800000 + base + offset, bounded at 0x6000 | [HW] |
| 0x0293 / 0x0294 | 0xFA78 / 0xFA18 | TX counterpart: switch the modulator input (0xF30904 bits 4/5) and packet-engine source (0xF30800 bits 10/11), with length and start time. Probably **transmit straight from a RAM buffer**. lr2021-sdr uses 0x0293 for TX | [guess] |

### Modem and packet-engine knobs

| Opcode | Handler | Registers | What it does | Status |
|---|---|---|---|---|
| 0x0213 | 0xC860 | struct 0x800438 | Get/set internal parameters, sub-index 0x0B–0x15. `rc 213 1 0B` → 04 | [HW] |
| 0x0214 | 0xF86C | via 0x46D0: 0xF02004, 0xF02018, 0xF23050, 0xF2A00C, 0xF30128 | 1-byte param; clock / IRQ block configuration | [RE] |
| 0x021F | 0xF144 | 0xF30A30 | LoRa modem register not in the datasheet | [RE] |
| 0x0226 | 0x4464 | 0xF30A1C | LoRa; also used by SetLoraPacketParams. Takes ≥ 6 bytes of params | [RE] |
| 0x022D | 0xF3FC | 0xF30A14, 0xF30A68, 0xF30B00, 0xF30018 | LoRa sync/detection | [RE] |
| 0x022E | 0xF38C | same as 0x022D | LoRa sync/detection, up to 3 × u16 | [RE] |
| 0x022F | 0xE404 | 0xF30C28[28:24], [22:21], [20]; RAM 0x8004C0 | **AGC configuration**. Sits next to SetAgcGainManual | [RE] |
| 0x0230 | 0xE1F4 | 0xF30814 | generic packet engine (FSK/OOK/BLE/Z-Wave) register write | [RE] |
| 0x0231 | 0xE27C | 0xF30818 | 〃 | [RE] |
| 0x0232 | 0xE358 | 0xF30828 | 〃 | [RE] |
| 0x0233 | 0xE170 | 0xF3081C | 〃 | [RE] |
| 0x0234 | 0xDACC | 0xF30830 / 0xF30834 | 〃 | [RE] |
| 0x0235 | 0xDA08 | 0xF30844 | 〃 | [RE] |
| 0x0236 | 0xE43C | 0xF30854 | 〃 | [RE] |
| 0x0237 | 0xE03C | 0xF30838 | 〃 | [RE] |
| 0x0238 | 0xE2B4 | 0xF30858 | 〃 | [RE] |
| 0x0239 | 0xD950 | 0xF30864 | 〃 | [RE] |
| 0x023A | 0xD9C0 | 0xF3085C | 〃 | [RE] |
| 0x023B | 0xE00C | 0xF30930 | 〃 | [RE] |
| 0x023C | 0xDF24 | 0xF30904 / 0xF30934 | 〃 | [RE] |
| 0x023D | 0xE0D8 | 0xF30800 | 〃 | [RE] |
| 0x023E | 0x5430 | RAM 0x8004B8 | getter, 6 bytes; all zero in LoRa RX | [HW] |
| 0x023F | 0x539C | RAM 0x8004C4 | getter, 18 bytes; all zero in LoRa RX | [HW] |
| 0x0263 | 0xE520 | | no-op returning 0 (a removed BLE command?) | [RE] |

0x0230–0x023D are mostly shadowed in RAM 0x800438–0x8004A8. They look like lower-level
versions of the documented FSK whitening / CRC / sync word / address commands.

## Hidden data in documented commands

| Command | Finding | Status |
|---|---|---|
| GetLoraPacketStatus (0x022A, handler 0x57E8) | **Returns 10 bytes.** Bytes 6–8 are freq_offset (documented in Rev 2.2, but RadioLib reads only 6 bytes). **Byte 9 = AGC gain step G1..G13** latched during the packet. See [agc-gain.md](agc-gain.md) | [HW] |
| SetPacketType (0x0207) | Type **0x0B** ("RFU" in the datasheet) is a raw/capture mode. See [raw-iq.md](raw-iq.md) | [HW] |
| ReadRegMem32 (0x0106) | A guarded CPU load that reads ROM, PRAM and RAM, not just registers. Unmapped addresses stall the chip | [HW] |

## Behavior of documented commands, measured on hardware

These came up while building a MeshCore driver and a dual-band scanner on RadioLib 7.x.

- **CAD exit mode:** CAD_RX is 0x01 (datasheet Table 6-18 agrees). RadioLib's
  `RADIOLIB_LR2021_CAD_EXIT_MODE_RX` was 0x02.
- **CAD-RX timeout tick:** 32.768 kHz, not the 32 MHz the datasheet text implies.
- **CalibFE:** stores up to 3 frequency calibrations. Calibrating both bands once is enough
  for band switching; recalibrating on every switch costs about 10 ms.
- **SetRxPath:** costs about 4.8 ms whenever the RX boost level changes. Use the same boost
  on both paths if you switch bands.
- **Band switch:** standby XOSC → SetRfFrequency → SetRxPath → SetLoraModulationParams →
  CAD takes about 1.2 ms, at 160–190 µs per SPI command.
- **SetPaConfig vs SetTxParams:** switching from the sub-GHz PA at +20 dBm to the HF PA
  (max +12 dBm) is rejected with CMD_FAIL. Lower the power first.
- **Preamble length:** an RX preamble setting shorter than the transmitter's (RX 8, TX 32)
  gives header CRC errors on almost every packet.
- **Warm sleep:** keeps PRAM (the patch's install routine just restores `jli_base` on wake)
  but loses register configuration. Recalibrate after waking.
