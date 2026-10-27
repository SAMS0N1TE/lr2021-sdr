// SPDX-License-Identifier: MIT
//! Raw FFI bindings to the LR2021-SDR C library: the radio HAL types, the
//! generic SPI device, receive sessions, the 0x0293 TX burst, the simulator,
//! the bridge protocol codec and, on Unix, the host client
//! (`include/lr2021_host.h`). Use the `lr2021` crate for a safe API.
//!
//! Small structs mirror the C headers with `#[repr(C)]`; a test checks every
//! size against the C compiler. Large caller-allocated structs (device,
//! simulator, parser, host arguments) are opaque and allocated by shims.
#![allow(non_camel_case_types)]
use core::ffi::{c_char, c_int, c_uint, c_void};

pub const LR2021_IQ_OK: c_int = 0;
pub const LR2021_IQ_ARGUMENT: c_int = -1;
pub const LR2021_IQ_STATE: c_int = -2;
pub const LR2021_IQ_TIMEOUT: c_int = -3;
pub const LR2021_IQ_DATA: c_int = -4;
pub const LR2021_IQ_IO: c_int = -5;
pub const LR2021_IQ_PATH_LF: u8 = 0;
pub const LR2021_IQ_PATH_HF: u8 = 1;
pub const LR2021_IQ_PATH_AUTO: u8 = 2;
pub const LR2021_IQ_MIN_HZ: u32 = 100_000_000;
pub const LR2021_IQ_MAX_HZ: u32 = 2_740_000_000;
pub const LR2021_IQ_TX_MAX_SAMPLES: u32 = 32767;
pub const LR2021_IQ_TX_CHECKED: u8 = 1;
pub const LR2021_IQ_TX_SIMO: u8 = 2;
pub const LR2021_SIM_COUNTER: c_int = 0;
pub const LR2021_SIM_TONE: c_int = 1;
pub const LR2021_BRIDGE_VERSION: u8 = 1;
pub const LR2021_BRIDGE_HEADER: usize = 8;
pub const LR2021_BRIDGE_MAX_PAYLOAD: usize = 2048;
pub const LR2021_BRIDGE_MAX_FRAME: usize = LR2021_BRIDGE_HEADER + LR2021_BRIDGE_MAX_PAYLOAD + 4;
pub const LR2021_BRIDGE_RX_DATA_HEADER: usize = 32;
pub const LR2021_BRIDGE_RX_MAX_PAIRS: usize = 960;
pub const LR2021_BRIDGE_RX_MAX_BYTES: usize = 1920;

