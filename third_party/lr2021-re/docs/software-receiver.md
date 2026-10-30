# Software LoRa receiver on LR2021 raw IQ

With raw IQ streaming out of the chip ([raw-iq.md](raw-iq.md)), the PC can demodulate LoRa
itself. These tools receive MeshCore traffic (910.525 MHz, BW 62.5 kHz, SF7, CR 4/5, sync
word 0x12) and compare the results against a hardware modem.

Python 3 with `numpy`, `scipy`, `pyserial` (`matplotlib` for plots). Run the tests with:

```bash
python3 -m unittest discover -s tools -p 'test_*.py'
```

## Decoder: `tools/lora_decode.py`

```bash
python3 tools/lora_decode.py captures/iter/c02.bin 83333.333       # file: interleaved int8 IQ
```

`decode(iq_complex, fs, sf=7, bw=62500) -> dict` runs the full receive chain:

1. resample to 1 sample per chip;
2. preamble/sync search;
3. dechirp + FFT;
4. header at reduced rate (/4);
5. Gray demapping, diagonal deinterleave, Hamming;
6. header checksum, dewhitening, payload CRC-16.

The bit-level steps follow EPFL's [gr-lora_sdr](https://github.com/tapparelj/gr-lora_sdr).

Sync sweeps the symbol grid over one symbol. The grid is aligned where the preamble
(upchirp) bin equals the sync (downchirp) bin, and that bin gives the integer CFO. Sync word
symbols 8 and 16 confirm the lock. The decoder applies fractional CFO and tries
quarter-chip sampling phases. Those two fixes took the 12 saved test captures from 8/12 to
12/12 CRC-valid.

For CRC-valid packets, `decode()` adds a `meshcore` dictionary parsed by
`tools/meshcore_decode.py`. It covers routes, transport codes, path hashes, adverts, ACKs,
trace fields and encrypted-message metadata. Advert signatures are parsed but not verified,
and encrypted bodies stay ciphertext.

`snr_db` is an envelope estimate and isn't calibrated across AGC changes. `cfo_hz` is the
correction for the selected timing grid, not a calibrated oscillator measurement.

## Listening

| Tool | How it receives | Use |
|---|---|---|
| `continuous_capture.py --decode` | Keeps RX running and streams IQ to disk while a decoder follows the file, using overlapping 2 s windows at 0.5 s stride | Normal listening and recording |
| `live_decode.py` | Triggered snapshots of ~0.79 s; stops RX to transfer, then rearms | Snapshot and trigger diagnostics |
| `decode_continuous.py <iq.bin>` | Offline decode of a recording | |

```bash
python3 -u tools/continuous_capture.py --seconds 0 --decode     # Ctrl-C for a checked stop
python3 -u tools/continuous_capture.py --seconds 60 --format iq16 --bandwidth 31.25 --dec 2
python3 -u tools/live_decode.py --diagnostics --save captures/live
```

Boards are autodetected by USB description (`tools/radio_ports.py`); use `--port` to
override. Capture mode leaves the MeshCore loop held, so reboot the receiver afterwards.

## Hardware vs software comparison

`tools/continuous_compare.py --seconds 600` listens with the LR2021 software receiver and a
RAK3401 running MeshCore's `kiss_modem` firmware (SX1262 hardware modem) at the same time.
Both radios only listen. The host sends KISS control frames only; `tools/kiss_rx.py` is a
receive-only KISS host. Packets are matched one-to-one by exact bytes and receive time
(50 ms tolerance).

Results are **conditional relative miss fractions**, not absolute PER, because packets that
both receivers miss are invisible. RAK-only packets only count as software misses when their
whole airtime lies inside retained IQ.

| Run | RAK | Software | Matched | Notes |
|---|---:|---:|---:|---|
| 60 s continuous | 3 | 3 | 3 | no packets outside retained IQ |
| 93 s snapshots | 17 | 13 | 13 | 4 RAK-only, all outside retained IQ; 12/12 eligible decoded |

These are small samples, not a sensitivity ranking.

A note on nRF52 USB: without host traffic, received frames could stay buffered in the
device until the next host command. The tools send periodic ignored newlines (LR2021) and
KISS pings (RAK) to keep the output flowing.

## Offline collision cancellation (SIC): `tools/lora_sic.py`

This is experimental successive interference cancellation, done entirely on the PC:

1. Decode a CRC-valid packet.
2. Regenerate its chirps from the decoded nibbles (re-encoding Hamming, interleaving and Gray
   mapping, rather than replaying hard decisions).
3. Separate timing from carrier offset using the downchirps, and fit a short complex FIR
   per chirp (17 taps by default).
4. Subtract the fitted waveform and search the residual again.

Only CRC-valid payloads are reported. A fit needs ≥ 1 dB occupied-energy reduction and
≥ 0.4 median explained power before it is subtracted. On clean live packets the replica
removed 13–15 dB of occupied-window energy.

```bash
python3 tools/sic_demo.py --out /tmp/sic_demo
python3 tools/sic_demo.py --weaker-db 6 --delay .043 --noise-snr 0 --seed 20261028 --out /tmp/sic_noisy
python3 tools/sic_sweep.py
python3 tools/lora_sic.py analysis/sic_demo/mixture.cf32 --format cf32 --plot
```

- **Demo** (`analysis/sic_demo/`): a *digital* mixture of two separately recorded RF packets.
  The second is delayed 43 ms, 6 dB weaker, shifted 2.3 kHz and phase-rotated. Normal
  acquisition finds one packet; SIC recovers both original 130-byte payloads exactly. The
  canceller never sees the source recordings or the known payloads.
- **With noise** (`analysis/sic_noisy_demo/`): same, with seeded complex Gaussian noise. Both
  are still recovered.
- **Sweep** (`analysis/sic_sweep/`): 40 cases (3/6/9/12 dB, 43/119 ms, five noise levels).
  SIC recovered both packets in 19 cases where the baseline found only one. There were no
  regressions or ghost payloads. At 3 dB neither packet is acquired, and SIC can't start
  without an initial CRC-valid decode.
- **Live:** about 15 minutes of live mesh plus an archive scan of 158 files (888 s). There
  were no additional recoveries, so natural over-the-air collision recovery hasn't been
  demonstrated. Those recordings aren't published because they contain third-party traffic.

`tools/sic_hunt.py` scans continuous recordings for natural overlaps. `tools/sic_archive.py`
batch-scans a capture tree.

## Other helpers

- `tools/stream_shot.py`: one triggered snapshot with `stream` + `streamdump`.
- `tools/spectro_bin.py`, `spectro_zoom.py`: spectrograms of int8 IQ files
  (`captures/stream_rak3.png`, `_zoom.png`).
- `tools/agc_sweep.py`, `bench_capture.py`: two-radio bench sweeps. A RAK repeater sends
  zero-hop adverts while the LR2021 captures.
