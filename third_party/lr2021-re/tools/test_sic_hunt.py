"""Prevent acquisition-only or nonoverlapping recoveries being called SIC collisions."""
import copy
import json
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

from lora_decode import DecodeError, load_iq8
from lora_sic import decode_sic
from sic_hunt import inspect_recoveries, read_summary, same_reception
from sic_demo import mix_recordings

ROOT = Path(__file__).resolve().parents[1]
FS = 250000 / 3


class HuntTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        a = load_iq8(ROOT / 'captures/iter/c00.bin')
        b = load_iq8(ROOT / 'captures/iter/c01.bin')
        cls.iq = mix_recordings(a, b, FS, noise_snr_db=0, seed=20261028)
        cls.result = decode_sic(cls.iq, FS, max_packets=3)

    def test_synthetic_positive_has_overlap_and_fails_original_iq_control(self):
        findings = inspect_recoveries(self.iq, FS, self.result)
        self.assertEqual(len(findings), 1)
        self.assertTrue(findings[0]['strict_collision_candidate'])
        self.assertGreater(findings[0]['overlaps'][0]['payload_symbol_overlap_s'], .3)
        self.assertFalse(findings[0]['recovered_without_subtraction'])

    def test_timing_hint_alone_is_not_evidence_for_cancellation(self):
        packet = self.result['packets'][1]
        with patch('sic_hunt.decode', return_value=packet):
            findings = inspect_recoveries(self.iq, FS, self.result)
        self.assertTrue(findings[0]['recovered_without_subtraction'])
        self.assertFalse(findings[0]['strict_collision_candidate'])

    def test_nonoverlapping_recovery_is_not_a_collision(self):
        result = copy.deepcopy(self.result)
        packet = result['packets'][1]
        packet['packet_start_s'] += 2
        packet['packet_end_s'] += 2
        packet['phy']['data_start_chips'] += 2 * 62500
        with patch('sic_hunt.decode', side_effect=DecodeError('no control decode')):
            findings = inspect_recoveries(self.iq, FS, result)
        self.assertEqual(findings[0]['overlaps'], [])
        self.assertFalse(findings[0]['strict_collision_candidate'])

    def test_baseline_recovery_is_not_new(self):
        result = dict(self.result, baseline_packets=self.result['packets'])
        self.assertEqual(inspect_recoveries(self.iq, FS, result), [])

    def test_repeats_are_distinguished_by_time(self):
        a = dict(payload='abcd', packet_end_s=1)
        self.assertTrue(same_reception(a, dict(a, packet_end_s=1.005)))
        self.assertFalse(same_reception(a, dict(a, packet_end_s=1.043)))

    def test_incomplete_recorder_summary_is_not_an_end_marker(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / 'summary.json'
            self.assertIsNone(read_summary(path))
            path.write_text('{"valid":')
            self.assertIsNone(read_summary(path))
            path.write_text(json.dumps(dict(valid=True)))
            self.assertEqual(read_summary(path), dict(valid=True))


if __name__ == '__main__':
    unittest.main()
