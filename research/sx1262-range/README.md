# SX1262 tuning range, gain and filters

[Research index](../../docs/research.md) / SX1262 range

The same questions as [Receive range and gain](../rx-range-and-gain/README.md),
asked of the SX1262 on the LilyGO T-Display-P4 SX1262 board: how far outside
its 150 to 960 MHz datasheet range the synthesizer locks, where the receiver
still hears a real signal, what the receive gain setting does, and which
undocumented settings work.

The SX1262 has no I/Q output, so everything here comes from what it does have:
`GetDeviceErrors` after each calibration and tune, the chip status, and
`GetRssiInst` in continuous GFSK receive with a narrow filter.

All results come from one board with its internal antenna selected. Levels are
the chip's own RSSI readings of board leakage, not calibrated sensitivity
figures.

## Summary

| Question | Result |
| --- | --- |
| Synthesizer lock, low window | **37 to 1447 MHz** |
| Synthesizer lock, high window | **2624 to 3494 MHz** |
| No lock | Below 37 MHz, 1448 to 2622 MHz, above 3494 MHz (`PLL_LOCK`) |
| Image calibration | `PLL_LOCK` below 40 MHz, `IMG_CAL` from 40 to about 113 MHz, clean from 114 MHz up |
| Strong direct reception | 360 to 1080 MHz (board markers 17 to 42 dB over the floor) |
| Weak reception | 1120 to 1400 MHz (1 to 14 dB, uneven); nothing at 1440 MHz |
| Below about 340 MHz | The strongest response sits 135 to 335 kHz away from the marker, and the offset moves with the filter setting |
| High window | Fixed-frequency signals 18 to 37 dB over the floor from 2640 to 3200 MHz, little or nothing above |
| Boosted against power saving gain | No change where board noise sets the floor; 1.7 to 1.9 dB lower floor at 1120 and 1400 MHz |
| Image products in band | About 30 dB down at 600 and 920 MHz; none visible at 400 MHz |
| Widest working GFSK filter | Code 0x08, about 0.9 MHz wide (datasheet maximum is 467 kHz) |
| Fastest valid RSSI step | About 1 ms per tune with a 58.6 kHz filter, no settling delay |

## Synthesizer and image calibration

**Method.** At each frequency: standby, `ClearDeviceErrors`, `CalibrateImage`
for the 4 MHz band around the tune, `GetDeviceErrors`, then `SetRfFrequency`,
`SetRx` continuous, `GetStatus`, `GetDeviceErrors` again and a few RSSI reads.
A 10 MHz pass from 30 to 1500 MHz and a 50 MHz pass from 1.5 to 4.09 GHz were
followed by 1 to 2 MHz passes over every edge.

| Frequency | Calibration | Receive |
| --- | --- | --- |
| 25 to 36 MHz | `PLL_LOCK` | `PLL_LOCK`, chip falls back to standby |
| 37 to 39 MHz | `PLL_LOCK` | Locks |
| 40 to 113 MHz | `IMG_CAL` (clean at a few points from 108 MHz) | Locks |
| 114 to 1440 MHz | Clean | Locks |
| 1441 to 1447 MHz | `PLL_LOCK` | Locks at most points |
| 1448 to 2622 MHz | `PLL_LOCK` | `PLL_LOCK` |
| 2624 to 2628 MHz | `PLL_LOCK` | Locks |
| 2630 to 3490 MHz | Clean | Locks |
| 3492 MHz and up | `PLL_LOCK` | `PLL_LOCK` from 3496 MHz |

`CalibrateImage` takes its band in 4 MHz units in one byte, so it cannot be
asked for anything above 1020 MHz. Above that the probe clamps it to 1016 to
1020 MHz; a clean result there only means the calibration ran.

The SX1262 frequency register goes to 4096 MHz, so the chip accepts every one
of these tunes without a command error. Only the lock flag and the status byte
show where it actually works.

## Receive response

**Method.** The board's 40 MHz clock leaks harmonics at every multiple of
40 MHz. At each multiple the receiver was swept across it, 500 kHz either
side in 5 kHz steps with a 9.7 kHz filter, and the RSSI at the marker was
compared with the median 200 to 500 kHz away. A received marker stays at the
same absolute frequency while the tune moves; a synthesizer that has not
locked, or a spur that follows the tune, does not.

