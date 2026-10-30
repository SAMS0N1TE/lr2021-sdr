// SPDX-License-Identifier: MIT
//! Pure-Rust implementation of the LR2021-SDR bridge protocol, version 1
//! (`docs/bridge-protocol.md`): frame codec, resynchronizing parser, message
//! records, CS16/CS12 sample formats and a blocking [`Client`].
//! No dependencies beyond `std`. The crate's tests cross-check every encoder
//! and decoder against the C implementation.
mod client;
pub use client::{Block, Client, ClientError};

pub const VERSION: u8 = 1;
pub const HEADER: usize = 8;
pub const MAX_PAYLOAD: usize = 2048;
pub const RX_DATA_HEADER: usize = 32;
pub const RX_MAX_PAIRS: usize = 960;
pub const RX_MAX_BYTES: usize = 1920;
pub const TX_CHUNK: usize = 1024;
pub const TCP_PORT: u16 = 51021;

pub mod kind {
    pub const HELLO: u8 = 0x01;
    pub const SET_RX: u8 = 0x02;
    pub const RX_START: u8 = 0x03;
    pub const RX_STOP: u8 = 0x04;
    pub const SET_GAIN: u8 = 0x05;
    pub const TX_BEGIN: u8 = 0x06;
    pub const TX_DATA: u8 = 0x07;
    pub const TX_COMMIT: u8 = 0x08;
    pub const GET_TIME: u8 = 0x09;
    pub const BYE: u8 = 0x0A;
    pub const GET_GAIN: u8 = 0x0B;
    pub const RESPONSE: u8 = 0x80;
    pub const RX_DATA: u8 = 0xC0;
    pub const RX_END: u8 = 0xC1;
    pub const TX_END: u8 = 0xC2;
}
pub mod status {
    pub const EBUSY: i32 = -16;
    pub const EUNKNOWN: i32 = -17;
    pub const ELENGTH: i32 = -18;
    pub const ESTATE: i32 = -19;
    pub const EUNSUPPORTED: i32 = -20;
    pub const ECRC: i32 = -21;
    pub const ERANGE: i32 = -22;
}
pub mod caps {
    pub const RX: u16 = 1;
    pub const TX: u16 = 2;
    pub const TIMED_TX: u16 = 4;
    pub const LIVE_GAIN: u16 = 8;
    pub const RATE_250K: u16 = 16;
    pub const CS12: u16 = 32;
    pub const TX_CHECKED: u16 = 64;
    pub const RAW_ROUTES: u16 = 128;
    pub const GAIN_READBACK: u16 = 256;
}
pub const CS16: u8 = 0;
pub const CS12: u8 = 1;
/// Sources 0 and 1: int8 I, Q.
pub const IQ8: u8 = 2;
/// Source 6: signed 6-bit I, Q in bits 7..2, gain step in the low bits.
pub const IQ6G: u8 = 3;

/// CRC-32 (IEEE, reflected 0xEDB88320, init and final XOR 0xFFFFFFFF).
pub fn crc32(data: &[u8]) -> u32 {
    let mut c = !0u32;
    for &b in data {
        c ^= b as u32;
        for _ in 0..8 { c = (c >> 1) ^ (0xEDB8_8320 & (c & 1).wrapping_neg()); }
    }
    !c
}

/// Encodes one frame. Panics if the payload exceeds [`MAX_PAYLOAD`].
pub fn encode(kind: u8, sequence: u16, payload: &[u8]) -> Vec<u8> {
    assert!(payload.len() <= MAX_PAYLOAD, "payload too long");
    let mut f = Vec::with_capacity(HEADER + payload.len() + 4);
    f.extend_from_slice(&[b'L', b'R', VERSION, kind]);
    f.extend_from_slice(&sequence.to_le_bytes());
    f.extend_from_slice(&(payload.len() as u16).to_le_bytes());
    f.extend_from_slice(payload);
    let crc = crc32(&f[2..]);
    f.extend_from_slice(&crc.to_le_bytes());
    f
}

#[derive(Clone, Debug, PartialEq, Eq)]
pub struct Frame { pub kind: u8, pub sequence: u16, pub payload: Vec<u8> }