#[repr(C)]
#[derive(Clone, Copy, Debug, Default)]
pub struct lr2021_iq_profile {
    pub lr2021: bool, pub configured: bool, pub exclusive_lora: bool, pub receiving: bool,
    pub fw_major: c_uint, pub fw_minor: c_uint, pub spreading_factor: c_uint,
    pub frequency_hz: u32, pub bandwidth_hz: u32,
    pub rx_path: u8, pub gain_step: u8, pub rx_boost: u8,
    pub route: u8, pub input_code: u8, pub route_rate_hz: u32,
}
pub type lr2021_iq_sink = Option<unsafe extern "C" fn(*mut c_void, *const u8, usize, u64) -> bool>;
#[repr(C)]
#[derive(Clone, Copy)]
pub struct lr2021_iq_hal {
    pub context: *mut c_void,
    pub lock: Option<unsafe extern "C" fn(*mut c_void)>,
    pub unlock: Option<unsafe extern "C" fn(*mut c_void)>,
    pub snapshot: Option<unsafe extern "C" fn(*mut c_void, *mut lr2021_iq_profile) -> c_int>,
    pub read32: Option<unsafe extern "C" fn(*mut c_void, u32, *mut u32) -> c_int>,
    pub write32: Option<unsafe extern "C" fn(*mut c_void, u32, u32) -> c_int>,
    pub write: Option<unsafe extern "C" fn(*mut c_void, u16, *const u8, usize) -> c_int>,
    pub read: Option<unsafe extern "C" fn(*mut c_void, u16, *mut u8, usize) -> c_int>,
    pub standby: Option<unsafe extern "C" fn(*mut c_void) -> c_int>,
    pub receive: Option<unsafe extern "C" fn(*mut c_void) -> c_int>,
    pub reset: Option<unsafe extern "C" fn(*mut c_void) -> c_int>,
    pub restore: Option<unsafe extern "C" fn(*mut c_void, *const lr2021_iq_profile) -> c_int>,
    pub fast: Option<unsafe extern "C" fn(*mut c_void, bool) -> c_int>,
    pub clear_fifo: Option<unsafe extern "C" fn(*mut c_void) -> c_int>,
    pub read_fifo: Option<unsafe extern "C" fn(*mut c_void, *mut u8, usize) -> c_int>,
    pub now_us: Option<unsafe extern "C" fn(*mut c_void) -> u64>,
    pub sleep_us: Option<unsafe extern "C" fn(*mut c_void, c_uint)>,
    pub write_fifo: Option<unsafe extern "C" fn(*mut c_void, *const u8, usize) -> c_int>,
}
#[repr(C)]
#[derive(Clone, Copy, Debug, Default)]
pub struct lr2021_iq_result {
    pub bytes: usize,
    pub status: u32, pub nominal_rate_hz: u32, pub max_fifo_level: u32, pub polls: u32,
    pub elapsed_us: u64,
    pub fifo_flags: u8, pub stopped_by_sink: bool,
    pub capture_error: c_int, pub restore_error: c_int, pub recovery_error: c_int,
    pub fifo_capacity: c_uint,
    pub validated_bytes: usize,
    pub max_service_gap_us: u64,
    pub start_us: u64,
    pub gain_changes: c_uint,
}
#[repr(C)]
pub struct lr2021_iq_session {
    pub duration_ms: c_uint, pub fifo_capacity: c_uint,
    pub gain: Option<unsafe extern "C" fn(*mut c_void, *mut u8, *mut u8) -> bool>,
    pub gain_context: *mut c_void,
    pub start_us: u64,
    pub max_read: c_uint, pub check_bytes: c_uint,
    pub buffer: *mut u8, pub buffer_size: usize,
    pub level_every: c_uint,
}
#[repr(C)]
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct lr2021_iq_rx {
    pub frequency_hz: u32, pub bandwidth_hz: u32,
    pub gain_step: u8, pub boost: u8, pub path: u8,
    pub route: u8, pub input_code: u8, pub rate_hz: u32,
}
#[repr(C)]
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct lr2021_iq_rfsw { pub dio: u8, pub modes: u8, pub pull: u8 }
#[repr(C)]
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct lr2021_iq_board {
    pub tcxo: u8, pub tcxo_voltage: u8, pub rfsw_count: u8,
    pub tcxo_ticks: u32,
    pub rfsw: [lr2021_iq_rfsw; 6],
}
#[repr(C)]
#[derive(Clone, Copy, Debug)]
pub struct lr2021_iq_tx {
    pub samples: *const i8, pub pairs: usize, pub repeats: c_uint,
    pub rate_hz: u32, pub frequency_hz: u32, pub power_half_db: c_int,
    pub start_us: u64, pub pa: u8, pub flags: u8,
}
#[repr(C)]
#[derive(Clone, Copy, Debug, Default)]
pub struct lr2021_iq_tx_result {
    pub error: c_int, pub stop_error: c_int, pub recovery_error: c_int,
    pub tx_flags: u8,
    pub final_level: c_uint, pub min_level: c_uint, pub refills: c_uint,
    pub irq: u32, pub untested: u32,
    pub queued_bytes: usize,
    pub started_us: u64, pub elapsed_us: u64, pub max_service_gap_us: u64,
}

