# SPDX-License-Identifier: MIT
import argparse, sys, time


def open_port(port):
    import serial
    s = serial.Serial()
    s.port, s.baudrate, s.timeout = port, 115200, 0.2
    s.dtr = s.rts = False
    s.open()
    time.sleep(0.3)
    s.read(1 << 20)
    s.write(b'\r\n')
    time.sleep(0.2)
    s.read(1 << 20)
    return s


def finished(buf):
    last = buf.rstrip().split(b'\n')[-1].strip()
    word = last.split(b' ')[0] if last else b''
    return last.endswith(b'done') or b'done ' in last or word in (b'rc', b'mw', b'err', b'gain', b'rxrestore')


def run(s, cmd, timeout):
    s.write((cmd + '\r\n').encode())
    buf, end = b'', time.time() + timeout
    while time.time() < end:
        d = s.read(65536)
        if d:
            buf += d
            end = time.time() + timeout
            body = buf.split(cmd.encode(), 1)[-1]
            if finished(body.replace(b'\r', b'')):
                break
    return buf.decode(errors='replace').replace('\r', '')


def streamdump(s, path, timeout=60):
    s.write(b'streamdump\r\n')
    buf, end = b'', time.time() + timeout
    while time.time() < end and b'STREAMBIN' not in buf:
        buf += s.read(4096)
    if b'STREAMBIN' not in buf:
        raise SystemExit('no STREAMBIN in the reply')
    i = buf.index(b'STREAMBIN')
    while b'\n' not in buf[i:]:
        buf += s.read(64)
    nl = buf.index(b'\n', i)
    summary = buf[:i].decode(errors='replace').replace('\r', '').strip().split('\n')[-1]
    n = int(buf[i:nl].split()[1])
    data = buf[nl + 1:]
    s.timeout = 2
    while len(data) < n:
        d = s.read(n - len(data))
        if not d:
            break
        data += d
    open(path, 'wb').write(data[:n])
    return summary, min(len(data), n), n


def main():
    ap = argparse.ArgumentParser(description='Script the bridge firmware console without resetting the board')
    ap.add_argument('port')
    ap.add_argument('-t', '--timeout', type=float, default=10, help='seconds of silence that end a command')
    ap.add_argument('-o', '--out', help='append command output to this file')
    ap.add_argument('--streamdump', metavar='FILE', help='after the commands, fetch the last stream ring into FILE')
    ap.add_argument('cmds', nargs='*')
    a = ap.parse_args()
    s = open_port(a.port)
    out = open(a.out, 'a') if a.out else None
    for c in a.cmds:
        text = run(s, c, a.timeout)
        print(text, end='' if text.endswith('\n') else '\n', file=out or sys.stdout)
    if a.streamdump:
        summary, got, n = streamdump(s, a.streamdump)
        print(summary)
        print(f'saved {got} of {n} bytes -> {a.streamdump}')
    s.close()


if __name__ == '__main__':
    main()
