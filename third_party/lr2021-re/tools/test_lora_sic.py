"""Offline SIC acceptance tests against separately recorded, real RF packets."""
from pathlib import Path
import copy
import unittest

import numpy as np

from lora_decode import decode, load_iq8, _header_ok
from lora_sic import cancel_packet, corrected_symbols, decode_sic, json_report, waveform
from sic_demo import mix_recordings

ROOT = Path(__file__).resolve().parents[1]
FS = 250000 / 3


class SICTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.a = load_iq8(ROOT / 'captures/iter/c00.bin')
        cls.b = load_iq8(ROOT / 'captures/iter/c01.bin')
        cls.pa = decode(cls.a, FS, return_phy=True)
        cls.pb = decode(cls.b, FS)
        cls.expected = {cls.pa['payload'], cls.pb['payload']}

    def test_real_capture_symbol_reconstruction_uses_decoded_fec_not_fft_decisions(self):
        expected = self.pa['phy']['raw_symbols']
        self.assertEqual(corrected_symbols(self.pa), expected)
        disturbed = dict(self.pa, phy=dict(self.pa['phy'], raw_symbols=[0] * len(expected)))
        self.assertEqual(corrected_symbols(disturbed), expected)
        self.assertNotIn('phy', self.pb)  # default decoder output stays compact

    def test_overlapping_recordings_recover_two_exact_payloads(self):
        for delay, weaker_db, cfo, phase in ((.043, 6, 2300, .7), (.119, 9, -1700, -1.1)):
            with self.subTest(delay=delay, weaker_db=weaker_db, cfo=cfo):
                mixture = mix_recordings(self.a, self.b, FS, delay, weaker_db, cfo, phase)
                original = mixture.copy()
                result = decode_sic(mixture, FS, max_packets=3, return_stages=True)
                self.assertEqual({p['payload'] for p in result['baseline_packets']}, {self.pa['payload']})
                self.assertEqual({p['payload'] for p in result['packets']}, self.expected)
                self.assertEqual(len(result['packets']), 2)
                self.assertTrue(all(p['crc_ok'] and p['header_ok'] for p in result['packets']))
                self.assertTrue(all(s['accepted'] for s in result['cancellations']))
                self.assertGreater(result['cancellations'][0]['occupied_power_reduction_db'], 5)
                self.assertEqual(len(result['residual']), len(mixture))
                self.assertEqual(len(result['stages']), 2)
                np.testing.assert_array_equal(mixture, original)
                self.assertEqual(json_report(result)['packets'][1]['payload'], self.pb['payload'].hex())

    def test_regenerated_waveforms_decode_at_all_four_coding_rates(self):
        # Exercise the new encoder against the existing independent PHY decoder,
        # including full header parity and CRC bytes from an actual RF packet.
        for cr in (1, 2, 3, 4):
            with self.subTest(cr=cr):
                packet = copy.deepcopy(self.pa)
                packet['cr'] = cr
                header = packet['phy']['header_nibbles']
                header[2] = cr * 2 + 1
                for checksum in range(32):
                    header[3], header[4] = checksum >> 4, checksum & 15
                    if _header_ok(header):
                        break
                packet['phy'].update(grid_start_chips=1024, data_start_chips=2592, up_symbols=10)
                packet['cfo_hz'] = 543
                length = (2592 + len(corrected_symbols(packet)) * 128 + 1024) * 4
                signal, _ = waveform(packet, length)
                rng = np.random.default_rng(12)
                signal += .005 * (rng.normal(size=length) + 1j * rng.normal(size=length))
                result = decode(signal, 250000)
                self.assertTrue(result['crc_ok'])
                self.assertEqual(result['cr'], cr)
                self.assertEqual(result['payload'], self.pa['payload'])

    def test_seeded_noise_recovery_and_noise_power(self):
        clean_mix = mix_recordings(self.a, self.b, FS)
        noisy = mix_recordings(self.a, self.b, FS, noise_snr_db=0, seed=20261028)
        repeated = mix_recordings(self.a, self.b, FS, noise_snr_db=0, seed=20261028)
        np.testing.assert_array_equal(noisy, repeated)
        other = mix_recordings(self.a, self.b, FS, noise_snr_db=0, seed=20261029)
        self.assertFalse(np.array_equal(noisy, other))
        # B has normalized power 1 before 6 dB attenuation. 0 dB added-noise
        # SNR means AWGN has the same power as the attenuated whole recording.
        expected_noise_power = 10 ** (-6 / 10)
        measured = float(np.mean(abs(noisy - clean_mix) ** 2))
        self.assertAlmostEqual(measured / expected_noise_power, 1, delta=.03)
        result = decode_sic(noisy, FS, max_packets=3)
        self.assertEqual({p['payload'] for p in result['baseline_packets']}, {self.pa['payload']})
        self.assertEqual({p['payload'] for p in result['packets']}, self.expected)
        self.assertTrue(all(p['crc_ok'] for p in result['packets']))

    def test_nonfinite_noise_setting_is_rejected(self):
        for value in (float('nan'), float('inf'), -float('inf')):
            with self.subTest(value=value), self.assertRaises(ValueError):
                mix_recordings(self.a, self.b, FS, noise_snr_db=value)

    def test_single_packet_does_not_create_a_second_reception(self):
        result = decode_sic(self.a, FS)
        self.assertEqual(len(result['packets']), 1)
        self.assertEqual(result['packets'][0]['payload'], self.pa['payload'])
        self.assertGreater(result['cancellations'][0]['occupied_power_reduction_db'], 10)

    def test_noise_is_unchanged_without_a_crc_valid_packet(self):
        rng = np.random.default_rng(721)
        noise = (rng.normal(size=16000) + 1j * rng.normal(size=16000)).astype(np.complex64)
        result = decode_sic(noise, FS)
        self.assertEqual(result['packets'], [])
        self.assertEqual(result['cancellations'], [])
        np.testing.assert_array_equal(result['residual'], noise)

    def test_no_initial_crc_has_no_oracle_fallback(self):
        # Equal-ish power can defeat acquisition of either packet. SIC must
        # admit failure instead of using the known source recordings.
        mixture = mix_recordings(self.a, self.b, FS, .043, 3, 2300, .7)
        result = decode_sic(mixture, FS)
        self.assertEqual(result['baseline_packets'], [])
        self.assertEqual(result['packets'], [])
        np.testing.assert_array_equal(result['residual'], mixture)

    def test_non_crc_packet_cannot_be_subtracted(self):
        packet = dict(self.pa, crc_ok=False)
        with self.assertRaisesRegex(ValueError, 'CRC-valid'):
            cancel_packet(np.ones(1000, np.complex64), packet)

    def test_invalid_parameters_and_unbounded_windows_are_rejected(self):
        for args in (dict(sf=8), dict(bw=125000), dict(taps=0), dict(taps=34), dict(max_packets=0)):
            with self.subTest(args=args), self.assertRaises(ValueError):
                decode_sic(self.a, FS, **args)
        for iq, fs in ((self.a, float('nan')), (self.a, 0), (self.a, 1000),
                       (np.zeros((100, 100), np.complex64), FS),
                       (np.zeros(round(FS * 4) + 1, np.complex64), FS),
                       (np.full(10000, np.nan, np.complex64), FS)):
            with self.assertRaises(ValueError):
                decode_sic(iq, fs)


if __name__ == '__main__':
    unittest.main()