#[repr(C)]
#[derive(Clone, Copy, Debug)]
pub struct lr2021_bridge_hello {
    pub status: i32, pub version: u16, pub caps: u16,
    pub rom_major: u8, pub rom_minor: u8, pub max_payload: u16,
    pub clock_hz: u32, pub tx_capacity: u32,
    pub name: [c_char; 32], pub id: [c_char; 32],
}
#[repr(C)]
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct lr2021_bridge_rx {
    pub frequency_hz: u32, pub bandwidth_hz: u32, pub gain_step: u8, pub boost: u8, pub path: u8,
    pub route: u8, pub input_code: u8, pub rate_hz: u32,
}
#[repr(C)]
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct lr2021_bridge_rx_ack { pub status: i32, pub radio_errors: u16, pub rate_hz: u32 }
#[repr(C)]
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct lr2021_bridge_tx_begin {
    pub bytes: u32, pub crc32: u32, pub rate_hz: u32, pub frequency_hz: u32,
    pub power_half_db: i8, pub repeats: u8, pub start_time: u64, pub pa: u8, pub flags: u8,
}
#[repr(C)]
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct lr2021_bridge_rx_data {
    pub generation: u32, pub rate_hz: u32, pub first_pair: u64, pub start_time: u64,
    pub dropped: u32, pub pairs: u16, pub format: u8, pub shift: u8,
}
#[repr(C)]
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct lr2021_bridge_rx_end {
    pub generation: u32, pub capture_error: i32, pub recovery_error: i32,
    pub reason: u8, pub fifo_flags: u8, pub max_fifo_level: u16,
    pub dropped: u32, pub gain_changes: u32,
    pub pairs: u64, pub start_time: u64, pub elapsed_us: u64, pub max_service_gap_us: u64,
}
#[repr(C)]
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct lr2021_bridge_tx_end {
    pub error: i32, pub stop_error: i32, pub recovery_error: i32,
    pub tx_flags: u8, pub final_level: u16, pub min_level: u16,
    pub irq: u32, pub samples: u32,
    pub start_time: u64, pub elapsed_us: u64, pub max_service_gap_us: u64,
    pub untested: u32,
}
#[repr(C)]
#[derive(Clone, Copy, Debug)]
pub struct lr2021_bridge_frame { pub type_: u8, pub sequence: u16, pub length: u16, pub payload: *const u8 }
pub type lr2021_bridge_frame_cb = Option<unsafe extern "C" fn(*mut c_void, *const lr2021_bridge_frame)>;

#[repr(C)]
pub struct lr2021_iq_dev { _private: [u8; 0] }
#[repr(C)]
pub struct lr2021_rs_simdev { _private: [u8; 0] }
#[repr(C)]
pub struct lr2021_bridge_parser { _private: [u8; 0] }

