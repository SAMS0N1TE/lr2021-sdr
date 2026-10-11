# ESP32-P4 capture firmware

Receive-only USB console for the public C capture library. No LakeShark build
or board adapter is required for the default hardware.

## Hardware

- LilyGO T-Display-P4 with LR2021 module, ROM 1.24.
- ESP32-P4 revisions 0.1 through 1.x, 16 MB flash. The supplied build does not
  support P4 revision 3.x; rebuild for that revision in ESP-IDF menuconfig.
- External antenna on MMCX1; use an antenna for the frequency being received.
- USB data connection to the LilyGO USB-to-UART console (CH343), 115200 baud.

Default wiring: SCLK 2, MOSI 3, MISO 4, CS 24, BUSY 6. XL9535 at I2C address
0x20 uses SDA 7 / SCL 8. Radio reset is IO16 (bit 14); antenna select is IO1.
The adapter selects external MMCX1, LDO regulation and a 3.3 V TCXO, and drives
DIO8 for LF reception. It does not load a PRAM patch or transmit.

Other P4 boards need `idf.py menuconfig` under **LR2021 capture board**.
Check reset wiring, oscillator supply and RF switch routing before connecting
a different module. Direct GPIO reset is supported. Other boards are untested.
Native USB Serial/JTAG is also supported when selected as the console in
ESP-IDF menuconfig; the prebuilt image uses UART0 through the USB bridge.

## Flash a prebuilt image

Download the ESP32-P4 firmware ZIP from
[Releases](https://github.com/SAMS0N1TE/lr2021-sdr/releases), extract it, and
run these commands inside the extracted directory. Replace `COM3` with the
P4's serial port (`/dev/ttyACM0` on Linux).

```sh
python -m pip install esptool pyserial
python -m esptool --chip esp32p4 --port COM3 write_flash 0x2000 bootloader.bin 0x8000 partition-table.bin 0x10000 lr2021_capture.bin
python -m serial.tools.miniterm COM3 115200 --raw
```

Flashing replaces the installed firmware and partition table. Back up any
firmware and data you need first. The screen and audio are unused; use the
USB console. Exit miniterm with Ctrl+]. Close other serial monitors before flashing.
If connection fails, hold BOOT, press RESET, release BOOT and retry.
SHA256SUMS lists the checksums of the included binaries.

## Quick test

At the `lriq>` prompt:

```text
info
tune 154785000
check 3000
capture 100
dump
```

`info` must report ROM 1.24, configured=1 and errors=0000. `check` runs one
bounded receive session, checks FIFO loss flags and discards the samples.
Success prints `result=PASS`, zero capture/recovery errors and rejected=0.
It tests capture service, not RF sample-perfect continuity or P25 decoding.

`capture` retains up to 100 ms of IQ16LE in internal RAM. `dump` exports it
after reception stops, with sequential byte offsets and CRC32. Save the dump
as `capture.log` using your terminal's logging function, then use the host tool:

```sh
lriq_tool unpack-fifo capture.log capture.iq16
```

The converter checks offsets, CRC, FIFO flags and recovery before creating the
output. It refuses existing output files. Samples are signed little-endian
int16 I then Q, nominally 125000 complex pairs/s. Amplitude is uncalibrated.
Use `dump` immediately after `capture`; `check` or `tune` invalidates the saved
recording. A failed capture cannot be dumped.

| Command | Argument | Default |
| --- | --- | --- |
| info | none | Radio version and errors |
| tune | Frequency in Hz, 150000000..960000000 | 154785000 at boot |
| check | 1..3000 ms | 3000 |
| capture | 1..100 ms | 100 |
| dump | none | Last successful recording |

Separate captures have gaps. This firmware does not provide live USB sample
streaming, P25 decoding, audio or the LakeShark display. The three-second check
limit keeps the synchronous capture below the watchdog deadline.

## Board check

The application was flashed and tested on a T-Display-P4, P4 revision 1.0,
with LR2021 ROM 1.24. The existing board bootloader and partition table were
retained for this test.

- Retuned between 915 MHz and 154.785 MHz; radio errors remained zero.
- Three-second check accepted 1500020 bytes, peak FIFO 132 bytes, largest
  service gap 260 us, zero loss flags and successful recovery.
- A 100 ms recording exported 50016 bytes. The C host converter verified
  offsets, CRC32 and recovery, producing 12504 IQ16 pairs.
- Rejected an out-of-range check duration without starting a capture.

This validates the console capture path. Other wiring and clean-install
bootloader behavior have not been tested on hardware.

## Build

Use ESP-IDF 5.5.4 in an activated ESP-IDF shell:

```sh
cd firmware/esp32p4
idf.py set-target esp32p4
idf.py build
idf.py -p COM3 flash monitor
```

To package the default build from the repository root:

```sh
python tools/package_firmware.py
```

The ZIP includes the application, bootloader, partition table, instructions
and checksums. Source is GPL-3.0; the ESP-IDF dependency retains its own licenses.
