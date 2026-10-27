// SPDX-License-Identifier: MIT
use crate::*;
use std::collections::VecDeque;
use std::fmt;
use std::io::{Read, Write};
use std::net::{TcpStream, ToSocketAddrs};
use std::sync::{Arc, Condvar, Mutex};
use std::time::{Duration, Instant};

#[derive(Debug)]
pub enum ClientError {
    Io(std::io::Error),
    /// Device status code (see `status` and docs/bridge-protocol.md).
    Status(i32),
    Timeout,
    Protocol(&'static str),
    Closed,
}
impl fmt::Display for ClientError {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            ClientError::Io(e) => write!(f, "I/O: {e}"),
            ClientError::Status(s) => write!(f, "device status {s}"),
            ClientError::Timeout => write!(f, "no response"),
            ClientError::Protocol(m) => write!(f, "protocol: {m}"),
            ClientError::Closed => write!(f, "link closed"),
        }
    }
}
impl std::error::Error for ClientError {}
impl From<std::io::Error> for ClientError { fn from(e: std::io::Error) -> Self { ClientError::Io(e) } }
type Result<T> = std::result::Result<T, ClientError>;

/// Received pairs with their header. `iq` holds interleaved I, Q.
#[derive(Clone, Debug)]
pub struct Block { pub header: RxData, pub iq: Vec<i16> }
impl Block {
    /// Nominal device time of the first pair, in nanoseconds.
    pub fn time_ns(&self) -> u64 {
        let h = &self.header;
        h.start_time * 1000 + if h.rate_hz == 0 { 0 } else { h.first_pair * 1_000_000_000 / h.rate_hz as u64 }
    }
}

#[derive(Default)]
struct State {
    responses: VecDeque<Frame>,
    blocks: VecDeque<Block>,
    rx_end: Option<RxEnd>,
    tx_end: Option<TxEnd>,
    generation: Option<u32>,
    closed: bool,
    crc_errors: u64,
}
/// Blocking client. One thread reads the link; requests wait for matching
/// responses by type and sequence. RX blocks queue until read (up to 8192).
pub struct Client {
    writer: Mutex<Box<dyn Write + Send>>,
    shared: Arc<(Mutex<State>, Condvar)>,
    seq: Mutex<u16>,
    pub timeout: Duration,
}
impl Client {
    pub fn connect_tcp<A: ToSocketAddrs>(addr: A) -> Result<Client> {
        let s = TcpStream::connect(addr)?;
        s.set_nodelay(true)?;
        let r = s.try_clone()?;
        Ok(Client::new(r, s))
    }
    /// Uses any byte stream, such as a serial port already configured raw at
    /// the bridge baud rate.
    pub fn new<R: Read + Send + 'static, W: Write + Send + 'static>(mut reader: R, writer: W) -> Client {
        let shared = Arc::new((Mutex::new(State::default()), Condvar::new()));
        let s2 = shared.clone();
        std::thread::spawn(move || {
            let mut parser = Parser::new();
            let mut buf = vec![0u8; 16384];
            let mut frames = Vec::new();
            loop {
                let n = match reader.read(&mut buf) {
                    Ok(0) | Err(_) => break,
                    Ok(n) => n,
                };
                parser.push(&buf[..n], &mut frames);
                let (m, cv) = &*s2;
                let mut st = m.lock().unwrap();
                st.crc_errors = parser.crc_errors;
                for f in frames.drain(..) {
                    match f.kind {
                        kind::RX_DATA => {
                            let Some(h) = RxData::parse(&f.payload) else { continue };
                            if Some(h.generation) != st.generation || st.blocks.len() >= 8192 { continue; }
                            let mut iq = Vec::with_capacity(2 * h.pairs as usize);
                            if unpack_samples(h.format, h.shift, &f.payload[RX_DATA_HEADER..], &mut iq) {
                                st.blocks.push_back(Block { header: h, iq });
                            }
                        }
                        kind::RX_END => {
                            if let Some(e) = RxEnd::parse(&f.payload) { if Some(e.generation) == st.generation { st.rx_end = Some(e); } }
                        }
                        kind::TX_END => st.tx_end = TxEnd::parse(&f.payload),
                        k if k & kind::RESPONSE != 0 && k < kind::RX_DATA => {
                            // Data may follow the RX_START response in the same read:
                            // accept its generation here, not after the caller wakes.
                            if k == kind::RX_START | kind::RESPONSE && f.payload.len() >= 12 && f.payload[..4] == [0; 4] {
                                st.generation = Some(u32::from_le_bytes(f.payload[4..8].try_into().unwrap()));
                            }
                            st.responses.push_back(f)
                        }
                        _ => {}
                    }
                }
                cv.notify_all();
            }
            let (m, cv) = &*s2;
            m.lock().unwrap().closed = true;
            cv.notify_all();
        });
        Client { writer: Mutex::new(Box::new(writer)), shared, seq: Mutex::new(0), timeout: Duration::from_secs(5) }
    }
    /// Frames dropped for a bad CRC so far.
    pub fn crc_errors(&self) -> u64 { self.shared.0.lock().unwrap().crc_errors }

