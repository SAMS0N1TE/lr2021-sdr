"""Interior waveform fit to linearly interpolated, quantized input I/Q.
One timing offset, one carrier offset and one complex gain; no equalizer,
DC subtraction, per-block normalization or amplitude/phase correction.
"""
import numpy as np,json,pathlib,sys
name=sys.argv[1];rate=int(sys.argv[2]);pattern=int(sys.argv[3]);fs=62500
q=np.array(json.loads(pathlib.Path('v8-patterns.json').read_text())[str(pattern)]['iq_pairs'],float);q=q[:,0]+1j*q[:,1]
root=pathlib.Path('rf');z=np.fromfile(root/(name+'.iq8'),np.int8).reshape(-1,2).astype(float)
y=np.mean(((z[:,0]+1j*z[:,1])/128).reshape(-1,32)*np.exp(-2j*np.pi*np.arange(32)/8),axis=1)
pw=np.convolve(abs(y)**2,np.ones(13)/13,'same');noise=float(np.median(pw));active=np.flatnonzero(pw>noise*30);s=active[0]/fs
# Fixed interior window covers continuously changing samples, not plateaus.
ix=np.arange(int((s+10/rate)*fs),int((s+54/rate)*fs));t=ix/fs-s;obs=y[ix]
def fit(starts,freqs):
 global best
 derot=obs[None,:]*np.exp(-2j*np.pi*freqs[:,None]*t)
 for start in starts:
  pos=(t-start)*rate;want=np.interp(pos,np.arange(64),q.real)+1j*np.interp(pos,np.arange(64),q.imag)
  norm=float(np.vdot(want,want).real);gain=derot@want.conj()/norm
  error=np.sum(abs(derot-gain[:,None]*want)**2,axis=1)/np.sum(abs(gain[:,None]*want)**2,axis=1)
  k=int(np.argmin(error))
  if best is None or error[k]<best[0]:best=(float(error[k]),float(start),float(freqs[k]),complex(gain[k]),want.copy(),derot[k].copy())
best=None
fit(np.arange(-.0002,.000601,1/fs),np.arange(-600,101,5))
fit(np.arange(best[1]-2/fs,best[1]+2.1/fs,.25/fs),np.arange(best[2]-5,best[2]+5.1,.25))
error,delay,cfo,gain,want,derot=best;corrected=derot/gain
phase=np.angle(corrected/want)*180/np.pi
r=dict(name=name,rate=rate,pattern=pattern,burst_s=[float(s),float(active[-1]/fs)],noise_power=noise,clipped_fraction=float(np.mean(abs(z)>=127)),timing_offset_from_detected_start_s=delay,cfo_hz=cfo,complex_gain=[gain.real,gain.imag],interior_waveform_evm_percent=float(100*np.sqrt(error)),amplitude_correlation=float(np.corrcoef(abs(want),abs(corrected))[0,1]),phase_rms_deg=float(np.sqrt(np.mean(phase**2))),expected_amplitude_range=[float(min(abs(want))),float(max(abs(want)))],measured_amplitude_range=[float(np.percentile(abs(corrected),1)),float(np.percentile(abs(corrected),99))],fit_sample_count=len(ix),reference='linear interpolation of quantized I8/Q8 input; interior samples10..54; common timing/CFO/gain only')
(root/(name+'.smooth.json')).write_text(json.dumps(r,indent=2));print(json.dumps(r,indent=2))
