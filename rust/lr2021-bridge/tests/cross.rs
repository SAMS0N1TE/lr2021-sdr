// SPDX-License-Identifier: MIT
//! Cross tests: the pure-Rust codec and client against the C implementation
//! (lr2021-sys): frames both ways, every record both ways, CS12 packing, and
//! a live session with the C bridge server and simulator.
use core::ffi::c_void;
use lr2021_bridge::*;
use lr2021_sys as sys;

struct Rng(u64);
impl Rng {
    fn next(&mut self) -> u64 { self.0 ^= self.0 << 13; self.0 ^= self.0 >> 7; self.0 ^= self.0 << 17; self.0 }
    fn bytes(&mut self, n: usize) -> Vec<u8> { (0..n).map(|_| self.next() as u8).collect() }
}

unsafe extern "C" fn collect(ctx: *mut c_void, f: *const sys::lr2021_bridge_frame) {
    let out = &mut *(ctx as *mut Vec<Frame>);
    let f = &*f;
    let payload = if f.length == 0 { Vec::new() } else { std::slice::from_raw_parts(f.payload, f.length as usize).to_vec() };
    out.push(Frame { kind: f.type_, sequence: f.sequence, payload });
}
fn c_parse(stream: &[u8]) -> (Vec<Frame>, u32) {
    let mut out: Vec<Frame> = Vec::new();
    unsafe {
        let p = sys::lr2021_rs_parser_new();
        // Odd-sized pieces exercise partial frames in both parsers.
        for piece in stream.chunks(37) {
            sys::lr2021_bridge_parse(p, piece.as_ptr(), piece.len(), Some(collect), &mut out as *mut _ as *mut c_void);
        }
        let crc = sys::lr2021_rs_parser_crc_errors(p);
        sys::lr2021_rs_parser_free(p);
        (out, crc)
    }
}
fn c_encode(kind: u8, seq: u16, payload: &[u8]) -> Vec<u8> {
    let mut buf = vec![0u8; sys::LR2021_BRIDGE_MAX_FRAME];
    let n = unsafe { sys::lr2021_bridge_encode(buf.as_mut_ptr(), buf.len(), kind, seq, payload.as_ptr(), payload.len()) };
    buf.truncate(n);
    buf
}

#[test]
fn frames_match_both_ways_with_noise_and_corruption() {
    let mut rng = Rng(0x1234_5678_9ABC_DEF1);
    let mut stream = Vec::new();
    let mut expect = Vec::new();
    for i in 0..300u16 {
        let len = (rng.next() % 2049) as usize;
        let payload = rng.bytes(len);
        let kind = rng.next() as u8;
        let rust = encode(kind, i, &payload);
        assert_eq!(rust, c_encode(kind, i, &payload), "encoders differ at frame {i}");
        if i % 7 == 0 { stream.extend(rng.bytes(5)); } // console noise
        if i % 11 == 0 {
            let mut bad = rust.clone();
            let at = 8 + (rng.next() as usize % (bad.len() - 8));
            bad[at] ^= 0x40;
            stream.extend(bad);
        }
        stream.extend(&rust);
        expect.push(Frame { kind, sequence: i, payload });
    }
    let (c_frames, c_crc) = c_parse(&stream);
    let mut parser = Parser::new();
    let mut r_frames = Vec::new();
    for piece in stream.chunks(53) { parser.push(piece, &mut r_frames); }
    assert_eq!(c_frames, expect);
    assert_eq!(r_frames, expect);
    assert_eq!(parser.crc_errors, c_crc as u64);
}

