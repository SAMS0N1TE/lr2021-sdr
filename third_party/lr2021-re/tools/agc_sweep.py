"""Compare LR2021 raw-mode gain at different RAK powers using zero-hop adverts.

Requires debug firmware; streamstats adds gain/clip telemetry when supported.
Restores RAK power to -9 dBm and leaves the receiver held with hardware AGC enabled.
"""
import argparse
from concurrent.futures import ThreadPoolExecutor
from datetime import datetime
import json
from pathlib import Path
import time

import numpy as np
import serial
from bench_capture import command
from live_decode import one_capture, read_until
from lora_decode import decode, load_iq8, DecodeError
from radio_ports import default_port


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--powers', type=int, nargs='+', default=[-9, 0, 10])
    ap.add_argument('--gains', type=int, nargs='+', default=[0, 9])
    ap.add_argument('--count', type=int, default=1)
    ap.add_argument('--ratio', type=int, default=0, help='0=timed capture; 3=test normal triggering')
    ap.add_argument('--save', default='captures/agc')
    args = ap.parse_args()
    if any(g not in range(14) for g in args.gains):
        ap.error('gains must be 0..13')
    if any(p < -9 or p > 22 for p in args.powers):
        ap.error('bench powers must be -9..22 dBm')
    directory = Path(args.save) / datetime.now().strftime('%Y%m%d_%H%M%S')
    directory.mkdir(parents=True)
    results = []
    with serial.Serial(default_port('XIAO', '/dev/ttyACM0'), 115200, timeout=.2, exclusive=True) as rx, \
            serial.Serial(default_port('RAK3401', '/dev/ttyACM1'), 115200, timeout=.2, exclusive=True) as tx:
        command(rx, 'hold on')
        try:
            for power in args.powers:
                command(tx, f'set tx {power}')
                for gain in args.gains:
                    command(rx, f'rc 021A 0 {gain:02X}')
                    for attempt in range(args.count):
                        def transmit():
                            time.sleep(1.2)
                            reply = command(tx, 'advert.zerohop')
                            if 'OK - zerohop' not in reply:
                                raise RuntimeError(f'transmit failed: {reply}')
                        start = time.monotonic()
                        with ThreadPoolExecutor(max_workers=1) as pool:
                            sent = pool.submit(transmit)
                            # Disable the power trigger so its failure cannot hide the packet.
                            summary, data = one_capture(rx, f'stream 0 {args.ratio} 3400 8 8 3', 3.4)
                            sent.result()
                        if not data:
                            raise RuntimeError(f'no IQ: {summary}')
                        stem = f'p{power:+d}_g{gain:02d}_{attempt}'
                        path = directory / f'{stem}.bin'
                        path.write_bytes(data)
                        rx.write(b'streamstats\r')
                        stats = read_until(rx, b'streamstats done', 4).decode(errors='replace')
                        (directory / f'{stem}.stats.txt').write_text(stats)
                        row = dict(power_dbm=power, manual_gain=gain, file=str(path),
                                   summary=summary, elapsed_s=time.monotonic() - start)
                        rows = [list(map(int, line.split())) for line in stats.splitlines()
                                if len(line.split()) == 5 and all(t.isdigit() for t in line.split())]
                        if rows:
                            values = np.array(rows)
                            row.update(actual_gains=sorted(set(values[:, 2].tolist())),
                                       clipped_samples=int(values[:, 4].sum()))
                        iq = load_iq8(path)
                        # Timed packet is approximately 2.7..3.18 seconds; ring covers 2.61..3.4.
                        noise = iq[:4000]
                        packet = iq[14000:42000]
                        row.update(noise_power=float(np.mean(abs(noise) ** 2)),
                                   packet_power=float(np.mean(abs(packet) ** 2)))
                        try:
                            result = decode(iq, 250000 / 3)
                            row.update(crc_ok=result.get('crc_ok'), error=result.get('error'),
                                       meshcore=result.get('meshcore'),
                                       test_advert=result.get('meshcore', {}).get('body', {}).get('public_key')
                                       == 'aac8a2275ac71e9edcec1b60fc279836bd72f118fbe05bc2a538b5874e32fd3a')
                        except DecodeError as e:
                            row.update(crc_ok=False, error=str(e))
                        results.append(row)
                        (directory / 'results.json').write_text(json.dumps(results, indent=2))
                        print('RESULT', json.dumps({k: v for k, v in row.items() if k != 'meshcore'}), flush=True)
        finally:
            command(tx, 'set tx -9')
            command(rx, 'rc 021A 0 00')
    print('Saved', directory, flush=True)


if __name__ == '__main__':
    main()
