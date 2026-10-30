# Dumping and disassembling the LR2021 ROM

The LR2021 has an internal ARCv2 EM CPU running from an 80 KB mask ROM, plus a 4 KB Patch RAM
(PRAM) that Semtech's driver loads with a fix-up blob. This page describes how the ROM was
read out and turned into a disassembly. You can repeat it on a chip you own. The ROM is
Semtech's copyright, so don't redistribute your dump or disassembly.

## 1. Reading memory over SPI

The documented `ReadRegMem32` command (opcode 0x0106) reads 32-bit words at a 24-bit address:

```
TX: 01 06  A2 A1 A0  N          (address big-endian, N = 1..32 words)
RX: status(2)  W0[31:24] W0[23:16] W0[15:8] W0[7:0]  W1 ...
```

Words come back big-endian on the wire. The CPU is little-endian, so to rebuild the bytes as
the CPU sees them, write each word back out little-endian (`tools/tobin.py` does this).

The datasheet presents 0x0106 as a register-access command. It is a guarded CPU load (ROM
`guarded_load32` at 0xD760, see [rom-functions.md](rom-functions.md)), so it can reach anything
the CPU can see, including the ROM. The original plan was to make the chip copy the ROM out
through a custom PRAM command (the patch's extension hook, see [pram-patch.md](pram-patch.md)).
That turned out to be unnecessary.

### Finding the ROM

The PRAM patch calls fixed ROM addresses (0x24E0, 0xB3A4, 0xBC88, ...), so the ROM had to
span at least 0x0000–0x12500 in the CPU view. The `romprobe` debug command read a few words
at candidate bases plus those offsets and checked for ARC function prologues (`enter_s`,
`push_s blink`). Base 0x000000 matched: the host view of the ROM uses the same addresses as
the CPU view.

### Things that hang the chip

ReadRegMem32's guard catches CPU exceptions. Reading an unmapped address doesn't raise one:
it **stalls the bus**. The guard never fires, the command never completes, and BUSY stays high
(RadioLib reports -705) until the chip is reset. Known stalls:

- 0x100000 (just below PRAM);
- 0x102000 and above (PRAM is only 4 KB on this part);
- the PRAM write window 0x801000–0x801FFF (write-only from the host);
- a RAM bank that is mapped to the capture DMA (see [raw-iq.md](raw-iq.md)).

0x014000 to at least 0x01FFFF reads back as zeros, so it is safe to over-read the end of
the ROM. See [memory-map.md](memory-map.md).

## 2. The dump

With the debug firmware ([firmware/README.md](../firmware/README.md)) on the board, open the
MeshCore repeater serial console and run:

```
mr 0 32768
```

`mr` reads in 32-word chunks and prints `AAAAAA: w0 w1 ... w7` lines. The full 128 KB read
takes about 4 seconds. Capture it and convert:

```bash
python3 tools/sc.py -p /dev/ttyACM0 -t 30 -o dump/dump_0_20000.txt "mr 0 32768"
python3 tools/tobin.py dump/dump_0_20000.txt dump/rom_full.bin
head -c 81920 dump/rom_full.bin > dump/lr2021_rom_0000-13FFF.bin     # 0x14000 bytes
sha256sum dump/lr2021_rom_0000-13FFF.bin
```

Our chip's ROM hashed to
`e531400a29f660aa50dbbb4de5df01d1b73818faa767c0494a5d0df3f48da821`. If yours matches, every
address in these docs applies directly. Sanity checks:

- the first word is 0x000000CC (reset vector → 0xCC);
- the following vectors are 0x000069A8;
- the last nonzero word is at 0x13FFC.

PRAM can be read back the same way at its CPU address, 0x101000 (`mr 101000 1024`). Load
Semtech's patch first. Note that the host must *write* PRAM through 0x801000.

## 3. Disassembly

The CPU is ARCv2 EM: aux IDENTITY (0x04) reads 0x46 through undocumented command 0x0103.
Ubuntu's `binutils-arc-linux-gnu` handles it. `docker/Dockerfile` builds an image with it:

```bash
docker build -t arc-binutils docker/
tools/disasm.sh          # dump/lr2021_rom_0000-13FFF.bin -> disasm/rom_raw.txt, rom_annotated.txt
```

`disasm.sh` does three things:

1. Wraps the raw binary in an ELF with `objcopy -I binary -O elf32-littlearc -B arc`, renaming
   `.data` to an executable `.text`.
2. **Patches the ELF `e_machine` field (offset 18) to 195 (EM_ARC_COMPACT2).** `objcopy -B arc`
   tags the file as ARC600. objdump then decodes the 16-bit major opcodes 0x08–0x0B as 32-bit
   instructions, and most of the listing turns into `.word`.
3. Runs `objdump -d -M cpu=em`, then `tools/annot.py`.

`annot.py` resolves `jli_s N` calls. The ROM's reset code sets `jli_base` = 0x100, and the
table at 0x100–0x82F holds 460 `b target` entries. Every `jli_s N` gets annotated with
`; -> target`. Without this, most calls in the listing are opaque.

## 4. Analysis passes

| Tool | Output | What it does |
|---|---|---|
| (manual) | `analysis/opcode_table.txt` | The router at 0x23F4 indexes two tables: system 0x13BEC (55 slots, 0x0100–0x0136) and radio 0x13CD0 (168 slots, 0x0200–0x02A7). Each slot was read and compared against the datasheet's command list and RadioLib's opcode names. |
| `tools/fingerprint.py UNDOC` | `analysis/register_fingerprints.txt` | For each handler: the parameter length check, the peripheral (0xF0xxxx–0xF4xxxx) and RAM (0x80xxxx) addresses it touches, its callees, and the addresses those callees touch. |
| `tools/jli_map.py` | `analysis/jli_table.csv` | JLI index → ROM address, with known names and Semtech's PRAM hooks. |
| `rc <op> <n> [bytes]` on hardware | `analysis/response_lengths.txt`, [commands.md](commands.md) | Calls a command with probe parameters and checks the reply and the error register. |

The undocumented commands were then read handler by handler, cross-checked against hardware
(register diffs with `mr`, raw calls with `rc`), and tagged in the docs:

- **[HW]**: verified on hardware;
- **[RE]**: read from code only;
- **[guess]**: plausible but unverified.

## 5. Finding a register by diffing

The AGC gain register was found without the ROM:

1. Enter continuous LoRa RX.
2. Force each gain step with SetAgcGainManual (0x021A, g = 1..13).
3. Dump the 0xF30xxx register block after each step (the logged sweep used `mr F30100 64`)
   and diff the dumps.

The field that tracked g was 0xF30154[23:20] (`logs/gain_sweep.txt`). The ROM then confirmed
it: the RX-done path copies that field into the LoRa packet status struct. See
[agc-gain.md](agc-gain.md).
