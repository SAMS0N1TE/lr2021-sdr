import pathlib
import sys
import unittest

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1] / 'tools'))
from lr2021_make_temperature import generate
from lr2021_tx_upload import validate


class TemperatureGenerator(unittest.TestCase):
    def test_rf_verified_packets(self):
        for args, expected in [({}, '5a80ea8e'),
                (dict(temperature=-12.3, sensor_id=167, channel=3, battery_ok=False,
                      sample_rate=4800), 'a72f858f'),
                (dict(temperature=37.8, sensor_id=49, channel=2,
                      sample_rate=19200, rows=3), '31917a31')]:
            data, metadata = generate(**args)
            validate(data)
            self.assertEqual(metadata['packet_hex'], expected)
            self.assertLessEqual(metadata['duration_s'], .9)
            self.assertEqual(set(data[1::2]), {0})
            self.assertEqual(set(data[::2]), {0, 48})

    def test_bad_checksum_changes_only_checksum_byte(self):
        good, a = generate()
        bad, b = generate(corrupt=True)
        self.assertEqual(a['packet_hex'][:6], b['packet_hex'][:6])
        self.assertEqual(int(a['packet_hex'][6:], 16) ^ int(b['packet_hex'][6:], 16), 1)
        self.assertNotEqual(good, bad)

    def test_limits(self):
        for args in [dict(temperature=-40.1), dict(temperature=70.1),
                dict(temperature=float('nan')), dict(sensor_id=256),
                dict(channel=0), dict(amplitude=57), dict(amplitude=0),
                dict(rows=2), dict(sample_rate=125000), dict(sample_rate=19200)]:
            with self.subTest(args=args), self.assertRaises(ValueError):
                generate(**args)


if __name__ == '__main__':
    unittest.main()
