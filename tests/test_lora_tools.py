# SPDX-License-Identifier: MIT
import json, os, sys, tempfile, unittest
from collections import deque
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
CAPTURES = ROOT / 'third_party' / 'lr2021-re' / 'captures' / 'iter'
try:
    import numpy as np, scipy
except ImportError:
    print('numpy or scipy not installed: skipped')
    sys.exit(77)
sys.path.insert(0, str(ROOT / 'tools'))
import lr2021_lora
from lr2021_bridge import IQ8

RATE = 83333


class FakeBridge:
    def __init__(self, iq8, skip_frame=None):
        self.hello_info = dict(caps=511, version=1, rom=(1, 24), name='fake', id='fake')
        self.data, self.ends, self.skip = deque(), deque(), skip_frame
        self.iq8, self.stopped, self.rx = iq8, False, None

    def set_rx(self, *args):
        self.rx = args
        return 0, RATE

    def rx_start(self, duration_ms=0, fmt=0):
        pairs = len(self.iq8) // 2
        for n, first in enumerate(range(0, pairs, 960)):
            if n == self.skip:
                continue
            count = min(960, pairs - first)
            header = dict(generation=1, rate_hz=RATE, first_pair=first, start_time=0, dropped=0, pairs=count,
                          format=IQ8, shift=0)
            self.data.append((header, self.iq8[2 * first:2 * (first + count)]))
        return 1, RATE

    def read(self, timeout=0.2):
        if self.data and not self.stopped:
            return self.data.popleft()
        if not self.ends:
            self.ends.append(1)
        return None

    def call(self, kind, payload=b'', timeout=3.0):
        self.stopped = True
        return b''

    def wait_end(self, timeout=3.0):
        return dict(capture_error=0, recovery_error=0, pairs=len(self.iq8) // 2, dropped=0)

    def get_gain(self, timeout=2.0):
        return dict(status=0, step=13, manual=0, gain_value=139, rssi_raw=240, device_time=1)

    def close(self):
        pass


def use(bridge):
    lr2021_lora.Bridge.open = staticmethod(lambda target, baud=0: bridge)


class LoraToolTests(unittest.TestCase):
    def test_capture_decodes_the_three_recorded_packets(self):
        data = b''.join((CAPTURES / n).read_bytes() for n in ('c00.bin', 'c01.bin', 'c00.bin'))
        use(FakeBridge(data))
        with tempfile.TemporaryDirectory() as tmp:
            directory, summary = lr2021_lora.capture(['fake', '--route', 'source0', '--seconds', '0',
                                                      '--out', tmp, '--decode'])
            self.assertTrue(summary['valid'])
            self.assertEqual(summary['sample_format'], 'iq8')
            self.assertEqual((directory / 'iq.bin').read_bytes(), data)
            packets = [json.loads(l) for l in (directory / 'packets.jsonl').read_text().splitlines()]
            self.assertEqual(len(packets), 3)
            self.assertEqual(packets[0]['payload'], packets[2]['payload'])
            self.assertNotEqual(packets[0]['payload'], packets[1]['payload'])
            self.assertEqual(summary['decoder']['packets'], 3)

    def test_a_dropped_frame_is_zero_filled_and_marks_the_capture(self):
        data = (CAPTURES / 'c00.bin').read_bytes()
        use(FakeBridge(data, skip_frame=5))
        with tempfile.TemporaryDirectory() as tmp:
            directory, summary = lr2021_lora.capture(['fake', '--route', 'source0', '--seconds', '0', '--out', tmp])
            iq = (directory / 'iq.bin').read_bytes()
            self.assertFalse(summary['valid'])
            self.assertEqual((summary['gaps'], summary['gap_pairs']), (1, 960))
            self.assertEqual(len(iq), len(data))
            self.assertEqual(iq[2 * 5 * 960:2 * 6 * 960], bytes(2 * 960))

    def test_snapshot_triggers_on_the_packet_and_decodes_it(self):
        rng = np.random.default_rng(1)
        quiet = rng.integers(-2, 3, 2 * RATE, dtype=np.int8).tobytes()
        packet = (CAPTURES / 'c02.bin').read_bytes()
        use(FakeBridge(quiet + packet + quiet))
        with tempfile.TemporaryDirectory() as tmp:
            lr2021_lora.snapshot(['fake', '--route', 'source0', '--seconds', '0', '--out', tmp, '--decode',
                                  '--pre', '0.1', '--post', '0.9', '--ratio', '4'])
            metas = [json.loads(p.read_text()) for p in Path(tmp).glob('shot_*.json')]
            self.assertEqual(len(metas), 1)
            self.assertIn('power', metas[0]['reason'])
            self.assertTrue(metas[0]['decoded']['crc_ok'])


if __name__ == '__main__':
    unittest.main()
