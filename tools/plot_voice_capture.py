"""Render the measured voice-call spectrum and waterfall (numpy, matplotlib)."""
import json
from pathlib import Path
import numpy as np
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
from matplotlib.colors import LinearSegmentedColormap

root = Path(__file__).resolve().parents[1]
d = json.loads((root / "docs/data/voice-spectrum.json").read_text())
f = np.asarray(d["frequency_offset_khz"])
t = np.asarray(d["time_ms"])
p = np.asarray(d["waterfall_db"])
s = np.asarray(d["mean_spectrum_db"])
bg, panel, ink, muted, accent = "#ffffff", "#ffffff", "#152238", "#52647a", "#087f8c"
cmap = LinearSegmentedColormap.from_list("radio", [bg, "#e4f1f7", "#9fcde0", "#3b9eb6", "#27638f", "#172b55"])
plt.rcParams.update({"font.family": "DejaVu Sans", "font.size": 11,
    "text.color": ink, "axes.labelcolor": muted, "xtick.color": muted,
    "ytick.color": muted, "axes.edgecolor": "#cbd5e1", "savefig.facecolor": bg})

def axes_style(ax):
    ax.set_facecolor(panel)
    ax.spines[["top", "right"]].set_visible(False)
    ax.tick_params(length=0, pad=8)
    ax.set_xlim(f[0], f[-1])

fig = plt.figure(figsize=(15, 10), facecolor=bg)
fig.text(.075, .95, "LR2021-SDR  /  LIVE RECEPTION", color=accent, fontsize=12, weight="bold")
fig.text(.075, .875, "Raw I/Q.", fontsize=34, weight="bold")
fig.text(.075, .832, "Measured samples from a voice-triggered LR2021 capture", color=muted, fontsize=13)
fig.text(.9, .953, "154.7850 MHz", ha="right", fontsize=16, weight="bold")
fig.text(.9, .917, "125 ksample/s  |  C4FM", ha="right", color=muted, fontsize=11)

a = fig.add_axes([.075, .59, .825, .175]); axes_style(a)
a.fill_between(f, -55, s, color=accent, alpha=.11)
a.plot(f, s, color=accent, lw=1.35)
a.set_ylim(-55, 3); a.set_yticks([-50,-25,0])
a.grid(axis="y", color="#cbd5e1", alpha=.45, lw=.6)
a.set_ylabel("Relative power (dB)")
a.set_title("01  CHANNEL SPECTRUM", loc="left", color=ink, fontsize=12, pad=14, weight="bold")
a.tick_params(labelbottom=False)

b = fig.add_axes([.075, .13, .825, .38]); axes_style(b)
im = b.pcolormesh(f, t, p, shading="nearest", cmap=cmap, vmin=-55, vmax=0, rasterized=True)
b.invert_yaxis()
b.set_ylabel("Time into recording (ms)")
b.set_xlabel("Frequency offset from 154.7850 MHz (kHz)", labelpad=13)
b.set_title("02  WATERFALL", loc="left", color=ink, fontsize=12, pad=14, weight="bold")
cax = fig.add_axes([.925,.13,.012,.38])
cb = fig.colorbar(im, cax=cax, ticks=[-50,-25,0]); cb.set_label("Relative power (dB)")
cb.outline.set_visible(False)
fig.text(.075,.036,"LIVE VOICE CAPTURE  /  IQ16  /  HANN FFT",color=accent,fontsize=10)
fig.text(.9,.036,f'{d["pairs"]:,} complex samples',ha="right",color=muted,fontsize=10)
fig.savefig(root / "docs/images/voice-iq-hero-white.png", dpi=180)

fig2, ax = plt.subplots(figsize=(15,7), facecolor=bg)
fig2.subplots_adjust(left=.08,right=.9,top=.84,bottom=.16)
axes_style(ax)
im=ax.pcolormesh(f,t,p,shading="nearest",cmap=cmap,vmin=-55,vmax=0,rasterized=True)
ax.invert_yaxis()
ax.set_xlabel("Frequency offset from 154.7850 MHz (kHz)",labelpad=13)
ax.set_ylabel("Time into recording (ms)")
fig2.text(.08,.92,"LR2021  /  P25 VOICE-CALL WATERFALL",fontsize=22,weight="bold")
fig2.text(.08,.87,"154.7850 MHz  |  Raw IQ16  |  Nominal 125 ksample/s",color=muted,fontsize=12)
cb=fig2.colorbar(im,ax=ax,pad=.025,fraction=.025); cb.set_label("Relative power (dB)")
cb.outline.set_visible(False)
fig2.savefig(root / "docs/images/voice-waterfall-white.png",dpi=180)