/// Streaming parser: scan for "LR" + version, check length and CRC, and on
/// any failure drop one byte and scan again.
#[derive(Default)]
pub struct Parser { buf: Vec<u8>, pub crc_errors: u64, pub skipped: u64 }
impl Parser {
    pub fn new() -> Self { Self::default() }
    pub fn push(&mut self, data: &[u8], out: &mut Vec<Frame>) {
        self.buf.extend_from_slice(data);
        let mut start = 0;
        loop {
            let b = &self.buf[start..];
            if b.is_empty() { break; }
            if b[0] != b'L' || (b.len() > 1 && b[1] != b'R') || (b.len() > 2 && b[2] != VERSION) {
                start += 1; self.skipped += 1; continue;
            }
            if b.len() < HEADER { break; }
            let len = u16::from_le_bytes([b[6], b[7]]) as usize;
            if len > MAX_PAYLOAD { start += 1; self.skipped += 1; continue; }
            if b.len() < HEADER + len + 4 { break; }
            let crc = u32::from_le_bytes(b[HEADER + len..HEADER + len + 4].try_into().unwrap());
            if crc32(&b[2..HEADER + len]) != crc { start += 1; self.crc_errors += 1; self.skipped += 1; continue; }
            out.push(Frame { kind: b[3], sequence: u16::from_le_bytes([b[4], b[5]]), payload: b[HEADER..HEADER + len].to_vec() });
            start += HEADER + len + 4;
        }
        self.buf.drain(..start);
    }
}

struct R<'a>(&'a [u8], usize);
impl<'a> R<'a> {
    fn u8(&mut self) -> u8 { let v = self.0[self.1]; self.1 += 1; v }
    fn u16(&mut self) -> u16 { let v = u16::from_le_bytes(self.0[self.1..self.1 + 2].try_into().unwrap()); self.1 += 2; v }
    fn u32(&mut self) -> u32 { let v = u32::from_le_bytes(self.0[self.1..self.1 + 4].try_into().unwrap()); self.1 += 4; v }
    fn i32(&mut self) -> i32 { self.u32() as i32 }
    fn u64(&mut self) -> u64 { let v = u64::from_le_bytes(self.0[self.1..self.1 + 8].try_into().unwrap()); self.1 += 8; v }
    fn text(&mut self) -> String {
        let s = &self.0[self.1..self.1 + 32]; self.1 += 32;
        let n = s.iter().position(|&c| c == 0).unwrap_or(31).min(31);
        String::from_utf8_lossy(&s[..n]).into_owned()
    }
}
macro_rules! sized { ($b:expr, $n:expr) => { if $b.len() != $n { return None; } }; }