#[test]
fn records_match_both_ways() {
    let mut rng = Rng(42);
    for _ in 0..200 {
        let b = rng.bytes(84);
        // hello: C unpack -> C pack must equal Rust parse -> Rust pack.
        let mut h: sys::lr2021_bridge_hello = unsafe { core::mem::zeroed() };
        // Names are NUL-padded ASCII.
        let mut name = b.clone();
        for base in [20usize, 52] {
            let len = (rng.next() % 32) as usize;
            for j in 0..32 { name[base + j] = if j < len { b'a' + (rng.next() % 26) as u8 } else { 0 }; }
        }
        assert_eq!(unsafe { sys::lr2021_bridge_unpack_hello(name.as_ptr(), 84, &mut h) }, 0);
        let mut c = [0u8; 84];
        unsafe { sys::lr2021_bridge_pack_hello(c.as_mut_ptr(), &h) };
        assert_eq!(Hello::parse(&name).unwrap().to_bytes(), c.to_vec());

        let rx = RxConfig { frequency_hz: rng.next() as u32, bandwidth_hz: rng.next() as u32, gain_step: rng.next() as u8, boost: rng.next() as u8, path: rng.next() as u8,
            route: rng.next() as u8, input_code: rng.next() as u8, rate_hz: rng.next() as u32 };
        let mut m = sys::lr2021_bridge_rx::default();
        let rb = rx.to_bytes();
        assert_eq!(rb.len(), 17);
        assert_eq!(unsafe { sys::lr2021_bridge_unpack_rx(rb.as_ptr(), rb.len(), &mut m) }, 0);
        assert_eq!((m.frequency_hz, m.bandwidth_hz, m.gain_step, m.boost, m.path, m.route, m.input_code, m.rate_hz),
            (rx.frequency_hz, rx.bandwidth_hz, rx.gain_step, rx.boost, rx.path, rx.route, rx.input_code, rx.rate_hz));
        let mut cb = [0u8; 17];
        assert_eq!(unsafe { sys::lr2021_bridge_pack_rx(cb.as_mut_ptr(), &m) }, 17);
        assert_eq!(RxConfig::parse(&cb), Some(rx));
        let short = RxConfig::parse(&cb[..11]).unwrap();
        assert_eq!((short.route, short.rate_hz), (0, 0));

        let tb = TxBegin { bytes: rng.next() as u32, crc32: rng.next() as u32, rate_hz: rng.next() as u32, frequency_hz: rng.next() as u32,
            power_half_db: rng.next() as i8, repeats: rng.next() as u8, start_time: rng.next(), pa: rng.next() as u8, flags: rng.next() as u8 };
        let mut ct = sys::lr2021_bridge_tx_begin::default();
        let tbb = tb.to_bytes();
        assert_eq!(unsafe { sys::lr2021_bridge_unpack_tx_begin(tbb.as_ptr(), tbb.len(), &mut ct) }, 0);
        let mut back = [0u8; 28];
        assert_eq!(unsafe { sys::lr2021_bridge_pack_tx_begin(back.as_mut_ptr(), &ct) }, 28);
        assert_eq!(back.to_vec(), tbb);
        assert_eq!(TxBegin::parse(&back), Some(tb));

        let end = sys::lr2021_bridge_rx_end { generation: rng.next() as u32, capture_error: rng.next() as i32, recovery_error: rng.next() as i32,
            reason: rng.next() as u8, fifo_flags: rng.next() as u8, max_fifo_level: rng.next() as u16, dropped: rng.next() as u32,
            gain_changes: rng.next() as u32, pairs: rng.next(), start_time: rng.next(), elapsed_us: rng.next(), max_service_gap_us: rng.next() };
        let mut eb = [0u8; 56];
        unsafe { sys::lr2021_bridge_pack_rx_end(eb.as_mut_ptr(), &end) };
        let r = RxEnd::parse(&eb).unwrap();
        assert_eq!((r.generation, r.capture_error, r.pairs, r.max_service_gap_us, r.max_fifo_level), (end.generation, end.capture_error, end.pairs, end.max_service_gap_us, end.max_fifo_level));

        let te = sys::lr2021_bridge_tx_end { error: rng.next() as i32, stop_error: rng.next() as i32, recovery_error: rng.next() as i32,
            tx_flags: rng.next() as u8, final_level: rng.next() as u16, min_level: rng.next() as u16, irq: rng.next() as u32, samples: rng.next() as u32,
            start_time: rng.next(), elapsed_us: rng.next(), max_service_gap_us: rng.next(), untested: rng.next() as u32 };
        let mut tbuf = [0u8; 53];
        unsafe { sys::lr2021_bridge_pack_tx_end(tbuf.as_mut_ptr(), &te) };
        let t = TxEnd::parse(&tbuf).unwrap();
        assert_eq!((t.error, t.min_level, t.samples, t.start_time, t.untested), (te.error, te.min_level, te.samples, te.start_time, te.untested));

        let pairs = 1 + (rng.next() % 480) as u16;
        let format = (rng.next() % 2) as u8;
        let d = RxData { generation: rng.next() as u32, rate_hz: rng.next() as u32, first_pair: rng.next(), start_time: rng.next(),
            dropped: rng.next() as u32, pairs, format, shift: if format == CS12 { (rng.next() % 5) as u8 } else { 0 } };
        let mut payload = d.to_bytes();
        payload.extend(rng.bytes(sample_bytes(format, pairs as usize)));
        let mut cd = sys::lr2021_bridge_rx_data::default();
        assert_eq!(unsafe { sys::lr2021_bridge_unpack_rx_data(payload.as_ptr(), payload.len(), &mut cd) }, 0);
        assert_eq!((cd.first_pair, cd.pairs, cd.format, cd.shift, cd.dropped), (d.first_pair, d.pairs, d.format, d.shift, d.dropped));
        assert_eq!(RxData::parse(&payload), Some(d));
        payload.push(0);
        assert!(RxData::parse(&payload).is_none());
        assert_ne!(unsafe { sys::lr2021_bridge_unpack_rx_data(payload.as_ptr(), payload.len(), &mut cd) }, 0);
    }
}

