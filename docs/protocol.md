# LR2021 raw source 2

These commands were observed on ROM 1.24. Other versions are unsupported.
Source routing does not require a custom PRAM patch.

| Item | Value |
| --- | --- |
| Route source 2 to SRAM | Command `0x0290`, payload `FF 00 00 00 00 92 00 00 00 FF 00` |
| Route source 2 to RX FIFO | Command `0x0290`, payload `FF 00 00 00 00 0A 00 00 00 00 01` |
| Capture status | Memory `0xF30054`; completion mask `0x03000000` |
| SRAM destination | `0x802000`, 256 words |
| Captured length | Command `0x0291`, two-byte big-endian count; expect 1024 |
| Active patch marker | Memory `0x800FF8`; reject `0x600DB002` |
| FIFO level | Command `0x011C`, two-byte big-endian count |
| FIFO flags | Command `0x011B`, RX then TX flags |
| Clear FIFO flags | Command `0x0114`, payload `3F 3F` |
| Read RX FIFO | Command `0x0001`, data follows two status bytes in the same SPI frame |

Ordinary command reads use a command frame followed by a response frame after
BUSY clears. FIFO data reads use the single-frame form above. Follow the
LR20xx SPI framing and status rules in the Semtech datasheet.

Finite sequence: save SRAM, standby, route source, start RX, wait up to 100 ms
for completion, standby, check length, read SRAM, reset, restore and verify
SRAM, restore radio configuration. Preserve both capture and recovery errors.

FIFO sequence: own the fast SPI bus, standby, route source, clear flags, start
RX, discard startup FIFO data, clear flags, poll and read aligned blocks.
Read sticky flags after the last delivered block and before standby. Then
restore bus speed, reset and restore the radio. No data is delivered after
the final flags check.

RX flags: empty `0x01`, low `0x02`, high `0x04`, full `0x08`, overflow `0x10`,
underflow `0x20`. Error bits `0x30` invalidate continuity. A level of 256 bytes
is rejected conservatively even before reading flags.

Nominal source-2 rates measured with known tones:

| LoRa bandwidth | Complex pairs/s |
| ---: | ---: |
| 31,250 Hz | 125,000 |
| 62,500 Hz | 250,000 |
| 125,000 Hz | 500,000 |
| 250,000 Hz | 1,000,000 |

Measurements used SF7 at 910.525 MHz and agreed within about 0.5%. They are
not clock or amplitude calibration. The FIFO API only supports the first row.
