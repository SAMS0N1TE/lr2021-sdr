# Usage: spectro_zoom.py <iq8.bin> <out.png> <fs_hz> <t0_ms> <t1_ms> [title]
import sys, numpy as np
import matplotlib; matplotlib.use('Agg'); import matplotlib.pyplot as plt
b = np.fromfile(sys.argv[1], dtype=np.int8).astype(float); fs = float(sys.argv[3])
t0, t1 = float(sys.argv[4]), float(sys.argv[5]); title = sys.argv[6] if len(sys.argv) > 6 else ''
iq = (b[0::2] + 1j * b[1::2])[int(t0 * fs / 1e3):int(t1 * fs / 1e3)]
fig, ax = plt.subplots(figsize=(15, 5.5))
nfft = 64
_, _, _, im = ax.specgram(iq, NFFT=nfft, Fs=fs / 1e3, noverlap=nfft - 4, cmap='magma', sides='twosided')
ax.set_xticks(np.arange(0, (t1 - t0) + 1e-9, 10)); ax.set_xticklabels([f'{t0 + t:.0f}' for t in np.arange(0, (t1 - t0) + 1e-9, 10)])
ax.set_xlabel('ms'); ax.set_ylabel('kHz'); ax.set_title(title)
plt.tight_layout(); plt.savefig(sys.argv[2], dpi=110)
