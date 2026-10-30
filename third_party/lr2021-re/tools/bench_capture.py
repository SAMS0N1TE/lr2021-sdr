"""Capture a low-power RAK zero-hop advert with the LR2021 debug receiver.

Stops normal mesh processing on the LR2021; reboot that receiver afterwards.
The 3.4-second window retains the scheduled packet even if the power trigger fails.
"""
import argparse
from concurrent.futures import ThreadPoolExecutor
from datetime import datetime
from pathlib import Path
import time

import numpy as np
import serial
from live_decode import one_capture
from lora_decode import decode, load_iq8, DecodeError
from meshcore_decode import summarize_packet
from radio_ports import default_port


def command(port, text):
    port.reset_input_buffer()
    port.write((text + '\r').encode())
    time.sleep(.6)
    reply = port.read(65536).decode(errors='replace').strip()
    print(f'{port.port} {text}: {reply!r}', flush=True)
    return reply


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--rx', default=default_port('XIAO', '/dev/ttyACM0'))
    ap.add_argument('--tx', default=default_port('RAK3401', '/dev/ttyACM1'))
    ap.add_argument('--power', type=int, default=-9)
    ap.add_argument('--gain', type=int, choices=range(14), default=None,
                    help='LR2021 gain 0=auto, 1..13=manual')
    ap.add_argument('--ratio', type=int, default=3)
    ap.add_argument('--count', type=int, default=1)
    ap.add_argument('--save', default='captures/debug')
    args = ap.parse_args()
    directory = Path(args.save)
    directory.mkdir(parents=True, exist_ok=True)
    with serial.Serial(args.rx, 115200, timeout=.2, exclusive=True) as rx, \
            serial.Serial(args.tx, 115200, timeout=.2, exclusive=True) as tx:
        command(tx, f'set tx {args.power}')
        command(tx, 'get tx')  # Firmware prints negative int8 values as unsigned (247 = -9).
        command(rx, 'hold on')
        if args.gain is not None:
            command(rx, f'rc 021A 0 {args.gain:02X}')
        for _ in range(args.count):
            def transmit():
                time.sleep(1.2)
                reply = command(tx, 'advert.zerohop')
                if 'OK' not in reply:
                    raise RuntimeError(f'RAK did not acknowledge zero-hop advert: {reply}')
            with ThreadPoolExecutor(max_workers=1) as pool:
                sent = pool.submit(transmit)
                summary, data = one_capture(rx, f'stream 0 {args.ratio} 3400 8 8 3', 3.4)
                sent.result()
            print(summary, flush=True)
            if not data:
                raise RuntimeError('no IQ returned')
            path = directory / (datetime.now().strftime('bench_%Y%m%d_%H%M%S_%f') + '.bin')
            path.write_bytes(data)
            print(f'saved {len(data)} bytes to {path}', flush=True)
            samples = np.frombuffer(data, dtype=np.int8).astype(float)
            blocks = samples[:len(samples) // 4096 * 4096].reshape(-1, 4096)
            print('block power:', np.mean(blocks ** 2, axis=1).round(1), flush=True)
            try:
                result = decode(load_iq8(path), 250000 / 3)
                print('CRC:', result.get('crc_ok'), 'error:', result.get('error'), flush=True)
                if 'meshcore' in result:
                    print(summarize_packet(result['meshcore']), flush=True)
            except DecodeError as e:
                print('DecodeError:', e, flush=True)


if __name__ == '__main__':
    main()
