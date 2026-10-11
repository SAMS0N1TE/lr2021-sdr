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
cmap = LinearSegmentedColormap.from_list("radio", ["#070d19", "#221842", "#69388f", "#dc507b", "#ffaf66", "#fff5c3"])
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
fig.savefig(root / "docs/images/voice-iq-hero-purple.png", dpi=180)

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
fig2.savefig(root / "docs/images/voice-waterfall-purple.png",dpi=180)

w = json.loads((root / "docs/data/voice-waveform.json").read_text())
assert w["source_sha256"] == d["source_sha256"]
fig3, axs = plt.subplots(2, 2, figsize=(16, 11), facecolor=bg)
fig3.subplots_adjust(left=.075, right=.90, top=.79, bottom=.12, hspace=.43, wspace=.25)
fig3.text(.075, .95, "LR2021-SDR  /  CAPTURE DETAIL", color=accent, fontsize=12, weight="bold")
fig3.text(.075, .885, "Raw I/Q.", fontsize=34, weight="bold")
fig3.text(.075, .839, "P25 Phase 1 C4FM  |  125,000 complex samples  |  27 decoded voice frames", color=muted, fontsize=13)
fig3.text(.90, .95, "154.7850 MHz", ha="right", fontsize=17, weight="bold")
fig3.text(.90, .91, "125 ksample/s", ha="right", color=muted, fontsize=12)
for ax in axs.flat:
    axes_style(ax)
    ax.grid(color="#cbd5e1", alpha=.4, lw=.6)

a = axs[0, 0]
a.plot(w["waveform_time_ms"], w["i"], color=accent, lw=1.6, label="I")
a.plot(w["waveform_time_ms"], w["q"], color="#9a4e9f", lw=1.6, label="Q")
a.set_xlim(w["waveform_time_ms"][0], w["waveform_time_ms"][-1])
a.set_title("01  I/Q WAVEFORM", loc="left", fontsize=12, weight="bold", pad=16)
a.set_xlabel("Time into recording (ms)", labelpad=10)
a.set_ylabel("Sample value (IQ16 counts)")
a.legend(frameon=False, ncol=2, loc="upper right")

b = axs[0, 1]
b.plot(w["envelope_time_ms"], w["envelope_rms"], color=accent, lw=1.4)
b.fill_between(w["envelope_time_ms"], 0, w["envelope_rms"], color=accent, alpha=.10)
b.set_xlim(0, 1000); b.set_ylim(bottom=0)
b.set_title("02  SIGNAL ENVELOPE", loc="left", fontsize=12, weight="bold", pad=16)
b.set_xlabel("Time into recording (ms)", labelpad=10)
b.set_ylabel("Complex RMS (IQ16 counts, 1 ms bins)")

c = axs[1, 0]
c.plot(f, s, color=accent, lw=1.5)
c.fill_between(f, -55, s, color=accent, alpha=.10)
c.set_ylim(-55, 3)
c.set_title("03  CHANNEL SPECTRUM", loc="left", fontsize=12, weight="bold", pad=16)
c.set_xlabel("Frequency offset (kHz)", labelpad=10)
c.set_ylabel("Relative power (dB)")

e = axs[1, 1]
e.grid(False)
im = e.pcolormesh(f, t, p, shading="nearest", cmap=cmap, vmin=-55, vmax=0, rasterized=True)
e.invert_yaxis()
e.set_title("04  WATERFALL", loc="left", fontsize=12, weight="bold", pad=16)
e.set_xlabel("Frequency offset (kHz)", labelpad=10)
e.set_ylabel("Time into recording (ms)")
pos = e.get_position()
bar = fig3.add_axes([.92, pos.y0, .012, pos.height])
cb = fig3.colorbar(im, cax=bar, ticks=[-50, -25, 0])
cb.set_label("Relative power (dB)"); cb.outline.set_visible(False)
fig3.text(.075, .035, "IQ16  /  CRC32 5FDC4E65  /  HANN FFT", color=accent, fontsize=10)
fig3.text(.90, .035, "Nominal sample rate; uncalibrated amplitude", ha="right", color=muted, fontsize=10)
fig3.savefig(root / "docs/images/voice-four-panel-purple.png", dpi=180)
