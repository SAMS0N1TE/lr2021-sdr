"""Verify the archived P25 RF recordings with an external, unmodified DSD-neo."""
import argparse
import hashlib
import json
from pathlib import Path
import sys
import tempfile

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / 'tools'))
from lr2021_decode_p25_capture import decode

p = argparse.ArgumentParser(description=__doc__)
p.add_argument('--dsd', type=Path, required=True)
a = p.parse_args()
root = Path(__file__).resolve().parent
expected = json.loads((root / 'reference/expected-imbe.json').read_text())
with tempfile.TemporaryDirectory(prefix='p25-replay-') as temporary:
    for case in json.loads((root / 'manifest.json').read_text()):
        capture = root / 'samples' / (case['name'] + '.iq8.gz')
        assert hashlib.sha256(capture.read_bytes()).hexdigest() == case['gzip_sha256']
        result = decode(capture, a.dsd, Path(temporary) / case['name'])
        assert result['raw_sha256'] == case['raw_sha256']
        assert result['imbe_frames'] == case['expected_imbe_frames']
        if case['expected_imbe_frames']:
            assert result['frame_types'] == dict(LDU1=2, LDU2=2, TDU=2)
            assert result['nac_values'] == ['293']
            assert result['group_source_pairs'] == [('4242', '123456')]
            assert result['imbe_data_hex'] == expected
            assert all(count == [0, 0] for count in result['imbe_error_counts'])
            assert result['decoder_error_totals']['irrecoverable header errors'] == 0
            assert abs(result['audio']['peak_frequency_hz'] - 1011) < 2
            assert result['audio']['frames'] == 5760
        else:
            assert not result['frame_types'] and result['audio'] is None
        print(case['name'], result['imbe_frames'], 'IMBE frames', result['audio'])
print('Two P25 RF captures and the received-noise control match recorded outcomes.')