#[derive(Clone, Debug, Default, PartialEq, Eq)]
pub struct Hello {
    pub status: i32, pub version: u16, pub caps: u16, pub rom_major: u8, pub rom_minor: u8,
    pub max_payload: u16, pub clock_hz: u32, pub tx_capacity: u32, pub name: String, pub id: String,
}
impl Hello {
    pub fn parse(b: &[u8]) -> Option<Self> {
        sized!(b, 84);
        let mut r = R(b, 0);
        Some(Hello { status: r.i32(), version: r.u16(), caps: r.u16(), rom_major: r.u8(), rom_minor: r.u8(),
            max_payload: r.u16(), clock_hz: r.u32(), tx_capacity: r.u32(), name: r.text(), id: r.text() })
    }
    pub fn to_bytes(&self) -> Vec<u8> {
        let mut v = Vec::with_capacity(84);
        v.extend(self.status.to_le_bytes()); v.extend(self.version.to_le_bytes()); v.extend(self.caps.to_le_bytes());
        v.extend([self.rom_major, self.rom_minor]); v.extend(self.max_payload.to_le_bytes());
        v.extend(self.clock_hz.to_le_bytes()); v.extend(self.tx_capacity.to_le_bytes());
        for s in [&self.name, &self.id] {
            let mut t = [0u8; 32];
            let n = s.len().min(31); t[..n].copy_from_slice(&s.as_bytes()[..n]); v.extend(t);
        }
        v
    }
}
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct RxConfig {
    pub frequency_hz: u32, pub bandwidth_hz: u32, pub gain_step: u8, pub boost: u8, pub path: u8,
    /// 0 source 2; 1, 2, 3 sources 0, 1, 6 at `rate_hz` with FSK filter code `input_code`.
    pub route: u8, pub input_code: u8, pub rate_hz: u32,
}
impl RxConfig {
    pub fn to_bytes(&self) -> Vec<u8> {
        let mut v = Vec::with_capacity(17);
        v.extend(self.frequency_hz.to_le_bytes()); v.extend(self.bandwidth_hz.to_le_bytes());
        v.extend([self.gain_step, self.boost, self.path, self.route, self.input_code]);
        v.extend(self.rate_hz.to_le_bytes()); v
    }
    /// Accepts the 17-byte record and the 11-byte source-2 form.
    pub fn parse(b: &[u8]) -> Option<Self> {
        if b.len() != 11 && b.len() != 17 { return None; }
        let mut r = R(b, 0);
        let mut c = RxConfig { frequency_hz: r.u32(), bandwidth_hz: r.u32(), gain_step: r.u8(), boost: r.u8(), path: r.u8(), ..Default::default() };
        if b.len() == 17 { c.route = r.u8(); c.input_code = r.u8(); c.rate_hz = r.u32(); }
        Some(c)
    }
}
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct TxBegin {
    pub bytes: u32, pub crc32: u32, pub rate_hz: u32, pub frequency_hz: u32,
    pub power_half_db: i8, pub repeats: u8, pub start_time: u64, pub pa: u8, pub flags: u8,
}
impl TxBegin {
    pub fn to_bytes(&self) -> Vec<u8> {
        let mut v = Vec::with_capacity(28);
        for x in [self.bytes, self.crc32, self.rate_hz, self.frequency_hz] { v.extend(x.to_le_bytes()); }
        v.extend([self.power_half_db as u8, self.repeats]); v.extend(self.start_time.to_le_bytes());
        v.extend([self.pa, self.flags]); v
    }
    pub fn parse(b: &[u8]) -> Option<Self> {
        sized!(b, 28);
        let mut r = R(b, 0);
        Some(TxBegin { bytes: r.u32(), crc32: r.u32(), rate_hz: r.u32(), frequency_hz: r.u32(),
            power_half_db: r.u8() as i8, repeats: r.u8(), start_time: r.u64(), pa: r.u8(), flags: r.u8() })
    }
}
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct RxData { pub generation: u32, pub rate_hz: u32, pub first_pair: u64, pub start_time: u64,
    pub dropped: u32, pub pairs: u16, pub format: u8, pub shift: u8 }
impl RxData {
    /// Parses the header and checks the data length for the format.
    pub fn parse(b: &[u8]) -> Option<Self> {
        if b.len() < RX_DATA_HEADER { return None; }
        let mut r = R(b, 0);
        let d = RxData { generation: r.u32(), rate_hz: r.u32(), first_pair: r.u64(), start_time: r.u64(),
            dropped: r.u32(), pairs: r.u16(), format: r.u8(), shift: r.u8() };
        let ok = d.pairs >= 1 && d.pairs as usize <= RX_MAX_PAIRS && d.shift <= 4 && d.format <= IQ6G
            && !(d.format != CS12 && d.shift != 0)
            && sample_bytes(d.format, d.pairs as usize) <= RX_MAX_BYTES
            && b.len() - RX_DATA_HEADER == sample_bytes(d.format, d.pairs as usize);
        ok.then_some(d)
    }
    pub fn to_bytes(&self) -> Vec<u8> {
        let mut v = Vec::with_capacity(RX_DATA_HEADER);
        v.extend(self.generation.to_le_bytes()); v.extend(self.rate_hz.to_le_bytes());
        v.extend(self.first_pair.to_le_bytes()); v.extend(self.start_time.to_le_bytes());
        v.extend(self.dropped.to_le_bytes()); v.extend(self.pairs.to_le_bytes()); v.extend([self.format, self.shift]); v
    }
}
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct RxEnd { pub generation: u32, pub capture_error: i32, pub recovery_error: i32, pub reason: u8,
    pub fifo_flags: u8, pub max_fifo_level: u16, pub dropped: u32, pub gain_changes: u32, pub pairs: u64,
    pub start_time: u64, pub elapsed_us: u64, pub max_service_gap_us: u64 }