#[test]
fn cs12_packing_matches_the_c_device() {
    let mut rng = Rng(7);
    for round in 0..500 {
        let pairs = 1 + (rng.next() % 480) as usize;
        let scale = [16i64, 2048, 4096, 32768][round % 4];
        let iq: Vec<i16> = (0..2 * pairs).map(|_| ((rng.next() as i64 % scale) as i16)).collect();
        let raw: Vec<u8> = iq.iter().flat_map(|v| v.to_le_bytes()).collect();
        let mut c = vec![0u8; 3 * pairs];
        let mut shift = 0u8;
        assert_eq!(unsafe { sys::lr2021_bridge_pack_samples(c.as_mut_ptr(), CS12 as u32, raw.as_ptr(), pairs, &mut shift) }, 3 * pairs);
        let (rust, rshift) = pack_cs12(&iq);
        assert_eq!((rust.as_slice(), rshift), (c.as_slice(), shift), "round {round}");
        let mut back = Vec::new();
        assert!(unpack_samples(CS12, shift, &c, &mut back));
        let mut cback = vec![0i16; 2 * pairs];
        assert_eq!(unsafe { sys::lr2021_bridge_unpack_samples(cback.as_mut_ptr(), CS12 as u32, shift as u32, c.as_ptr(), c.len(), pairs) }, 0);
        assert_eq!(back, cback);
        for (a, b) in iq.iter().zip(&back) {
            assert!(*b <= *a && (*a as i32 - *b as i32) < (1 << shift), "{a} -> {b} shift {shift}");
        }
    }
}

