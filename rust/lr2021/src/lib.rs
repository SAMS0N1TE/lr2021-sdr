// SPDX-License-Identifier: MIT
//! Safe Rust API over the LR2021-SDR C library.
//!
//! * [`Simulator`]: the library's LR2021 model behind the real device code
//!   (receive configuration, checked sessions with live gain, 0x0293 TX
//!   bursts), in virtual time, for tests and development without hardware.
//! * [`Host`] (Unix): the C host client, the same one the SoapySDR module
//!   uses, over a bridge firmware (serial or TCP), the local spidev backend or
//!   the in-process simulator.
//! * [`TxBurst::check`] and [`rate_for_bandwidth`]: the documented limits.
//!
//! Hardware behaviour is described in the repository docs; nothing here is
//! tested on hardware by the crate's own tests.
use core::ffi::{c_int, c_void, CStr};
use lr2021_sys as sys;
use std::fmt;

/// A status code from the C library (`LR2021_IQ_*`, bridge or host codes).
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct Error(pub i32);
impl fmt::Display for Error {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        let s = unsafe { CStr::from_ptr(sys::lr2021_bridge_strerror(self.0)) };
        write!(f, "{} ({})", s.to_string_lossy(), self.0)
    }
}
impl std::error::Error for Error {}
pub type Result<T> = std::result::Result<T, Error>;
fn check(e: c_int) -> Result<()> { if e == 0 { Ok(()) } else { Err(Error(e)) } }

pub const MIN_HZ: u32 = sys::LR2021_IQ_MIN_HZ;
pub const MAX_HZ: u32 = sys::LR2021_IQ_MAX_HZ;
pub const TX_MAX_SAMPLES: u32 = sys::LR2021_IQ_TX_MAX_SAMPLES;

/// Complex pairs/s for a LoRa bandwidth that can stream (31250 or 62500 Hz).
pub fn rate_for_bandwidth(bandwidth_hz: u32) -> Option<u32> {
    match unsafe { sys::lr2021_iq_rate(bandwidth_hz) } { 0 => None, r => Some(r) }
}
/// Pairs/s of the converter input an FSK filter code selects; source 1 runs at it.
pub fn input_rate(code: u8) -> u32 { unsafe { sys::lr2021_iq_input_rate(code) } }
pub fn bandwidth_for_rate(rate: u32) -> Option<u32> {
    match unsafe { sys::lr2021_iq_bandwidth(rate) } { 0 => None, b => Some(b) }
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Path { Lf = 0, Hf = 1, Auto = 2 }

/// Receive configuration. `gain_step` 0 selects the chip AGC.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct RxConfig {
    pub frequency_hz: u32, pub bandwidth_hz: u32, pub gain_step: u8, pub boost: u8, pub path: Path,
    /// Receive route; source 2 uses the bandwidth, the others `rate_hz` and `input_code`.
    pub route: Route, pub input_code: u8, pub rate_hz: u32,
}
/// Source 2: IQ16 at 4 x LoRa bandwidth. Sources 0, 1: IQ8; source 6: gain-tagged IQ6.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Route { Source2 = 0, Source0 = 1, Source1 = 2, Source6 = 3, Raw0B = 4 }
impl Default for RxConfig {
    fn default() -> Self {
        RxConfig { frequency_hz: 154_785_000, bandwidth_hz: 31_250, gain_step: 13, boost: 0, path: Path::Auto,
            route: Route::Source2, input_code: 17, rate_hz: 576_000 }
    }
}
impl RxConfig {
    pub fn sample_rate(&self) -> Option<u32> {
        match self.route {
            Route::Source2 | Route::Raw0B => rate_for_bandwidth(self.bandwidth_hz),
            Route::Source1 => Some(input_rate(self.input_code)),
            _ => Some(self.rate_hz),
        }
    }
    fn raw(&self) -> sys::lr2021_iq_rx {
        sys::lr2021_iq_rx { frequency_hz: self.frequency_hz, bandwidth_hz: self.bandwidth_hz,
            gain_step: self.gain_step, boost: self.boost, path: self.path as u8,
            route: self.route as u8, input_code: self.input_code, rate_hz: self.rate_hz }
    }
    pub fn validate(&self) -> Result<()> { check(unsafe { sys::lr2021_iq_rx_check(&self.raw()) }) }
}

