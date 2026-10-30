# ROM, memory map and command internals

[Guide hub](README.md) / ROM and memory map

What [beala/lr2021-re](https://github.com/beala/lr2021-re) (MIT, Alex Beal)
learned about the LR2021's internal ARCv2 EM CPU, its 80 KB mask ROM and its
4 KB Patch RAM, and how to repeat the ROM readout on your own chip with this
repository's firmware. lr2021-re's full write-ups and tables are kept in
[`third_party/lr2021-re`](../third_party/lr2021-re):
[memory map](../third_party/lr2021-re/docs/memory-map.md),
[ROM functions](../third_party/lr2021-re/docs/rom-functions.md),
[dumping and disassembly](../third_party/lr2021-re/docs/dumping.md),
[commands](../third_party/lr2021-re/docs/commands.md),
[PRAM patch](../third_party/lr2021-re/docs/pram-patch.md), and the data in
[`analysis/`](../third_party/lr2021-re/analysis): `opcode_table.txt` (every
command slot with its handler, documented or not), `register_fingerprints.txt`
and `undoc_fingerprints.txt` (what each handler checks, touches and calls),
`response_lengths.txt` and `jli_table.csv` (all 460 JLI entries).

The ROM is Semtech's copyrighted firmware. Neither repository contains it, its
disassembly or the datasheet; `/dump/`, `/disasm/` and `/datasheet/` are
ignored by git here. Do not commit or share a dump.

Tags: **[HW]** verified on this repository's LilyGO board, **[RE]** from
lr2021-re and not yet run here, **[guess]** unverified.

## Address space

| Range | What | Status |
| --- | --- | --- |
| 0x000000 to 0x013FFF | ROM, 80 KB, readable with ReadRegMem32 | [RE] |
| 0x014000 to 0x01FFFF | reads as zero | [RE] |
| 0x100000, 0x102000 and above | stall the bus until reset | [RE] |
| 0x101000 to 0x1018BF | PRAM, Semtech's patch when loaded (read view) | [RE] |
| 0x1018C0 to 0x101FFF | PRAM, the patch's RAM copy of the JLI table | [RE] |
| 0x800000 and up | RAM (DCCM, data only) | [RE] |
| 0x801000 to 0x801FFF | write-only PRAM load window; reading stalls | [RE] |
| 0x804000 | capture and RX data DMA base at boot | [RE] |
| 0x804400 | the 1 KB region this repository maps the RX FIFO to | [HW] |
| 0xF0xxxx to 0xF4xxxx | peripheral registers | [RE] |

ReadRegMem32 (0x0106) is a guarded CPU load, not only a register read, so it
reaches ROM, PRAM and RAM. A hole does not fault, it stalls, so the guard
never fires. A RAM bank mapped to an armed capture also stalls a read: stop
the capture (0x0290 mode 0) first.

Registers this repository has confirmed:

| Register | Meaning | Status |
| --- | --- | --- |
| 0xF30154 bits 23:20, 19:16 | AGC gain step in use, manual setting | [HW] |
| 0xF3012C bits 26:18 | gain value of the step | [HW] |
| 0xF301A8 bits 26:18 | instantaneous RSSI, as GetRssiInst | [HW] |
| 0xF30028, 0xF30030 | RX FIFO base and size | [HW] |
| aux 0x290 (`jli_base`) | 0x100 on an unpatched chip, the ROM's own JLI table that the reset code sets | [HW] |

See [chip gain readback](host-sdr.md#chip-gain-readback) and
[undocumented commands](undocumented-commands.md).

## ROM layout [RE]

| Address | What |
| --- | --- |
| 0x000000 | vector table; reset to 0xCC |
| 0x0000CC | reset: gp = 0x806000, jli_base = 0x100 |
| 0x000100 to 0x00082F | JLI table, 460 entries |
| 0x0023F4 | host command router (0x01xx system, 0x02xx radio) |
| 0x0024E0 | radio command dispatcher, the hook Semtech's patch wraps |
| 0x013948 | capture source bytes per sample: 2, 2, 4, 4, 4, 4, 2 for sources 0 to 6 |
| 0x013BEC | system command table, 55 entries |
| 0x013CD0 | radio command table, 168 entries |

Command handlers take r0 = parameter byte count, r1 = parameters, r2 = a
response struct, and return 2 (OK) or 3 (OK with data). 38 opcodes have real
handlers but are not in the datasheet ([undocumented commands](undocumented-commands.md)).

## Reading the ROM of your own chip

With the bridge firmware on the board and its console free:

```bash
python tools/lr2021_rom_dump.py COM14
```

It runs `mr 0 32768` through the [debug console](debug-console.md) (128 KiB,
about 30 s at 115200 baud), checks that every word arrived, writes the bytes
little-endian as the CPU sees them to `dump/rom_full.bin` and the 80 KB ROM to
`dump/lr2021_rom_0000-13FFF.bin`, and compares it with lr2021-re's chip:

| Check | lr2021-re's ROM |
| --- | --- |
| SHA-256 | `e531400a29f660aa50dbbb4de5df01d1b73818faa767c0494a5d0df3f48da821` |
| first word (reset vector) | 0x000000CC |
| second word | 0x000069A8 |
| last nonzero word | 0x13FFC |

If the hash matches, every address in lr2021-re's notes applies to your chip.
`--from-text` converts a saved `mr` listing instead of reading the board.

To disassemble, build lr2021-re's ARC binutils image and run its script from
the third-party folder with the dump in its `dump/` subfolder:

```bash
docker build -t arc-binutils third_party/lr2021-re/docker
mkdir -p third_party/lr2021-re/dump third_party/lr2021-re/disasm
cp dump/lr2021_rom_0000-13FFF.bin third_party/lr2021-re/dump/
sh third_party/lr2021-re/tools/disasm.sh
```

`tools/annot.py`, `fingerprint.py` and `jli_map.py` there annotate the listing
with the named functions and JLI indices. Keep `dump/` and `disasm/` out of
git in both places.

## Finding a register by diffing

lr2021-re found the AGC register by changing one setting and diffing a block
of registers: in continuous RX, force each gain step and dump the 0xF301xx
block after each one. The same works here with `rc 21A 0 <step>` and
`mr F30100 64` from the debug console.
