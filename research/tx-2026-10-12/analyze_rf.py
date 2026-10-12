import argparse,json,pathlib
import numpy as np
p=argparse.ArgumentParser();p.add_argument('name');p.add_argument('--rate',type=int,default=9600);p.add_argument('--pattern',type=int,default=0)
a=p.parse_args();base=pathlib.Path('rf')/a.name
z=np.fromfile(base.with_suffix('.iq8'),np.int8).reshape(-1,2).astype(np.float32)
x=(z[:,0]+1j*z[:,1])/128
# Exact 250 kHz downmix, then 8-tap averaging/decimation to 250 kS/s.
osc=np.exp(-2j*np.pi*np.arange(8)/8).astype(np.complex64)
y=np.mean(x[:len(x)//8*8].reshape(-1,8)*osc,axis=1);fs=250000
power=abs(y)**2
block=250
pwr=power[:len(power)//block*block].reshape(-1,block).mean(axis=1)
floor=float(np.median(pwr)); good=pwr>max(floor*10,1e-12)
edges=np.diff(np.r_[False,good,False].astype(int)); starts=np.flatnonzero(edges==1);ends=np.flatnonzero(edges==-1)
segments=[(int(s*block),int(e*block)) for s,e in zip(starts,ends) if e-s>=2]
result={'name':a.name,'floor_dbfs':float(10*np.log10(floor)),'peak_dbfs':float(10*np.log10(max(pwr))), 'clipped_fraction':float(np.mean(abs(z)>=127)), 'bursts_s':[(s/fs,e/fs) for s,e in segments]}
if segments:
 s,e=max(segments,key=lambda se:se[1]-se[0]); v=y[s:e]
 freq=np.angle(v[1:]*v[:-1].conj())*fs/(2*np.pi)
 # A local 5-sample mean suppresses detector noise without spanning a bit.
 freq=np.convolve(freq,np.ones(5)/5,'same')
 lo,hi=np.percentile(freq,[20,80]);center=(lo+hi)/2
 result.update(frequency_p20_hz=float(lo),frequency_p80_hz=float(hi),carrier_offset_hz=float(center))
 state=0x13579bdf;payload=[]
 for _ in range(64):
  state^=(state<<13)&0xffffffff;state^=state>>17;state^=(state<<5)&0xffffffff;state&=0xffffffff
  payload.append((state&255)^(255 if a.pattern else 0))
 bits=np.unpackbits(np.array(payload,dtype=np.uint8)).astype(np.int16)*2-1
 best={}
 for n in (32,64,128,256,512):
  best_n=None
  for phase in np.arange(0,fs/a.rate,0.5):
   indices=np.arange(phase,len(freq)-1,fs/a.rate).astype(int)
   obs=np.where(freq[indices]>center,1,-1)
   if len(obs)<n:continue
   scores=np.correlate(obs,bits[:n],'valid');k=int(np.argmax(abs(scores)));score=int(scores[k]);errors=(n-abs(score))//2
   if best_n is None or errors<best_n['errors']:
    best_n={'errors':errors,'bits':n,'inverted':score<0,'start_s':float((s+phase+k*fs/a.rate)/fs),'phase_samples':float(phase)}
  if best_n:best[str(n)]=best_n
 result['prefix_matches']=best
 # Save a compact demodulated excerpt for independent inspection.
 np.savez_compressed(base.with_suffix('.demod.npz'),frequency_hz=freq,fs=fs,start_sample=s)
base.with_suffix('.analysis.json').write_text(json.dumps(result,indent=2));print(json.dumps(result,indent=2))
