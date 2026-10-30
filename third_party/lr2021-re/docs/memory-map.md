# LR2021 memory map

Tags: **[HW]** = verified on hardware, **[RE]** = read from ROM code, **[guess]** = unverified.

All addresses are what the host reaches with ReadRegMem32 (0x0106) / WriteRegMem32 (0x0104).
These are the CPU's addresses, except for the PRAM write window.

## Address space [HW]

| Range | What | Notes |
|---|---|---|
| 0x000000–0x013FFF | ROM, 80 KB | Readable. Instruction memory (aux AUX_ICCM 0x208 = 0) |
| 0x014000–0x01FFFF+ | reads as zero | |
| 0x100000 | **stalls** the bus | reading hangs the chip until reset |
| 0x101000–0x1018BF | PRAM: Semtech patch (560 words) | read-only view; matches the uploaded words exactly |
| 0x1018C0–0x101FFF | PRAM: RAM copy of the ROM's 460-entry JLI table | aux JLI_BASE (0x290) = 0x1018C0 when the patch is active |
| 0x102000 | **stalls** | the ROM checks for a second PRAM slot here, but it doesn't exist on this part |
| 0x800000–... | RAM (DCCM, aux AUX_DCCM 0x18 = 0x800000) | data-only on ARC EM, so code can't run from here |
| 0x801000–0x801FFF | **write-only** window into PRAM | the host writes the patch here, then reads it back at 0x101000 |
| 0xF0xxxx–0xF4xxxx | peripheral registers | |

ReadRegMem32 is a guarded CPU load. Holes don't fault; they stall, so the guard never fires.

