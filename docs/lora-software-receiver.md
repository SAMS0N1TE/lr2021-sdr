# LoRa software receiver

[Guide hub](README.md) / LoRa software receiver

With raw I/Q streaming out of the LR2021, a computer can demodulate LoRa
itself. This uses the receiver from
[beala/lr2021-re](https://github.com/beala/lr2021-re/blob/main/docs/software-receiver.md)
(MIT, Alex Beal), kept unchanged in
[`third_party/lr2021-re/tools`](../third_party/lr2021-re/tools), with
[`tools/lr2021_lora.py`](../tools/lr2021_lora.py) feeding it from this
repository's bridge protocol over USB serial or Wi-Fi.

Python 3 with numpy and scipy; pyserial for the serial link and the KISS
comparison; matplotlib for plots.

## What the receiver does

`lora_decode.decode(iq, fs, sf=7, bw=62500)` runs the whole chain: resample
to one sample per chip, preamble and sync search, dechirp and FFT, the header
at reduced rate, Gray demapping, diagonal deinterleaving, Hamming decoding,
header checksum, dewhitening and the payload CRC-16. Its bit-level steps follow
EPFL's [gr-lora_sdr](https://github.com/tapparelj/gr-lora_sdr). It corrects
the integer and fractional carrier offset and tries quarter-chip sampling
phases. For CRC-valid packets, `meshcore_decode` parses MeshCore routes,
transport codes, path hashes, adverts, ACKs, trace fields and encrypted-message
metadata; it does not verify signatures or decrypt.

`lora_sic.py` adds collided-packet recovery (successive interference
cancellation): it decodes one packet, regenerates its chirps from the decoded
nibbles, fits and subtracts the replica, and searches the residual again. On
lr2021-re's digital mixtures it recovers both packets where the plain decoder
finds one; it has not yet recovered a natural over-the-air collision.

## Record and listen

```bash
python tools/lr2021_lora.py capture tcp:192.168.4.1 --seconds 0 --decode
python tools/lr2021_lora.py capture COM14 --cs12 --seconds 60 --decode
```

`capture` sets the receiver (default 910.525 MHz, LoRa BW 62.5 kHz, SF7, chip
AGC), streams it, and writes a folder in lr2021-re's layout: `iq.bin`
(interleaved int8 for IQ8 routes, little-endian int16 for source 2),
`frames.jsonl` (one line per bridge frame) and `summary.json`. With
`--decode` the overlapping-window decoder follows the file and prints each
CRC-valid packet; `packets.jsonl` keeps them. Ctrl-C stops cleanly. A gap in
the stream (a `first_pair` jump) is filled with zeros so sample times stay
right, and marks the capture invalid in the summary.

Routes: `source2` (default) is the LoRa receive path's IQ16 at 4 x the
bandwidth, 250000 pairs/s at BW 62.5 kHz, which needs Wi-Fi; `--cs12` cuts
it to 3 bytes a pair. `--route raw0b` gives the same rate as IQ8 in
lr2021-re's packet type 0x0B ([raw routes](host-sdr.md#raw-receive-routes)),
2 bytes a pair. `--route source0 --code C --rate R` uses the FSK-setup source
0 at an explicit rate.

## Snapshots

```bash
python tools/lr2021_lora.py snapshot tcp:192.168.4.1 --ratio 4 --count 5 --decode
```

`snapshot` keeps a short pre-trigger ring and saves `shot_*.iq8` or `.iq16`
with a JSON description when a 1024-pair block's power rises above `--ratio`
times the quiet baseline, or when the chip's AGC step (read with `GET_GAIN`)
drops below its quiet value. lr2021-re found the second trigger necessary:
with the AGC working, a strong packet barely raises the output power. The
firmware console's `stream` command does the same on the device
([debug console](debug-console.md)).

## Offline tools

All of lr2021-re's offline tools run on these recordings:

```bash
python third_party/lr2021-re/tools/decode_continuous.py captures/continuous/<run>/iq.bin --fs 250000 --format iq16
python third_party/lr2021-re/tools/lora_decode.py shot.iq8 250000
python third_party/lr2021-re/tools/sic_hunt.py captures/continuous/<run>/iq.bin --fs 250000
python third_party/lr2021-re/tools/sic_demo.py --out sic_demo
python third_party/lr2021-re/tools/spectro_bin.py shot.iq8
```

`decode_continuous.py` takes `--format iq16` for source 2 recordings and
`iq8` (the default) for the IQ8 routes; `sic_hunt.py` handles IQ8 recordings.
`sic_hunt.py` looks for natural overlaps in continuous recordings and
`sic_archive.py` scans a capture tree; `sic_demo.py` and `sic_sweep.py`
regenerate lr2021-re's mixtures and recovery sweep. Check each tool's `-h`
for its exact arguments.

## Hardware against software

```bash
python tools/lr2021_lora.py compare tcp:192.168.4.1 --rak COM9 --seconds 600
```

`compare` records with `--decode` while a second radio running MeshCore's
`kiss_modem` firmware listens on the same channel through lr2021-re's
receive-only KISS host, then matches packets by bytes and time (50 ms
tolerance) and writes `comparison.json`. As lr2021-re notes, the result is a
conditional miss fraction between the two receivers, not an absolute packet
error rate.

## Tests and limits

`lr2021re_tools` runs lr2021-re's 52 tests against its recorded adverts, and
`lora_tools` replays those adverts through a fake bridge: `capture --decode`
must return all three packets, a dropped frame must be zero-filled and
flagged, and `snapshot` must trigger on and decode a packet set between quiet
noise. Neither test involves the radio. Live decoding on the LilyGO board has
not been run yet: no MeshCore traffic was heard during the AGC readback runs
([research](../research/agc-readback/README.md)). Recordings of live traffic
contain other people's packets; keep them out of the repository (`/captures/`
is ignored).
