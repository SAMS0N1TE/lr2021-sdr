"""Read MeshCore wire packets (MeshCore src/Packet.{h,cpp}, Mesh.cpp, helpers/AdvertDataHelpers.cpp).

No keys are required for adverts or routing metadata. Encrypted message bodies
remain ciphertext; parsing an advert does not verify its Ed25519 signature.
All returned fields are JSON serializable. Malformed packets raise MeshCoreError.
"""
from datetime import datetime, timezone


class MeshCoreError(ValueError):
    pass


ROUTES = ('TRANSPORT_FLOOD', 'FLOOD', 'DIRECT', 'TRANSPORT_DIRECT')
TYPES = {
    0: 'REQ', 1: 'RESPONSE', 2: 'TXT_MSG', 3: 'ACK', 4: 'ADVERT',
    5: 'GRP_TXT', 6: 'GRP_DATA', 7: 'ANON_REQ', 8: 'PATH',
    9: 'TRACE', 10: 'MULTIPART', 11: 'CONTROL', 15: 'RAW_CUSTOM',
}
ROLES = {0: 'NONE', 1: 'CHAT', 2: 'REPEATER', 3: 'ROOM', 4: 'SENSOR'}


class _Reader:
    def __init__(self, data):
        self.data = bytes(data)
        self.pos = 0

    def take(self, n):
        if self.pos + n > len(self.data):
            raise MeshCoreError(f'truncated packet at byte {self.pos}: need {n} bytes')
        b = self.data[self.pos:self.pos + n]
        self.pos += n
        return b

    def uint(self, n):
        return int.from_bytes(self.take(n), 'little')

    def rest(self):
        return self.take(len(self.data) - self.pos)


def _advert(data):
    r = _Reader(data)
    key, timestamp, signature = r.take(32), r.uint(4), r.take(64)
    app = r.rest()
    if len(app) > 32:
        raise MeshCoreError('advert application data exceeds 32 bytes')
    result = dict(public_key=key.hex(), timestamp=timestamp,
                  timestamp_utc=datetime.fromtimestamp(timestamp, timezone.utc).isoformat(),
                  signature=signature.hex(), signature_verified=None, app_data=app.hex())
    if not app:
        return result
    a = _Reader(app)
    flags = a.uint(1)
    result.update(flags=flags, role=ROLES.get(flags & 15, f'UNKNOWN_{flags & 15}'))
    if flags & 0x10:
        result['latitude'] = int.from_bytes(a.take(4), 'little', signed=True) / 1e6
        result['longitude'] = int.from_bytes(a.take(4), 'little', signed=True) / 1e6
    if flags & 0x20:
        result['feature1'] = a.uint(2)
    if flags & 0x40:
        result['feature2'] = a.uint(2)
    if flags & 0x80:
        result['name'] = a.rest().split(b'\0', 1)[0].decode('utf-8', errors='replace')
    elif a.pos < len(app):
        result['extra'] = a.rest().hex()
    return result


def _body(kind, data):
    r = _Reader(data)
    if kind == 4:
        return _advert(data)
    if kind in (0, 1, 2, 8, 5, 6, 7):
        b = dict(encrypted=True)
        if kind in (5, 6):
            b['channel_hash'] = r.take(1).hex()
        else:
            b['destination_hash'] = r.take(1).hex()
            if kind == 7:
                b['sender_public_key'] = r.take(32).hex()
            else:
                b['source_hash'] = r.take(1).hex()
        b['mac'] = r.take(2).hex()
        b['ciphertext'] = r.rest().hex()
        if not b['ciphertext']:
            raise MeshCoreError('missing encrypted body')
        return b
    if kind == 3:
        b = dict(ack_crc=r.take(4).hex())
        if r.pos < len(data):
            b['extra'] = r.rest().hex()
        return b
    if kind == 9:
        b = dict(tag=r.uint(4), auth_code=r.uint(4), flags=r.uint(1))
        size = 1 << (b['flags'] & 3)
        path = r.rest()
        if len(path) % size:
            raise MeshCoreError('incomplete trace path hash')
        b['route_hash_size'] = size
        b['route'] = [path[i:i + size].hex() for i in range(0, len(path), size)]
        return b
    if kind == 10:
        flags = r.uint(1)
        inner = flags & 15
        if inner == 10:
            raise MeshCoreError('nested multipart packet')
        return dict(remaining=flags >> 4, type=TYPES.get(inner, f'UNKNOWN_{inner}'),
                    body=_body(inner, r.rest()))
    return dict(raw=data.hex())


def parse_packet(data):
    """Parse one PHY payload, excluding the LoRa header and PHY CRC bytes."""
    r = _Reader(data)
    header = r.uint(1)
    route, kind, version = header & 3, (header >> 2) & 15, header >> 6
    packet = dict(header=header, route=ROUTES[route], type=TYPES.get(kind, f'UNKNOWN_{kind}'),
                  version=version + 1)
    if route in (0, 3):
        packet['transport_codes'] = [r.uint(2), r.uint(2)]
    path_len = r.uint(1)
    size, count = (path_len >> 6) + 1, path_len & 63
    if size == 4 or size * count > 64:
        raise MeshCoreError(f'invalid path encoding 0x{path_len:02x}')
    path = r.take(size * count)
    packet.update(path_hash_size=size, path_count=count,
                  path=[path[i:i + size].hex() for i in range(0, len(path), size)])
    payload = r.rest()
    if not payload or len(payload) > 184:
        raise MeshCoreError(f'invalid payload length {len(payload)}')
    packet['payload_hex'] = payload.hex()
    if version:
        packet['body'] = dict(raw=payload.hex(), unsupported_version=True)
    else:
        packet['body'] = _body(kind, payload)
        if kind == 9 and route in (2, 3):
            packet['snr_db'] = [int.from_bytes(bytes([v]), 'little', signed=True) / 4 for v in path]
            packet.pop('path')  # TRACE collects SNR bytes in place of node hashes.
    return packet


def summarize_packet(packet):
    """Compact, single-line description for the live sniffer."""
    b = packet['body']
    parts = [packet['type'], packet['route'], f"hops={packet['path_count']}"]
    if packet['type'] == 'ADVERT' and 'public_key' in b:
        parts += [b.get('role', 'NONE'), repr(b.get('name', '')), f"key={b['public_key']}"]
        if 'latitude' in b:
            parts += [f"loc={b['latitude']:.6f},{b['longitude']:.6f}"]
        parts += [f"time={b['timestamp_utc']}"]
    elif b.get('encrypted'):
        for field in ('destination_hash', 'source_hash', 'channel_hash'):
            if field in b:
                parts += [f"{field}={b[field]}"]
        parts += [f"encrypted={len(b['ciphertext']) // 2}B"]
    elif 'ack_crc' in b:
        parts += [f"ack={b['ack_crc']}"]
    return ' '.join(parts)


if __name__ == '__main__':
    import json
    import sys
    print(json.dumps(parse_packet(bytes.fromhex(sys.argv[1])), indent=2, ensure_ascii=False))