PRAM is full: the 2240 B patch plus the 1840 B JLI table leave about 16 B free. See
[pram-patch.md](pram-patch.md#pram-budget).

## ROM layout [RE]

| Address | What |
|---|---|
| 0x000000 | Vector table. Reset → 0xCC; the other vectors → 0x69A8 etc. |
| 0x0000CC | Reset: gp = 0x806000, jli_base = 0x100 |
| 0x000100–0x00082F | JLI table: 460 `b target` entries (`analysis/jli_table.csv`) |
| 0x0023F4 | Host command router |
| 0x0024E0 | Radio (0x02xx) command dispatcher |
| 0x00A8F4 | Exception handler (JLI 266, entry 0x528), used by the guarded load/store/aux helpers |
| 0x013948 | Capture source bytes-per-sample table: 2,2,4,4,4,4,2 for sources 0–6 |
| 0x013BEC | System command table, 55 entries (0x0100–0x0136) |
| 0x013CD0 | Radio command table, 168 entries (0x0200–0x02A7) |

## RAM variables

| Address | What | Status |
|---|---|---|
| 0x800000 / 0x800001 | bytes written by 0x012B case 2 before calling 0x643C | [RE] |
| 0x80002C | current packet type (0x0B = hidden raw mode) | [HW] |
| 0x8002E8, 0x8003F0 | other packet-status structs, probably FSK/FLRC | [guess] |
| 0x8003C0 | LoRa packet status struct. +0x8 / +0xA: frequency offset; +0xC: AGC gain step latched at RX | [HW] |
| 0x800438–0x8004A8 | shadows of the generic packet-engine registers (0x0213, 0x0230–0x023D) | [RE] |
| 0x8004B8 / 0x8004C4 | data returned by getters 0x023E (6 B) / 0x023F (18 B) | [HW] |
| 0x8004C0 | AGC config shadow (0x022F) | [RE] |
| 0x80060C | mask applied to forced IRQ bits (0x012B) | [RE] |
| 0x800648 / 0x80064A | stored params for calibration 0x7114 (0x0134 / 0x0135) | [RE] |
| 0x800773 | RX path flag (LF/HF); the PRAM patch watches it for DC-DC reconfig | [RE] |
| 0x8007BD | manual AGC gain setting (SetAgcGainManual) | [HW] |
| 0x8007F0 / 0x8007F4 | PRAM sub-command table pointer / count pointer (0x012D) | [RE] |
| 0x800968 | PRAM extension enable flag | [RE] |
| 0x800FF8 / 0x800FFC | "PRAM loaded" magic 0x600DB002 / version word, copied by the patch install | [HW] |
| 0x804000 | capture / RX data DMA base at boot (size code 6) | [HW] |
| 0x804400 | 1 KB region used for the RX FIFO while streaming (0xF30028 / 0xF30030) | [HW] |
| 0x806000 | gp (small-data base) | [RE] |
| 0x8060F8 | command router state | [RE] |
| 0x8077F8 | PRAM patch variables (5 words, zeroed by its INI table); +0 = extension callback | [RE] |

## Peripheral registers (not in the datasheet)

| Register | Field | Meaning | Status |
|---|---|---|---|
| 0xF30154 | [23:20] | current AGC gain step G1..G13 | [HW] |
| 0xF30154 | [19:16] | manual gain setting (0 = auto) | [HW] |
| 0xF30154 | [14:10] | copied to another status struct, meaning unknown | [RE] |
| 0xF3012C | [26:18] | gain value per step: 16,28,38,49,57,64,73,85,96,108,120,127,138 for G1..G13 (139 idle/auto). Probably total gain in 0.5 dB units | [HW] |
| 0xF301A8 | [26:18] | instantaneous RSSI (what GetRssiInst returns) | [RE] |
| 0xF30028 / 0xF30030 | | RX FIFO base address / size | [HW] |
| 0xF30C28 | [28:24], [22:21], [20] | AGC config (0x022F) | [RE] |
| 0xF30C28 | [9:5] | BLE frequency drift setting (PRAM hook 71) | [RE] |
| 0xF30C14 | [6:0] | BLE blocking setting (PRAM hook 69) | [RE] |
| 0xF30814 | [21:19] | BLE 2 Mbps setting (PRAM hook 72) | [RE] |
| 0xF30830 | bit 25 | OQPSK length-check bypass (PRAM 0x029F override) | [RE] |
| 0xF30D3C | | FLRC status extra field (PRAM hook 276) | [RE] |
| 0xF30B50 | [26:24] | RTToF (PRAM hook 224) | [RE] |
| 0xF30B74 / 0xF30B78 | | RTToF result inputs (PRAM hook 220) | [RE] |
| 0xF30904 | bits 4/5 | modulator input select (0x0293) | [RE] |
| 0xF30800 | bits 10/11 | packet-engine source select (0x0293/0x0294) | [RE] |
| 0xF30A30 | | LoRa modem register written by 0x021F | [RE] |
| 0xF30A14, 0xF30A68, 0xF30B00, 0xF30018 | | LoRa sync/detection (0x022D/0x022E) | [RE] |
| 0xF40144 | [6:0] | PLL frequency fractional bits, zeroed while ranging (PRAM hooks 267/268) | [RE] |
| 0xF20024 | bit 13 | DC-DC switcher control (PRAM hooks 414/417) | [RE] |
| 0xF2A004 | | DIO output bits (0x012B) | [RE] |
| 0xF2A008 | | force IRQ bits (0x012B) | [RE] |
| 0xF2A00C | | IRQ status (ClearIrq / GetAndClearIrqStatus) | [RE] |
| 0xF2F020 | | test-bus selector 0x00–0x5F (0x012F) | [guess] |

The full list of registers each command touches is in `analysis/register_fingerprints.txt`.

## CPU auxiliary registers (via 0x0103) [HW]

| Aux | Value | Meaning |
|---|---|---|
| 0x04 IDENTITY | 0x46 | ARCv2 EM |
| 0x18 AUX_DCCM | 0x800000 | data RAM base |
| 0x208 AUX_ICCM | 0 | instruction memory base |
| 0x290 JLI_BASE | 0x100 (ROM), 0x1018C0 (patch active) | jump-table base |
| RF_BUILD | 0x4B02 | |
| DCCM_BUILD | 0x0808 | |
| ICCM_BUILD | 0x0D07 | |
| AP_BUILD | 0x0407 | minimal actionpoints (2), matching 0x012C allowing n = 0..1 |
| MULTIPLY_BUILD | 0x020506 | |
| TIMER_BUILD | 0 | no timer0/1; aux 0x21 returns -707 |
| 0x220 + 3n / 0x221 + 3n / 0x222 + 3n | | actionpoint n match / mask / control (0x012C) |
| 0x80000008 | | RAM bank mapping between the CPU and the capture DMA |

## Frequency units

The PLL frequency is in steps of 15625 / 2^14 Hz. For example, 0x2D000000 = 720 MHz,
0x39300000 = 915 MHz, and 0xA00000 = 10 MHz. The DC-DC frequency at 0x80004C is stored as
Hz × 1.048576.