/// One RF switch entry: `modes` bits standby, RX LF, TX LF, RX HF, TX HF.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct RfSwitch { pub dio: u8, pub modes: u8, pub pull: u8 }
/// Board clock and RF switch settings applied after reset.
#[derive(Clone, Debug, Default, PartialEq, Eq)]
pub struct Board { pub tcxo: Option<(u8, u32)>, pub rf_switch: Vec<RfSwitch> }
impl Board {
    fn raw(&self) -> Result<sys::lr2021_iq_board> {
        if self.rf_switch.len() > 6 { return Err(Error(sys::LR2021_IQ_ARGUMENT)); }
        let mut b = sys::lr2021_iq_board::default();
        if let Some((v, t)) = self.tcxo { b.tcxo = 1; b.tcxo_voltage = v; b.tcxo_ticks = t; }
        for (i, r) in self.rf_switch.iter().enumerate() {
            b.rfsw[i] = sys::lr2021_iq_rfsw { dio: r.dio, modes: r.modes, pull: r.pull };
        }
        b.rfsw_count = self.rf_switch.len() as u8;
        Ok(b)
    }
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Pa { Lf = 0, Hf = 1, Auto = 2 }
/// One transmit burst of signed I8,Q8 pairs.
#[derive(Clone, Copy, Debug)]
pub struct TxBurst<'a> {
    pub samples: &'a [[i8; 2]],
    pub repeats: u32,
    pub rate_hz: u32,
    pub frequency_hz: u32,
    /// SetTxParams units, 0.5 dB; -18 is -9 dBm, the tested setting.
    pub power_half_db: i32,
    /// Device time to start at, 0 for immediately.
    pub start_us: u64,
    pub pa: Pa,
    /// Keep only hardware limits; see `untested`.
    pub checked: bool,
}
impl<'a> TxBurst<'a> {
    pub fn new(samples: &'a [[i8; 2]], rate_hz: u32, frequency_hz: u32) -> Self {
        TxBurst { samples, repeats: 1, rate_hz, frequency_hz, power_half_db: -18, start_us: 0, pa: Pa::Auto, checked: false }
    }
    fn raw(&self) -> sys::lr2021_iq_tx {
        sys::lr2021_iq_tx { samples: self.samples.as_ptr() as *const i8, pairs: self.samples.len(),
            repeats: self.repeats, rate_hz: self.rate_hz, frequency_hz: self.frequency_hz,
            power_half_db: self.power_half_db, start_us: self.start_us, pa: self.pa as u8,
            flags: if self.checked { sys::LR2021_IQ_TX_CHECKED } else { 0 } }
    }
    /// Validates against hardware limits, or the measured contract when checked.
    pub fn check(&self) -> Result<()> { check(unsafe { sys::lr2021_iq_tx_check(&self.raw()) }) }
    /// Bits of the tested contract this burst leaves (`LR2021_IQ_TX_UNTESTED_*`).
    pub fn untested(&self) -> u32 { unsafe { sys::lr2021_iq_tx_untested(&self.raw()) } }
}

pub use sys::lr2021_iq_result as StreamReport;
pub use sys::lr2021_iq_tx_result as TxReport;
pub use sys::lr2021_iq_gain_state as GainState;

/// Simulated LR2021 with the library's device adapter, in virtual time.
pub struct Simulator { s: *mut sys::lr2021_rs_simdev }
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Pattern { Counter, Tone(u32) }