    /// Sends a request; returns the response payload after the status.
    pub fn request(&self, k: u8, payload: &[u8]) -> Result<Vec<u8>> {
        let mut seq = self.seq.lock().unwrap();
        *seq = seq.wrapping_add(1);
        let s = *seq;
        self.writer.lock().unwrap().write_all(&encode(k, s, payload))?;
        let (m, cv) = &*self.shared;
        let deadline = Instant::now() + self.timeout;
        let mut st = m.lock().unwrap();
        loop {
            if let Some(i) = st.responses.iter().position(|f| f.kind == k | kind::RESPONSE && f.sequence == s) {
                let f = st.responses.remove(i).unwrap();
                st.responses.retain(|f| f.sequence != s);
                if f.payload.len() < 4 { return Err(ClientError::Protocol("short response")); }
                let code = i32::from_le_bytes(f.payload[..4].try_into().unwrap());
                return if code == 0 { Ok(f.payload[4..].to_vec()) } else { Err(ClientError::Status(code)) };
            }
            if st.closed { return Err(ClientError::Closed); }
            let now = Instant::now();
            if now >= deadline { return Err(ClientError::Timeout); }
            st = cv.wait_timeout(st, deadline - now).unwrap().0;
        }
    }
    pub fn hello(&self) -> Result<Hello> {
        let mut p = 0i32.to_le_bytes().to_vec();
        p.extend(self.request(kind::HELLO, &[VERSION, 0])?);
        Hello::parse(&p).ok_or(ClientError::Protocol("hello length"))
    }
    /// Returns (radio errors, rate).
    pub fn set_rx(&self, rx: &RxConfig) -> Result<(u16, u32)> {
        let r = self.request(kind::SET_RX, &rx.to_bytes())?;
        if r.len() < 6 { return Err(ClientError::Protocol("ack length")); }
        Ok((u16::from_le_bytes([r[0], r[1]]), u32::from_le_bytes(r[2..6].try_into().unwrap())))
    }
    pub fn set_gain(&self, step: u8, boost: u8) -> Result<()> { self.request(kind::SET_GAIN, &[step, boost]).map(|_| ()) }
    /// Starts receive; returns (generation, rate).
    pub fn rx_start(&self, duration_ms: u32, format: u8) -> Result<(u32, u32)> {
        let mut p = 1024u16.to_le_bytes().to_vec();
        p.extend(duration_ms.to_le_bytes());
        if format != CS16 { p.push(format); }
        {
            let mut st = self.shared.0.lock().unwrap();
            st.generation = None; st.blocks.clear(); st.rx_end = None;
        }
        let r = self.request(kind::RX_START, &p)?;
        if r.len() < 8 { return Err(ClientError::Protocol("start length")); }
        let g = u32::from_le_bytes(r[..4].try_into().unwrap());
        self.shared.0.lock().unwrap().generation = Some(g);
        Ok((g, u32::from_le_bytes(r[4..8].try_into().unwrap())))
    }
    /// Next block, or None after `timeout` or once the stream has ended.
    pub fn read(&self, timeout: Duration) -> Result<Option<Block>> {
        let (m, cv) = &*self.shared;
        let deadline = Instant::now() + timeout;
        let mut st = m.lock().unwrap();
        loop {
            if let Some(b) = st.blocks.pop_front() { return Ok(Some(b)); }
            if st.rx_end.is_some() { return Ok(None); }
            if st.closed { return Err(ClientError::Closed); }
            let now = Instant::now();
            if now >= deadline { return Ok(None); }
            st = cv.wait_timeout(st, deadline - now).unwrap().0;
        }
    }
    pub fn rx_end(&self) -> Option<RxEnd> { self.shared.0.lock().unwrap().rx_end }
    /// Stops receive and waits for RX_END. Unread blocks stay readable.
    pub fn rx_stop(&self) -> Result<RxEnd> {
        self.request(kind::RX_STOP, &[])?;
        let (m, cv) = &*self.shared;
        let deadline = Instant::now() + self.timeout;
        let mut st = m.lock().unwrap();
        loop {
            if let Some(e) = st.rx_end { return Ok(e); }
            if st.closed { return Err(ClientError::Closed); }
            let now = Instant::now();
            if now >= deadline { return Err(ClientError::Timeout); }
            st = cv.wait_timeout(st, deadline - now).unwrap().0;
        }
    }
    /// Uploads signed I8,Q8 pairs (interleaved) and transmits them. `p.bytes`
    /// and `p.crc32` are filled in. Blocks until TX_END.
    pub fn transmit(&self, iq: &[i8], mut p: TxBegin) -> Result<TxEnd> {
        let bytes: Vec<u8> = iq.iter().map(|&v| v as u8).collect();
        p.bytes = bytes.len() as u32; p.crc32 = crc32(&bytes);
        self.request(kind::TX_BEGIN, &p.to_bytes())?;
        for (i, c) in bytes.chunks(TX_CHUNK).enumerate() {
            let mut d = ((i * TX_CHUNK) as u32).to_le_bytes().to_vec();
            d.extend_from_slice(c);
            self.request(kind::TX_DATA, &d)?;
        }
        self.shared.0.lock().unwrap().tx_end = None;
        self.request(kind::TX_COMMIT, &[])?;
        let pairs = (iq.len() / 2) as u64 * p.repeats.max(1) as u64;
        let mut wait = self.timeout + Duration::from_micros(pairs * 1_000_000 / p.rate_hz.max(1) as u64);
        if p.start_time != 0 {
            if let Ok(now) = self.time_us() { wait += Duration::from_micros(p.start_time.saturating_sub(now)); }
        }
        let (m, cv) = &*self.shared;
        let deadline = Instant::now() + wait;
        let mut st = m.lock().unwrap();
        loop {
            if let Some(e) = st.tx_end.take() {
                let code = if e.error != 0 { e.error } else if e.stop_error != 0 { e.stop_error } else { e.recovery_error };
                return if code == 0 { Ok(e) } else { Err(ClientError::Status(code)) };
            }
            if st.closed { return Err(ClientError::Closed); }
            let now = Instant::now();
            if now >= deadline { return Err(ClientError::Timeout); }
            st = cv.wait_timeout(st, deadline - now).unwrap().0;
        }
    }
    pub fn time_us(&self) -> Result<u64> {
        let r = self.request(kind::GET_TIME, &[])?;
        r.get(..8).map(|b| u64::from_le_bytes(b.try_into().unwrap())).ok_or(ClientError::Protocol("time length"))
    }
    pub fn bye(&self) -> Result<()> { self.request(kind::BYE, &[]).map(|_| ()) }
}
