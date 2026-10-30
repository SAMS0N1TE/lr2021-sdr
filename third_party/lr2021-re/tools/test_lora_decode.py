"""Run with: python3 -m unittest discover -s tools -p 'test_*.py'."""
from pathlib import Path
import unittest

from lora_decode import decode, load_iq8
from meshcore_decode import parse_packet, summarize_packet, MeshCoreError

ROOT = Path(__file__).resolve().parents[1]
KEY = 'aac8a2275ac71e9edcec1b60fc279836bd72f118fbe05bc2a538b5874e32fd3a'


class CaptureTests(unittest.TestCase):
    def test_saved_adverts(self):
        files = (sorted((ROOT / 'captures/iter').glob('c*.bin'))
                 + [ROOT / 'captures/stream_rak3.bin']
                 + sorted((ROOT / 'captures/live').glob('cap_000*.bin')))
        self.assertEqual(len(files), 12)
        for path in files:
            with self.subTest(capture=path.name):
                result = decode(load_iq8(path), 83333.333)
                self.assertTrue(result['header_ok'])
                self.assertTrue(result['crc_ok'])
                self.assertEqual(len(result['payload']), 130)
                packet = result['meshcore']
                self.assertEqual(packet['type'], 'ADVERT')
                self.assertEqual(packet['path_count'], 0)
                self.assertEqual(packet['body']['public_key'], KEY)
                self.assertEqual(packet['body']['name'], 'RAK3401 1W Repeater')
                self.assertEqual(packet['body']['role'], 'REPEATER')


class PacketTests(unittest.TestCase):
    def test_transport_and_multibyte_path(self):
        p = parse_packet(bytes.fromhex('0f3412785642aabbccdd01020304'))
        self.assertEqual(p['transport_codes'], [0x1234, 0x5678])
        self.assertEqual(p['path'], ['aabb', 'ccdd'])
        self.assertEqual(p['body']['ack_crc'], '01020304')

    def test_encrypted_message(self):
        p = parse_packet(bytes.fromhex('09001234abcd') + bytes(16))
        self.assertEqual(p['body']['destination_hash'], '12')
        self.assertEqual(p['body']['source_hash'], '34')
        self.assertEqual(p['body']['mac'], 'abcd')
        self.assertEqual(p['body']['ciphertext'], '00' * 16)
        self.assertIn('encrypted=16B', summarize_packet(p))

    def test_optional_advert_fields(self):
        app = (bytes([0xf2]) + int(-12345678).to_bytes(4, 'little', signed=True)
               + int(45678901).to_bytes(4, 'little', signed=True)
               + bytes.fromhex('34127856') + 'Relay 🛰'.encode())
        p = parse_packet(bytes.fromhex('1100') + bytes(100) + app)
        b = p['body']
        self.assertEqual(b['latitude'], -12.345678)
        self.assertEqual(b['longitude'], 45.678901)
        self.assertEqual(b['feature1'], 0x1234)
        self.assertEqual(b['feature2'], 0x5678)
        self.assertEqual(b['name'], 'Relay 🛰')
        self.assertIsNone(b['signature_verified'])

    def test_future_version_kept_raw(self):
        p = parse_packet(bytes.fromhex('510001'))
        self.assertTrue(p['body']['unsupported_version'])

    def test_malformed_packets(self):
        for data in (b'', b'\x11', bytes.fromhex('110100'), bytes.fromhex('11c001'),
                     bytes.fromhex('11bf01'), bytes.fromhex('110001'),
                     bytes.fromhex('09001234abcd'),
                     bytes.fromhex('1100') + bytes(100) + b'\x10'):
            with self.subTest(data=data.hex()):
                with self.assertRaises(MeshCoreError):
                    parse_packet(data)


if __name__ == '__main__':
    unittest.main()
