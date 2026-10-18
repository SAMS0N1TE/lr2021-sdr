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

Native USB Serial/JTAG is also supported when selected as the console in
ESP-IDF menuconfig; the prebuilt image uses UART0 through the USB bridge.

## Other boards

`board` shows the pending profile. Kconfig supplies the defaults, which match
LilyGO. Edits apply only after `board save` and a reset or power cycle. NVS is
loaded before radio access; the console stays available if radio setup fails.

Example: direct active-low reset, optional IRQ, 1.8 V TCXO with 2 ms startup,
and one LF RX switch. SPI stays at SCLK/MOSI/MISO/CS 2/3/4/24:

```text
board default
board set reset gpio 5 low
board set busy 6
board set irq gpio 9 dio 9
board set tcxo 1.8
board set tcxo ticks 64000
board rfsw clear
board rfsw 8 0 1 0 0 0
board
board save
```

Reboot after saving. Pin settings use `board set <busy|sclk|mosi|miso|cs> <gpio>`.
Reset accepts `gpio <pin> low`, `gpio <pin> high`, or `xl9535`. XL9535 retains
LilyGO rail and MMCX1 control; its pins use `board set sda <gpio>` and `scl`.
Use `board set irq none` to disable IRQ. IRQ GPIO/DIO are reserved in the boot
profile but have no radio IRQ mapping or handler; capture still polls.

TCXO accepts `off`, or 1.6/1.7/1.8/2.2/2.4/2.7/3.0/3.3 V (codes 0..7).
`board set tcxo ticks <count>` sets an unsigned 32-bit delay in 32 MHz periods.
Stock is code 7, 32768 ticks. Off uses the crystal after reboot.

`board rfsw <dio> <standby> <rx_lf> <tx_lf> <rx_hf> <tx_hf> [pull]` sets
one of six entries. Levels are 0/1, packed as bits 0..4 in that order. There
is no separate TX HP bit. DIO is 5..11. Function is RF switch (2); sleep pull
is 0 none, 1 down, 2 up, or 3 auto. Default pull is 3, except DIO5 requires 2.
Use `board rfsw <dio> remove` or `board rfsw clear` to remove entries. See
[Semtech sections 6.8 and 6.11](https://cdn.robotshop.com/media/S/See/RB-See-1255/pdf/semtech-lr2021-lora-evaluation-kit-eu868-xiao-nrf54l15-lr2021_v1_1_datasheet.pdf)
for the native command fields.

GPIOs are 0..54, excluding USB 26/27 and console UART pins (stock 37/38).
P4 flash, PSRAM and high-speed USB use dedicated pads without GPIO numbers.
Shared GPIOs, duplicate DIOs and IRQ/switch conflicts are refused. Check the
schematic for other attached devices. `board default` stages build defaults;
save and reboot to restore them. Invalid saved profiles leave the radio off;
correct them in the console. NVS errors never erase other data. Other boards
are untested. Capture, receive-only operation and ROM 1.24 requirements stay
the same.

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
This command reports FIFO service and recovery results. For the live P25
receiver and voice results, see [LakeShark integration](../../docs/live-streaming.md).

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
