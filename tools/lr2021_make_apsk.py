#!/usr/bin/env python3
"""Generate an experimental 8-byte 8-APSK frame and its exact I8/Q8 samples."""
import argparse,binascii,json,math,pathlib
def make_frame(payload):
    if len(payload)!=8:raise ValueError('This experimental frame requires exactly eight payload bytes')
    crc=binascii.crc_hqx(payload,0xffff);bits=''.join(f'{v:08b}' for v in payload+crc.to_bytes(2,'big'));bits+='0'*((-len(bits))%3)
    training=[0,4,1,5,3,7,2,6,5,0,7,2,4,1,6,3];data=[int(bits[i:i+3],2) for i in range(0,len(bits),3)];symbols=training+data
    pairs=[];a0=24;p0=math.pi/4
    for sym in symbols:
        a=48 if sym&4 else 24;p=math.pi/4+(sym&3)*math.pi/2;delta=(p-p0+math.pi)%(2*math.pi)-math.pi
        for n in range(8):
            f=min(1,(n+1)/4);aa=a0+(a-a0)*f;pp=p0+delta*f;pairs.append([round(aa*math.cos(pp)),round(aa*math.sin(pp))])
        a0=a;p0=p
    return dict(payload_hex=payload.hex(),crc16_ccitt_false=f'{crc:04X}',training_symbols=training,data_symbols=data,symbols=symbols,samples_per_symbol=8,iq_pairs=pairs)
def main():
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('payload',help='Exactly 8 ASCII characters');p.add_argument('output',type=pathlib.Path);a=p.parse_args()
    f=make_frame(a.payload.encode('ascii'));a.output.write_bytes(bytes(v&255 for pair in f['iq_pairs'] for v in pair));a.output.with_suffix('.json').write_text(json.dumps(f,indent=2));print(f"Wrote {len(f['iq_pairs'])} complex samples; CRC {f['crc16_ccitt_false']}")
if __name__=='__main__':main()
