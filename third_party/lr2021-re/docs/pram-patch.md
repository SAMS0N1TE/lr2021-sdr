# Semtech's LR2021 PRAM patch

Semtech ships a firmware patch for the LR2021 that the host loads into the chip's 4 KB
Patch RAM at every reset. These notes cover the version in
[Lora-net/usp](https://github.com/Lora-net/usp) tag v1.2.1
(`smtc_rac_lib/radio_drivers/lr20xx_driver/inc/lr20xx_pram_lr2021.h`, Clear BSD license).
That is LR20xx driver v2.0.2: patch level 2, type 3, version 0x13, 560 words.

To disassemble it yourself, write the words little-endian to a file and run objdump the same
way as for the ROM ([dumping.md](dumping.md)), with `--adjust-vma=0x101000`.

## Loading [HW]

1. Reset the chip, then standby RC.
2. Write the 560 words with WriteRegMem32 to host address **0x801000**. This is a write-only
   window: reading it stalls the chip.
3. Send ActivatePram (0x012D). The ROM's `pram_check` (0x24FC) looks for the magic
   0x600DB002 at CPU 0x101000 and runs the install entry.
4. Check RAM 0x800FF8 = 0x600DB002 and 0x800FFC = the version word. The patch can also be
   read back at CPU 0x101000.

Load it before calibration. The patch survives warm
sleep, because the install entry only restores `jli_base` on wake.

## Layout (CPU addresses) [RE]

| Address | Contents |
|---|---|
| 0x101000 | magic 0x600DB002 |
| 0x101004 | type/version 0x00031304 (type 3, version 0x13) |
| 0x101008 | entry: install → 0x10104C |
| 0x10100C | entry: extension enable → 0x101018 |
| 0x101018–0x101876 | code |
| 0x101878 | "INI\x02" init table: (0x8077F8, 0x14) zeroes the patch's 5 RAM variables |
| 0x1018B0 | 8-byte coefficient table (used by 0x101744) |
| 0x1018C0–0x101FFF | (beyond the image) RAM copy of the ROM's 460-entry JLI table |

## Install (0x10104C) [RE]

On a cold boot, install:

1. calls ROM 0xB3A4(460) to build the RAM JLI table at 0x1018C0;
2. calls ROM 0xBC88(index, fn) 21 times to swap entries for patch functions;
3. sets jli_base (aux 0x290) to 0x1018C0;
4. copies the magic and version to 0x800FF8 / 0x800FFC.

The host reads those last two words back to confirm "PRAM loaded".

On warm wake (retention), install only points jli_base back at 0x1018C0.

## Hooks [RE]

| JLI | Patch fn | What it does | Matching change in Semtech's driver |
|---|---|---|---|
| 61 | 0x10120C | Radio command dispatcher. Overrides 0x0200 SetRfFrequency, 0x029F SetOqpskParams, 0x024B GetFlrcPacketStatus and 0x029A GetZwavePacketStatus, else calls ROM 0x24E0. Optional extension callback at [0x8077F8] if [0x800968] != 0 | |
| 389, 53, 246 | 0x1012D4 | Re-run the DC-DC config when the RX-path flag [0x800773] changes, on a state change, or when the RF frequency moves ≥ 10 MHz | replaces the host DC-DC workaround |
| 414, 417 | | Clear / set bit 13 of DC-DC switcher reg 0xF20024 around ROM functions | |
| 267, 268, 274, 275 | | RTToF: zero the low 7 bits of PLL freq 0xF40144 while ranging (packet type 4), restore after | removed `rttof_truncate_pll_freq_step` |
| 224 | | RTToF: 0xF30B50 bits 24–26 | removed `rttof_extended_stuck_second_request` |
| 220 | | Reads 0xF30B74/78 and converts through ROM 0x7E30 into the result (likely an RTToF RSSI fix) | |
| 69, 71, 72 | | BLE: 0xF30C14[6:0] = 0x7C (blocking), 0xF30C28[9:5] = 30 (frequency drift), 0xF30814[21:19] (2 Mbps) | removed BLE workarounds |
| 276 + 0x024B | | FLRC packet status gains a 24-bit field from 0xF30D3C (scaled) | |
| 450 + 0x029A | | Z-Wave packet status reports the channel of the last packet | changelog v2.0.2 |
| 0x029F override | 0x1016D8 | OQPSK params: new length-check-bypass bit → 0xF30830 bit 25 | changelog v2.0.2 |
| 435, 439 | | OOK/RAW demod reconfig with the coefficient table (purpose unclear) | |
| 390, 130 | | small tweaks (unclear) | |

The ROM functions behind each index are listed in
[rom-functions.md](rom-functions.md#rom-functions-that-the-pram-patch-replaces).

### DC-DC config (0x1012D4)

| Condition | Setting |
|---|---|
| RX on the HF path | rise time = 15 |
| ADC decimation == 1 | DC-DC frequency 4.3 MHz, otherwise 2.8 MHz (RAM 0x80004C = Hz × 1.048576). Same as the old host workaround |
| **new:** RF < 720 MHz | rise time = 8 |
| **new:** RF ≥ 720 MHz | rise time = 13, DC-DC frequency halved |

This only matters when the chip runs on its DC-DC (SIMO) regulator. The chip defaults to
LDO, and RadioLib never enables DC-DC.

### Extension hook

For 0x02xx commands, the patched dispatcher first calls the function pointer at RAM 0x8077F8
if the word at 0x800968 is nonzero:

- r0 = opcode low byte, r1/r2 = params, r3 = response struct;
- returning 255 means "not handled", and the command falls through to normal handling.

Semtech's enable entry (0x10100C → 0x101018) sets 0x800968 = 1 and calls an init at CPU
0x102000 with r0 = 0x8077F8. On warm wake, install calls it again if 0x800968 != 0. 0x102000
is the ROM's second PRAM slot, which doesn't exist on the LR2021 (reading it stalls). So that
path is either meant for a part with more PRAM, or dead here.

ActivatePram (0x012D) also has a ROM-level way to add commands: with extra params it
dispatches into a PRAM-provided sub-command table (pointer at 0x8007F0).

## PRAM budget

PRAM is effectively full (about 16 B free). The breakdown below is for anyone who wants to
fit their own code:

| Bytes | Part | Needed for LoRa + DC-DC? |
|---|---|---|
| 1840 | RAM copy of the 460-entry JLI table (0x1018C0–0x101FFF) | yes: every hook is a swap in this table |
| ~520 | header, install, extension enable, dispatcher core, INI/coefficient tables | yes |
| ~380 | DC-DC config + hooks 389/53/246/414/417, SetRfFrequency override | yes, if using DC-DC |
| ~150 | hooks 390, 130 (purpose unclear) | keep until understood |
| 76 | BLE workarounds (hooks 69/71/72, 0x101424–0x10146F) | no |
| 84 | RTToF 220/224 (0x101558–0x1015AB) | no |
| 680 | **0x1015D0–0x101877, one contiguous block**: RTToF PLL hooks 267/268/274/275, FLRC 276, OOK/RAW 435/439 + helpers, OQPSK helper 0x1016D8, Z-Wave 450, init helper 0x1016C4 | no |
| ~36 | dispatcher cases for 0x029F / 0x024B / 0x029A (0x101292–0x1012C3) | no |

The non-LoRa parts separate cleanly. The 0x1015D0+ block is only referenced from three places:

- the install routine's hook registrations (0x101122–0x10119E);
- one call at 0x1011D0 (→ 0x1016C4);
- the OQPSK dispatcher case (0x101296 → 0x1016D8).

Skipping those frees about 680 B of contiguous space (about 870 B in total) without touching
the LoRa or DC-DC fixes. The alternatives are to hook through the two hardware actionpoints
(0x012C) instead of JLI swaps, or to add commands through the 0x012D sub-command table.
