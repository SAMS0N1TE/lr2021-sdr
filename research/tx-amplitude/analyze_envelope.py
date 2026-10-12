"""Fixed-gain envelope measurements; no per-block normalization or AGC.
Pattern 4: eight held positive I levels, Q=0. Pattern 9: OOK control.
Usage: python analyze_envelope.py capture-name rate pattern
"""
import sys,json,pathlib,numpy as np
name=sys.argv[1];rate=int(sys.argv[2]);pattern=int(sys.argv[3]);fs=250000
root=pathlib.Path('rf');z=np.fromfile(root/(name+'.iq8'),np.int8).reshape(-1,2).astype(float)
y=np.mean(((z[:,0]+1j*z[:,1])/128).reshape(-1,8)*np.exp(-2j*np.pi*np.arange(8)/8),axis=1)
pw=np.convolve(abs(y)**2,np.ones(50)/50,'same');noise=float(np.median(pw));active=np.flatnonzero(pw>30*noise)
assert len(active),'No detectable burst'
s,e=active[[0,-1]]/fs
r=dict(name=name,rate=rate,pattern=pattern,burst_s=[float(s),float(e)],noise_power=noise,clipped_fraction=float(np.mean(abs(z)>=127)))
if pattern==4:
 levels=np.array([16,32,48,64,80,96,112,32]);amps=[]
 for i in range(8):
  a=int((s+(i*8+2)/rate)*fs);b=int((s+(i*8+6)/rate)*fs)
  amps.append(float(np.sqrt(max(0,np.mean(abs(y[a:b])**2)-noise))))
 r.update(input_i=levels.tolist(),plateau_rms=amps,observed_max_min_ratio=max(amps)/min(amps),expected_max_min_ratio=7,correlation=float(np.corrcoef(levels,amps)[0,1]))
else:
 # Packet control: report settled envelope distribution. Alternating four-byte
 # all-zero/all-one blocks dominate the burst; exclude burst edges.
 v=abs(y[int((s+.003)*fs):int((e-.003)*fs)])
 lo,hi=np.percentile(v,[25,75]);r.update(envelope_q25=float(lo),envelope_q75=float(hi),envelope_q75_q25_db=float(20*np.log10(hi/lo)))
(root/(name+'.envelope.json')).write_text(json.dumps(r,indent=2));print(json.dumps(r,indent=2))
