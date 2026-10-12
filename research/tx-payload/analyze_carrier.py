import numpy as np,pathlib,json,sys
name=sys.argv[1];p=pathlib.Path('rf')/(name+'.iq8');fs=62500
z=np.fromfile(p,np.int8).reshape(-1,2).astype(float);y=np.mean(((z[:,0]+1j*z[:,1])/128).reshape(-1,32)*np.exp(-2j*np.pi*np.arange(32)/8),axis=1)
pw=np.convolve(abs(y)**2,np.ones(13)/13,'same');noise=float(np.median(pw));active=np.flatnonzero(pw>noise*30);s=active[0]/fs
ix=np.arange(int((s+10/9600)*fs),int((s+54/9600)*fs));t=ix/fs;v=y[ix];coef=np.polyfit(t-t[0],np.unwrap(np.angle(v)),1);v*=np.exp(-1j*coef[0]*(t-t[0]));gain=np.mean(v)
r=dict(name=name,cfo_hz=float(coef[0]/(2*np.pi)),carrier_residual_percent=float(100*np.sqrt(np.mean(abs(v-gain)**2))/abs(gain)),receiver_rms=float(abs(gain)),noise_power=noise,scope='Combined transmitter, propagation and receiver residual at constant I=48,Q=0; not an isolated receiver noise measurement')
p.with_suffix('.carrier.json').write_text(json.dumps(r,indent=2));print(json.dumps(r,indent=2))