struct Ctx<'s, 'g> {
    sink: &'s mut dyn FnMut(&[i16], u64) -> bool,
    gain: Option<&'g mut dyn FnMut() -> Option<(u8, u8)>>,
    panicked: bool,
}
unsafe extern "C" fn sink_tramp(c: *mut c_void, p: *const u8, n: usize, first: u64) -> bool {
    let ctx = &mut *(c as *mut Ctx<'_, '_>);
    if ctx.panicked { return false; }
    let mut buf = vec![0i16; n / 2];
    let bytes = std::slice::from_raw_parts(p, n);
    for (i, v) in buf.iter_mut().enumerate() { *v = i16::from_le_bytes([bytes[2 * i], bytes[2 * i + 1]]); }
    match std::panic::catch_unwind(std::panic::AssertUnwindSafe(|| (ctx.sink)(&buf, first))) {
        Ok(more) => more,
        Err(_) => { ctx.panicked = true; false }
    }
}
unsafe extern "C" fn gain_tramp(c: *mut c_void, step: *mut u8, boost: *mut u8) -> bool {
    let ctx = &mut *(c as *mut Ctx<'_, '_>);
    let Some(g) = ctx.gain.as_mut() else { return false };
    match std::panic::catch_unwind(std::panic::AssertUnwindSafe(|| g())) {
        Ok(Some((s, b))) => { *step = s; *boost = b; true }
        _ => false,
    }
}

impl Simulator {
    pub fn new(pattern: Pattern, board: Option<&Board>, rx: &RxConfig) -> Result<Self> {
        let (p, tone) = match pattern { Pattern::Counter => (sys::LR2021_SIM_COUNTER, 0), Pattern::Tone(hz) => (sys::LR2021_SIM_TONE, hz) };
        let s = unsafe { sys::lr2021_rs_simdev_new(p, tone) };
        if s.is_null() { return Err(Error(sys::LR2021_IQ_IO)); }
        let sim = Simulator { s };
        let b = board.map(|b| b.raw()).transpose()?;
        check(unsafe { sys::lr2021_rs_simdev_open(s, b.as_ref().map_or(core::ptr::null(), |b| b), &rx.raw()) })?;
        Ok(sim)
    }
    fn dev(&mut self) -> *mut sys::lr2021_iq_dev { unsafe { sys::lr2021_rs_simdev_dev(self.s) } }
    fn hal(&mut self) -> *const sys::lr2021_iq_hal { unsafe { sys::lr2021_rs_dev_hal(self.dev()) } }
    /// Applies a receive configuration; returns GetErrors (IMG_CAL is not fatal).
    pub fn configure(&mut self, rx: &RxConfig) -> Result<u16> {
        check(unsafe { sys::lr2021_iq_dev_configure(self.dev(), &rx.raw()) })?;
        Ok(unsafe { sys::lr2021_rs_dev_errors(self.dev()) })
    }
    /// Runs one checked receive session. `sink` gets interleaved I,Q values and
    /// the index of the first pair; return false to stop. `gain` is polled
    /// between FIFO reads and may return a new (step, boost).
    pub fn stream(&mut self, duration_ms: u32, fifo_capacity: u32, sink: &mut dyn FnMut(&[i16], u64) -> bool,
                  gain: Option<&mut dyn FnMut() -> Option<(u8, u8)>>) -> Result<StreamReport> {
        let mut ctx = Ctx { sink, gain, panicked: false };
        let cp = &mut ctx as *mut Ctx<'_, '_> as *mut c_void;
        let mut session = sys::lr2021_iq_session { duration_ms, fifo_capacity, gain: Some(gain_tramp), gain_context: cp, start_us: 0,
            max_read: 0, check_bytes: 0, buffer: core::ptr::null_mut(), buffer_size: 0, level_every: 0,
            gain_read_every: 0, chip_gain: None };
        let mut r = StreamReport::default();
        let e = unsafe { sys::lr2021_iq_stream_session(self.hal(), &mut session, Some(sink_tramp), cp, &mut r) };
        if ctx.panicked { panic!("lr2021: stream callback panicked"); }
        check(e).map(|_| r)
    }
    pub fn read_gain(&mut self) -> Result<GainState> {
        let mut g = GainState::default();
        check(unsafe { sys::lr2021_iq_read_gain(self.hal(), &mut g) }).map(|_| g)
    }
    pub fn transmit(&mut self, burst: &TxBurst, board: Option<&Board>) -> Result<TxReport> {
        let b = board.map(|b| b.raw()).transpose()?;
        let mut r = TxReport::default();
        let e = unsafe { sys::lr2021_iq_transmit(self.hal(), b.as_ref().map_or(core::ptr::null(), |b| b), &burst.raw(), &mut r) };
        check(e).map(|_| r)
    }
    /// Records up to `bytes` of what the TX path consumes from its FIFO.
    pub fn capture_tx(&mut self, bytes: usize) -> Result<()> {
        check(unsafe { sys::lr2021_rs_simdev_capture(self.s, bytes) })
    }
    pub fn captured_tx(&self) -> &[u8] {
        let mut n = 0usize;
        let p = unsafe { sys::lr2021_rs_simdev_captured(self.s, &mut n) };
        if p.is_null() { &[] } else { unsafe { std::slice::from_raw_parts(p, n) } }
    }
    /// Injects a FIFO overflow after this many more produced pairs (0 clears).
    pub fn overflow_after(&mut self, pairs: u64) { unsafe { sys::lr2021_rs_simdev_overflow_after(self.s, pairs) } }
    pub fn time_us(&mut self) -> u64 { unsafe { sys::lr2021_rs_simdev_time_us(self.s) } }
}
impl Drop for Simulator { fn drop(&mut self) { unsafe { sys::lr2021_rs_simdev_free(self.s) } } }

