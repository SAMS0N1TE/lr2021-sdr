from pathlib import Path
import sys
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools'))
try:
    import numpy as np
    from lr2021_make_p25 import modulate
    from lr2021_decode_p25_capture import demodulate
except ImportError:
    np = None


@unittest.skipIf(np is None, 'NumPy is required for P25 waveform tools')
class P25Tools(unittest.TestCase):
    def test_rf_verified_waveform(self):
        root = ROOT / 'research/tx-p25/inputs'
        data, metadata = modulate((root / 'voice-tone.p25').read_bytes())
        self.assertEqual(data, (root / 'voice-tone.iq8').read_bytes())
        self.assertEqual(metadata['sample_count'], 7776)
        self.assertEqual(metadata['duration_s'], .405)
        pairs = np.frombuffer(data, np.int8).astype(int).reshape(-1, 2)
        self.assertTrue(np.all(np.sum(pairs*pairs, axis=1) <= 56*56))

    def test_buffer_bounds(self):
        packet = (ROOT / 'research/tx-p25/inputs/voice-tone.p25').read_bytes()
        for args in [(b'', 19200, 48), (packet, 38400, 48),
                     (packet, 125000, 48), (packet, 19200, 56)]:
            with self.subTest(args=args[1:]), self.assertRaises(ValueError):
                modulate(*args)

    def test_squelched_input(self):
        self.assertFalse(np.any(demodulate(bytes(64000))))
        for data in [b'', bytes(63)]:
            with self.assertRaises(ValueError):
                demodulate(data)


if __name__ == '__main__':
    unittest.main()
