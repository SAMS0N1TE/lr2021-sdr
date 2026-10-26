# GNSS and 2.4 GHz: first checks and next experiments

[Research index](../../docs/research.md) · [Contributor's faster export report](../community-2026-10/README.md)

Updated 2026-10-26. The first hardware checks establish tuning and sample
delivery. **No satellite acquisition, GNSS position, Wi-Fi packet decode or
Wi-Fi CSI has been demonstrated here.** Potential directions below are hypotheses;
there is no claim of being the first to try them.

## First measurements

LilyGO T-Display-P4, LR2021 ROM 1.24, installed LakeShark LR
`2.18.0-g026a538409e8`, ESP-IDF 5.5.4. No firmware was flashed. The operator
reported a Wi-Fi antenna on MMCX1, no GNSS-specific antenna, and a separate
RTL-SDR V3 with another Wi-Fi antenna. The RF switch selection was not read
back. The commands below address the **LR2021**, not the attached RTL-SDR.

All sessions used HF input, requested gain step 13, boost 4 and a 40 ms
duration. Actual post-SetRx gain was not read back. Source 2, ci16 little endian.

| Center | Nominal pairs/s | Captured pairs | Peak FIFO bytes | Outcome |
| --- | ---: | ---: | ---: | --- |
| 1575.020 MHz, lower control | 125000 | 4998 | 44 | No reported FIFO error |
| 1575.420 MHz, GPS L1 | 125000 | 4999 | 212 | No reported FIFO error |
| 1575.820 MHz, upper control | 125000 | 5000 | 44 | No reported FIFO error |
| 2412 MHz, trial 1 | 250000 | 9920 | 640 | No reported FIFO error |
| 2437 MHz, trial 1 | 250000 | 9973 | 176 | No reported FIFO error |
| 2462 MHz, trial 1 | 250000 | 10000 | 884 | No reported FIFO error |
| 2412 MHz, trial 2 | 250000 | 9997 | 172 | No reported FIFO error |
| 2437 MHz, trial 2 | 250000 | 9973 | 484 | No reported FIFO error |
| 2462 MHz, trial 2 | 250000 | 9973 delivered | 1016 | **Rejected: flags 0x1C, ESP_ERR_INVALID_RESPONSE** |

Every row reported successful tuning, no chip error bits and successful
recovery. The last row includes FIFO overflow bit `0x10`; its samples are not
used as continuous data. The board subsequently reported its prior LoRa RX
configuration and no chip errors. The GPS module continued tracking satellites.

No-error flags do not prove sample sequence continuity or exact sample-clock
rate. Counts differ from ideal duration*rate, and no independent sequence marker
or clock was used. These are short windows separated by tuning and UART transfer
gaps. Wi-Fi channel centers are frequency labels, not identifications of the
signals received. No controlled access-point traffic was generated.

Redacted metadata and SHA-256 digests are in [results.json](results.json). Raw
I/Q and full console logs remain local because unrelated RF traffic and device
identifiers may be present. Raw files are not distributed by this change.

## GNSS: a real lead, with a bandwidth problem

GPS L1 C/A uses a 1575.42 MHz carrier, a 1.023 Mchip/s PRN code and a 1 ms
code period ([GPS interface specifications](https://www.gps.gov/interface-control-documents-icds-interface-specifications-iss)).
The LR2021 can tune there, but tuning into the band is only the first step.
Spread-spectrum signals normally need correlation; an FFT peak is not satellite
evidence. An L1 antenna, suitable filtering/LNA and the correct module input
matter more than whether a connector fits.

A 433/868/915 MHz LoRa antenna is not an established improvement over a Wi-Fi
antenna at L1. Keep the Wi-Fi antenna for 2.4 GHz tests. Use a characterized L1
antenna/feed for a meaningful GNSS sensitivity test. An active GNSS antenna
also needs its specified DC supply through a suitable bias tee and DC isolation
where needed; do not assume MMCX1 supplies that bias. The onboard GNSS antenna
and LR2021 antenna path are separate.

### Calculated upper-bound model

For rectangular C/A chips, an ideal two-sided rectangular passband B retains
`integral[-B/2,B/2] sinc(f/1023000)^2 df / 1023000` of total signal power.
The following is an ideal matched-filter energy loss, not measured sensitivity.
It assumes a known code, perfect reference filtering and no quantization/clock
loss. **Export sample rate is not measured analog passband.**

| Hypothetical full passband | C/A energy retained | Ideal energy loss | IQ8 SPI payload utilization |
| --- | ---: | ---: | ---: |
| 125 kHz | 12.17% | 9.15 dB | 12.5% |
| 250 kHz | 24.04% | 6.19 dB | 25.0% |
| 664.8 kHz | 58.06% | 2.36 dB | 66.48% |
| 1 MHz | 76.44% | 1.17 dB | 100%, no overhead margin |
| 2.046 MHz | 90.28% | 0.44 dB | 204.6%, impossible on 16 MHz SPI |

Reproduce with `python tools/lr2021_research.py budget`. A receiver does not
need to retain the entire main lobe merely to acquire a PRN, but narrower filters
lose signal energy and code-delay resolution. The new source-1 report therefore
makes **assisted narrowband acquisition** worth testing; it does not make a
normal full-band GPS SDR stream fit over this SPI interface.

### Exploratory correlation

`tools/gnss_acquire.py` generates GPS PRNs 1..32, models a rectangular filter
and searches code phase/carrier offset with 1 ms coherent, noncoherently summed
windows. The first run used 30 ms, a modeled 31.25 kHz full passband and
offsets -40..+40 kHz in 1 kHz steps. Model filtering follows the hypothesized
carrier offset; the actual LoRa filter response is still unknown.

The largest peak/mean score was about **1.93 at L1 versus 2.09 in the lower
control**. These are search maxima, not C/N0, SNR or detection probabilities.
There is no calibrated false-alarm threshold, code tracking or navigation-data
decode. The similar maxima provide no convincing acquisition evidence. An
offset control is not guaranteed to remove all GNSS energy: spread-spectrum
energy also exists away from the carrier. See the [GNSS-SDR acquisition
methodology](https://gnss-sdr.org/docs/sp-blocks/acquisition/) for a validated
receiver's detection approach.

Next: establish source-1 rate/filter/gain first; obtain an L1 feed; use the
L76K's visible PRNs as an acquisition aid; search oscillator error plus satellite
Doppler; repeat on held-out captures. Require repeatable PRN/code-phase/Doppler
behavior and false-alarm controls before saying a satellite was received. A
position fix additionally requires observables, timing, ephemerides and a solver.

## Wi-Fi analysis: useful narrow slices

The LR2021 covers 2.4 GHz, but ordinary 20 MHz 802.11 captures are far wider
than this host export. Legacy OFDM uses 312.5 kHz subcarrier spacing
([Keysight's PHY reference](https://helpfiles.keysight.com/csg/89600B/Webhelp/Subsystems/wlan-ofdm/content/ofdm_80211-overview.htm)).
A narrow slice can measure local energy and burst timing. It cannot supply the
whole preamble and subcarrier set for a general packet decoder or normal CSI.
No wider-than-passband information can be restored by host interpolation.

The best next test uses an operator-owned AP sending a known on/off traffic
pattern. Compare captures in a channel's occupied region and outside it,
including offsets from channel center: a center null or spur can mislead.
Repeat at fixed gain and check clipping, full/overflow flags and recovery.
Report energy in the sampled slice and capture interval, not whole-channel
utilization. Sequential sweeps do not observe all frequencies simultaneously.

Promising extension: use the C6 Wi-Fi radio for packet/channel identity while
the LR2021 measures narrowband energy and BLE activity. Timestamp both streams
explicitly. This could reveal non-Wi-Fi interferers and coexistence effects;
timing correlation alone does not identify a transmitter.

## Prioritized experiments

| Priority | Angle | First useful result |
| --- | --- | --- |
| 1 | Source-1 versus source-2, FSK rate map, gain order | Measured format/filter/clock and loss behavior on this board |
| 2 | Controlled 2.4 GHz coexistence slices | AP on/off energy response with off-channel controls |
| 3 | L76K binary receiver diagnostics | Checksum-valid replies to version/port/clock/measurement queries |
| 4 | Assisted L1 PRN acquisition | Repeated held-out code/carrier peaks above measured null cases |
| 5 | Chip-side PRN correlation or filtered decimation | Investigate compute/SRAM feasibility before modifying ARC code |
| 6 | Passive reflections/phase stability | Establish oscillator coherence and reference channel first; no radar/ranging claim |

The fifth direction could bypass SPI by exporting accumulated correlations
instead of every sample. It requires a new chip-side implementation and measured
compute/memory budgets; there is no established public API for it here.

## Reproduce these bounded RX checks

Requires LakeShark firmware with `lora lrprobe`, pyserial, and NumPy for the
calculations/correlator. A new output directory is required each run.

```text
python tools/lr2021_research.py capture --port COM14 --plan gnss --out local-gnss --antenna "describe antenna, input and environment"
python tools/lr2021_research.py capture --port COM14 --plan wifi --out local-wifi --antenna "describe antenna, input and environment" --repeats 2
python tools/gnss_acquire.py local-gnss/gnss-0-1575420.ci16 --rate 125000 --passband 31250 --span 40000 --step 1000 --ms 30
```

Tools require matching echoed commands/prompts, reject incomplete/reordered
sample transfers and capture/FIFO/recovery errors, and hash accepted raw bytes.
They do not flash firmware, toggle reset lines, or transmit. `lrprobe` temporarily
owns and reconfigures the radio and uses its firmware recovery path afterward.
It may interrupt a current receiver, so run it while that radio is idle.

For the separate GNSS module, see [L76K/CASIC research](../l76k/README.md).
