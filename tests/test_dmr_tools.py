from pathlib import Path
import sys
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools'))
from lr2021_tx_upload import validate
try:
    import numpy as np
    from lr2021_make_dmr import (bits_bytes, bptc_encode, crc32_msb, crc_ccitt, csbk_bursts,
        data_call, lc_block, modulate, slot_type, voice_call)
    from lr2021_decode_dmr import bptc_decode, decode
except ImportError:
    np = None

# Outputs of unmodified MMDVM-Host encoders (commit 590c5313), copied from
# LakeShark bench/fixtures/dmr_reference_vectors.h: all 256 Slot Type words,
# then nine-octet LC with header/terminator RS masks and the BPTC payload of
# a burst whose sync and Slot Type bits are zero.
SLOT_WORDS = (
    '00000018eb0293e031d504a970527c063a907b4208dc60952d0a4f80bc130c7510dfba0ee6f0f684'
    '1036711b8c12a59132b2149f01511b160ce1782518ea11964a1a79f1bf741c4361dcdd1ed081f5e3'
    '206cd21e2622ff32371824c5a254b12656427d8f28b0b293e02a2352bade2c19c2d9772e8a22f049'
    '305aa31d4132c943347f34f3d357d63660337ee83886c390873a1523b9b93c2fb3da103ebc53f32e'
    '40d9941572424a743c4c4470e45fe546e30476db4805f498b44a9614b18a4cac84d2234e3f64fb1d'
    '50efe51615527c053f2b5446955c8256d57575bc5833859bd35aa065b2ed5c9af5d1445e0915f87a'
    '60b54613bf6226a63a81641c365928668fd670166869269e796afac6b7476cc056d4ee6e53b6fdd0'
    '70833710d87210d739e6742a475a4f76b9a77371785f579d1e7accb7b4207cf627d7897e65c7feb7'
    '803da81b3182ae48320f8494d851a6860738789888e1c896f78a7228bfc98c48b8dc608edb58f55e'
    '900bd91856929839316894a2a952c19631497bff98d7b995909a4459bcae9c7ec9df079eed29f639'
    'a0517a1dfca2c29a34c2a4f80a576ba66bea7e55a88d1a903aaa1efab904ac246adaadaeb78af393'
    'b0670b1e9bb2f4eb37a5b4ce7b540cb65d9b7d32b8bb6b935dba288bba63bc121bd9cabe81fbf0f4'
    'c0e43c16a8c277dc3f96c44d4c5c3fc6deac7501c8385c9b6ecaabbcb250cc912cd1f9ce02ccf8c7'
    'd0d24d15cfd241ad3cf1d47b3d5f58d6e8dd7666d80e2d9809da9dcdb137dca75dd29ede34bdfba0'
    'e088ee1065e21b0e395be4219e5af2e6b27e73cce8548e9da3eac76eb49decfdfed734ee6e1efe0a'
    'f0be9f1302f22d7f3a3cf417ef5995f6840f70abf862ff9ec4faf11fb7fafccb8fd453fe586ffd6d')
LC_VECTORS = [
    ('00000000005b1234569c55f1', '02080a18151809e04ee04520400000000000000002b8154815d03e001841410213'),
    ('00000000005b123456935afe', '02670acc15a809984e904580400000000000000003ac167012b032401041580220'),
    ('8310c0fedcba6543212c5083', '19b1fc23d73ae0089580f243800000000000000000e960186519cae26a1c292f75'),
    ('8310c0fedcba654321235f8c', '19defcf7d78ae07095f0f2e3800000000000000001fd63206279c6a2621c302f46'),
    ('ff55aa123456789abcbcf43f', '7423115fbfb703147f2ea83300000000000000000027fc3cfc4f5c223fc8a216df'),
    ('ff55aa123456789abcb3fb30', '744c118bbf07036c7f5ea89300000000000000000133ff04fb2f506237c8bb16ec'),
]


