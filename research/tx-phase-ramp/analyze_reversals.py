"""Fit held plateau phases without using desired phase to choose timing."""
import pathlib,json,sys,numpy as np
name=sys.argv[1];rate=int(sys.argv[2]);pattern=int(sys.argv[3]);fs=250000
patterns=json.loads(pathlib.Path('v5-patterns.json').read_text())
q=np.array(patterns[str(pattern)]['iq_pairs'],float)
want=np.angle(q[7::8,0]+1j*q[7::8,1])
z=np.fromfile(pathlib.Path('rf')/(name+'.iq8'),np.int8).reshape(-1,2).astype(float)
y=np.mean(((z[:,0]+1j*z[:,1])/128).reshape(-1,8)*np.exp(-2j*np.pi*np.arange(8)/8),axis=1)
pw=np.convolve(abs(y)**2,np.ones(50)/50,'same');active=np.flatnonzero(pw>np.median(pw)*30);s=active[0]/fs
best=None
for start in np.arange(s-.00008,s+.0002,1/fs):
 windows=[(int((start+(i*8+5)/rate)*fs),int((start+(i*8+7)/rate)*fs)) for i in range(8)]
 xs=[];ps=[]
 for a,b in windows:
  t=np.arange(a,b)/fs;ph=np.unwrap(np.angle(y[a:b]));xs.append(t-t.mean());ps.append(ph-ph.mean())
 xx=np.concatenate(xs);pp=np.concatenate(ps);slope=np.dot(xx,pp)/np.dot(xx,xx)
 error=float(np.mean((pp-slope*xx)**2))
 if best is None or error<best[0]:best=(error,start,slope,windows)
error,start,slope,windows=best
angles=np.array([np.angle(np.mean(y[a:b]*np.exp(-1j*slope*np.arange(a,b)/fs))) for a,b in windows])
relative=np.angle(np.exp(1j*(angles-angles[0])));wanted=np.angle(np.exp(1j*(want-want[0])))
jump=np.angle(np.exp(1j*np.diff(angles)));wjump=np.angle(np.exp(1j*np.diff(want)))
r=dict(name=name,rate=rate,pattern=pattern,burst_s=[s,active[-1]/fs],start_s=float(start),cfo_hz=float(slope/(2*np.pi)),plateau_fit_rms_deg=float(np.sqrt(error)*180/np.pi),clipped_fraction=float(np.mean(abs(z)>=127)),expected_relative_deg=(wanted*180/np.pi).tolist(),measured_relative_deg=(relative*180/np.pi).tolist(),relative_error_deg=(np.angle(np.exp(1j*(relative-wanted)))*180/np.pi).tolist(),expected_jump_deg=(wjump*180/np.pi).tolist(),measured_jump_deg=(jump*180/np.pi).tolist(),jump_error_deg=(np.angle(np.exp(1j*(jump-wjump)))*180/np.pi).tolist())
print(json.dumps(r,indent=2));(pathlib.Path('rf')/(name+'.reversals.json')).write_text(json.dumps(r,indent=2))
