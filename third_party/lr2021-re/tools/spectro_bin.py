# Usage: spectro_bin.py <iq8.bin> <out.png> [fs_hz] [title]   int8 interleaved I,Q
import sys, numpy as np
import matplotlib; matplotlib.use('Agg'); import matplotlib.pyplot as plt
b = np.fromfile(sys.argv[1], dtype=np.int8).astype(float)
fs = float(sys.argv[3]) if len(sys.argv) > 3 else 250e3
title = sys.argv[4] if len(sys.argv) > 4 else sys.argv[1]
iq = b[0::2] + 1j * b[1::2]
n = len(iq)
fig, ax = plt.subplots(2, 1, figsize=(14, 8), gridspec_kw={'height_ratios': [3, 1]})
nfft = 128
ax[0].specgram(iq, NFFT=nfft, Fs=fs / 1e3, noverlap=nfft - 16, cmap='viridis', sides='twosided')
ax[0].set_ylabel('kHz'); ax[0].set_xlabel('ms'); ax[0].set_title(f'{title}: {n} IQ samples @ {fs/1e3:.0f} kS/s = {n/fs*1e3:.0f} ms')
env = np.convolve(np.abs(iq), np.ones(256) / 256, 'same')
ax[1].plot(np.arange(n) / fs * 1e3, env, lw=0.6); ax[1].set_xlabel('ms'); ax[1].set_ylabel('|IQ| (avg)'); ax[1].set_xlim(0, n / fs * 1e3)
plt.tight_layout(); plt.savefig(sys.argv[2], dpi=100)
print(n, 'samples', round(n / fs * 1e3, 1), 'ms')
