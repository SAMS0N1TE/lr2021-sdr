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
| Frequency | 100..2740 MHz | 150..960 MHz (unchecked: 100..2740) |
| Rates | 125000, 250000 | 4800 ... 125000 (unchecked: 1..1000000) |
| Formats | CS16 (native), CF32 | CS8 (native, full scale 56), CS16, CF32 |
| Gain | STEP 1..13, BOOST 0..7, AGC | PA, dBm |
| Antennas | AUTO, LF, HF | AUTO, LF, HF |
| Time | `HAS_TIME` on every read | `HAS_TIME` schedules a burst |

`test_soapy.cpp` exercises the module against the in-process simulator and
runs as the `soapy_module` CTest. The module has not yet been run against a
board.