#[cfg(unix)]
#[test]
fn rust_client_against_c_server_and_simulator() {
    use std::os::fd::AsRawFd;
    use std::time::Duration;
    let listener = std::net::TcpListener::bind("127.0.0.1:0").unwrap();
    let addr = listener.local_addr().unwrap();
    let server = std::thread::spawn(move || {
        let (s, _) = listener.accept().unwrap();
        unsafe { sys::host::lr2021_rs_serve_sim(s.as_raw_fd(), sys::LR2021_SIM_COUNTER) }
    });
    let c = Client::connect_tcp(addr).unwrap();
    let h = c.hello().unwrap();
    assert_eq!((h.version, h.rom_major, h.rom_minor, h.id.as_str()), (1, 1, 24, "rust-test"));
    assert_eq!(c.set_rx(&RxConfig { frequency_hz: 433_920_000, bandwidth_hz: 62_500, gain_step: 13, boost: 0, path: 2, ..Default::default() }).unwrap().1, 250_000);
    assert!(matches!(c.set_rx(&RxConfig { frequency_hz: 50_000_000, bandwidth_hz: 62_500, ..Default::default() }), Err(ClientError::Status(-1))));
    for format in [CS16, CS12] {
        let (generation, rate) = c.rx_start(0, format).unwrap();
        assert_eq!(rate, 250_000);
        let (mut pairs, mut unflagged, mut flagged, mut last, mut next) = (0u64, 0, 0, 0u32, None::<u64>);
        let until = std::time::Instant::now() + Duration::from_millis(600);
        let mut gained = false;
        while std::time::Instant::now() < until {
            let Some(b) = c.read(Duration::from_millis(200)).unwrap() else { continue };
            assert_eq!(b.header.generation, generation);
            let jump = next.map_or(false, |n| n != b.header.first_pair);
            if jump { flagged += 1; }
            for (i, p) in b.iq.chunks(2).enumerate() {
                let v = (p[0] as u16 as u32) | ((p[1] as u16 as u32) << 16);
                if format == CS16 && pairs > 0 && v != last.wrapping_add(1) && !(i == 0 && jump) { unflagged += 1; }
                last = v; pairs += 1;
            }
            next = Some(b.header.first_pair + b.header.pairs as u64);
            if !gained && pairs > 50_000 {
                c.set_gain(12, 0).unwrap();
                assert!(matches!(c.set_gain(12, 7), Err(ClientError::Status(status::EBUSY))));
                gained = true;
            }
        }
        let end = c.rx_stop().unwrap();
        assert_eq!(unflagged, 0);
        assert!(flagged == 0 || end.dropped > 0);
        assert!(pairs > 30_000 && end.capture_error == 0 && end.gain_changes == 1);
    }
    // Source 0 at 576000 pairs/s arrives as IQ8 in frames of up to 960 pairs.
    let raw = RxConfig { frequency_hz: 433_920_000, gain_step: 8, path: 2, route: 1, input_code: 17, rate_hz: 576_000, ..Default::default() };
    assert_eq!(c.set_rx(&raw).unwrap().1, 576_000);
    c.rx_start(0, CS16).unwrap();
    let (mut pairs, mut largest, mut next) = (0u64, 0u16, None::<u64>);
    let until = std::time::Instant::now() + Duration::from_millis(300);
    while std::time::Instant::now() < until {
        let Some(b) = c.read(Duration::from_millis(200)).unwrap() else { continue };
        assert_eq!(b.header.format, IQ8);
        assert!(next.map_or(true, |n| n <= b.header.first_pair));
        next = Some(b.header.first_pair + b.header.pairs as u64);
        largest = largest.max(b.header.pairs);
        pairs += b.header.pairs as u64;
    }
    let end = c.rx_stop().unwrap();
    assert!(pairs > 50_000 && end.capture_error == 0 && largest == 960, "{pairs} {largest}");
    c.set_rx(&RxConfig { frequency_hz: 433_920_000, bandwidth_hz: 31_250, gain_step: 13, path: 2, ..Default::default() }).unwrap();
    let iq: Vec<i8> = (0..4000).map(|i| (i % 60 - 30) as i8).collect();
    let t = c.transmit(&iq, TxBegin { rate_hz: 19_200, frequency_hz: 433_920_000, power_half_db: -18, repeats: 2, ..Default::default() }).unwrap();
    assert_eq!((t.samples, t.untested), (4000, 0));
    let r = c.transmit(&iq, TxBegin { rate_hz: 20_000, frequency_hz: 433_920_000, power_half_db: -18, repeats: 1, flags: 1, ..Default::default() });
    assert!(matches!(r, Err(ClientError::Status(status::ERANGE))));
    let t = c.transmit(&iq, TxBegin { rate_hz: 20_000, frequency_hz: 433_920_000, power_half_db: -18, repeats: 1, ..Default::default() }).unwrap();
    assert_eq!(t.untested, 1);
    assert!(c.time_us().unwrap() > 0);
    c.bye().unwrap();
    assert_eq!(server.join().unwrap(), 0);
}
