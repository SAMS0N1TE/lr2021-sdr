import pathlib,sys,unittest
sys.path.insert(0,str(pathlib.Path(__file__).resolve().parents[1]/'tools'))
from lr2021_make_apsk import make_frame
from lr2021_tx_upload import validate
class TxTools(unittest.TestCase):
    def test_payload_and_quantization(self):
        frame=make_frame(b'LR2021IQ')
        self.assertEqual(frame['crc16_ccitt_false'],'3A9B')
        bits=''.join(f'{s:03b}' for s in frame['data_symbols'])
        self.assertEqual(bytes(int(bits[i:i+8],2) for i in range(0,80,8)),b'LR2021IQ\x3a\x9b')
        raw=bytes(v&255 for pair in frame['iq_pairs'] for v in pair)
        self.assertEqual(len(raw),688);validate(raw)
    def test_buffer_boundaries(self):
        validate(bytes([56,0]));validate(bytes([200,0]));validate(bytes(16384))
        for data in [b'',b'\0',bytes(16386),bytes([57,0]),bytes([128,0]),bytes([40,40])]:
            with self.assertRaises(ValueError):validate(data)
    def test_frame_length(self):
        for data in [b'',b'1234567',b'123456789']:
            with self.assertRaises(ValueError):make_frame(data)
if __name__=='__main__':unittest.main()
