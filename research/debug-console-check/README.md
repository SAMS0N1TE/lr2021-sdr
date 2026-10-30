# Debug console and raw0b check

[Research index](../../docs/research.md) / Debug console check

First hardware run of the [debug console](../../docs/debug-console.md)
commands ported from [beala/lr2021-re](https://github.com/beala/lr2021-re)
and of the `raw0b` receive route, on the LilyGO T-Display-P4 LR2021
(ROM 1.24), USB serial, 2026-10-30. [HW] marks results verified here.

| Check | Result |
| --- | --- |
| `info`, `err`, `gain` | radio configured, no errors; `gain` in standby answers STATE with step 10, as expected [HW] |
| `rc 103 4 00 00 00 04` (aux IDENTITY) | 0x00000046, ARCv2 EM, as lr2021-re reports [HW] |
| `romprobe`, `mr 0 ...` | ROM at base 0: reset vector 0x000000CC, then 0x000069A8, as lr2021-re reports [HW] |
| `tools/lr2021_rom_dump.py` (`mr 0 32768`) | the 80 KB ROM's SHA-256 is `e531400a29f660aa50dbbb4de5df01d1b73818faa767c0494a5d0df3f48da821`, the same as lr2021-re's chip, so its ROM addresses apply here; last nonzero word at 0x13FFC. The dump stays in the ignored `dump/` folder [HW] |
| `mr F30154 1` | 0x68A00A10 in standby: step field 10, manual 0 [HW] |
| `mw 804000 12345678` right after boot | **timed out (-3)**: the boot capture route owns the 0x804000 bank, and writing it stalls the chip, the hazard lr2021-re describes; `rxrestore` recovered the radio [HW] |
| `rc 207 0 0B`, then `cap D0 2048 100` | the 8 KiB one-shot capture filled all 2048 words; the write pointer reached 0x2000 in about 35 ms; stopped, read and restored cleanly [HW] |
| `capwait D0 50 400` | 8 captures, no trigger with no signal (timeout), restored [HW] |
| `stream 0 0 1000 16`, `streamdump`, `streamstats` | packet type 0x0B, source 0: 250008 bytes in 1 s (125004 pairs/s of IQ8), FIFO peak 248 bytes, AGC at 13 in every 4 KiB block; `streamdump` returned the full 131072-byte ring [HW] |
| `raw0b` over the bridge, BW 31.25 kHz | 999996 pairs in 8 s, 124999 pairs/s, 4 x the bandwidth as lr2021-re reports; no device drops, no capture error. The host saw 3 frames (2880 pairs) lost on the 4 Mbaud serial link, zero-filled and flagged by `tools/lr2021_lora.py` [HW] |

Logs: [`logs/capture.txt`](logs/capture.txt) (mw, cap, capwait and stream).
The read-only session log is not committed because `romprobe` prints ROM
words, which are Semtech's.

Not run yet: `pktstatus` with a known transmitter, live LoRa decoding (no
traffic heard), `raw0b` at BW 62.5 kHz (500 kB/s, needs Wi-Fi), and the
SoapySDR `raw0b` route on the board.
