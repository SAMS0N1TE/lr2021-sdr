# ESP32-P4 bridge and capture firmware

Puts the LR2021 on a computer: the firmware serves the
[bridge protocol](../../docs/bridge-protocol.md) over the USB serial console or
Wi-Fi, so the SoapySDR module and the host library can receive and transmit
([host SDR guide](../../docs/host-sdr.md)). The console also keeps the finite
capture commands. No LakeShark build or board adapter is required for the
default hardware.

## Hardware

- LilyGO T-Display-P4 with LR2021 module, ROM 1.24.
- ESP32-P4 revisions 0.1 through 1.x, 16 MB flash. The supplied build does not
  support P4 revision 3.x; rebuild for that revision in ESP-IDF menuconfig.
- An antenna on MMCX1 (the default route) or the internal antenna, chosen with
  `antenna`. Fit an antenna on MMCX1 before transmitting on it.
- USB data connection to the LilyGO USB-to-UART console (CH343), 115200 baud.

Default wiring: SCLK 2, MOSI 3, MISO 4, CS 24, BUSY 6. XL9535 at I2C address
0x20 uses SDA 7 / SCL 8. Radio reset is IO16 (bit 14); antenna select is IO1.
The adapter uses LDO regulation and a 3.3 V TCXO. The RF switch table drives
DIO8 in LF receive and LF transmit, DIO6 in HF receive, DIO7 in HF transmit
and DIO10 in HF receive and HF transmit. It does not load a PRAM patch, and it
transmits only when a host sends a TX burst.

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
correct them in the console. NVS errors never erase other data. The ROM 1.24 requirement stays the same.

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

For continuous streaming, use the bridge below. P25 decoding, audio and the
display are in LakeShark. The console `check` runs for up to three seconds,
within the watchdog deadline.

## Use it from a computer

`bridge [baud]` switches the console port to the binary bridge protocol until
the host sends BYE; the host client and the SoapySDR module send it themselves.
Over Wi-Fi the board listens on TCP port 51021 as soon as it has joined a
network:

```text
wifi set "<ssid>" "<password>"
wifi
antenna external
```

`wifi` shows the address; `wifi clear` forgets the network. Credentials are kept
in NVS and the board rejoins at boot. Wi-Fi goes through the board's ESP32-C6
(esp_hosted over SDIO). `antenna internal|external` selects the internal antenna
or MMCX1 for receive and transmit and is saved.

| Command | Argument | Default |
| --- | --- | --- |
| bridge | Baud, 115200..5000000 | 4000000 |
| wifi | `set <ssid> <password>`, `clear`, or none for status | Joins the saved network at boot |
| antenna | `internal` or `external` | external (MMCX1) |

The radio worker runs on core 1. The radio SPI bus runs at 26.67 MHz. The FIFO
is read in frames of up to 1,020 bytes: the cache-line multiple of a long read
goes by DMA from aligned buffers and the rest through the ESP32-P4's SPI
registers, under one chip select. Other frames, TX FIFO writes included, use
the registers in 64-byte pieces; a TX FIFO write sent as one continuous DMA
burst lost data. At 2 MB/s (500000 pairs/s CS16 and 1000000 pairs/s IQ8), six
60 s runs with DMA reads peaked at 686-870 FIFO bytes with service gaps of
377-433 us, against 900-956 bytes and 462-481 us on the registers alone.
`spibench` times the bus and `spiclk` sets its clock. A
PSRAM output queue (8 MiB) holds about 8 s of 16-bit samples at 250000
pairs/s, so a Wi-Fi pause does not cost samples.

| Link | Rate and format | Result on the LilyGO board |
| --- | --- | --- |
| USB serial, 4 Mbaud | 125000 pairs/s, CS12 | 60 s: no device drops; 99.8% delivered |
| Wi-Fi TCP | 125000 pairs/s, CS16 | 60 s, every pair delivered |
| Wi-Fi TCP | 250000 pairs/s, CS16 | 10 min, 150,003,050 pairs, every pair delivered |
| Wi-Fi TCP | 500000 pairs/s, CS16 | 60 s, 30,001,910 pairs, every pair delivered |
| Wi-Fi TCP | source 0, 576000 pairs/s, IQ8 | 60 s, 34,561,720 pairs, every pair delivered |
| Wi-Fi TCP | source 1, 800000 pairs/s, IQ8 | 60 s, 48,000,354 pairs, every pair delivered |

The bridge runs the LilyGO CH343 at 4 Mbaud, with CS12 for 125000 pairs/s.
On Windows, give the serial port a receive buffer of a few megabytes before
streaming; the default 4 KB fills in about 10 ms at 400 kB/s.

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

This validates the console capture path.

The bridge was tested on the same board, with Wi-Fi through its ESP32-C6
(2026-10-27):

- Receive as in the table above. Through SoapySDR over TCP, 60 s at 125000
  and 250000 pairs/s had no overflows and exact timestamp steps; a live gain
  step caused no gap, and a retune one overflow.
- Transmit at -9 dBm with 433 MHz antennas: SoapySDR `writeStream` bursts at
  433.92 MHz were received by a HackRF One at the expected offset and length;
  a timed burst started 11 us after the requested time; a burst during receive
  cost one receive overflow.
- Transmit at 915 MHz, -9 dBm, 16000 samples of a +10 kHz tone over TCP: 250000,
  500000 and 1000000 samples/s all ended with TX_DONE and no FIFO error flags,
  and an RTL-SDR measured bursts of 63, 32 and 16 ms (64, 32 and 16 nominal). At
  1000000 samples/s the TX FIFO never fell below 207 bytes. FIFO writes of up to
  1020 bytes per frame carry this; each write also costs about 40 us of BUSY, so
  62-byte writes topped out near 0.8 MB/s.
- Transmit on the HF PA at 2440 MHz, -9 dBm, same burst: 125000, 500000 and
  1000000 samples/s ended with TX_DONE, and a HackRF One measured bursts of 128,
  32 and 16 ms at +10 kHz, 48 dB over the floor. With DIO7 and DIO10 left out
  of HF transmit, the same burst did not show.

## Build

Use ESP-IDF 5.5.4 in an activated ESP-IDF shell:

```sh
cd firmware/esp32p4
idf.py set-target esp32p4
idf.py build
idf.py -p COM3 flash monitor
```

The first build fetches `espressif/esp_hosted` and `espressif/esp_wifi_remote`
from the ESP-IDF component registry (`main/idf_component.yml`).

To package the default build from the repository root:

```sh
python tools/package_firmware.py
```

The ZIP includes the application, bootloader, partition table, instructions
and checksums. Source is MIT licensed; the ESP-IDF dependency retains its own licenses.
