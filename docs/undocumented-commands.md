# Undocumented LR2021 commands

[Guide hub](README.md) / Undocumented commands

Host opcodes that ROM 1.24 implements but the LR20xx datasheet (Rev 2.2) does
not list, beyond the 0x0290 to 0x0294 capture and TX family this repository
already uses ([capture engine](capture-engine.md), [TX discovery](tx-discovery.md)).
The opcode list, handler addresses and descriptions come from
[beala/lr2021-re](https://github.com/beala/lr2021-re/blob/main/docs/commands.md)
by Alex Beal (MIT), which in turn credits this repository for 0x0290 to 0x0293.

Tags follow lr2021-re: **[HW]** verified on this repository's LilyGO
T-Display-P4 LR2021 (ROM 1.24), **[RE]** read from ROM or taken from lr2021-re
and not yet run here, **[guess]** unverified. Where lr2021-re has its own [HW]
result, the entry says so, but the tag here stays [RE] until it runs on this
board.

## Rules for this repository

The purpose-built probes below only use commands that leave the chip's state
unchanged. The [debug console](debug-console.md) also has lr2021-re's raw
access (`mr`, `mw`, `rc`), which can send any opcode, including the ones in
[Do not call](#do-not-call); those are documented from lr2021-re only and
were not run here.

## Read-only probes

The bridge firmware's console command `undocprobe [all|aux|param|getters]`
([debug console](debug-console.md)) runs these probes. It reapplies the saved
receive configuration when it finishes.

| Opcode | Probe | What lr2021-re reports | Status here |
| --- | --- | --- | --- |
| 0x0103 | aux 0x290 (`jli_base`), 4 bytes back | Read CPU auxiliary register through `guarded_lr` (0xD798); a bad aux returns -707 cleanly. [HW] in lr2021-re | [HW]: reads 0x00000100 with data status |
| 0x0213 | one parameter byte, sub-index 0x0B to 0x15, 1 byte back | Get/set internal parameters in the struct at 0x800438; `0x0B` reads 0x04. [HW] in lr2021-re | [HW]: 0x0B reads 0x04 and 0x0C reads 0x00, both with data status; 0x0D, 0x11, 0x12 and 0x15 answer OK with no data; 0x0E to 0x10, 0x13 and 0x14 answer PERR |
| 0x023E | no parameters, 6 bytes back, in standby and in LoRa RX | Getter for RAM 0x8004B8; all zero in LoRa RX. [HW] in lr2021-re | [HW]: all zero in standby and in LoRa RX |
| 0x023F | no parameters, 18 bytes back, in standby and in LoRa RX | Getter for RAM 0x8004C4; all zero in LoRa RX. [HW] in lr2021-re | [HW]: bytes 0 to 16 zero and byte 17 0x79, in standby and in LoRa RX |

Run on 2026-10-30 on the LilyGO board with no PRAM loaded
([log](../research/undocumented-commands/logs/undocprobe_20261030.txt));
the radio restored cleanly. `jli_base` 0x100 is the ROM's own 460-entry JLI
table, the value lr2021-re reads in the reset code at 0x0000CC, not 0x1018C0,
the address Semtech's patch installs, as expected for an unpatched chip
([ROM and memory map](rom-and-memory.md)). The 0x0213 sub-indexes that answer OK without data may take
a longer parameter list, which lr2021-re's notes do not give; a one-byte
call to them could be a set with a missing value [guess], so they are not
probed again. The final 0x79 of 0x023F differs from lr2021-re's all-zero
result and is unexplained.

## Documented from ROM only

| Opcode | What lr2021-re reports | Status |
| --- | --- | --- |
| 0x022F | AGC configuration, next to SetAgcGainManual (handler 0xE404): writes 0xF30C28 bits 28:24, 22:21 and 20, and RAM 0x8004C0 | [RE]. Not called: it changes AGC behaviour, and this work only reads the AGC ([chip gain readback](host-sdr.md#chip-gain-readback)) |

## Do not call

These change chip state in ways this repository does not recover from, or
reach test hardware. They are listed so nobody tries them by accident. All
[RE] from lr2021-re's [command notes](https://github.com/beala/lr2021-re/blob/main/docs/commands.md).

| Opcode | What lr2021-re reports |
| --- | --- |
| 0x0102 | Write CPU auxiliary register (`guarded_sr`, 0xD80C). Includes `jli_base` and the mapping register that decides what RAM banks the CPU sees |
| 0x012C | Program the two hardware actionpoints (aux 0x220 to 0x222 + 3n): a breakpoint and ROM patch mechanism |
| 0x012F | Writes 0xF2F020 with a selector 0x00 to 0x5F; selector 0xFF reconfigures pads. Looks like an internal test bus or signal mux, possibly routed to DIOs [guess] |
| 0x0134, 0x0135 | Per-frequency calibration variants through the CalibFE frequency-list parser; they touch 0xF30154 (AGC), the synthesizer and 0xF40024 |
| 0x0136 | CalibFE with a leading block-mask byte |

## PRAM load window

lr2021-re reports that host address 0x801000, where PRAM is loaded, is a
write-only window: reading it stalls the chip [RE]. Capture banks 1 to 3 of
the 0x0290 RAM route also start at 0x801000 ([capture engine](capture-engine.md#sram-capacity-and-ownership)).

Checked in the code: no path in the library, the device adapter, the bridge
server, the firmware or the tools reads 0x801000 to 0x801FFF. Captures use
bank 4 (0x802000), the PRAM check reads the marker at 0x800FF8, and the gain
readback reads fixed registers at 0xF30xxx. So no guard was added to
`lr2021_iq.c`.

Not yet checked on hardware: whether a read of 0x801000 also stalls while a
bank 1 to 3 capture owns that memory. The read is expected to need a chip
reset afterwards, so it waits for an explicit decision to run it.
