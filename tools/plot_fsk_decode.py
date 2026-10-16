"""Plot a 2-FSK burst decoded from an LR2021 I/Q capture (LakeShark .sub, IQ16LE, 125 ksample/s).

Envelope, discriminator trace with the decoded fields, spectrum and waterfall.
usage: python plot_fsk_decode.py <capture.sub> <out.png> [baud]
"""
import sys
import numpy as np
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
from matplotlib.colors import LinearSegmentedColormap

src, out = sys.argv[1], sys.argv[2]
baud = float(sys.argv[3]) if len(sys.argv) > 3 else 2400.0

raw, fs, hz = bytearray(), 125000.0, 0
for line in open(src, encoding="ascii", errors="replace"):
    if line.startswith("IQ_Data: "):
        raw += bytes.fromhex(line[9:].strip())
    elif line.startswith("SampleRate: "):
        fs = float(line.split()[1])
    elif line.startswith("Frequency: "):
        hz = int(line.split()[1])
v = np.frombuffer(bytes(raw[: len(raw) // 4 * 4]), dtype="<i2").astype(np.float64)
x = v[0::2] + 1j * v[1::2]
n, T = len(x), fs / baud
t_ms = np.arange(n) / fs * 1e3

# Burst, discriminator, slicer
pw = np.convolve(np.abs(x) ** 2, np.ones(25) / 25, mode="same")
on = pw > np.percentile(pw, 10) * 10
idx = np.flatnonzero(on)
a, b = idx[0] + 60, idx[-1] - 20
disc = np.angle(x[1:] * np.conj(x[:-1])) * fs / (2 * np.pi)
disc = np.concatenate([[0], np.convolve(disc, np.ones(13) / 13, mode="same")])
seg = disc[a:b]
f_lo, f_hi = np.percentile(seg, 10), np.percentile(seg, 90)
thr = (f_lo + f_hi) / 2
centre = hz + thr
lev = (seg > thr).astype(int)
ch = np.flatnonzero(np.diff(lev)) + 1
runs = []
for s0, e0 in zip(np.concatenate([[0], ch]), np.concatenate([ch, [len(lev)]])):
    lv, ln = lev[s0], e0 - s0
    if runs and (ln < T / 2 or runs[-1][0] == lv):
        runs[-1][1] += ln
    else:
        runs.append([lv, ln])
bits, starts, pos = "", [], a
for lv, ln in runs:
    k = max(1, int(round(ln / T)))
    for j in range(k):
        starts.append(pos + j * ln / k)
    bits += str(lv) * k
    pos += ln
sync = "1101001110010001" * 2
k = bits.find(sync)
if k < 0:
    sys.exit("sync D391D391 not found")
body = bits[k + 32:]
by = [int(body[i:i + 8], 2) for i in range(0, len(body) // 8 * 8, 8)]


def crc16(data, c=0xFFFF):
    for byte in data:
        c ^= byte << 8
        for _ in range(8):
            c = ((c << 1) ^ 0x8005) & 0xFFFF if c & 0x8000 else (c << 1) & 0xFFFF
    return c


npay = next((L for L in range(1, len(by) - 1) if crc16(by[:L]) == (by[L] << 8 | by[L + 1])), None)
if npay is None:
    sys.exit("no CRC-16 match")
crc = by[npay] << 8 | by[npay + 1]
fields = [("PREAMBLE", 0, k), ("SYNC  D391 D391", k, k + 32),
          ("PAYLOAD  " + " ".join("%02X" % v for v in by[:npay]), k + 32, k + 32 + 8 * npay),
          ("CRC-16  %04X" % crc, k + 32 + 8 * npay, k + 32 + 8 * npay + 16)]

# House style (plot_voice_capture.py)
bg, ink, muted, accent, violet = "#ffffff", "#152238", "#52647a", "#087f8c", "#9a4e9f"
cmap = LinearSegmentedColormap.from_list("radio", ["#070d19", "#221842", "#69388f", "#dc507b", "#ffaf66", "#fff5c3"])
plt.rcParams.update({"font.family": "DejaVu Sans", "font.size": 11, "text.color": ink, "axes.labelcolor": muted,
                     "xtick.color": muted, "ytick.color": muted, "axes.edgecolor": "#cbd5e1", "savefig.facecolor": bg})
fig = plt.figure(figsize=(16, 12), facecolor=bg)
gs = fig.add_gridspec(3, 2, height_ratios=[1, 1.25, 1], left=.075, right=.9, top=.8, bottom=.07, hspace=.55, wspace=.22)


def style(ax, title):
    ax.set_facecolor(bg)
    for s in ("top", "right"):
        ax.spines[s].set_visible(False)
    ax.grid(color="#cbd5e1", alpha=.4, lw=.6)
    ax.set_title(title, loc="left", fontsize=12, weight="bold", pad=14)


fig.text(.075, .95, "LR2021-SDR  /  PACKET DECODE", color=accent, fontsize=12, weight="bold")
fig.text(.075, .885, "2-FSK, decoded.", fontsize=34, weight="bold")
fig.text(.075, .842, "%d baud  |  %.1f kHz tone spacing  |  %d-byte payload, CRC-16 checks" %
         (baud, (f_hi - f_lo) / 1e3, npay), color=muted, fontsize=13)
fig.text(.90, .95, "%.4f MHz" % (centre / 1e6), ha="right", fontsize=17, weight="bold")
fig.text(.90, .91, "%d ksample/s  |  %.0f ms captured" % (fs / 1e3, n / fs * 1e3), ha="right", color=muted, fontsize=12)

# 01 envelope
e = fig.add_subplot(gs[0, 0])
style(e, "01  SIGNAL ENVELOPE")
pdb = 10 * np.log10(pw + 1)
pdb -= np.median(pdb[~on])
e.plot(t_ms, pdb, color=accent, lw=1.1)
e.fill_between(t_ms, pdb.min(), pdb, color=accent, alpha=.10)
e.set_xlim(0, t_ms[-1]); e.set_xlabel("Time into recording (ms)"); e.set_ylabel("Power over floor (dB)")
e.axvspan(a / fs * 1e3, b / fs * 1e3, color=violet, alpha=.06)
e.text((a + b) / 2 / fs * 1e3, pdb.max() * .55, "%.1f ms burst" % ((idx[-1] - idx[0]) / fs * 1e3),
       ha="center", color=violet, fontsize=10, weight="bold")

# 03 spectrum
s = fig.add_subplot(gs[0, 1])
style(s, "02  BURST SPECTRUM")
burst = x[idx[0]:idx[-1]]
N = 1 << int(np.ceil(np.log2(len(burst))))
S = np.fft.fftshift(np.abs(np.fft.fft(burst * np.hanning(len(burst)), N)) ** 2)
f = (np.fft.fftshift(np.fft.fftfreq(N, 1 / fs)) + hz - centre) / 1e3
Sd = 10 * np.log10(S / S.max() + 1e-12)
Sd = np.convolve(Sd, np.ones(9) / 9, mode="same")
s.plot(f, Sd, color=accent, lw=1.2); s.fill_between(f, -60, Sd, color=accent, alpha=.10)
s.set_xlim(-30, 30); s.set_ylim(-60, 3)
for ft, name in ((f_lo, "space"), (f_hi, "mark")):
    fk = (ft + hz - centre) / 1e3
    s.axvline(fk, color=violet, lw=.9, ls="--")
    s.text(fk, 0, " " + name, color=violet, fontsize=10, va="top")
s.set_xlabel("Offset from centre (kHz)"); s.set_ylabel("Relative power (dB)")

# 02 discriminator with fields (full width)
d = fig.add_subplot(gs[1, :])
style(d, "03  FREQUENCY TRACE AND DECODED BITS")
dk = (disc + hz - centre) / 1e3
lo, hi = max(0, a - 250), min(n, b + 250)
d.plot(t_ms[lo:hi], dk[lo:hi], color=accent, lw=1.0)
d.axhline(0, color="#cbd5e1", lw=.8)
d.set_xlim(t_ms[lo], t_ms[hi - 1]); d.set_ylim(-11, 17)
d.set_xlabel("Time into recording (ms)"); d.set_ylabel("Frequency (kHz)")
shades = ["#e7f2f3", "#f1e6f2", "#e7f2f3", "#f1e6f2"]
for (name, i0, i1), col in zip(fields, shades):
    if i1 > len(starts):
        continue
    t0 = starts[i0] / fs * 1e3
    t1 = (starts[i1] if i1 < len(starts) else starts[-1] + T) / fs * 1e3
    d.axvspan(t0, t1, ymin=0, ymax=.72, color=col, zorder=0)
    d.text((t0 + t1) / 2, 12.5, name, ha="center", fontsize=10.5, weight="bold",
           color=violet if "SYNC" in name or "CRC" in name else accent)
for i in range(k, min(len(bits), k + 32 + 8 * (npay + 2))):
    tt = (starts[i] + T / 2) / fs * 1e3
    d.text(tt, -9.3, bits[i], ha="center", fontsize=8.5, color=ink, family="DejaVu Sans Mono")

# 04 waterfall
w = fig.add_subplot(gs[2, :])
style(w, "04  WATERFALL")
nf, hop = 128, 16
frames = np.array([np.fft.fftshift(np.abs(np.fft.fft(x[i:i + nf] * np.hanning(nf))) ** 2)
                   for i in range(0, n - nf, hop)])
P = 10 * np.log10(frames / frames.max() + 1e-12)
ff = (np.fft.fftshift(np.fft.fftfreq(nf, 1 / fs)) + hz - centre) / 1e3
tt = (np.arange(len(frames)) * hop + nf / 2) / fs * 1e3
keep = (ff > -30) & (ff < 30)
im = w.pcolormesh(tt, ff[keep], P[:, keep].T, shading="nearest", cmap=cmap, vmin=-55, vmax=0, rasterized=True)
w.set_xlim(t_ms[lo], t_ms[hi - 1]); w.grid(False)
w.set_xlabel("Time into recording (ms)"); w.set_ylabel("Offset (kHz)")
cax = fig.add_axes([.915, .07, .012, .19])
cb = fig.colorbar(im, cax=cax, ticks=[-50, -25, 0]); cb.set_label("Relative power (dB)")

fig.text(.075, .025, "IQ16  /  QUADRATURE DISCRIMINATOR  /  CRC-16 POLY 8005", color=accent, fontsize=10)
fig.text(.90, .025, "Uncalibrated amplitude", ha="right", color=muted, fontsize=10)
fig.savefig(out, dpi=180)
print("bits after sync:", body, "payload", by[:npay], "crc %04X" % crc, "->", out)
