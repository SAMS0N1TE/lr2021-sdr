# Porting

Implement `lr2021_iq_hal` over an LR2021 driver. Call from task context with
exclusive radio ownership. Hooks return 0 on success, nonzero on failure.
Every SPI operation must check BUSY and radio command status with a bounded
timeout. All hooks run under `lock`; avoid acquiring a non-recursive lock twice.

| Hook | Requirement |
| --- | --- |
| `snapshot` | Report actual chip, ROM version and current configuration. Set `exclusive_lora` only with no TX, scan, alternate modem or experiment active. |
| `read32`, `write32` | Internal memory access using host-order uint32 values. |
| `write`, `read` | LR20xx command transactions; `read` returns payload without status bytes. |
| `standby`, `receive` | Stop RX / arm the configured LoRa receiver. |
| `reset` | Hardware reset and clock/regulator initialization; leave RX stopped. |
| `restore` | Reapply all saved modem, frequency, antenna and IRQ settings, then resume RX if previously receiving. |
| `fast` | Acquire SPI bus and use 16 MHz polling transfers; disable restores normal speed and releases ownership. A failed enable must leave the bus unchanged. |
| `clear_fifo`, `read_fifo` | Clear RX FIFO / read raw bytes with status validation. |
| `now_us`, `sleep_us` | Monotonic microsecond clock / bounded delay. |

Keep any configuration beyond `lr2021_iq_profile` in the HAL context. In
particular, reset must not erase the host's saved configuration before restore.
This library does not configure GPIOs, power, RF switches, TCXO or antenna paths.

Finite capture needs two non-overlapping 256-word buffers. Reset does not
reliably preserve SRAM, so restoration is verified after reset and before RX
resumes. Check the function return and all three result errors.

FIFO capture requires LoRa BW31.25 kHz. Its 256-byte FIFO fills in roughly
512 us at the nominal rate. Use internal RAM, adequate task priority and a
sink that copies immediately. Avoid logging, allocation, scheduler delays and
other radio calls in the sink. Separate windows have gaps.

The sink receives little-endian I/Q int16 pairs plus the first pair index.
Returning false ends acquisition normally. Treat all delivered blocks as
provisional until the function returns 0. Full FIFO, sticky error flags and
failed recovery reject the complete window. Never clear an error merely to
make a recording appear continuous.
