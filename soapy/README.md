# SoapySDR module

`lr2021Support` registers the SoapySDR driver `lr2021`. It is a thin C++ layer
over the C host client in [`include/lr2021_host.h`](../include/lr2021_host.h);
the chip and protocol logic is in the C library. Built from the top-level CMake
project when SoapySDR is found (Linux and macOS).

```bash
cmake -S .. -B ../build && cmake --build ../build --parallel
```

```bash
SOAPY_SDR_PLUGIN_PATH=../build/soapy SoapySDRUtil --probe="driver=lr2021,sim=tone"
```

Device arguments, rates, gain, timestamps and the TX contract are described in
[Use the LR2021 as an SDR](../docs/host-sdr.md). In short:

| | RX | TX |
| --- | --- | --- |
| Frequency | 100..2740 MHz | 100..2740 MHz |
| Rates | source 2: 125000, 250000, 500000, 1000000; `route=source0/1/6`: 1000..4000000 | 1..1000000 (measured: 4800 ... 125000) |
| Formats | CS16 (native), CF32 | CS8 (native, full scale 56), CS16, CF32 |
| Gain | STEP 1..13, BOOST 0..7, AGC | PA, dBm |
| Antennas | AUTO, LF, HF | AUTO, LF, HF |
| Time | `HAS_TIME` on every read | `HAS_TIME` schedules a burst |

`tx_checked=true` restricts TX to the measured contract. `test_soapy.cpp`
exercises the module against the in-process simulator and runs as the
`soapy_module` CTest; on a LilyGO T-Display-P4 board over Wi-Fi the module
streamed 250000 samples/s for 10 minutes with every sample delivered, and its
writeStream bursts, including timed ones, were received by a HackRF.
