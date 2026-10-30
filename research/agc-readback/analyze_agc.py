# SPDX-License-Identifier: MIT
import argparse, csv, sys
from collections import Counter

TABLE = [16, 28, 38, 49, 57, 64, 73, 85, 96, 108, 120, 127, 138]


def rows(path):
    with open(path, newline='') as f:
        return [r for r in csv.DictReader(f) if not r[next(iter(r))].startswith('#')]


def num(v):
    try:
        return float(v)
    except (TypeError, ValueError):
        return None


def sweep(path, clip_dbfs=-1.0, min_snr_db=20.0):
    r = rows(path)
    print('step  idle(step,manual,value)  stream(step,manual,value)  expect  pass  tone_dbfs')
    failures = 0
    for x in r:
        s = int(x['step_set'])
        expect = TABLE[s - 1]
        idle = (int(x['idle_step']), int(x['idle_manual']), int(x['idle_value']))
        stream = tuple(int(x[k]) if x[k] != '' else None for k in ('stream_step', 'stream_manual', 'stream_value'))
        ok = idle == (s, s, expect) and stream in ((s, s, expect), (None, None, None))
        failures += not ok
        print(f"{s:4}  {str(idle):23}  {str(stream):25}  {expect:6}  {'yes' if ok else 'NO':4}  {x['tone_dbfs']}")
    print(f'experiment 1: {len(r) - failures} of {len(r)} steps match the lr2021-re table')
    pts = []
    for x in r:
        tone, peak, floor = num(x['tone_dbfs']), num(x['peak_dbfs']), num(x['floor_dbfs'])
        if None in (tone, peak, floor) or peak > clip_dbfs or tone - floor < min_snr_db:
            continue
        pts.append((int(x['idle_value']), tone, int(x['step_set'])))
    if len(pts) < 3:
        print('experiment 3: fewer than 3 usable steps (need a carrier well above the floor and below clipping)')
        return failures
    n = len(pts)
    mx = sum(p[0] for p in pts) / n
    my = sum(p[1] for p in pts) / n
    sxx = sum((p[0] - mx) ** 2 for p in pts)
    slope = sum((p[0] - mx) * (p[1] - my) for p in pts) / sxx
    resid = max(abs(p[1] - (my + slope * (p[0] - mx))) for p in pts)
    print(f'experiment 3: {n} usable steps, tone level rises {slope:.3f} dB per gain_value unit '
          f'(0.5 expected if the unit is 0.5 dB), worst residual {resid:.2f} dB')
    print('step  gain_value  tone_dbfs  dB_from_previous')
    prev = None
    for value, tone, step in sorted(pts, key=lambda p: p[2]):
        d = '' if prev is None else f'{tone - prev:.2f}'
        print(f'{step:4}  {value:10}  {tone:9.2f}  {d}')
        prev = tone
    return failures


def log(path):
    r = [x for x in rows(path) if x['chip_step'] != '']
    if not r:
        print('no gain samples')
        return 1
    steps = Counter(int(x['chip_step']) for x in r)
    changes = sum(1 for a, b in zip(r, r[1:]) if a['chip_step'] != b['chip_step'])
    print(f'{len(r)} samples, chip step min {min(steps)} max {max(steps)}, changes {changes}, '
          f"distribution {dict(sorted(steps.items()))}")
    host = [x for x in r if x.get('host_step')]
    if host:
        agree = sum(1 for x in host if x['host_step'] == x['chip_step'])
        print(f'host gain control: {agree} of {len(host)} samples report the step the host set, '
              f"host steps {dict(sorted(Counter(int(x['host_step']) for x in host).items()))}")
    return 0


def iq6(path):
    r = rows(path)
    if not r:
        print('no samples')
        return 1
    agree = sum(int(x['agree']) for x in r)
    print(f'experiment 4: source 6 gain bits equal the readback in {agree} of {len(r)} intervals')
    for x in r:
        if x['agree'] != '1':
            print(f"  t={x['t_s']} iq6 mode {x['iq6_mode']} ({x['iq6_min']}..{x['iq6_max']}) chip {x['chip_step']}")
    return len(r) - agree


def main(argv=None):
    p = argparse.ArgumentParser(description='Summaries of agc_readback.py logs')
    p.add_argument('kind', choices=['sweep', 'log', 'iq6'])
    p.add_argument('csv')
    a = p.parse_args(argv)
    return {'sweep': sweep, 'log': log, 'iq6': iq6}[a.kind](a.csv)


if __name__ == '__main__':
    sys.exit(1 if main() else 0)