extern "C" {
    pub fn lr2021_iq_rate(bandwidth_hz: u32) -> c_uint;
    pub fn lr2021_iq_bandwidth(rate: c_uint) -> u32;
    pub fn lr2021_iq_input_rate(code: u8) -> c_uint;
    pub fn lr2021_iq_path(hz: u32, path: c_uint) -> c_uint;
    pub fn lr2021_iq_rx_check(rx: *const lr2021_iq_rx) -> c_int;
    pub fn lr2021_iq_strerror(e: c_int) -> *const c_char;
    pub fn lr2021_iq_dev_configure(d: *mut lr2021_iq_dev, rx: *const lr2021_iq_rx) -> c_int;
    pub fn lr2021_iq_stream_session(h: *const lr2021_iq_hal, s: *mut lr2021_iq_session,
        sink: lr2021_iq_sink, ctx: *mut c_void, r: *mut lr2021_iq_result) -> c_int;
    pub fn lr2021_iq_tx_rate_ok(rate: u32) -> bool;
    pub fn lr2021_iq_tx_untested(t: *const lr2021_iq_tx) -> u32;
    pub fn lr2021_iq_tx_check(t: *const lr2021_iq_tx) -> c_int;
    pub fn lr2021_iq_transmit(h: *const lr2021_iq_hal, b: *const lr2021_iq_board,
        t: *const lr2021_iq_tx, r: *mut lr2021_iq_tx_result) -> c_int;

    pub fn lr2021_rs_simdev_new(pattern: c_int, tone_hz: u32) -> *mut lr2021_rs_simdev;
    pub fn lr2021_rs_simdev_free(s: *mut lr2021_rs_simdev);
    pub fn lr2021_rs_simdev_open(s: *mut lr2021_rs_simdev, b: *const lr2021_iq_board, rx: *const lr2021_iq_rx) -> c_int;
    pub fn lr2021_rs_simdev_dev(s: *mut lr2021_rs_simdev) -> *mut lr2021_iq_dev;
    pub fn lr2021_rs_simdev_capture(s: *mut lr2021_rs_simdev, capacity: usize) -> c_int;
    pub fn lr2021_rs_simdev_captured(s: *const lr2021_rs_simdev, n: *mut usize) -> *const u8;
    pub fn lr2021_rs_simdev_time_us(s: *mut lr2021_rs_simdev) -> u64;
    pub fn lr2021_rs_simdev_overflow_after(s: *mut lr2021_rs_simdev, pairs: u64);
    pub fn lr2021_rs_dev_hal(d: *mut lr2021_iq_dev) -> *const lr2021_iq_hal;
    pub fn lr2021_rs_dev_errors(d: *const lr2021_iq_dev) -> u16;
    pub fn lr2021_rs_sizeof(which: c_int) -> usize;

    pub fn lr2021_bridge_crc32(crc: u32, p: *const u8, n: usize) -> u32;
    pub fn lr2021_bridge_encode(out: *mut u8, cap: usize, type_: u8, seq: u16, p: *const u8, n: usize) -> usize;
    pub fn lr2021_rs_parser_new() -> *mut lr2021_bridge_parser;
    pub fn lr2021_rs_parser_free(p: *mut lr2021_bridge_parser);
    pub fn lr2021_rs_parser_crc_errors(p: *const lr2021_bridge_parser) -> u32;
    pub fn lr2021_bridge_parse(p: *mut lr2021_bridge_parser, d: *const u8, n: usize, cb: lr2021_bridge_frame_cb, ctx: *mut c_void);
    pub fn lr2021_bridge_pack_hello(p: *mut u8, m: *const lr2021_bridge_hello) -> usize;
    pub fn lr2021_bridge_unpack_hello(p: *const u8, n: usize, m: *mut lr2021_bridge_hello) -> c_int;
    pub fn lr2021_bridge_pack_rx(p: *mut u8, m: *const lr2021_bridge_rx) -> usize;
    pub fn lr2021_bridge_unpack_rx(p: *const u8, n: usize, m: *mut lr2021_bridge_rx) -> c_int;
    pub fn lr2021_bridge_pack_rx_ack(p: *mut u8, m: *const lr2021_bridge_rx_ack) -> usize;
    pub fn lr2021_bridge_unpack_rx_ack(p: *const u8, n: usize, m: *mut lr2021_bridge_rx_ack) -> c_int;
    pub fn lr2021_bridge_pack_tx_begin(p: *mut u8, m: *const lr2021_bridge_tx_begin) -> usize;
    pub fn lr2021_bridge_unpack_tx_begin(p: *const u8, n: usize, m: *mut lr2021_bridge_tx_begin) -> c_int;
    pub fn lr2021_bridge_pack_rx_data(p: *mut u8, m: *const lr2021_bridge_rx_data) -> usize;
    pub fn lr2021_bridge_unpack_rx_data(p: *const u8, n: usize, m: *mut lr2021_bridge_rx_data) -> c_int;
    pub fn lr2021_bridge_pack_rx_end(p: *mut u8, m: *const lr2021_bridge_rx_end) -> usize;
    pub fn lr2021_bridge_unpack_rx_end(p: *const u8, n: usize, m: *mut lr2021_bridge_rx_end) -> c_int;
    pub fn lr2021_bridge_pack_tx_end(p: *mut u8, m: *const lr2021_bridge_tx_end) -> usize;
    pub fn lr2021_bridge_unpack_tx_end(p: *const u8, n: usize, m: *mut lr2021_bridge_tx_end) -> c_int;
    pub fn lr2021_bridge_sample_bytes(format: c_uint, pairs: usize) -> usize;
    pub fn lr2021_bridge_pack_samples(out: *mut u8, format: c_uint, cs16: *const u8, pairs: usize, shift: *mut u8) -> usize;
    pub fn lr2021_bridge_unpack_samples(out: *mut i16, format: c_uint, shift: c_uint, d: *const u8, bytes: usize, pairs: usize) -> c_int;
    pub fn lr2021_bridge_strerror(e: c_int) -> *const c_char;
}

