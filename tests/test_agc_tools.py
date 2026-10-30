# SPDX-License-Identifier: MIT
import csv, os, subprocess, sys, tempfile, threading

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, 'research', 'agc-readback'))
try:
    import numpy
except ImportError:
    print('numpy not installed: skipped')
    sys.exit(77)
import agc_readback, analyze_agc

TABLE = analyze_agc.TABLE


def serve(exe, amplitude):
    p = subprocess.Popen([exe, '--port', '0', '--pattern', 'tone', '--tone', '433910000',
                          '--amplitude', str(amplitude)], stdout=subprocess.PIPE, text=True)
    line = p.stdout.readline()
    assert 'port' in line, line
    threading.Thread(target=p.stdout.read, daemon=True).start()
    return p, int(line.split()[-1])


def read(path):
    with open(path, newline='') as f:
        return list(csv.DictReader(f))


def main(exe):
    tmp = tempfile.mkdtemp()
    p, port = serve(exe, 4000)
    try:
        dev = f'tcp:127.0.0.1:{port}'
        out = os.path.join(tmp, 'sweep.csv')
        agc_readback.main([dev, '--out', out, 'sweep', '--ms', '150', '--steps', '1,7,13', '--tone-offset', '10000'])
        r = read(out)
        assert [int(x['step_set']) for x in r] == [1, 7, 13], r
        for x in r:
            s = int(x['step_set'])
            assert (int(x['idle_step']), int(x['idle_manual']), int(x['idle_value'])) == (s, s, TABLE[s - 1]), x
            assert x['stream_step'] == '' or int(x['stream_step']) == s, x
        tones = [float(x['tone_dbfs']) for x in r]
        assert tones[0] < tones[1] < tones[2], tones
        assert analyze_agc.sweep(out) == 0
        out = os.path.join(tmp, 'log.csv')
        agc_readback.main([dev, '--out', out, 'log', '--seconds', '1.2', '--interval-ms', '200'])
        steps = [int(x['chip_step']) for x in read(out) if x['chip_step']]
        assert steps and set(steps) == {13}, steps
        out = os.path.join(tmp, 'iq6.csv')
        agc_readback.main([dev, '--out', out, 'iq6', '--seconds', '1.2', '--interval-ms', '200', '--rate', '125000'])
        r = read(out)
        assert r and all(x['agree'] == '1' for x in r), r
    finally:
        p.terminate()
        p.wait()
    p, port = serve(exe, 20000)
    try:
        out = os.path.join(tmp, 'strong.csv')
        agc_readback.main([f'tcp:127.0.0.1:{port}', '--out', out, 'log', '--seconds', '1.2', '--interval-ms', '200'])
        steps = [int(x['chip_step']) for x in read(out) if x['chip_step']]
        assert steps and set(steps) == {5}, steps
        out = os.path.join(tmp, 'host.csv')
        agc_readback.main([f'tcp:127.0.0.1:{port}', '--out', out, 'log', '--step', '13', '--host-agc',
                           '--seconds', '1.5', '--interval-ms', '200'])
        r = [x for x in read(out) if x['chip_step']]
        assert r and all(x['chip_step'] == x['host_step'] for x in r), r
    finally:
        p.terminate()
        p.wait()
    print('agc readback tools against the bridge simulator passed')


if __name__ == '__main__':
    main(sys.argv[1])
