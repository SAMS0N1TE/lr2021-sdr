from pathlib import Path
import tempfile
import threading
import unittest
import numpy as np

from decode_continuous import decode_file


class ContinuousFileTests(unittest.TestCase):
    def test_window_boundary_and_repeated_payloads(self):
        root = Path(__file__).resolve().parents[1] / 'captures/iter'
        data = b''.join((root / name).read_bytes() for name in ('c00.bin', 'c01.bin', 'c00.bin'))
        done = threading.Event()
        done.set()
        packets = []
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / 'iq.bin'
            path.write_bytes(data)
            result = decode_file(path, 250000 / 3, done, packets.append)
        # Third packet crosses the first two-second window's end. Repeated
        # payloads are preserved, while overlapping-window copies are removed.
        self.assertEqual(result['packets'], 3)
        self.assertEqual(packets[0]['payload'], packets[2]['payload'])
        self.assertNotEqual(packets[0]['payload'], packets[1]['payload'])
        self.assertLess(packets[0]['t_end_s'], packets[1]['t_end_s'])
        self.assertLess(packets[1]['t_end_s'], packets[2]['t_end_s'])

    def test_iq16_preserves_decoded_payload_and_sample_times(self):
        root = Path(__file__).resolve().parents[1] / 'captures/iter'
        data = (root / 'c00.bin').read_bytes()
        done = threading.Event()
        done.set()
        events = []
        with tempfile.TemporaryDirectory() as directory:
            for sample_format in ('iq8', 'iq16'):
                path = Path(directory) / 'iq.bin'
                path.write_bytes(data if sample_format == 'iq8' else
                                 (np.frombuffer(data, np.int8).astype('<i2') * 256).tobytes())
                packets = []
                result = decode_file(path, 250000 / 3, done, packets.append,
                                     sample_format=sample_format)
                self.assertEqual(result['packets'], 1)
                events.append(packets[0])
        self.assertEqual(events[0]['payload'], events[1]['payload'])
        self.assertEqual(events[0]['sample_end'], events[1]['sample_end'])


if __name__ == '__main__':
    unittest.main()
