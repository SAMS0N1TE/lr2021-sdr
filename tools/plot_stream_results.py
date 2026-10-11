"""Render the hardware session measurements (requires matplotlib)."""
import json
from pathlib import Path
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt

root = Path(__file__).resolve().parents[1]
data = json.loads((root / "docs/data/live-streaming.json").read_text())
rows = data["sessions"]
labels = [r["name"] for r in rows]
fig, axes = plt.subplots(1, 2, figsize=(12, 5), layout="constrained")
fig.patch.set_facecolor("#f5f7fa")
for ax in axes:
    ax.set_facecolor("#f5f7fa")
    ax.spines[["top", "right"]].set_visible(False)
    ax.grid(axis="x", alpha=.18)
    ax.set_axisbelow(True)
seconds = [r["elapsed_ms"] / 1000 for r in rows]
axes[0].barh(labels, seconds, color="#117fa5", height=.55)
for i, value in enumerate(seconds):
    axes[0].text(value, i, f"  {value:.1f} s", va="center")
axes[0].set_xlim(0, max(seconds) * 1.25)
axes[0].set_xlabel("One receive session (seconds)")
axes[0].set_title("Capture and decoder input running together")
peaks = [r["max_fifo_bytes"] for r in rows]
axes[1].barh(labels, peaks, color="#117fa5", height=.55, label="Peak observed occupancy")
axes[1].barh(labels, [1024 - p for p in peaks], left=peaks,
             color="#dce6ec", height=.55, label="Headroom at observed peak")
for i, value in enumerate(peaks):
    axes[1].text(value - 12, i, str(value), va="center", ha="right", color="white")
axes[1].axvline(1024, color="#e58032", linestyle="--")
axes[1].set_xlim(0, 1100)
axes[1].set_xlabel("FIFO bytes (1024-byte configuration)")
axes[1].set_title("Measured FIFO headroom")
axes[1].legend(loc="lower center", bbox_to_anchor=(.5, -.32), frameon=False)
fig.suptitle("LR2021 live raw I/Q | 154.785 MHz | nominal 125 ksample/s", fontsize=15)
fig.text(.02, -.06,
         "Three separate receive sessions. Reported FIFO loss: 0. Queue rejections: 0.", fontsize=10)
fig.savefig(root / "docs/images/live-streaming-results.png", dpi=170, bbox_inches="tight")
