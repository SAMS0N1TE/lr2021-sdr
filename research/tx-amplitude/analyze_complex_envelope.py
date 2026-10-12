"""Known plateau I/Q, one common CFO and complex gain, no block normalization.
This measures settled plateaus, not transient or full-waveform EVM.
"""
import pathlib,json,sys,numpy as np
name=sys.argv[1];rate=int(sys.argv[2]);pattern=int(sys.argv[3]);fs=250000
q=np.array(json.loads(pathlib.Path('v7-patterns.json').read_text())[str(pattern)]['plateau_iq'],float)
want=q[:,0]+1j*q[:,1];root=pathlib.Path('rf')
z=np.fromfile(root/(name+'.iq8'),np.int8).reshape(-1,2).astype(float)
y=np.mean(((z[:,0]+1j*z[:,1])/128).reshape(-1,8)*np.exp(-2j*np.pi*np.arange(8)/8),axis=1)
pw=np.convolve(abs(y)**2,np.ones(50)/50,'same');noise=float(np.median(pw));active=np.flatnonzero(pw>noise*6)
start=active[0]/fs+.00008-(8/rate if pattern==11 else 0)
windows=[(int((start+(i*8+2)/rate)*fs),int((start+(i*8+6)/rate)*fs)) for i in range(8)]
xx=[];pp=[]
for i,(a,b) in enumerate(windows):
 if abs(want[i])<12:continue
 t=np.arange(a,b)/fs;ph=np.unwrap(np.angle(y[a:b]));weight=float(np.sqrt(np.mean(abs(y[a:b])**2)))
 xx.extend((t-t.mean())*weight);pp.extend((ph-ph.mean())*weight)
xx=np.array(xx);pp=np.array(pp);slope=np.dot(xx,pp)/np.dot(xx,xx)
obs=np.array([np.mean(y[a:b]*np.exp(-1j*slope*(np.arange(a,b)/fs-start))) for a,b in windows])
gain=np.vdot(want,obs)/np.vdot(want,want);fit=obs/gain
amp=np.array([np.sqrt(max(0,np.mean(abs(y[a:b])**2)-noise)) for a,b in windows])
mask=abs(want)>0
r=dict(name=name,rate=rate,pattern=pattern,start_s=float(start),cfo_hz=float(slope/(2*np.pi)),clipped_fraction=float(np.mean(abs(z)>=127)),noise_power=noise,input_iq=q.tolist(),input_magnitude=abs(want).tolist(),plateau_rms=amp.tolist(),measured_iq_after_common_gain=[[float(v.real),float(v.imag)] for v in fit],settled_plateau_evm_percent=float(100*np.linalg.norm(fit-want)/np.linalg.norm(want)),phase_error_deg=(np.angle(fit[mask]/want[mask])*180/np.pi).tolist(),amplitude_correlation=float(np.corrcoef(abs(want),amp)[0,1]),amplitude_linear_fit=np.polyfit(abs(want),amp,1).tolist())
(root/(name+'.complex-envelope.json')).write_text(json.dumps(r,indent=2));print(json.dumps(r,indent=2))
