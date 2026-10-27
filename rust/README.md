# Rust crates

| Crate | What it is |
| --- | --- |
| `lr2021-sys` | Raw bindings to the C library in this repository, compiled with `cc`: radio HAL types, the generic SPI device, receive sessions, the 0x0293 TX burst, the simulator, the bridge codec and, on Unix, the host client. A test checks every mirrored struct against the C compiler. |
| `lr2021` | Safe API. `Simulator` runs the library's device code against the LR2021 model (sessions with live gain, TX with capture, fault injection). `Host` opens the same devices as the SoapySDR module (`serial`, `host`, `spidev`, `sim` arguments). `RxConfig` selects source 2 or the raw routes. `TxBurst::check` applies the hardware limits, or the measured contract with `checked`. |
| `lr2021-bridge` | The [bridge protocol](../docs/bridge-protocol.md) in pure Rust with no dependencies: frames, parser, records, CS16/CS12/IQ8/IQ6G and a blocking `Client` over TCP or any byte stream. |

```bash
cargo test --workspace
```

`lr2021-bridge/tests/cross.rs` cross-checks the Rust codec against the C one
(frames with noise and corruption, every record both ways, CS12 packing) and
runs the Rust client against the C bridge server and simulator over TCP.

```rust
let c = lr2021_bridge::Client::connect_tcp(("192.168.4.1", lr2021_bridge::TCP_PORT))?;
c.set_rx(&lr2021_bridge::RxConfig { frequency_hz: 433_920_000, bandwidth_hz: 31_250, gain_step: 13, boost: 0, path: 2 })?;
c.rx_start(0, lr2021_bridge::CS16)?;
while let Some(block) = c.read(std::time::Duration::from_secs(1))? {
    // block.iq: interleaved I, Q; block.header.first_pair jumps on any loss
}
```

For a serial port, configure the tty raw at the bridge baud rate first (for
example with a serial crate or `stty`), enter bridge mode with the console
command `bridge <baud>`, then pass the port to `Client::new`. The C host client
used by `lr2021::Host` does all of that itself.

`lr2021-sys` builds from the repository's `src/` directory; set
`LR2021_SDR_SRC` to build against another checkout. All crates are MIT licensed.
