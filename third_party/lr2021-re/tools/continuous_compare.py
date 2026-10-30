"""Receive-only comparison: continuous LR2021 IQ/decoder versus RAK KISS modem.

Requires iqstream firmware on the XIAO and KISS firmware on the RAK. The
comparison uses approximate host timing with a 30 ms guard / 50 ms match limit.
"""
import argparse
import json
import re
import time

import serial
from compare_rx import compare_packets, print_summary
from continuous_capture import main as capture
from kiss_rx import KissReceiver
from live_decode import read_until
from radio_ports import default_port


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--seconds', type=float, default=120, help='0=until Ctrl-C')
    ap.add_argument('--rx', default=default_port('XIAO', '/dev/ttyACM0'))
    ap.add_argument('--rak', default=default_port('RAK3401', '/dev/ttyACM1'))
    ap.add_argument('--spi', type=int, choices=(8, 16, 32), default=16)
    ap.add_argument('--gain', type=int, choices=range(14), default=0)
    ap.add_argument('--out', default='captures/continuous_per')
    args = ap.parse_args()
    # Read the receiver's actual channel instead of silently assuming defaults.
    with serial.Serial(args.rx, 115200, timeout=.05, exclusive=True) as rx:
        rx.write(b'\rhold on\r')
        if b'hold st=0 on' not in read_until(rx, b'hold st=0 on', 3):
            raise RuntimeError('XIAO did not enter hold mode')
        rx.write(b'get radio\r')
        reply = read_until(rx, b'  -> > ', 3).decode(errors='replace')
        config = re.search(r'>\s+([\d.]+),([\d.]+),(\d+),(\d+)', reply)
        if not config:
            raise RuntimeError(f'cannot read XIAO radio configuration: {reply!r}')
        frequency, bw, sf, cr = float(config[1]) * 1e6, float(config[2]) * 1000, int(config[3]), int(config[4])
        if sf != 7 or bw != 62500:
            raise RuntimeError('continuous comparison currently requires SF7/BW62.5')
    packets = []
    with serial.Serial(args.rak, 115200, timeout=.005, exclusive=True) as port:
        rak = KissReceiver(port, packets.append)
        rak.start()
        try:
            rak.configure(frequency, bw, sf, cr)
            before = rak.stats()
            directory, transport = capture(['--port', args.rx, '--seconds', str(args.seconds),
                                            '--spi', str(args.spi), '--gain', str(args.gain),
                                            '--out', args.out, '--decode'])
            time.sleep(.05)
            after = rak.stats()
        finally:
            rak.close()
    (directory / 'rak.jsonl').write_text(''.join(json.dumps(p) + '\n' for p in packets))
    start = transport['command_sent_monotonic']
    finish = start + transport['samples'] / transport['sample_rate_hz']
    # Allow delivery latency at session boundaries; coverage guards still
    # exclude partial packets from the decoder-miss denominator.
    reference = [p for p in packets if start - .05 <= p['t'] <= finish + .05]
    software = []
    for line in (directory / 'packets.jsonl').read_text().splitlines():
        p = json.loads(line)
        software.append(dict(payload=p['payload'], t_end=start + p['t_end_s']))
    result = compare_packets(reference, software, [dict(retained_start=start, retained_end=finish)], sf, bw, cr)
    result['transport_valid'] = transport['valid']
    result['valid'] = True
    result['decoder'] = transport.get('decoder')
    result['rak_hardware_counter_delta'] = {k: (after[k] - before[k]) % (1 << 32) for k in before}
    result['counter_interval_note'] = 'Hardware counters span listener setup/decoder drain too; packet matching uses IQ duration.'
    if (transport.get('decoder') or {}).get('error'):
        result['valid'] = False
        result['lr_miss_fraction_relative_to_rak'] = result['rak_miss_fraction_relative_to_lr'] = None
        result['interpretation'] = 'INVALID COMPARISON: file decoder failed; transport summary is separate.'
    (directory / 'comparison.json').write_text(json.dumps(result, indent=2))
    print_summary(result)
    print(f'Comparison: {directory}/comparison.json', flush=True)


if __name__ == '__main__':
    main()
