#!/usr/bin/env python3
"""Generate a bounded-test reference file; never opens hardware.

Credit: sfowlr's original LR2021 source-route and ROM research enabled the
source-6 classifier experiments. This waveform is a new local test fixture.
"""
from pathlib import Path
import hashlib
import json
import numpy as np

ROOT=Path(__file__).resolve().parent
RATE=2_000_000
BAUD=2_000
OFFSET=50_000
DEVIATION=3_000
AMPLITUDE=64
PREAMBLE=[i%2 for i in range(32)]
SYNC=[(0xD391DA26>>i)&1 for i in range(31,-1,-1)]
PAYLOAD=b'LS-S6v1!'
BITS=PREAMBLE+SYNC+[(b>>i)&1 for b in PAYLOAD for i in range(7,-1,-1)]
assert len(BITS)==128
symbols=np.tile(np.array(BITS,dtype=np.int8),8)
samples_per_symbol=RATE//BAUD
frequency=np.repeat(OFFSET+DEVIATION*(2*symbols.astype(np.int32)-1),samples_per_symbol)
# Define sample0 at zero phase. Continuous-phase FSK, no Gaussian shaping.
phase=2*np.pi*np.r_[0,np.cumsum(frequency[:-1],dtype=np.float64)]/RATE
z=AMPLITUDE*np.exp(1j*phase)
data=np.empty(2*len(z),dtype=np.int8)
data[0::2]=np.rint(z.real).astype(np.int8)
data[1::2]=np.rint(z.imag).astype(np.int8)
path=ROOT/'known-2fsk-2kbaud-ci8.bin'
path.write_bytes(data.tobytes())
manifest={
    'credit':'sfowlr original LR2021 source-route/ROM research; local known-waveform fixture.',
    'format':'interleaved signed int8 I then Q',
    'sample_rate_hz':RATE,'symbols_per_second':BAUD,
    'carrier_offset_hz':OFFSET,'deviation_hz':DEVIATION,
    'symbol0_offset_hz':OFFSET-DEVIATION,'symbol1_offset_hz':OFFSET+DEVIATION,
    'iq_peak_amplitude':AMPLITUDE,'samples_per_symbol':samples_per_symbol,
    'frame_bits':BITS,'frame_bits_string':''.join(map(str,BITS)),
    'preamble_bits':32,'sync_hex':'D391DA26','sync_bits':32,
    'payload_ascii':PAYLOAD.decode(),'payload_bits':64,
    'frame_symbols':len(BITS),'frame_duration_ms':1000*len(BITS)/BAUD,
    'file_symbols':len(symbols),'file_duration_seconds':len(z)/RATE,
    'complex_samples':len(z),'bytes':len(data),'repeated_frames':8,
    'phase_wrap_cycles':float(frequency.sum()/RATE),
    'sha256':hashlib.sha256(data.tobytes()).hexdigest(),
    'rf_example':{'hackrf_center_hz':915000000,'waveform_center_hz':915050000},
    'transmit_limit':'Parent must use a finite sample count, e.g. -n 6000000 at2MS/s for3seconds; no unbounded TX.'}
(ROOT/'known-2fsk-reference.json').write_text(json.dumps(manifest,indent=2),encoding='utf-8')
assert np.max(np.abs(data.astype(int)))<=AMPLITUDE
assert abs(manifest['phase_wrap_cycles']-round(manifest['phase_wrap_cycles']))<1e-8
print(json.dumps({k:manifest[k] for k in ('sample_rate_hz','symbols_per_second','frame_duration_ms','file_duration_seconds','bytes','sha256')},indent=2))

# The independent FM control used 1kHz modulation and 3kHz peak deviation.
t=np.arange(RATE//2)/RATE
fm_phase=2*np.pi*OFFSET*t-3*np.cos(2*np.pi*1000*t)
fm=AMPLITUDE*np.exp(1j*fm_phase)
fm_bytes=np.column_stack((np.rint(fm.real),np.rint(fm.imag))).astype(np.int8).tobytes()
(ROOT/'known-fm-1k-ci8.bin').write_bytes(fm_bytes)
(ROOT/'known-fm-reference.json').write_text(json.dumps({
    'sample_rate_hz':RATE,'baseband_center_hz':OFFSET,'modulating_tone_hz':1000,
    'peak_deviation_hz':3000,'amplitude':AMPLITUDE,'seconds':.5,
    'sha256':hashlib.sha256(fm_bytes).hexdigest()},indent=2),encoding='utf-8')
