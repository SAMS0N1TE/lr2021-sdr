#!/bin/sh
# Rebuild the ROM disassembly from dump/lr2021_rom_0000-13FFF.bin.
# Needs the arc-binutils image: docker build -t arc-binutils docker/
set -e
cd "$(dirname "$0")/.."
docker run --rm -u "$(id -u):$(id -g)" -v "$PWD":/w -w /w arc-binutils sh -c '
  arc-linux-gnu-objcopy -I binary -O elf32-littlearc -B arc \
    --rename-section .data=.text,code,alloc,load,readonly,contents \
    dump/lr2021_rom_0000-13FFF.bin disasm/rom_arcv2.o'
# objcopy tags the ELF as ARC600; ARCv2 (e_machine 195) is needed to decode 16-bit opcodes 0x08-0x0B
python3 -c "import struct,sys;p='disasm/rom_arcv2.o';d=bytearray(open(p,'rb').read());struct.pack_into('<H',d,18,195);open(p,'wb').write(d)"
docker run --rm -u "$(id -u):$(id -g)" -v "$PWD":/w -w /w arc-binutils arc-linux-gnu-objdump -d -M cpu=em disasm/rom_arcv2.o \
  | sed 's/_binary_dump_lr2021_rom_0000_13FFF_bin_start+//' > disasm/rom_raw.txt
python3 -I tools/annot.py disasm/rom_raw.txt disasm/rom_annotated.txt