@unittest.skipIf(np is None, 'NumPy is required for DMR waveform tools')
class DmrTools(unittest.TestCase):
    def test_reference_vectors(self):
        words = [int(SLOT_WORDS[i:i + 5], 16) for i in range(0, len(SLOT_WORDS), 5)]
        self.assertEqual([slot_type(i >> 4, i & 15) for i in range(256)], words)
        for n, (lc, wire) in enumerate(LC_VECTORS):
            lc = bytes.fromhex(lc)
            self.assertEqual(lc_block(lc[:9], 1 + n % 2), lc)
            info = bptc_encode(lc)
            self.assertEqual(bits_bytes(info[:98] + [0] * 68 + info[98:]).hex(), wire)
            for position in range(196):
                corrupt = list(info)
                corrupt[position] ^= 1
                # Coded bit 0 is the unprotected reserved bit R(3).
                self.assertEqual(bptc_decode(corrupt), (lc, int(position > 0)))

    def test_crc_engines(self):
        self.assertEqual(crc_ccitt(b'123456789'), 0x31C3 ^ 0xFFFF)
        self.assertEqual(crc32_msb(b'123456789'), 0x765E7680 ^ 0xFFFFFFFF)

    def check(self, bursts, rate, expected):
        data, metadata = modulate(bursts, rate)
        validate(data)
        self.assertLessEqual(metadata['sample_count'], 8192)
        self.assertEqual(metadata['sample_count'] % (rate * 6 // 100), 0)
        result = decode(data, rate)
        self.assertEqual([b['bits'] for b in result['bursts']],
                         [b['burst'] for b in metadata['bursts']])
        for got, want in zip(result['bursts'], expected):
            self.assertTrue(got['ok'])
            self.assertEqual(got['sync_errors'], 0)
            self.assertEqual(got['bptc_corrections'], 0)
            self.assertLess(abs(got['gain_hz'] - 1944), 1944 * .01)
            for dibit, hz in zip(('00', '01', '10', '11'), (648, 1944, -648, -1944)):
                self.assertLess(abs(got['levels_hz'][dibit] - hz), 20)
            self.assertEqual({k: got[k] for k in want}, want)
        self.assertEqual(len(result['bursts']), len(expected))
        return result

    def test_voice_and_csbk_loopback(self):
        ids = dict(source=3120001, destination=91, colour_code=7)
        for private in (False, True):
            lc = dict(ids, call='private' if private else 'group', fid=0, service_options=0)
            self.check(voice_call(3120001, 91, 7, private), 19200,
                       [dict(lc, type='voice_lc_header')] * 2 + [dict(lc, type='terminator_lc')] * 2)
        csbk = dict(ids, type='csbk', csbko=0x3D, group=1, data_follows=0)
        self.check(csbk_bursts(3120001, 91, 7, count=3), 19200,
                   [dict(csbk, blocks_to_follow=n) for n in (2, 1, 0)])
        for bursts, name in ((voice_call(1, 2, 0, headers=1, terminators=0), 'voice_lc_header'),
                             (voice_call(1, 2, 0, headers=0, terminators=1), 'terminator_lc'),
                             (csbk_bursts(1, 2, 0, count=1), 'csbk')):
            self.check(bursts, 125000, [dict(type=name, source=1, destination=2, colour_code=0)])

    def test_data_loopback(self):
        payload = b'Hello from the LR2021 SDR'
        bursts = data_call(payload, 1234, 5678, 3, private=True, preambles=2)
        header = dict(type='data_header', source=1234, destination=5678, group=0, dpf=2,
                      sap=4, blocks=3, pad=7, fsn=8)
        result = self.check(bursts, 19200, [dict(type='csbk', blocks_to_follow=5, data_follows=1),
                             dict(type='csbk', blocks_to_follow=4), header]
                            + [dict(type='rate_half_data')] * 3)
        self.assertEqual(result['messages'], [dict(ok=True, source=1234, destination=5678,
                                                   sap=4, payload=payload.hex())])

    def test_offset_and_noise(self):
        payload = bytes(range(40))
        data, _ = modulate(data_call(payload, 7, 9), 19200)
        iq = np.frombuffer(data, np.int8).reshape(-1, 2).astype(float)
        iq = (iq[:, 0] + 1j * iq[:, 1]) * np.exp(2j * np.pi * 400 * np.arange(len(iq)) / 19200)
        # About 9 dB SNR across the 19.2 kHz sampled bandwidth.
        rng = np.random.default_rng(2021)
        iq += rng.normal(0, 12, (len(iq), 2)) @ [1, 1j]
        noisy = np.clip(np.rint(np.column_stack([iq.real, iq.imag])), -127, 127).astype(np.int8)
        result = decode(noisy.tobytes(), 19200)
        self.assertEqual(len(result['bursts']), 6)
        self.assertTrue(all(b['ok'] for b in result['bursts']))
        self.assertGreater(sum(b['bptc_corrections'] for b in result['bursts']), 0)
        self.assertTrue(all(abs(b['offset_hz'] - 400) < 100 for b in result['bursts']))
        self.assertEqual(result['messages'][0]['payload'], payload.hex())

    def test_corrupted_crc_rejected(self):
        bursts = data_call(b'checksum', 7, 9, preambles=0)
        bits = list(bursts[-1]['bits'])
        info = bits[:98] + bits[166:]
        block, _ = bptc_decode(info)
        flipped = bytes([block[0] ^ 1]) + block[1:]
        coded = bptc_encode(flipped)
        bursts[-1] = dict(bursts[-1], bits=coded[:98] + bits[98:166] + coded[98:])
        result = decode(modulate(bursts, 19200)[0], 19200)
        self.assertFalse(result['messages'][0]['ok'])
        self.assertFalse(result['bursts'][-1]['ok'])

    def test_buffer_bounds(self):
        seven = csbk_bursts(1, 2, count=7)
        self.assertEqual(modulate(seven)[1]['sample_count'], 8064)
        self.assertEqual(modulate(seven)[1]['max_repeats'], 2)
        for args in [(csbk_bursts(1, 2, count=8), 19200, 48), (csbk_bursts(1, 2), 125000, 48),
                     (seven[:1], 76800, 48), (seven[:1], 38400, 48), (seven[:1], 9600, 48),
                     (seven[:1], 19200, 56), ([], 19200, 48)]:
            with self.subTest(rate=args[1], amplitude=args[2]), self.assertRaises(ValueError):
                modulate(*args)
        for call in [lambda: voice_call(0, 2), lambda: voice_call(1, 1 << 24),
                     lambda: voice_call(1, 2, 16), lambda: voice_call(1, 2, 1, False, 0, 0),
                     lambda: csbk_bursts(1, 2, count=0), lambda: data_call(b'', 1, 2)]:
            with self.assertRaises(ValueError):
                call()

    def test_silence_has_no_bursts(self):
        self.assertEqual(decode(bytes(4096), 19200), dict(bursts=[], messages=[]))


if __name__ == '__main__':
    unittest.main()