The markers did more than stay put: their offset from the nominal frequency
grew in proportion to frequency, +5 kHz at 400 to 680 MHz, +10 kHz around
1000 MHz and +14 kHz at 1400 MHz. That is a steady 10 ppm difference between
the board clock and the radio's reference, which is what a correctly tuned
receiver shows when it hears a signal made from a different clock.

| Marker | Over floor | Notes |
| --- | --- | --- |
| 40, 80, 120 MHz | 2 to 6 dB at the marker | Stronger response 135 kHz above it (see below) |
| 160 to 320 MHz | 4 to 19 dB at the marker | Stronger response 135 or 225 kHz above it at most markers |
| 360 to 1080 MHz | 17 to 42 dB | Clean direct reception |
| 1120 MHz | 13 dB | |
| 1160 to 1360 MHz | 1 to 9 dB | 1240 MHz showed nothing |
| 1400 MHz | 14 dB | |
| 1440 MHz | None | Lock is intermittent here |
| 2640 to 3080 MHz | 22 to 37 dB (strongest line) | See the high window below |
| 3120 to 3200 MHz | 18 to 24 dB (strongest line) | |
| 3240 to 3480 MHz | Under 9 dB, mostly none | |

**Below about 340 MHz.** Up to 320 MHz the strongest response to each marker
is not at the marker. With 4.8 to 19.5 kHz filters it appears when the tune is
135 kHz above the marker (80 to 200 MHz) or 225 kHz above it (160 to
320 MHz). With a 58.6 kHz filter the same responses move to 200 and 335 kHz.
A separate emitter would not move when the filter changes, so this is the
receiver's own intermediate frequency at work: an image or wrong-sideband
response. At 320 MHz it is 28 dB stronger than the direct response. From
360 MHz up the direct response wins and does not move with the filter.

In practice, FSK reception below about 340 MHz behaves as if the receiver
were mistuned by that offset. Tuning F plus the offset to hear F follows from
these results but has not been tried on a real transmission.

**The high window.** Between 2640 and 3200 MHz each 40 MHz multiple brings a
comb of lines about 64 kHz apart, strongest about 185 kHz below the multiple.
Those lines stay at the same frequency when the filter changes from 4.8 to
156 kHz, and their offset drifts with the same 10 ppm clock difference, so the
receiver is hearing real signals at those frequencies. The comb itself is a
property of the leakage, so these runs show that the receive chain works up
there but do not pin down its frequency accuracy to better than about 25 kHz.
A 467 kHz wide pass found a 4 MHz spaced family of lines next to each multiple
and a floor near -96 dBm.

## Gain

Register 0x08AC selects power saving (0x94) or boosted (0x96) receive gain.
Each marker was swept with a 58.6 kHz filter, 32 reads a step:

| Marker | Power saving: marker / floor | Boosted: marker / floor |
| --- | --- | --- |
| 400 MHz | -93.8 / -117.5 dBm | -93.2 / -117.3 dBm |
| 600 MHz | -79.2 / -112.2 dBm | -79.1 / -111.5 dBm |
| 920 MHz | -80.7 / -110.8 dBm | -81.2 / -110.8 dBm |
| 1120 MHz | -112.7 / -122.0 dBm | -112.2 / -123.7 dBm |
| 1400 MHz | -113.0 / -122.3 dBm | -113.1 / -124.2 dBm |
| 2840 MHz | -107.0 / -111.7 dBm | -106.7 / -111.7 dBm |

From 400 to 920 MHz the floor in a 58.6 kHz filter sits 10 to 15 dB above
thermal noise, so on this board the receiver hears the board before it hears
its own noise, and the gain setting cannot help. At 1120 and 1400 MHz, where
the board is quieter, boosted gain lowers the floor by 1.7 to 1.9 dB, about the
datasheet's figure.

Other values written to 0x08AC (0x90, 0x92, 0x97, 0x9E) behaved like one of the
two documented ones. 0x00 raised the marker reading by about 4 dB at 400, 920
and 2840 MHz with an unchanged floor. Without a calibrated source it is not
known whether that is real gain or a shifted RSSI reading.

## GFSK filter widths past the datasheet

