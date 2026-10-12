#!/usr/bin/env python3
"""Upload signed interleaved I8/Q8 to the LakeShark experimental TX console.
Requires pyserial. Upload alone does not transmit; --send explicitly transmits.
Current firmware fixes RF at 915 MHz/-9 dBm and bounds each run to <950 ms.
"""
import argparse,pathlib,re,time,zlib

def validate(data):
    if not data or len(data)>16384 or len(data)%2:
        raise ValueError('Expected 2..16384 bytes of interleaved signed I8/Q8')
    for n in range(0,len(data),2):
        i=int.from_bytes(data[n:n+1],'little',signed=True)
        q=int.from_bytes(data[n+1:n+2],'little',signed=True)
        if i*i+q*q>56*56:
            raise ValueError(f'Sample {n//2} exceeds the supported magnitude limit of 56')

def main():
    import serial
    from serial.tools import list_ports
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('iq8',type=pathlib.Path)
    g=p.add_mutually_exclusive_group(required=True);g.add_argument('--serial');g.add_argument('--port')
    p.add_argument('--send',type=int,choices=[4800,9600,19200,38400,76800,125000]);p.add_argument('--repeats',type=int,default=1)
    a=p.parse_args();data=a.iq8.read_bytes();validate(data)
    if not 1<=a.repeats<=16:raise ValueError('Repeats must be 1..16')
    if len(data)//2*a.repeats>32767:raise ValueError('Transmission exceeds the ROM limit of 32767 complex samples')
    if a.send and len(data)//2*a.repeats/a.send>.9:raise ValueError('Transmission exceeds the 900 ms sample-duration limit')
    if not a.send and a.repeats!=1:raise ValueError('--repeats requires --send')
    port=a.port
    if a.serial:
        ports=[v.device for v in list_ports.comports() if v.serial_number==a.serial]
        if len(ports)!=1:raise RuntimeError('Expected exactly one board with the requested USB serial number')
        port=ports[0]
    crc=zlib.crc32(data)
    c=serial.serial_for_url(port,do_not_open=True);c.baudrate=115200;c.timeout=.05;c.write_timeout=2;c.dtr=False;c.rts=False;c.open()
    def command(text,expected,timeout=3):
        c.write((text+'\r').encode('ascii'));buf='';deadline=time.monotonic()+timeout
        while time.monotonic()<deadline:
            buf+=c.read(max(1,c.in_waiting)).decode('utf-8',errors='replace')
            if re.search(r'lrtx-upload: error|lrtx: upload not ready',buf):raise RuntimeError('Board rejected the upload')
            match=re.search(expected,buf)
            if match:return match.group(0)
            if len(buf)>65536:raise RuntimeError('Unexpectedly large console response')
        raise TimeoutError('Missing expected console acknowledgement; no further commands sent')
    try:
        # Drain old console output without resetting the board.
        deadline=time.monotonic()+.25
        while time.monotonic()<deadline:c.read(max(1,c.in_waiting))
        command(f'lora lrtx-upload begin {len(data)} 0x{crc:08X}',rf'lrtx-upload: next=0 size={len(data)}\b')
        for off in range(0,len(data),64):
            block=data[off:off+64];command(f'lora lrtx-upload chunk {off} {block.hex()}',rf'lrtx-upload: next={off+len(block)}\b')
        print(command('lora lrtx-upload commit',rf'lrtx-upload: ready bytes={len(data)} crc32={crc:08X}\b'))
        if a.send:
            result=command(f'lora lrtx-send {a.send} {a.repeats}',r'lrtx: mode=[^\r\n]+[\r\n]',timeout=4).strip()
            print(result)
            flags=re.search(r'txflags=([0-9A-Fa-f]{2})\b',result)
            if not flags or int(flags[1],16)&0x30 or not all(s in result for s in ['txlevel=0','errors=0000 final_errors=0000','result=ESP_OK stop=ESP_OK recovery=ESP_OK']):
                raise RuntimeError('Transmit did not complete cleanly')
    finally:c.close()

if __name__=='__main__':main()
