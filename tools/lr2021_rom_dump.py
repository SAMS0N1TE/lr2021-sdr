# SPDX-License-Identifier: MIT
import argparse, hashlib, re, struct, sys
from pathlib import Path

ROM_BYTES = 0x14000
KNOWN_SHA256 = 'e531400a29f660aa50dbbb4de5df01d1b73818faa767c0494a5d0df3f48da821'
LINE = re.compile(r'^([0-9A-F]{6}):((?: [0-9A-F]{8})+)\s*$')


def parse(text):
    words = {}
    for line in text.replace('\r', '').split('\n'):
        m = LINE.match(line.strip())
        if not m:
            continue
        if '!st=' in line:
            raise ValueError(f'read error in: {line}')
        addr = int(m[1], 16)
        for i, w in enumerate(m[2].split()):
            words[addr + 4 * i] = int(w, 16)
    if not words:
        raise ValueError('no mr lines in the output')
    end = max(words) + 4
    missing = [a for a in range(0, end, 4) if a not in words]
    if missing:
        raise ValueError(f'{len(missing)} words missing, first at 0x{missing[0]:06X}')
    return b''.join(struct.pack('<I', words[a]) for a in range(0, end, 4))


def check(rom):
    first, second = struct.unpack_from('<II', rom, 0)
    last = max((i for i in range(0, len(rom), 4) if rom[i:i + 4] != b'\0\0\0\0'), default=-1)
    return dict(sha256=hashlib.sha256(rom).hexdigest(), reset_vector=first, second_vector=second,
                last_nonzero=last, matches_lr2021_re=hashlib.sha256(rom).hexdigest() == KNOWN_SHA256)


def main():
    ap = argparse.ArgumentParser(description='Read the LR2021 ROM of your own chip through the debug console mr command')
    ap.add_argument('port', nargs='?', help='console serial port; omit with --from-text')
    ap.add_argument('--out', default='dump', help='folder for the dump (ignored by git; never commit it)')
    ap.add_argument('--words', type=int, default=32768, help='words to read from address 0 (default 128 KiB)')
    ap.add_argument('--from-text', help='convert a saved mr listing instead of reading the board')
    a = ap.parse_args()
    out = Path(a.out)
    out.mkdir(parents=True, exist_ok=True)
    if a.from_text:
        text = Path(a.from_text).read_text(errors='replace')
    else:
        if not a.port:
            ap.error('port is required unless --from-text is given')
        sys.path.insert(0, str(Path(__file__).resolve().parent))
        from lr2021_console import open_port, run
        s = open_port(a.port)
        text = run(s, f'mr 0 {a.words}', 30)
        s.close()
        (out / f'mr_0_{a.words}.txt').write_text(text)
    full = parse(text)
    (out / 'rom_full.bin').write_bytes(full)
    rom = full[:ROM_BYTES]
    path = out / 'lr2021_rom_0000-13FFF.bin'
    path.write_bytes(rom)
    c = check(rom)
    print(f"{path}: {len(rom)} bytes, sha256 {c['sha256']}")
    print(f"reset vector 0x{c['reset_vector']:08X} (lr2021-re: 0x000000CC), next 0x{c['second_vector']:08X} "
          f"(0x000069A8), last nonzero word at 0x{c['last_nonzero']:05X} (0x13FFC)")
    print('matches the ROM lr2021-re analysed' if c['matches_lr2021_re'] else
          'differs from the ROM lr2021-re analysed: its addresses may not apply')
    return 0


if __name__ == '__main__':
    sys.exit(main())