/// Host client (POSIX). `lr2021_host` and its arguments are opaque.
#[cfg(unix)]
pub mod host {
    use super::*;
    #[repr(C)]
    pub struct lr2021_host { _private: [u8; 0] }
    #[repr(C)]
    pub struct lr2021_host_args { _private: [u8; 0] }
    #[repr(C)]
    #[derive(Clone, Copy, Debug, Default)]
    pub struct lr2021_host_rx_meta {
        pub time_ns: u64, pub first_pair: u64, pub generation: u32, pub rate_hz: u32,
        pub discontinuity: bool, pub ended: bool,
    }
    extern "C" {
        pub fn lr2021_rs_args_new() -> *mut lr2021_host_args;
        pub fn lr2021_rs_args_free(a: *mut lr2021_host_args);
        pub fn lr2021_rs_host_meta_size() -> usize;
        pub fn lr2021_rs_serve_sim(fd: c_int, pattern: c_int) -> c_int;
        pub fn lr2021_host_args_set(a: *mut lr2021_host_args, k: *const c_char, v: *const c_char) -> c_int;
        pub fn lr2021_host_open(a: *const lr2021_host_args, err: *mut c_char, n: usize) -> *mut lr2021_host;
        pub fn lr2021_host_close(h: *mut lr2021_host);
        pub fn lr2021_host_backend(h: *const lr2021_host) -> c_int;
        pub fn lr2021_host_hello(h: *const lr2021_host) -> *const lr2021_bridge_hello;
        pub fn lr2021_host_set_rx(h: *mut lr2021_host, rx: *const lr2021_bridge_rx, ack: *mut lr2021_bridge_rx_ack) -> c_int;
        pub fn lr2021_host_set_gain(h: *mut lr2021_host, step: c_uint, boost: c_uint) -> c_int;
        pub fn lr2021_host_rx_start(h: *mut lr2021_host, duration_ms: c_uint) -> c_int;
        pub fn lr2021_host_rx_read(h: *mut lr2021_host, iq: *mut i16, max: usize, timeout_us: c_uint, m: *mut lr2021_host_rx_meta) -> core::ffi::c_long;
        pub fn lr2021_host_rx_stop(h: *mut lr2021_host, end: *mut lr2021_bridge_rx_end) -> c_int;
        pub fn lr2021_host_rx_active(h: *mut lr2021_host) -> bool;
        pub fn lr2021_host_tx(h: *mut lr2021_host, iq: *const i8, pairs: usize, p: *const lr2021_bridge_tx_begin, end: *mut lr2021_bridge_tx_end) -> c_int;
        pub fn lr2021_host_time(h: *mut lr2021_host, us: *mut u64) -> c_int;
        pub fn lr2021_host_strerror(e: c_int) -> *const c_char;
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use core::mem::size_of;
    #[test]
    fn layouts_match_the_c_compiler() {
        let rust = [size_of::<lr2021_iq_profile>(), size_of::<lr2021_iq_result>(), size_of::<lr2021_iq_session>(),
            size_of::<lr2021_iq_rx>(), size_of::<lr2021_iq_tx>(), size_of::<lr2021_iq_tx_result>(),
            size_of::<lr2021_iq_board>(), size_of::<lr2021_bridge_hello>(), size_of::<lr2021_bridge_rx>(),
            size_of::<lr2021_bridge_rx_ack>(), size_of::<lr2021_bridge_tx_begin>(), size_of::<lr2021_bridge_rx_data>(),
            size_of::<lr2021_bridge_rx_end>(), size_of::<lr2021_bridge_tx_end>(), size_of::<lr2021_bridge_frame>(),
            size_of::<lr2021_iq_hal>()];
        for (i, &n) in rust.iter().enumerate() {
            assert_eq!(n, unsafe { lr2021_rs_sizeof(i as c_int) }, "struct {i}");
        }
        #[cfg(unix)]
        assert_eq!(size_of::<host::lr2021_host_rx_meta>(), unsafe { host::lr2021_rs_host_meta_size() });
    }
    #[test]
    fn crc_matches_ieee() {
        assert_eq!(unsafe { lr2021_bridge_crc32(0, b"123456789".as_ptr(), 9) }, 0xCBF4_3926);
    }
}