impl RxEnd {
    pub fn parse(b: &[u8]) -> Option<Self> {
        sized!(b, 56);
        let mut r = R(b, 0);
        Some(RxEnd { generation: r.u32(), capture_error: r.i32(), recovery_error: r.i32(), reason: r.u8(),
            fifo_flags: r.u8(), max_fifo_level: r.u16(), dropped: r.u32(), gain_changes: r.u32(), pairs: r.u64(),
            start_time: r.u64(), elapsed_us: r.u64(), max_service_gap_us: r.u64() })
    }
}
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct Gain { pub status: i32, pub step: u8, pub manual: u8, pub gain_value: u16, pub rssi_raw: u16, pub device_time: u64 }
impl Gain {
    pub fn parse(b: &[u8]) -> Option<Self> {
        sized!(b, 18);
        let mut r = R(b, 0);
        Some(Gain { status: r.i32(), step: r.u8(), manual: r.u8(), gain_value: r.u16(), rssi_raw: r.u16(), device_time: r.u64() })
    }
    pub fn to_bytes(&self) -> Vec<u8> {
        let mut v = Vec::with_capacity(18);
        v.extend(self.status.to_le_bytes()); v.extend([self.step, self.manual]);
        v.extend(self.gain_value.to_le_bytes()); v.extend(self.rssi_raw.to_le_bytes()); v.extend(self.device_time.to_le_bytes()); v
    }
}
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct TxEnd { pub error: i32, pub stop_error: i32, pub recovery_error: i32, pub tx_flags: u8,
    pub final_level: u16, pub min_level: u16, pub irq: u32, pub samples: u32, pub start_time: u64,
    pub elapsed_us: u64, pub max_service_gap_us: u64, pub untested: u32 }
impl TxEnd {
    pub fn parse(b: &[u8]) -> Option<Self> {
        sized!(b, 53);
        let mut r = R(b, 0);
        Some(TxEnd { error: r.i32(), stop_error: r.i32(), recovery_error: r.i32(), tx_flags: r.u8(),
            final_level: r.u16(), min_level: r.u16(), irq: r.u32(), samples: r.u32(), start_time: r.u64(),
            elapsed_us: r.u64(), max_service_gap_us: r.u64(), untested: r.u32() })
    }
}

pub fn sample_bytes(format: u8, pairs: usize) -> usize {
    match format { CS12 => 3 * pairs, IQ8 | IQ6G => 2 * pairs, _ => 4 * pairs }
}
/// Decodes packed pairs into interleaved int16 I, Q.
pub fn unpack_samples(format: u8, shift: u8, data: &[u8], out: &mut Vec<i16>) -> bool {
    if format == CS16 {
        if data.len() % 4 != 0 || shift != 0 { return false; }
        out.extend(data.chunks_exact(2).map(|c| i16::from_le_bytes([c[0], c[1]])));
        return true;
    }
    if format == IQ8 || format == IQ6G {
        if shift != 0 { return false; }
        // Scaled to the CS16 range: IQ8 by 256, IQ6 (bits 7..2) by 1024.
        out.extend(data.iter().map(|&b| if format == IQ8 { (b as i8 as i16) * 256 } else { ((b as i8 >> 2) as i16) * 1024 }));
        return true;
    }
    if format != CS12 || data.len() % 3 != 0 || shift > 4 { return false; }
    let sign12 = |v: u32| -> i32 { let v = (v & 0xFFF) as i32; if v > 2047 { v - 4096 } else { v } };
    for c in data.chunks_exact(3) {
        let w = c[0] as u32 | (c[1] as u32) << 8 | (c[2] as u32) << 16;
        out.push((sign12(w) << shift) as i16);
        out.push((sign12(w >> 12) << shift) as i16);
    }
    true
}
/// CS12 packing as the device does it: the smallest shift that fits
/// -2048..2047, values shifted toward minus infinity.
pub fn pack_cs12(iq: &[i16]) -> (Vec<u8>, u8) {
    let peak = iq.iter().map(|&v| if v < 0 { -(v as i32) - 1 } else { v as i32 }).max().unwrap_or(0);
    let mut s = 0u8;
    while s < 4 && (peak >> s) > 2047 { s += 1; }
    let mut out = Vec::with_capacity(iq.len() / 2 * 3);
    for p in iq.chunks_exact(2) {
        let (a, b) = ((p[0] as i32) >> s, (p[1] as i32) >> s); // arithmetic shift floors
        let w = (a as u32 & 0xFFF) | ((b as u32 & 0xFFF) << 12);
        out.extend([w as u8, (w >> 8) as u8, (w >> 16) as u8]);
    }
    (out, s)
}
