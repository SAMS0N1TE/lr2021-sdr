# DMR transmit waveforms

[Guides](README.md) / [Transmitting](transmit.md) / DMR

**Software builds DMR Tier II bursts and modulates them for the LR2021's I/Q playback. Nothing has been transmitted yet: every result on this page comes from software, not from a radio.**

## Make a burst file

```text
python tools/lr2021_make_dmr.py voice dmr-voice.iq8 --source 123456 --destination 4242 --cc 1
python tools/lr2021_make_dmr.py csbk dmr-csbk.iq8 --count 2
python tools/lr2021_make_dmr.py data dmr-data.iq8 --text "LR2021 DMR"
python tools/lr2021_decode_dmr.py dmr-data.iq8
```

Requires NumPy. Each command writes the I8/Q8 buffer plus a `.json` sidecar
listing every burst's 33 octets, timing, deviation and the largest
`--repeats` that fits the playback limits. `--private` makes an individual
call instead of a group call. The decoder reads the sidecar's sample rate
and exits non-zero unless every burst decodes.

The files use the [TX playback](tx-playback.md) upload format. **They have
not been sent through the radio.** The upload command would be
`python tools/lr2021_tx_upload.py dmr-voice.iq8 --serial YOUR_USB_SERIAL --send 19200`,
on the fixed 915 MHz test frequency. Transmit only where you are licensed or
otherwise allowed to.

## What it sends

| Kind | Bursts (default) | Contents |
| --- | --- | --- |
| `voice` | 2 Voice LC Headers, 2 Terminators with LC | Full LC: group (FLCO 0) or unit-to-unit (FLCO 3), FID 0, destination, source; RS(12,9) masked `96`/`99` |
| `csbk` | 2 Preamble CSBKs | CSBKO `3D`, blocks to follow counting down; CRC-16 masked `A5A5` |
| `data` | 1 Preamble CSBK, Data Header, rate 1/2 blocks | Unconfirmed header (DPF 2, SAP 4, pad count, blocks to follow) with CRC-16 masked `CCCC`; 12 octets per block, CRC-32 in the last |

Every burst carries the mobile-station data sync `D5D7F77FD757`, a
Golay(20,8) Slot Type with colour code and data type, and a BPTC(196,96)
payload. Bursts are 27.5 ms, one per 60 ms frame, as a radio using one
timeslot in direct mode sends them. The carrier ramps up and down inside
the 1.25 ms guard times and is off in the other slot. There is no CACH,
which a mobile does not send.

Modulation is 4800 symbols/s 4FSK at +1944, +648, -648 and -1944 Hz for
dibits 01, 00, 10 and 11, shaped by a root-raised-cosine filter with roll-off
0.2, truncated at eight symbols either side. Frequency is integrated into
phase at radius 48, like the [P25 maker](../tools/lr2021_make_p25.py).
Peak deviation between symbols reaches about 3.2 kHz; 99% of the power falls
within 7.4 kHz.

## Why 19,200 samples/s

| Rate | DMR use |
| --- | --- |
| 19,200 (default) | Up to seven 60 ms frames in one 8,192-sample upload |
| 125,000 | One burst per upload |
| 76,800 | Rejected: halved FSK deviation in the [rate findings](tx-playback.md#sample-rate-and-frequency-findings) |
| 38,400, 9,600, 4,800 | Rejected: no FSK measurement, or one sample per symbol |

The upload limit, not the 32,767-sample ROM limit, decides how much fits:
16,384 bytes is 8,192 samples, 427 ms at 19,200 but 65.5 ms at 125,000.
Buffers are whole 60 ms frames, so repeated playback keeps the slot timing.
The generator stops with an error when bursts do not fit.

The rate table measured correct deviation only at 125,000; it did not test
2-FSK at 19,200. P25 is the closer check: it is RF-verified at 19,200 with
the same 4,800 symbols/s and similar deviation. Re-measuring that
[archived capture](../research/tx-p25/samples/) with a discriminator and
four-level clustering gives 3,936 Hz between outer levels and 1,284 Hz between
inner levels; the uploaded P25 buffer measured the same way gives 3,901 and
1,268 Hz. At 19,200 the radio kept deviation to within about 1%. The
38,400 inversion fits a ±20 kHz tone aliasing past that rate's 19.2 kHz
Nyquist limit, which DMR would not reach, but DMR at 38,400 is untested.

I/Q output, not frequency samples: the frequency-sample selector exists only
in the 64-sample diagnostic probe. Its measured 156 Hz per count would put
the ±648 Hz symbols about 3.5% off, and its scale was not tested at other
deviation settings. The upload path accepts I8/Q8 only.

## Checked in software

`tests/test_dmr_tools.py` and the decoder check the generated buffers. The
decoder does not see the expected bits: it uses an FM discriminator, an RRC
matched filter, sync search and four-level slicing, then the Slot Type,
BPTC, RS and CRC checks.

| Check | Result |
| --- | --- |
| MMDVM-Host reference vectors, copied from LakeShark | All 256 Slot Type words; 6 LC/RS parities; 6 BPTC payloads bit-exact |
| BPTC single-bit errors | All 195 protected positions corrected in all 6 payloads |
| CRC engines | Standard check values for the CRC-16 and CRC-32 polynomials |
| Loopback, all burst types, 19,200 and 125,000 | Every burst bit-exact, zero corrections; LC, CSBK and header fields and data payload recovered |
| Decoded symbol levels | Outer within 0.4% of ±1944 Hz, inner within 1.1% of ±648 Hz |
| 400 Hz offset and noise, about 9 dB SNR in 19.2 kHz | All six bursts and the message CRC pass after BPTC corrections |
| Corrupted data block | Message CRC-32 rejected |

## Not verified

- **No on-air test has happened.** Hardware behaviour for DMR, including
  the off-slot gaps and power ramps, is unmeasured.
- No independent DMR decoder has read these files. DSD-FME and DSD were not
  installed in WSL when this was written. `lr2021_decode_dmr.py --wav` writes
  48 kHz discriminator audio for such a check.
- The reference vectors cover the Slot Type, BPTC and LC RS codes. The CSBK
  and data header layouts, the CRC masks, the data header's full-message and
  fragment-number values, and the CRC-32 octet order (pairs swapped, sent
  low octet first) follow a reading of ETSI TS 102 361 and have only been
  checked against this project's own decoder.
- No voice bursts, embedded LC, confirmed or rate 3/4 data, or BS-sourced
  bursts. Voice would need AMBE+2 frames, which this project does not
  implement.

Next: [Transmitting](transmit.md) · [TX playback](tx-playback.md) · [P25](p25.md)
