"""Replay every archived RF capture through an external, unmodified rtl_433."""
import argparse
import hashlib
import json
from pathlib import Path
import sys

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / 'tools'))
from lr2021_decode_temperature_capture import decode

p = argparse.ArgumentParser(description=__doc__)
p.add_argument('--rtl433', type=Path, required=True)
a = p.parse_args()
root = Path(__file__).resolve().parent
for case in json.loads((root / 'manifest.json').read_text()):
    capture = root / 'samples' / (case['name'] + '.iq8.gz')
    assert hashlib.sha256(capture.read_bytes()).hexdigest() == case['gzip_sha256']
    result = decode(capture, a.rtl433)
    assert result['raw_sha256'] == case['raw_sha256']
    records, expected = result['records'], case['expected']
    assert len(records) == int(expected is not None), (case['name'], records)
    if expected:
        assert records[0]['model'] == 'Acurite-606TX'
        assert records[0]['mic'] == 'CHECKSUM'
        assert all(records[0][key] == value for key, value in expected.items())
    print(case['name'], json.dumps(records))
print('All five RF captures match the recorded outcomes.')
