#!/usr/bin/env python3
"""Exploratory GPS L1 C/A correlation of ci16_le, NOT a validated detector.

NumPy only. Uses a band-limited periodic C/A reference. The filter is a model,
not a measurement of the LR2021 channel. Peaks require independent validation.
"""
import argparse
import json
from pathlib import Path
import numpy as np

TAPS = ((2,6),(3,7),(4,8),(5,9),(1,9),(2,10),(1,8),(2,9),
        (3,10),(2,3),(3,4),(5,6),(6,7),(7,8),(8,9),(9,10),
        (1,4),(2,5),(3,6),(4,7),(5,8),(6,9),(1,3),(4,6),
        (5,7),(6,8),(7,9),(8,10),(1,6),(2,7),(3,8),(4,9))


def ca(prn):
    if not 1 <= prn <= 32:
        raise ValueError("PRN must be 1..32")
    a, b = TAPS[prn-1]
    g1, g2 = [1]*10, [1]*10
    result = []
    for _ in range(1023):
        bit = g1[9] ^ g2[a-1] ^ g2[b-1]
        result.append(1.0 if bit == 0 else -1.0)
        f1 = g1[2] ^ g1[9]
        f2 = g2[1] ^ g2[2] ^ g2[5] ^ g2[7] ^ g2[8] ^ g2[9]
        g1, g2 = [f1]+g1[:-1], [f2]+g2[:-1]
    return np.array(result)


def reference(prn, rate, passband, offset=0):
    if rate % 1000 or not 10000 <= rate <= 2046000 or not 0 < passband <= rate:
        raise ValueError("Use integer samples/ms, rate 10k..2046k, passband <= rate")
    n = rate//1000
    base = np.repeat(ca(prn), 4)
    spectrum = np.fft.fftshift(np.fft.fft(base))
    # 1 kHz grid; select exactly n bins with correct odd/even alignment.
    bins = np.arange(n) - n//2
    selected = spectrum[2046+bins].copy()
    # After carrier wipeoff, a fixed receiver filter is centered at -offset.
    selected[np.abs(bins*1000 + offset) > passband/2] = 0
    wave = np.fft.ifft(np.fft.ifftshift(selected)) * n/4092
    return wave / np.sqrt(np.mean(np.abs(wave)**2))


def acquire(x, rate, passband, prns, span=10000, step=500, ms=20):
    n = rate//1000
    count = min(ms, len(x)//n)
    if count < 5:
        raise ValueError("Need at least 5 ms of samples")
    x = x[:n*count].reshape(count, n)
    x = x - x.mean()
    # Relative ranking only: colored noise and multiple searches change the null.
    times = np.arange(count*n).reshape(count,n)/rate
    best = [dict(prn=int(p), peak_to_mean=0.0) for p in prns]
    for doppler in range(-span, span+1, step):
        refs = [np.conj(np.fft.fft(reference(p, rate, passband, doppler))) for p in prns]
        spectra = np.fft.fft(x*np.exp(-2j*np.pi*doppler*times), axis=1)
        for j, ref in enumerate(refs):
            power = np.abs(np.fft.ifft(spectra*ref, axis=1))**2
            power = power.sum(axis=0)
            index = int(power.argmax())
            ratio = float(power[index]/max(float(power.mean()), 1e-30))
            if ratio > best[j]["peak_to_mean"]:
                best[j].update(peak_to_mean=ratio, offset_hz=doppler, code_lag_sample=index)
    return dict(kind="exploratory_ranking_not_GNSS_detection", rate_hz=rate,
                assumed_full_passband_hz=passband, noncoherent_ms=count,
                search_span_hz=span, search_step_hz=step,
                detected=None, candidates=sorted(best,key=lambda z:-z["peak_to_mean"]))


def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument("file",type=Path)
    p.add_argument("--rate",type=int,required=True)
    p.add_argument("--passband",type=int,required=True)
    p.add_argument("--span",type=int,default=10000)
    p.add_argument("--step",type=int,default=500)
    p.add_argument("--ms",type=int,default=20)
    a=p.parse_args()
    if not 100 <= a.step <= 5000 or not 0 <= a.span <= 60000 or not 5 <= a.ms <= 100:
        p.error("step 100..5000, span 0..60000, ms 5..100")
    if a.rate % 1000 or not 10000 <= a.rate <= 2046000 or not 0 < a.passband <= a.rate:
        p.error("rate 10k..2046k with integer samples/ms, passband <= rate")
    if a.span + a.passband/2 >= a.rate/2:
        p.error("Search plus passband must fit inside sample Nyquist interval")
    raw=np.fromfile(a.file,dtype="<i2")
    if raw.size%2:p.error("Unaligned IQ16")
    result=acquire(raw[::2].astype(float)+1j*raw[1::2],a.rate,a.passband,list(range(1,33)),a.span,a.step,a.ms)
    print(json.dumps(result,indent=2))


if __name__=="__main__":main()
