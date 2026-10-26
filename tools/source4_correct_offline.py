#!/usr/bin/env python3
"""Local offline source-4 correction for LakeShark capture logs.

Research credit: sfowlr supplied the original LR2021 source-route/ROM research.
The cancellation models are a local follow-up on archived LakeShark captures.
This tool opens no COM port, transmits nothing, and changes no repository.

Examples:
  python source4_correct_offline.py ON.txt --baseline OFF1.txt OFF2.txt --out corrected.npz
  python source4_correct_offline.py ON.txt --method fold128 --out folded.npz

Template mode subtracts an external quiet mean without fitting the signal.
Fold128 is an SF7 diagnostic model. It removes real signals at the comb
f/Fs = (m - 1/3)/128, including 41.6667 kHz at Fs=8 MHz. Keep the raw data.
Only known complete source-4 captures with successful restoration are accepted.
"""
import argparse
import hashlib
import json
from pathlib import Path
import re
import numpy as np


def load_capture(path):
    text=path.read_text(encoding='utf-8',errors='replace')
    lines=re.findall(r'^lrcap: (\d+) 0x([0-9A-Fa-f]{8})\s*$',text,re.M)
    if not lines or [int(i) for i,_ in lines] != list(range(len(lines))):
        raise ValueError(f'{path.name}: incomplete, duplicate or reordered indices')
    summaries=re.findall(r'^lrcap: source=4 (.+)$',text,re.M)
    if len(summaries)!=1:
        raise ValueError(f'{path.name}: expected exactly one source-4 summary')
    fields=dict(re.findall(r'(\w+)=([^\s]+)',summaries[0]))
    if any(fields.get(k)!='ESP_OK' for k in ('result','restore','recovery')):
        raise ValueError(f'{path.name}: capture, restoration or recovery failed')
    if int(fields.get('words','0'))!=len(lines) or int(fields.get('bytes','0'))!=4*len(lines):
        raise ValueError(f'{path.name}: count does not match readback')
    w=np.array([int(h,16) for _,h in lines],dtype=np.uint32)
    lo=(w&65535).astype(np.uint16).view(np.int16).astype(np.float64)
    hi=(w>>16).astype(np.uint16).view(np.int16).astype(np.float64)
    return lo+1j*hi,{'path':str(path.resolve()),'sha256':hashlib.sha256(path.read_bytes()).hexdigest(),**fields}


def fold128(z):
    n=np.arange(len(z)); phase=n%128
    rotation=np.exp(-2j*np.pi*(n//128)/3)
    # Each phase is trained on only ~16 blocks in an 8192-byte capture.
    template=np.array([np.mean((z/rotation)[phase==i]) for i in range(128)])
    return rotation*template[phase]


def rms(z): return float(np.sqrt(np.mean(abs(z)**2)))


def main():
    p=argparse.ArgumentParser(description=__doc__,formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument('capture',type=Path)
    p.add_argument('--baseline',nargs='+',type=Path)
    p.add_argument('--method',choices=('template','fold128'),default='template')
    p.add_argument('--sample-rate',type=float,default=8_000_000,
                   help='Nominal frequency-axis assumption; not a new clock measurement')
    p.add_argument('--out',required=True,type=Path)
    args=p.parse_args()
    if args.out.exists(): p.error('Output already exists; choose a new output path')
    if args.sample_rate<=0: p.error('Sample rate must be positive')
    if args.method=='template' and not args.baseline: p.error('Template method needs --baseline')
    if args.method=='fold128' and args.baseline: p.error('Fold128 learns from its input; omit --baseline')
    if args.out.resolve()==args.capture.resolve(): p.error('Output cannot overwrite input')
    z,source=load_capture(args.capture)
    training=[]
    if args.method=='template':
        arrays=[]
        for path in args.baseline:
            a,info=load_capture(path)
            if info['sha256']==source['sha256']: p.error('Input cannot also be a baseline')
            if len(a)!=len(z): p.error('All baselines must have the same length as the input')
            arrays.append(a); training.append(info)
        model=np.mean(arrays,axis=0)
    else:
        if len(z)<256: p.error('Fold128 needs at least two blocks')
        model=fold128(z)
    corrected=z-model
    metadata={'credit':'sfowlr original LR2021 route/ROM research; local offline cancellation follow-up.',
       'kind':'offline_derived_data','method':args.method,'source':source,'training':training,
       'sample_rate_hz_nominal':args.sample_rate,'pairs':len(z),'raw_rms_counts':rms(z),
       'corrected_rms_counts':rms(corrected),'continuous_capture_claimed':False,
       'fold128_erased_comb':'f/Fs=(integer-1/3)/128' if args.method=='fold128' else None,
       'template_limit':'Validate stability across SF, bandwidth, gain, boot and temperature before reuse.'}
    args.out.parent.mkdir(parents=True,exist_ok=True)
    # Use an explicit file handle so NumPy never silently appends a suffix.
    with args.out.open('xb') as f:
        np.savez_compressed(f,raw=z,model=model,corrected=corrected,metadata_json=json.dumps(metadata))
    print(json.dumps({k:v for k,v in metadata.items() if k not in ('source','training')},indent=2))


if __name__=='__main__': main()
