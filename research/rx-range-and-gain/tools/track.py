"""Pair +10 kHz / +30 kHz probes: a real receive shows a peak that moves by -20 kHz between them."""
import csv, sys
from collections import OrderedDict

rows = list(csv.DictReader(open(sys.argv[1])))
groups = OrderedDict()
for r in rows:
    a = r["args"].split()
    khz = int(a[0]); base = khz - (khz % 1000 if khz % 1000 in (10, 30) else 0)
    path = a[4] if len(a) > 4 else "auto"
    groups.setdefault((base, path), {})[khz % 1000] = r

def peaks(r):
    out = []
    for i in (1, 2, 3):
        try: out.append((int(r["pk%d_hz" % i]), float(r["pk%d_db" % i])))
        except (ValueError, KeyError, TypeError): pass
    return out

for (base, path), g in groups.items():
    a, b = g.get(10), g.get(30)
    if not a or not b:
        continue
    best = None
    for fa, da in peaks(a):
        for fb, db in peaks(b):
            if abs((fa - fb) - 20000) <= 400:
                if not best or min(da, db) > best[2]:
                    best = (fa, fb, min(da, db))
    errs = (a.get("err_names") or "") + ("/" + b["err_names"] if b.get("err_names") and b["err_names"] != a.get("err_names") else "")
    verdict = "TRACKS" if best else "-"
    tone = "tone %+6d Hz %5.1f dB" % (best[0] - 10000, best[2]) if best else ""
    print("%5d MHz %-4s %-7s %-22s rms %6s/%6s %s %s" % (base // 1000, a.get("path", "?"), verdict, tone,
          a.get("rms_dbfs", "?"), b.get("rms_dbfs", "?"), errs, "" if a.get("result") == "ESP_OK" else a.get("result", "")[:40]))