The bandwidth byte looks like a two-bit mantissa (bits 3 and 4) and a three-bit
exponent (bits 0 to 2); the datasheet table only uses exponents 1 to 7. Each
code was measured by sweeping across the isolated 400 MHz marker and taking
the span within 6 dB of the peak:

| Code | Datasheet | Measured 6 dB span |
| --- | --- | --- |
| 0x19 | 312 kHz | 280 kHz |
| 0x11 | 373.6 kHz | 340 kHz |
| 0x09 | 467 kHz | 420 kHz |
| 0x18 | Not listed | 590 kHz |
| 0x10 | Not listed | 720 kHz |
| 0x08 | Not listed | 910 kHz |
| 0x01 | Not listed | 910 kHz, same as 0x08 |
| 0x00, 0x20 | Not listed | Flat at about -90 dBm across 16 MHz; the marker is lost |
| 0x28 | Not listed | Same as 0x08 (bit 5 ignored) |

The chip accepted every code without a command error. Exponent 0 doubles the
widest documented filters, as the pattern predicts, and is usable. Codes with
both fields zero are not.

## RSSI sweep speed

Each step is standby, `SetRfFrequency`, `SetRx`, an optional wait and one or
more `GetRssiInst` reads, over a 2 MHz SPI bus. Across the 920 MHz marker:

| Filter | Wait | Time per step | Marker seen |
| --- | --- | --- | --- |
| 58.6 kHz | None | 1.0 ms | Yes, same level as with 2 ms |
| 58.6 kHz | 1 ms | 1.9 ms | Yes |
| 4.8 kHz | 0.3 or 1 ms | 1.1 to 1.9 ms | No |
| 4.8 kHz | 4 ms | 5.7 ms | Yes |

So about 1000 tunes a second with a wide filter, and narrow filters need
several milliseconds to settle. Running `CalibrateImage` at every step adds
about 13 ms.

## Compared with the LR2021

| LR2021 result | SX1262 on this board |
| --- | --- |
| Receives 100 to 2740 MHz continuously | Two windows: 37 to 1447 MHz and 2624 to 3494 MHz, with a gap from 1448 to 2622 MHz |
| 108 to 137 MHz airband opened | Locks and calibrates from 114 MHz, but the receiver favours a response about 135 kHz off |
| 1 to 1.5 GHz gap opened | Up to 1447 MHz, with markers 4 to 14 dB over the floor against 30 to 40 dB in band |
| 2.5 to 2.7 GHz opened | 2624 to about 3200 MHz receives, past the LR2021's 2740 MHz top, but RSSI only |
| Bluetooth LE at 2.4 GHz | Out of reach: no lock between 1448 and 2622 MHz |
| I/Q stream | None: RSSI and the built-in demodulators only |
| Image rejection about 41 dB | About 30 dB at 600 to 920 MHz; inverted below about 340 MHz |
| 13 gain steps plus boost; boost adds up to 1.4 dB | One gain register: about 2 dB, and only where the board is quiet |

## Reproduce

The measurements used one console command in a LakeShark development build:

`lora sxprobe <start_kHz> [stop_kHz] [step_kHz] [bw] [boost|save|0xNN] [reads] [settle_us] [none|once|each]`

It steps the SX1262 from start to stop (at most 2001 points), with `bw` either
a GFSK bandwidth in Hz from the datasheet table or a raw code such as `0x08`,
the 0x08AC gain value, RSSI reads per step, the wait after `SetRx`, and whether
to run image calibration never, at the first step or at every step. It prints
one line per step with the calibration and receive error flags, both status
bytes, the mean, highest and lowest RSSI and the step time. It never
transmits, and it puts back the LoRa configuration and receive that were
running before.

## Limits of these results

- One board, one antenna, one location. The internal antenna and its matching
  are built for the 868 and 915 MHz bands, so levels outside them say as much
  about the board as about the chip.
- The markers are leakage from the board itself: they prove the receive chain
  works at a frequency, not how sensitive it is there.
- The lock windows belong to this chip at this temperature and may move a few
  MHz on other parts.
- The SX1262 reports RSSI in 0.5 dB steps and floors at -127.5 dBm, which
  hides the noise floor with filters narrower than about 20 kHz.
