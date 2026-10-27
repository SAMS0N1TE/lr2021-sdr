# Developer guide

[Guides](README.md) / Development

The portable C11 receive library and host tools live in this repository. LakeShark supplies the handheld integration, with experimental transmit support distributed as a separate integration.

## Build the library and tools

Requires CMake 3.18+ and a C11 compiler. Windows builds use MinGW-w64. Run from the repository root:

```sh
cmake -S . -B build
cmake --build build
ctest --test-dir build --output-on-failure
cmake --install build --prefix install
```

Link `lr2021_iq` for radio capture and `lriq` for parsing and measurement. Both use caller-owned storage. Start with [capture.c](../examples/capture.c), then implement the [board hooks](porting.md).

## Capture and inspect

```sh
lriq_tool capture COM3 capture 10
lriq_tool analyze capture-0000.bin
lriq_tool unpack-fifo capture.log capture.iq16
```

`capture` requires Windows and firmware implementing `lora lrcap`. Replace `COM3` with the board's port. It opens at 115200 baud without resetting the board; a count of `0` runs until Ctrl+C.

`analyze` reads one 1024-byte SRAM window. `unpack-fifo` validates a compact console transaction and exports interleaved IQ16. See the [console reference](console.md) for the formats.

## Choose an integration

- [SoapySDR module, host client and Rust crates](host-sdr.md)
- [rtl_tcp server](rtl-tcp.md)
- [Bridge protocol](bridge-protocol.md)
- [Generic SPI device, sessions and TX burst API](../include/lr2021_iq_dev.h)
- [Standalone ESP32-P4 USB firmware](../firmware/esp32p4/README.md)
- [Continuous receive capture](continuous.md)
- [LakeShark TX integration and upload commands](tx-playback.md)
- [Technical overview and measured capabilities](technical-overview.md)
- [RX protocol and sample layout](protocol.md)

Next: [Research](research.md) · [Hardware](hardware.md)
