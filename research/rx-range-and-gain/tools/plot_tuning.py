"""Draw the tuning-range result as an SVG without plotting libraries.

usage: python plot_tuning.py <tuning csv> [<more csv> ...] <out.svg>
A frequency counts as received when one of the three strongest tones moves
by -20 kHz between the +10 kHz and +30 kHz probes.
"""
import csv, sys

paths, out = sys.argv[1:-1], sys.argv[-1]
groups = {}
for p in paths:
    for r in csv.DictReader(open(p)):
        a = r["args"].split()
        khz = int(a[0]); off = khz % 1000
        if off not in (10, 30):
            continue
        groups.setdefault((khz - off, r.get("path") or "?"), {})[off] = r

def peaks(r):
    out = []
    for i in (1, 2, 3):
        try: out.append((int(r["pk%d_hz" % i]), float(r["pk%d_db" % i])))
        except (ValueError, KeyError, TypeError): pass
    return out

points = []  # (MHz, path, dB or None, failure text)
for (base, path), g in sorted(groups.items()):
    a, b = g.get(10), g.get(30)
    if not a or not b or path == "?":
        continue
    best = None
    for fa, da in peaks(a):
        for fb, db in peaks(b):
            if abs((fa - fb) - 20000) <= 400 and (best is None or min(da, db) > best):
                best = min(da, db)
    fail = "PLL" if "PLL_LOCK" in (a.get("err_names") or "") else "CAL" if "IMG_CAL" in (a.get("err_names") or "") else ""
    points.append((base / 1000.0, path, best, fail))

W, H, L, R, T, B = 900, 360, 56, 20, 30, 50
fmax, dbmax = 3800.0, 60.0
x = lambda f: L + (W - L - R) * f / fmax
y = lambda d: T + (H - T - B) * (1 - d / dbmax)
s = ['<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 %d %d" font-family="sans-serif" font-size="12">' % (W, H),
     '<rect width="100%" height="100%" fill="#ffffff"/>']
for lo, hi, label in ((150, 960, "datasheet LF 150 to 960"), (1500, 2500, "datasheet HF 1500 to 2500")):
    s.append('<rect x="%.1f" y="%d" width="%.1f" height="%d" fill="#e8f1f8"/>' % (x(lo), T, x(hi) - x(lo), H - T - B))
    s.append('<text x="%.1f" y="%d" fill="#5a7a90" text-anchor="middle">%s</text>' % ((x(lo) + x(hi)) / 2, T - 8, label))
for d in range(0, 61, 10):
    s.append('<line x1="%d" x2="%d" y1="%.1f" y2="%.1f" stroke="#dddddd"/>' % (L, W - R, y(d), y(d)))
    s.append('<text x="%d" y="%.1f" text-anchor="end" fill="#444">%d</text>' % (L - 6, y(d) + 4, d))
for f in range(0, 3801, 500):
    s.append('<text x="%.1f" y="%d" text-anchor="middle" fill="#444">%d</text>' % (x(f), H - B + 18, f))
s.append('<text x="%.1f" y="%d" text-anchor="middle" fill="#222">MHz</text>' % ((L + W - R) / 2, H - 10))
s.append('<text x="14" y="%.1f" transform="rotate(-90 14 %.1f)" text-anchor="middle" fill="#222">tone over floor, dB</text>' % ((T + H - B) / 2, (T + H - B) / 2))
colors = {"LF": "#1f6fb2", "HF": "#d9822b"}
for f, path, db, fail in points:
    if db is not None:
        s.append('<circle cx="%.1f" cy="%.1f" r="3.2" fill="%s"/>' % (x(f), y(db), colors[path]))
    else:
        c = "#c0392b" if fail else "#999999"
        s.append('<path d="M%.1f %.1f l6 6 m0 -6 l-6 6" stroke="%s" stroke-width="1.5"/>' % (x(f) - 3, y(0) - 9, c))
lx = W - R - 300
s.append('<circle cx="%d" cy="%d" r="4" fill="%s"/><text x="%d" y="%d">LF input, tone tracks retune</text>' % (lx, T + 14, colors["LF"], lx + 10, T + 18))
s.append('<circle cx="%d" cy="%d" r="4" fill="%s"/><text x="%d" y="%d">HF input, tone tracks retune</text>' % (lx, T + 32, colors["HF"], lx + 10, T + 36))
s.append('<path d="M%d %d l6 6 m0 -6 l-6 6" stroke="#c0392b" stroke-width="1.5"/><text x="%d" y="%d">calibration or PLL lock error</text>' % (lx - 3, T + 45, lx + 10, T + 54))
s.append('</svg>')
open(out, "w", newline="\n").write("\n".join(s) + "\n")
print("points", len(points), "->", out)
