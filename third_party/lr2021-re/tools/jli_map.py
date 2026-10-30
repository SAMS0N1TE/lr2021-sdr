#!/usr/bin/env python3
"""Build analysis/jli_table.csv from your own ROM disassembly.

usage: python3 tools/jli_map.py [disasm/rom_annotated.txt] [out.csv]

The output holds only addresses, names and descriptions, no ROM bytes. Names come from the
NAMES / PRAM_HOOKS tables below. (Command handlers are reached through the opcode tables
at 0x13BEC / 0x13CD0, not through JLI; see analysis/opcode_table.txt.)
"""
import csv
import re
import sys

NAMES = {
    # addr: (name, description)
    0x19B8: ('', 'called by the PRAM patch'),
    0x19D0: ('', 'called by GetVersion / GetRxFifoLevel; reads 0xF3013C'),
    0x20F0: ('', 'state-change helper; PRAM hook 53 re-runs DC-DC config after it'),
    0x23B0: ('read_be24', 'r0 = param ptr; returns big-endian 24-bit value (addresses in commands)'),
    0x23C0: ('read_be32', 'r0 = param ptr; returns big-endian 32-bit value'),
    0x23F4: ('host_cmd_router', 'opcode high byte 0x01 -> system table 0x13BEC, 0x02 -> radio table 0x13CD0'),
    0x24E0: ('radio_cmd_dispatch', '0x02xx dispatcher: ld.as r12,[0x13CD0, op&0xFF]; PRAM hook 61 wraps it'),
    0x24FC: ('pram_check', 'checks magic 0x600DB002 at CPU 0x101000 or 0x102000'),
    0x3F70: ('', 'FIFO IRQ flag clear helper'),
    0x46D0: ('', 'clock/IRQ block config (F02004, F02018, F23050, F2A00C, F30128); used by 0x0214'),
    0x47E8: ('', 'clock/TCXO helper used by GetStatus, ConfigLfClock, SetTcxoMode'),
    0x569C: ('', 'error/FIFO flag read helper (GetErrors, GetFifoIrqFlags)'),
    0x56E8: ('', 'error/FIFO flag read helper (GetErrors, GetFifoIrqFlags)'),
    0x58CC: ('', 'called by GetRxFifoLevel and the PRAM patch'),
    0x5B88: ('parse_freq_list', 'CalibFE-style frequency list parser (0x0123, 0x0134-0x0136)'),
    0x7114: ('', 'per-frequency calibration run by 0x0134/0x0135 (RAM params 0x80064A / 0x800648)'),
    0x7E30: ('', 'value conversion used by the PRAM RTToF result hook 220'),
    0x9FEC: ('', 'RF frequency move; PRAM hook 246 re-runs DC-DC config on >= 10 MHz moves'),
    0xA8F4: ('exception_handler', 'reads ECR; optional hook [gp+64], else sets fault flag [gp+8]; target of guarded helpers'),
    0xD760: ('guarded_load32', '*r1 = *r0 with exception recovery (handler 0x528); ReadRegMem32 core'),
    0xD798: ('guarded_lr', '*r1 = aux[r0] with exception recovery; used by 0x0103'),
    0xD7D4: ('guarded_store32', '*r0 = r1 with exception recovery; WriteRegMem32 core'),
    0xD80C: ('guarded_sr', 'aux[r0] = r1 with exception recovery; used by 0x0102'),
    0xE47C: ('set_agc_gain_manual', 'g 0..13 -> RAM 0x8007BD and 0xF30154[19:16] (0 = auto)'),
    0xFDC0: ('', 'RX-path change; PRAM hook 389 re-runs DC-DC config when flag 0x800773 changes'),
    0x1101C: ('', 'DC-DC related; PRAM hook 414 clears bit 13 of 0xF20024 around it'),
    0x113B0: ('', 'DC-DC related; PRAM hook 417 sets bit 13 of 0xF20024 around it'),
    0x12478: ('', 'called by the PRAM patch'),
}

PRAM_HOOKS = {
    # JLI index: what Semtech's PRAM patch (type 3, version 0x13) replaces it with
    61: 'radio-cmd dispatcher wrapper: overrides 0x0200, 0x029F, 0x024B, 0x029A; extension callback',
    389: 'DC-DC reconfig when RX-path flag 0x800773 changes',
    53: 'DC-DC reconfig on state change',
    246: 'DC-DC reconfig when RF frequency moves >= 10 MHz',
    414: 'clear bit 13 of DC-DC switcher reg 0xF20024 around the ROM function',
    417: 'set bit 13 of DC-DC switcher reg 0xF20024 around the ROM function',
    267: 'RTToF: zero low 7 bits of PLL freq 0xF40144 while ranging',
    268: 'RTToF: zero low 7 bits of PLL freq 0xF40144 while ranging',
    274: 'RTToF: restore PLL freq after ranging',
    275: 'RTToF: restore PLL freq after ranging',
    224: 'RTToF: 0xF30B50 bits 24-26 (extended stuck second request fix)',
    220: 'RTToF: reads 0xF30B74/78, converts via 0x7E30 (likely RSSI fix)',
    69: 'BLE: 0xF30C14 low 7 bits = 0x7C (blocking)',
    71: 'BLE: 0xF30C28[9:5] = 30 (frequency drift)',
    72: 'BLE: 0xF30814[21:19] (2 Mbps)',
    276: 'FLRC packet status: adds a 24-bit field from 0xF30D3C',
    450: 'Z-Wave packet status: channel of last packet',
    435: 'OOK/RAW demod reconfig with coefficient table',
    439: 'OOK/RAW demod reconfig with coefficient table',
    390: 'small tweak (purpose unclear)',
    130: 'small tweak (purpose unclear)',
}


def main():
    dis = sys.argv[1] if len(sys.argv) > 1 else 'disasm/rom_annotated.txt'
    out = sys.argv[2] if len(sys.argv) > 2 else 'analysis/jli_table.csv'

    jli = {}
    for line in open(dis):
        m = re.match(r'\s*([0-9a-f]+):\s.*\bb\s+\d+\s+;([0-9a-f]+)', line)
        if m:
            a = int(m.group(1), 16)
            if 0x100 <= a < 0x830 and (a - 0x100) % 4 == 0:
                jli[(a - 0x100) // 4] = int(m.group(2), 16)

    with open(out, 'w', newline='') as f:
        w = csv.writer(f)
        w.writerow(['jli_index', 'rom_addr', 'name', 'description', 'pram_patch_hook'])
        for i in sorted(jli):
            a = jli[i]
            name, desc = NAMES.get(a, ('', ''))
            w.writerow([i, f'0x{a:05X}', name, desc, PRAM_HOOKS.get(i, '')])
    print(len(jli), 'entries ->', out)


if __name__ == '__main__':
    main()