#[cfg(unix)]
pub use host::*;
#[cfg(unix)]
mod host {
    use super::*;
    use std::ffi::CString;
    use sys::host as h;
    pub use sys::host::lr2021_host_rx_meta as RxMeta;
    pub use sys::lr2021_bridge_rx_end as RxEnd;
    pub use sys::lr2021_bridge_tx_end as TxEnd;
    pub use sys::lr2021_bridge_gain as ChipGain;

    #[derive(Clone, Debug)]
    pub struct Hello { pub version: u16, pub caps: u16, pub rom: (u8, u8), pub clock_hz: u32, pub tx_capacity: u32, pub name: String, pub id: String }
    /// The C host client: serial, TCP, local spidev or simulator backend.
    pub struct Host { h: *mut h::lr2021_host }
    // The C client serializes requests internally; one reader at a time.
    unsafe impl Send for Host {}
    unsafe impl Sync for Host {}
    fn hcheck(e: c_int) -> Result<()> { if e == 0 { Ok(()) } else { Err(Error(e)) } }
    impl Host {
        /// Opens a device from key/value arguments, as in a SoapySDR device
        /// string: `serial`, `baud`, `host`, `port`, `spidev`, `gpiochip`,
        /// `reset`, `busy`, `tcxo`, `rfsw`, `sim`, `format`, ...
        pub fn open(args: &[(&str, &str)]) -> std::result::Result<Host, String> {
            let a = unsafe { h::lr2021_rs_args_new() };
            if a.is_null() { return Err("out of memory".into()); }
            for (k, v) in args {
                let (ck, cv) = (CString::new(*k).map_err(|e| e.to_string())?, CString::new(*v).map_err(|e| e.to_string())?);
                if unsafe { h::lr2021_host_args_set(a, ck.as_ptr(), cv.as_ptr()) } < 0 {
                    unsafe { h::lr2021_rs_args_free(a) };
                    return Err(format!("bad value for {k}"));
                }
            }
            let mut err = [0 as core::ffi::c_char; 256];
            let p = unsafe { h::lr2021_host_open(a, err.as_mut_ptr(), err.len()) };
            unsafe { h::lr2021_rs_args_free(a) };
            if p.is_null() { return Err(unsafe { CStr::from_ptr(err.as_ptr()) }.to_string_lossy().into_owned()); }
            Ok(Host { h: p })
        }
        pub fn hello(&self) -> Hello {
            let m = unsafe { &*h::lr2021_host_hello(self.h) };
            let s = |a: &[core::ffi::c_char; 32]| unsafe { CStr::from_ptr(a.as_ptr()) }.to_string_lossy().into_owned();
            Hello { version: m.version, caps: m.caps, rom: (m.rom_major, m.rom_minor), clock_hz: m.clock_hz,
                tx_capacity: m.tx_capacity, name: s(&m.name), id: s(&m.id) }
        }
        /// Configures receive; returns (radio errors, nominal rate).
        pub fn set_rx(&self, rx: &RxConfig) -> Result<(u16, u32)> {
            let m = sys::lr2021_bridge_rx { frequency_hz: rx.frequency_hz, bandwidth_hz: rx.bandwidth_hz,
                gain_step: rx.gain_step, boost: rx.boost, path: rx.path as u8,
                route: rx.route as u8, input_code: rx.input_code, rate_hz: rx.rate_hz };
            let mut ack = sys::lr2021_bridge_rx_ack::default();
            hcheck(unsafe { h::lr2021_host_set_rx(self.h, &m, &mut ack) })?;
            Ok((ack.radio_errors, ack.rate_hz))
        }
        pub fn set_gain(&self, step: u8, boost: u8) -> Result<()> { hcheck(unsafe { h::lr2021_host_set_gain(self.h, step as u32, boost as u32) }) }
        pub fn rx_start(&self, duration_ms: u32) -> Result<()> { hcheck(unsafe { h::lr2021_host_rx_start(self.h, duration_ms) }) }
        /// Reads interleaved I,Q into `iq` (two values per pair). Ok((0, _)) is a timeout.
        pub fn read(&self, iq: &mut [i16], timeout_us: u32) -> Result<(usize, RxMeta)> {
            let mut m = RxMeta::default();
            let n = unsafe { h::lr2021_host_rx_read(self.h, iq.as_mut_ptr(), iq.len() / 2, timeout_us, &mut m) };
            if n < 0 { Err(Error(n as i32)) } else { Ok((n as usize, m)) }
        }
        pub fn rx_stop(&self) -> Result<RxEnd> {
            let mut e = RxEnd::default();
            hcheck(unsafe { h::lr2021_host_rx_stop(self.h, &mut e) }).map(|_| e)
        }
        pub fn rx_active(&self) -> bool { unsafe { h::lr2021_host_rx_active(self.h) } }
        pub fn transmit(&self, burst: &TxBurst) -> Result<TxEnd> {
            let p = sys::lr2021_bridge_tx_begin { bytes: 0, crc32: 0, rate_hz: burst.rate_hz, frequency_hz: burst.frequency_hz,
                power_half_db: burst.power_half_db as i8, repeats: burst.repeats as u8, start_time: burst.start_us,
                pa: burst.pa as u8, flags: if burst.checked { sys::LR2021_IQ_TX_CHECKED } else { 0 } };
            let mut e = TxEnd::default();
            hcheck(unsafe { h::lr2021_host_tx(self.h, burst.samples.as_ptr() as *const i8, burst.samples.len(), &p, &mut e) }).map(|_| e)
        }
        pub fn time_us(&self) -> Result<u64> {
            let mut t = 0u64;
            hcheck(unsafe { h::lr2021_host_time(self.h, &mut t) }).map(|_| t)
        }
        pub fn gain(&self) -> Result<ChipGain> {
            let mut g = ChipGain::default();
            hcheck(unsafe { h::lr2021_host_get_gain(self.h, &mut g) }).map(|_| g)
        }
    }
    impl Drop for Host { fn drop(&mut self) { unsafe { h::lr2021_host_close(self.h) } } }
}

