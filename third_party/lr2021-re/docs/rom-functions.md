# LR2021 ROM functions

Named and described ROM functions, by CPU address. "JLI" is the function's index in the
ROM's jump table at 0x100: ROM code calls it with `jli_s N`, and Semtech's PRAM patch
redirects calls by swapping entries in a RAM copy of that table. All 460 indices are in
`analysis/jli_table.csv`. Command handler addresses for every opcode are in
`analysis/opcode_table.txt`.

Tags: **[HW]** = verified on hardware, **[RE]** = read from code, **[guess]** = unverified.
Arguments follow the ARCv2 ABI: r0, r1, r2, ... in; r0 out; `blink` = return address.

## Startup and infrastructure

| Address | JLI | Name | Arguments / result | What it does | Status |
|---|---|---|---|---|---|
| 0x0000 | | vector table | | Reset vector → 0xCC. Most other vectors → 0x69A8 | [RE] |
| 0x00CC | | reset | | Sets gp = 0x806000 and jli_base (aux 0x290) = 0x100, then starts the firmware | [RE] |
| 0xA8F4 | 266 | exception handler | | Reads ECR (aux 0x403). Calls the optional hook at [gp+64] if one is set, otherwise sets the fault flag at [gp+8], then decodes the exception cause. The guarded helpers point the handler slot [gp+28] at its JLI entry 0x528 | [RE] |
| 0x236C | | actionpoint exception handler | | Installed by 0x012C | [RE] |
| 0x23B0 | 56 | read_be24 | r0 = byte ptr → r0 = p[0]<<16 \| p[1]<<8 \| p[2] | Parses 24-bit big-endian command parameters (memory addresses) | [RE] |
| 0x23C0 | 57 | read_be32 | r0 = byte ptr → r0 = big-endian u32 | Parses 32-bit command parameters | [RE] |
| 0xB3A4 | | build RAM JLI table | r0 = entry count (460) | Copies the ROM JLI table into RAM at the end of PRAM. Called by the PRAM patch install | [RE] |
| 0xBC88 | | swap JLI entry | r0 = index, r1 = new target | Points one RAM JLI entry at a patch function | [RE] |

### Guarded memory and aux access

All four helpers follow the same pattern:

1. Clear the fault flag at [gp+8].
2. Save the exception handler slot at [gp+28] and install 0x528 (JLI entry 266 → 0xA8F4).
3. Do one access, then `sync`.
4. Restore the handler slot and return the fault flag (0 = OK).

A bus stall on an unmapped address raises no exception, so these helpers can't catch it.

| Address | JLI | Name | Arguments | Used by |
|---|---|---|---|---|
| 0xD760 | 341 | guarded_load32 | r0 = address, r1 = out ptr | ReadRegMem32 (0x0106), WriteRegMemMask32 |
| 0xD798 | 342 | guarded_lr | r0 = aux reg, r1 = out ptr | 0x0103 read aux register |
| 0xD7D4 | 343 | guarded_store32 | r0 = address, r1 = value | WriteRegMem32 (0x0104), WriteRegMemMask32 |
| 0xD80C | 344 | guarded_sr | r0 = aux reg, r1 = value | 0x0102 write aux register |

## Command dispatch

