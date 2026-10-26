# LR2021 raw I/Q: follow-up to sfowlr's findings

[Research index](../../docs/research.md) / Raw I/Q follow-up

In [LakeShark discussion #16](https://github.com/SAMS0N1TE/LakeShark/discussions/16),
[sfowlr](https://github.com/sfowlr) reported new LR2021 ROM 1.24 results from a
Meshnology W12. Their [RadioFoundryGateway hardware notes](https://github.com/RadioFoundry/RadioFoundryGateway/blob/e2a85653aab8ef13a497ed2b8891f102fac711c4/docs/spec/lr2021-iq.md)
and [reader](https://github.com/RadioFoundry/RadioFoundryGateway/blob/e2a85653aab8ef13a497ed2b8891f102fac711c4/esp_idf/main/fe_lr2021.c)
give the command settings and implementation behind the discussion. Credit for
the W12 discoveries belongs to sfowlr. The tests below are our independent
checks on a LilyGO T-Display-P4 with LR2021 ROM 1.24; they are not W12 results.

## What sfowlr reported

| Finding | W12 result |
| --- | --- |
| FSK rate selection | With raw-route byte 0 set to `FF`, the FSK `rx_bw` code selects a rate of 32 MHz / (mode × decimation). |
| Sample formats | Source 1 exports signed 8-bit I/Q after the channel filter; source 2 exports signed 16-bit I/Q. |
| High-rate receive | Source 1 streamed about 664.8 thousand complex pairs/s without reported loss at 16 MHz SPI. |
| Gain order | `SetRx` reset a pre-set manual gain to step 13; writing gain after entering RX worked. |
| Transmit | Raw TX selector 1 scaled as sample × frequency deviation / 64 across their tested rates, without an RX/TX reset requirement. |
| Four-level FSK | Opcode `0x023B`, argument `01`, produced DMR bursts that an independent receiver decoded. |

Their notes also describe a source-0 rate converter and a separate PRAM patch
for four-level FSK **receive**. Native four-level transmit and receive are
different experiments.

## What we measured on LakeShark

The board used an experimental, receive-only LakeShark lab command and the
Wi-Fi antenna already attached to the LR2021 path. SPI was configured for
16 MHz. SCLK was not measured at a pin. Each lab capture checked the FIFO
flags and chip errors, then restored the normal LoRa receiver. Raw I/Q and
console logs remain local because they may contain unrelated RF traffic.

| Check | LakeShark result |
| --- | --- |
| Source format | Source 1 delivered two bytes per candidate complex pair and source 2 four. Interpreting them as interleaved IQ8 and IQ16 produced a marker that moved by about −20 kHz under a +20 kHz retune. This supports the format and frequency-axis interpretation; it does not calibrate amplitude or prove sample continuity. |
| FSK rate codes | Source 1 delivered about 200k pairs/s with code 163, 400k with 162, and 500k with 154 or 26 in short captures. The complete code-to-rate table was not measured. |
| Manual gain | Direct reads of register `0xF30154` during RX showed pre-set step 1 as `0x11` and step 13 as `0xDD` after `SetRx`, in our FSK and LoRa sequences. This differs from the W12 result. We still write gain after RX in the normal lab path. |
| TX scaling and DMR | Not tested on this board. No four-level TX, DMR decode, or RX PRAM patch is claimed here. |

### The 666k IQ8 transport test

We used FSK `rx_bw=0x12` (decimal 18), route
`FF 00 00 00 00 09 00 00 00 00 01`, and source 1 at 920.010 MHz. The
original 62-byte CPU SPI reads overflowed in all eight 20/250 ms count-only
trials. Raising only the lab task priority did not fix it. A gated DMA path
with up to 1,022 FIFO payload bytes per read passed five 20 ms, three 250 ms,
one 2 s and one 3 s count-only runs. Each returned `ESP_OK`, FIFO flags `0x04`
with no overflow/underflow bits, zero chip errors, and successful LoRa recovery.

| Requested duration | Delivered bytes | Candidate IQ8 pair rate from bytes / elapsed time |
| --- | ---: | ---: |
| 2 s | 2,666,420 | 666,560 pairs/s |
| 3 s | 3,999,848 | 666,597 pairs/s |

Two 50 ms raw-IQ captures were complete through the serial parser and had
clean FIFO flags. Their strongest board-associated marker moved about −19.9
kHz under a +20 kHz retune, using a 666,667-pair/s frequency axis. This
reproduces sfowlr's **bounded high-rate FIFO transport** on a second board.
FIFO flags do not prove that every sample index was preserved. A known
nonrepeating input and a physical SCLK measurement would close those gaps.

### Source-0 rate converter

The [gateway notes](https://github.com/RadioFoundry/RadioFoundryGateway/blob/e2a85653aab8ef13a497ed2b8891f102fac711c4/docs/spec/lr2021-iq.md)
give FSK code `0x21` (800k modem input) and route
`21 00 08 CA 00 08 00 00 00 00 01` for 576,000 IQ8 pairs/s out. We tried
that receive-only setting at 920.010 MHz with the same large FIFO reads.
Three 1 s count-only runs delivered 1,151,852–1,151,932 bytes each, about
575.9k candidate pairs/s. All three had flags `0x04`, zero chip errors and
LoRa recovery. Two complete 20 ms captures showed the strongest marker move
−20.00 kHz under a +20 kHz retune on the requested 576k axis.

One 2 s attempt overflowed at 1.785 s after a 1.251 ms service gap and
recovered LoRa. The short tone comparison below tests relative response on
this board; sfowlr's complete W12 filter characterization remains separate.

### Known 2.4 GHz input and a longer raw capture

We transmitted a short, unmodulated HackRF tone at 2412.050 MHz into a nearby
Wi-Fi antenna, with the LR2021 also on a Wi-Fi antenna. The antennas were
about 13 cm apart. The HackRF RF amplifier and antenna bias were off. At a
fixed transmit setting and LR gain 1, the source-1 and source-0 IQ8 peaks
moved with the expected retunes. The roughly 0.4–0.6 kHz excess offset is
consistent with two free-running frequency references. This gives us a known
external RF marker; it is not a calibrated sensitivity measurement.

For the filter comparison we held the HackRF carrier and transmit setting
fixed, moved the LR center, and normalized each source to its +50 kHz tone
amplitude. Three passes at the key positive offsets gave these medians:

| Tone offset from LR center | Source 1, code 18 | Source 0, code 33 |
| --- | ---: | ---: |
| +50 kHz | 0 dB | 0 dB |
| +150 kHz | −7.9 dB | −3.7 dB |
| +175 kHz | −15.8 dB | −7.6 dB |

Source 0 retained more of this tone at +150 and +175 kHz in all three passes.
That supports the wider-response lead from sfowlr's W12 work. The over-air
path, independent oscillators, and nonmonotonic single-pass points prevent us
from assigning a calibrated 3 dB bandwidth or generalizing to other boards.

A separate +100 kHz known tone was recorded for 250 ms with source 1, code
18, into a 512 KiB PSRAM buffer before serial transfer. The host received
332,968 IQ8 bytes (166,484 candidate complex pairs) with every serial block
in order. The capture returned `ESP_OK`, FIFO flags `0x04`, zero chip errors,
and normal LoRa recovery. At the assumed 666,667-pair/s rate, the tone peak
was +100,434 Hz. A one-sample slip would cause about a 54° lasting phase
step at that tone. The largest detected lasting step was 10.4° using 128-pair
blocks and eight-block flanks, or 6.2° with 256-pair blocks. We found no
one-sample-sized persistent jump in this 250 ms capture. This phase screen
does not certify every index in the earlier 3 s count-only run, and it cannot
detect a pair of losses and insertions that cancel between its blocks.

## Remaining checks

- Use a cabled, attenuated input and several levels to measure calibrated
  passband, saturation and the reported IQ16 dynamic-range advantage.
- Send a known nonrepeating waveform to check missing samples and order
  through FIFO wrap over multi-second captures. Compare with an independent
  receiver.
- Compare exact W12 and P4 RX command order, patch state and register-read
  timing to resolve the gain-reset difference.
- Test TX scaling and native four-level FSK separately with an independent
  receiver before claiming DMR interoperability.

The [sanitized trial summaries](results.json) contain byte counts, timing and
error flags without RF samples or device identifiers. The present high-rate
DMA and converter work is in a local experimental LakeShark build. The public
standalone firmware still uses its existing
source-2 receive path; these measurements are not a new public firmware mode.