#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn rates_and_limits() {
        assert_eq!(rate_for_bandwidth(31_250), Some(125_000));
        assert_eq!(rate_for_bandwidth(62_500), Some(250_000));
        assert_eq!(rate_for_bandwidth(125_000), Some(500_000));
        assert_eq!(rate_for_bandwidth(250_000), Some(1_000_000));
        assert_eq!(rate_for_bandwidth(100_000), None);
        assert_eq!(RxConfig { route: Route::Source0, ..Default::default() }.sample_rate(), Some(576_000));
        assert_eq!(RxConfig { route: Route::Source1, input_code: 33, ..Default::default() }.sample_rate(), Some(800_000));
        assert_eq!(bandwidth_for_rate(250_000), Some(62_500));
        assert!(RxConfig { frequency_hz: 99_000_000, ..Default::default() }.validate().is_err());
        assert!(RxConfig { frequency_hz: 2_740_000_000, ..Default::default() }.validate().is_ok());
        let iq = vec![[30i8, 30]; 1000];
        let mut b = TxBurst::new(&iq, 19_200, 433_920_000);
        assert!(b.check().is_ok());
        b.rate_hz = 20_000;
        assert!(b.check().is_ok());
        assert_eq!(b.untested(), 1);
        b.checked = true;
        assert_eq!(b.check(), Err(Error(sys::LR2021_IQ_ARGUMENT)));
    }
    #[test]
    fn simulator_gain_readback() {
        let mut rx = RxConfig { bandwidth_hz: 62_500, gain_step: 7, ..Default::default() };
        let mut sim = Simulator::new(Pattern::Counter, None, &rx).unwrap();
        assert_eq!(sim.read_gain(), Err(Error(sys::LR2021_IQ_STATE)));
        rx.gain_step = 0;
        sim.configure(&rx).unwrap();
        assert_eq!(sim.read_gain(), Err(Error(sys::LR2021_IQ_STATE)));
    }
    #[test]
    fn simulator_session_gain_and_tx() {
        let mut sim = Simulator::new(Pattern::Counter, None, &RxConfig { bandwidth_hz: 62_500, ..Default::default() }).unwrap();
        let (mut pairs, mut last, mut gaps, mut blocks) = (0u64, 0u32, 0u32, 0u32);
        let mut sink = |iq: &[i16], first: u64| {
            assert_eq!(first, pairs);
            for p in iq.chunks(2) {
                let v = (p[0] as u16 as u32) | ((p[1] as u16 as u32) << 16);
                if pairs > 0 && v != last.wrapping_add(1) { gaps += 1; }
                last = v; pairs += 1;
            }
            blocks += 1;
            blocks < 100
        };
        let mut asked = false;
        let mut gain = || if asked { None } else { asked = true; Some((12u8, 0u8)) };
        let r = sim.stream(0, 1024, &mut sink, Some(&mut gain)).unwrap();
        assert_eq!(r.nominal_rate_hz, 250_000);
        assert_eq!(r.gain_changes, 1);
        assert_eq!(gaps, 0);
        assert!(r.validated_bytes / 4 >= 99 * 256);

        let iq: Vec<[i8; 2]> = (0..3000).map(|i| [(i % 70 - 35) as i8, (35 - i % 70) as i8]).collect();
        sim.capture_tx(6000).unwrap();
        let t = sim.transmit(&TxBurst::new(&iq, 125_000, 915_000_000), None).unwrap();
        assert_eq!(t.queued_bytes, 6000);
        let flat: Vec<u8> = iq.iter().flat_map(|p| [p[0] as u8, p[1] as u8]).collect();
        assert_eq!(sim.captured_tx(), &flat[..]);

        sim.overflow_after(10_000);
        let mut count = |_: &[i16], _: u64| true;
        assert_eq!(sim.stream(0, 1024, &mut count, None).unwrap_err(), Error(sys::LR2021_IQ_DATA));
    }
    #[cfg(unix)]
    #[test]
    fn host_over_in_process_simulator() {
        let h = Host::open(&[("sim", "counter")]).unwrap();
        assert_eq!(h.hello().rom, (1, 24));
        assert_eq!(h.set_rx(&RxConfig::default()).unwrap().1, 125_000);
        h.rx_start(0).unwrap();
        let mut iq = vec![0i16; 8192];
        let mut got = 0;
        while got < 50_000 { got += h.read(&mut iq, 200_000).unwrap().0; }
        let g = h.gain().unwrap();
        assert_eq!((g.step, g.manual, g.gain_value), (13, 13, 138));
        let end = h.rx_stop().unwrap();
        assert_eq!(end.capture_error, 0);
        assert!(end.pairs as usize >= got);
        assert!(Host::open(&[("port", "x")]).is_err());
    }
}
