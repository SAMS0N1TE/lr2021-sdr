#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Draw the bridge firmware results in docs/images.

    python tools/plot_bridge_results.py [hackrf_capture.cs8]

bridge-streaming.png uses the measured values below (LilyGO T-Display-P4 LR2021,
2026-10-27). tx-433-burst.png needs a HackRF capture (hackrf_transfer -r, int8 I/Q,
2 MS/s, centred 433.900 MHz) holding one LR2021 burst at 433.92 MHz.
"""
import sys
from pathlib import Path
import numpy as np
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt

root = Path(__file__).resolve().parents[1]
BG, BAR, LIGHT, MARK = "#f5f7fa", "#117fa5", "#dce6ec", "#e58032"

def style(ax):
    ax.set_facecolor(BG)
    for s in ("top", "right"):
        ax.spines[s].set_visible(False)
    ax.grid(axis="x", color="#e4e9ee")
    ax.set_axisbelow(True)

# Link, run, share of checked pairs delivered (%), worst FIFO service gap (us),
# time the 1024-byte FIFO holds at that rate (us).
RUNS = [
    ("USB serial 4 Mbaud, CS12\n125k, 60 s", 99.8, 359, 2048),
    ("Wi-Fi TCP, CS16\n125k, 60 s", 100.0, 286, 2048),
    ("Wi-Fi TCP, CS16\n250k, 10 min", 100.0, 720, 1024),
]

def streaming():
    labels = [r[0] for r in RUNS][::-1]
    share = [r[1] for r in RUNS][::-1]
    gap = [r[2] for r in RUNS][::-1]
    budget = [r[3] for r in RUNS][::-1]
    fig, axes = plt.subplots(1, 2, figsize=(12, 5), layout="constrained")
    fig.patch.set_facecolor(BG)
    for ax in axes:
        style(ax)
    a = axes[0]
    a.barh(labels, share, color=BAR, height=.55)
    for i, v in enumerate(share):
        a.text(v + 1, i, f"{v:g}%", va="center")
    a.set_xlim(0, 112)
    a.set_xlabel("Checked pairs that reached the host (%)")
    a.set_title("Delivered to the host")
    b = axes[1]
    b.barh(labels, budget, color=LIGHT, height=.55, label="FIFO holds (1024 bytes)")
    b.barh(labels, gap, color=BAR, height=.55, label="Worst service gap")
    for i, v in enumerate(gap):
        b.text(v + 25, i, f"{v} us", va="center")
    b.set_xlabel("Microseconds")
    b.set_title("Radio FIFO margin")
    b.legend(loc="lower center", bbox_to_anchor=(.5, -.32), ncol=2, frameon=False)
    fig.suptitle("LR2021 bridge firmware | ESP32-P4 | 16-bit source-2 I/Q", fontsize=15)
    fig.text(.02, -.05, "Radio side lossless in every run: no FIFO error flags.", fontsize=10)
    out = root / "docs/images/bridge-streaming.png"
    fig.savefig(out, dpi=170, bbox_inches="tight")
    print(out)

def burst(path):
    fs, f0, tune = 2e6, 433.900e6, 433.920e6
    x = np.fromfile(path, dtype=np.int8).astype(np.float32).reshape(-1, 2)
    z = x[:, 0] + 1j * x[:, 1]
    z -= z.mean()
    n = 1024
    nb = len(z) // n
    seg = z[: nb * n].reshape(nb, n) * np.hanning(n)
    spec = np.abs(np.fft.fftshift(np.fft.fft(seg, axis=1), axes=1)) ** 2
    freq = f0 + np.fft.fftshift(np.fft.fftfreq(n, 1 / fs))
    k = np.argmin(np.abs(freq - (tune + 10e3)))
    level = 10 * np.log10(spec[:, k - 2:k + 3].max(axis=1) / np.median(spec))
    on = np.flatnonzero(level > 15)
    if not len(on):
        sys.exit("no burst near 433.930 MHz in the capture")
    t0 = on[0] * n / fs
    lo, hi = max(0, on[0] - 60), min(nb, on[-1] + 60)
    band = (freq > tune - 60e3) & (freq < tune + 60e3)
    sg = 10 * np.log10(spec[lo:hi][:, band] / np.median(spec))
    t = (np.arange(lo, hi) * n / fs - t0) * 1e3
    fig, axes = plt.subplots(2, 1, figsize=(12, 6.5), layout="constrained", height_ratios=[3, 1.3], sharex=True)
    fig.patch.set_facecolor(BG)
    a = axes[0]
    a.set_facecolor(BG)
    a.pcolormesh(t, (freq[band] - tune) / 1e3, sg.T, vmin=0, vmax=45, cmap="viridis", shading="nearest")
    a.axhline(10, color=MARK, lw=.8, ls="--")
    a.set_ylabel("kHz from 433.920 MHz")
    a.set_title("HackRF One receiving the LR2021 (433 MHz antennas on both)")
    b = axes[1]
    style(b)
    b.plot(t, level[lo:hi], color=BAR, lw=1)
    b.set_ylabel("Tone over floor (dB)")
    b.set_xlabel("ms from the start of the burst")
    width = (on[-1] - on[0] + 1) * n / fs * 1e3
    fig.suptitle("SoapySDR writeStream burst: +10 kHz tone, 16000 samples at 125 ksample/s, -9 dBm", fontsize=14)
    fig.text(.02, -.04, f"Measured burst length {width:.0f} ms (128 ms nominal); "
             f"peak {level.max():.1f} dB over the capture's median bin.", fontsize=10)
    out = root / "docs/images/tx-433-burst.png"
    fig.savefig(out, dpi=150, bbox_inches="tight")
    print(out)

if __name__ == "__main__":
    streaming()
    if len(sys.argv) > 1:
        burst(sys.argv[1])