| Address | JLI | Name | What it does | Status |
|---|---|---|---|---|
| 0x23F4 | 59 | host_cmd_router | Routes by opcode high byte: 0x01 → system table 0x13BEC, 0x02 → 0x24E0. Others are rejected | [RE] |
| 0x24E0 | 61 | radio_cmd_dispatch | `ld.as r12,[0x13CD0, op & 0xFF]` and calls the handler with the [handler ABI](commands.md#handler-abi-re). The PRAM patch replaces JLI 61 with its own dispatcher at 0x10120C | [RE] |
| 0x24FC | 62 | pram_check | Checks for magic 0x600DB002 at CPU 0x101000, then 0x102000 (two PRAM slots; only the first exists on this part) | [RE] |
| 0xD568 / 0xD56C | | invalid-opcode stubs | Radio / system | [RE] |

## Selected command handlers

The full list is in `analysis/opcode_table.txt`, and the undocumented ones are covered in
[commands.md](commands.md).

| Address | Command | Notes | Status |
|---|---|---|---|
| 0xD4B4 | 0x0106 ReadRegMem32 | Params `addr(be24) n(1)`. The address must be word-aligned. Loops `guarded_load32` | [RE] |
| 0x12138 | 0x0104 WriteRegMem32 | Params `addr(be24) words(be32...)`. Loops `guarded_store32` | [RE] |
| 0x10AC4 | 0x012D ActivatePram | Calls `pram_check`; with extra params, dispatches to the PRAM sub-command table at [0x8007F0] | [RE] |
| 0x2C9C | 0x0123 CalibFE | Parses the frequency list with 0x5B88 | [RE] |
| 0x3284 | 0x0136 | CalibFE with a block mask | [RE] |
| 0x5C6C | 0x020B GetRssiInst | Returns 0xF301A8[26:18]. RSSI gain compensation is done in hardware | [RE] |
| 0xE4A8 | 0x021A SetAgcGainManual | Calls `set_agc_gain_manual` (0xE47C) | [HW] |
| 0x57E8 | 0x022A GetLoraPacketStatus | Builds **10** response bytes from the struct at 0x8003C0. Byte 9 = struct +0xC (gain step); bytes 6–8 = struct +0xA (low byte) and +0x8 (bits 15:0) | [HW] |
| 0xF9A0 | 0x0290 capture/DMA route | Worker at 0xCF7C; touches 0xF30044 / 0xF3004C / 0xF30050 and RAM 0x800560, 0x8060C0 | [HW] partly |
| 0x5B44 / 0x5B60 | 0x0291 / 0x0292 | Workers 0xCE98 / 0xCED4; 0xF30054 = write pointer | [HW] |
| 0xFA78 / 0xFA18 | 0x0293 / 0x0294 | Workers 0xD1A8 / 0xD134 | [guess] |

## Radio internals

| Address | JLI | Name | Arguments / effect | Status |
|---|---|---|---|---|
| 0xE47C | 367 | set_agc_gain_manual | r0 = g (0..13). Stores g in RAM 0x8007BD and 0xF30154[19:16]; 0 = automatic | [HW] |
| 0x8074 | | RX event (gain latch) | On IRQ status bit 7, copies 0xF30154[23:20] (current gain step) to 0x8003C0 + 0xC | [HW] (effect) |
| 0x33D6 | | (code location) | Compares 0xF3012C[26:18] (gain value) against a RAM threshold | [RE] |
| 0x5B88 | 144 | parse_freq_list | Frequency-list parser shared by CalibFE 0x0123 and 0x0134–0x0136 | [RE] |
| 0x7114 | 187 | per-frequency calibration | Run by 0x0134 / 0x0135 with params from RAM 0x80064A / 0x800648. Touches AGC (0xF30154) and synth (0xF40140/48) | [RE] |
| 0x46D0 | 106 | clock / IRQ block config | 1-byte arg from 0x0214. Writes 0xF02004, 0xF02018, 0xF23050, 0xF2A00C, 0xF30128 | [RE] |
| 0x7E30 | 194 | value conversion | Used by the PRAM RTToF result fix (hook 220) | [RE] |
| 0x2F0C | | RX path / FE setup | Shared by CalibFE, SetRxPath and 0x0136. Reads the path flag 0x800773; touches 0xF3013C, 0xF40200, 0xF40608 | [RE] |

## ROM functions that the PRAM patch replaces

Semtech's patch (type 3, version 0x13) swaps these JLI entries for its own functions. Each of
these ROM functions is the original that a patch wrapper calls before or after its fix. See
[pram-patch.md](pram-patch.md).

| JLI | ROM function | Area | What the patch adds |
|---|---|---|---|
| 53 | 0x20F0 | state change | Re-runs the DC-DC config |
| 61 | 0x24E0 | command dispatch | Overrides 0x0200, 0x029F, 0x024B, 0x029A; extension callback |
| 69 | 0x269C | BLE | 0xF30C14[6:0] = 0x7C (blocking) |
| 71 | 0x2750 | BLE | 0xF30C28[9:5] = 30 (frequency drift) |
| 72 | 0x27B8 | BLE | 0xF30814[21:19] (2 Mbps) |
| 130 | 0x4F28 | ? | small tweak, purpose unclear |
| 220 | 0x8DB8 | RTToF | Reads 0xF30B74/78 and converts through 0x7E30 (likely an RSSI fix) |
| 224 | 0x8EB8 | RTToF | 0xF30B50[26:24] |
| 246 | 0x9FEC | RF frequency | Re-runs the DC-DC config when the frequency moves ≥ 10 MHz |
| 267, 268 | 0xA968, 0xAAD4 | RTToF | Zeroes 0xF40144[6:0] (PLL fractional) while ranging |
| 274, 275 | 0xB15C, 0xB2B0 | RTToF | Restores the PLL after ranging |
| 276 | 0xB3DC | FLRC | Adds a 24-bit field from 0xF30D3C to the packet status |
| 389 | 0xFDC0 | RX path | Re-runs the DC-DC config when 0x800773 changes |
| 390 | 0xFEB0 | ? | small tweak, purpose unclear |
| 414 | 0x1101C | DC-DC | Clears 0xF20024 bit 13 around the call |
| 417 | 0x113B0 | DC-DC | Sets 0xF20024 bit 13 around the call |
| 435, 439 | 0x11AC0, 0x11E7C | OOK / raw | Demod reconfig with a coefficient table |
| 450 | 0x1245C | Z-Wave | Packet status reports the channel of the last packet |

Other ROM functions that the patch calls directly: 0x19B8 (42), 0x58CC (142), 0x12478 (451),
0x7E30 (194), 0x1101C (414), 0x113B0 (417).
