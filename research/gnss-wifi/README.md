# GNSS and 2.4 GHz receive checks

[Research index](../../docs/research.md) / GNSS and 2.4 GHz

These LakeShark captures show that the LR2021 tunes to GPS L1 and 2.4 GHz
and returns samples. **They do not show a satellite acquisition, a GNSS
position, a Wi-Fi packet decode or CSI.**

## Hardware and method

The receiver was a LilyGO T-Display-P4 LR2021 on ROM 1.24. The operator had
a Wi-Fi antenna on MMCX1 and no GNSS-specific antenna. The RF switch position
was not read back. The initial captures used the existing 16-bit source-2
path, fixed gain 13, boost 4, and 40 ms windows. These were receive-only
tests. Raw I/Q, NMEA coordinates and console logs remain local.

| Center frequency | Configured rate | Result |
| --- | ---: | --- |
| 1575.020, 1575.420 and 1575.820 MHz | 125k complex pairs/s | Three short captures without reported FIFO loss; no satellite signal identified. |
| 2412, 2437 and 2462 MHz | 250k complex pairs/s | Samples arrived; one repeat at 2462 MHz overflowed and was rejected. No Wi-Fi traffic was identified. |

All tuning attempts returned without chip error bits and restored normal LoRa
RX. A clean FIFO flag alone cannot prove sample-index continuity.

## GPS L1

GPS L1 C/A uses a 1575.42 MHz carrier and a 1.023 Mchip/s spreading code
([GPS interface specifications](https://www.gps.gov/interface-control-documents-icds-interface-specifications-iss)).
Tuning to L1 is not evidence of satellite reception: detection needs a PRN
code and Doppler search against a calibrated noise/control result. The
initial 31.25 kHz source-2 L1 search found similar maxima on-frequency and
at a lower-frequency control. We do not count it as an acquisition.

The LR2021's export rate is far narrower than a conventional GPS L1 SDR
capture. A narrowband assisted search may still be possible, with a signal
energy and delay-resolution penalty. Before interpreting such a search, we
need an L1 antenna/feed, a measured analog passband, a verified I/Q sample
clock and continuity, and repeatable PRN/code-phase/Doppler behavior.
The separate onboard GNSS module has its own antenna and receiver path; its
fix does not demonstrate reception through the LR2021. Its binary protocol
checks are in the [L76K note](../l76k/README.md).

[PocketSDR](https://github.com/tomojitakasu/PocketSDR) provides a useful
host-side reference for GPS acquisition and accepts interleaved IQ8 input.
Its normal RF front ends are wider than the LR2021 export tested here, so its
sensitivity results cannot be transferred to this hardware.

## Wi-Fi and other 2.4 GHz signals

A typical Wi-Fi channel is much wider than these IQ slices. The LR2021 can
measure energy and timing inside one narrow slice, but the current data
cannot reconstruct a full 20 MHz packet or normal CSI.

The LR2021's packet engine did decode BLE 1M advertisements on channels 37,
38 and 39 in separate eight-second receive-only windows. Tests of BLE 2M
and coded S2/S8 returned no frames, but no matching transmitter was known to
be present; those zero counts are inconclusive. [TheClams' LR2021 driver](https://github.com/TheClams/lr2021/tree/402a12ba350c7a5419acf595ca6ea529403d2dbc)
contains an undocumented `0x0230` setup lead for those PHYs. It is a lead for
a controlled packet-mode comparison, not a verified ROM 1.24 contract.
