# Datasheet and continuity research

Original offline review: 2026-10-11. Subsequent [hardware validation](extended-fifo.md)
confirms the extended FIFO can serve raw I/Q on the tested ROM. The research
questions below record what was unknown before those tests.

## Correct chip reference

[LilyGO T-Display-P4](https://github.com/Xinyuan-LilyGO/T-Display-P4) links
`LR2021_22_12_Datasheet_v2.1.pdf` in its README. The referenced file was absent
from its current main tree. This is a chip-family document, unlike NiceRF's
LoRa2021 module manual numbered v1.3. No Semtech LR2021 revision 1.3 was verified.

Retrieved and parsed these Semtech-authored documents from public mirrors:

- [Rev. 2.1, 13 April 2026, 243 pages](https://mm.digikey.com/Volume0/opasdata/d220001/medias/docus/8924/600_62785538.LR20xxDatasheet_V2_1.pdf).
- [Rev. 2.2, 29 July 2026, 250 pages](https://www.rlocman.ru/i/File/2026/09/09/LR20xxDatasheet_V2_2.pdf).
  SHA256: `7e55a15dcdbe044dd615eb1f1ea21c26bf8e28758d94face240998e5fbad25c2`.

The v2.2 FIFO table was also checked visually. Vendor PDFs are not included in
this repository. These document revisions do not establish the firmware version
or feature behavior of a particular board.

## Highest-value lead: extended RX FIFO

Rev. 2.2 section 5.3.3, pages 80-81, documents moving each FIFO from 256 to
1024 bytes. Configure before reception. CalibFE overwrites the extended FIFO
storage; pending samples cannot survive that operation. Sleep retention needs
explicit setup. Low-threshold IRQ values above 256 require the supplied workaround.

The official [FIFO driver](https://github.com/Lora-net/usp/blob/master/smtc_rac_lib/radio_drivers/lr20xx_driver/src/lr20xx_radio_fifo.c)
implements RX relocation with `0xF30028 = 0x804400` and
`0xF30030 = 0x3E8`. TX uses `0xF3002C = 0x804000` and
`0xF30034 = 0x3FC`. The RX size value is an encoding, not an instruction to
allocate a 1000-byte host buffer. The datasheet labels it 1024 bytes, but its
RX size arithmetic description is not self-consistent enough to infer the
precise overflow boundary. Measure that boundary on the target ROM.

The [workaround implementation](https://github.com/Lora-net/usp/blob/master/smtc_rac_lib/radio_drivers/lr20xx_driver/src/lr20xx_workarounds.c)
first configures ordinary IRQs with zero low thresholds, then writes the full
RX/TX threshold registers. Retention helpers also cover those registers.

This corrects any interpretation of our earlier 256-byte budget as a silicon
maximum. It is the configuration assumed by our current capture code. Packet
FIFO support does not prove that undocumented source-2 I/Q uses the relocated
storage correctly, or that this works on our tested ROM 1.24 without a patch.

### Calculated service budget

At the previously measured 125000 IQ16 pairs/s, input is 500000 bytes/s.
These are capacity calculations, not simulated or measured hardware results.

| Capacity assumption | Empty-to-full time | Remaining time at 128-byte occupancy |
| --- | --- | --- |
| Current 256 bytes | 512 us | 256 us |
| Conservative 1000 bytes | 2000 us | 1744 us |
| Documented nominal 1024 bytes | 2048 us | 1792 us |

At 16 MHz SPI, a 128-byte payload alone takes 64 us; the radio produces another
32 bytes during that transfer. Average drainage must still exceed input, after
command, BUSY, flag-check and scheduling costs. A bigger FIFO absorbs brief
stalls; it cannot repair sustained under-service or samples already lost.

### Required implementation and target checks

1. Record ROM/patch version and current FIFO registers. Establish exclusive
   ownership of the extended region before changing it. RX at 0x804400 overlaps
   inferred SRAM banks 3, 5 and 6; do not run the bank-capture experiment there
   at the same time. Also account for calibration scratch use.
2. Read back base/size after normal setup, after source-2 setup, and after
   starting RX. This detects a route or patch restoring default registers.
   Inspection of helper 0x10CBC found no direct writes to the two relocation
   registers; its callees and the complete setup path are not yet ruled out.
3. Make effective FIFO capacity explicit in the C capture API. The current
   `level >= 256` rejection must not be mistaken for hardware failure after
   relocation. Preserve bounded read chunks; enlargement does not require a
   single 1024-byte SPI transaction. Audit counter widths and IRQ thresholds.
4. With a known nonrepeating RF test sequence, measure FIFO level, wrap behavior,
   sample ordering and overflow flags. Verify recovery and restoration after
   both success and injected failure. A CW tone cannot prove all samples exist.
5. Compare the default and extended configurations under identical P4 UI/audio
   load. Record maximum service interval, BUSY duration, occupancy and missing
   samples, not just average throughput. Repeat across warm/cold initialization.

Do this before attempting a custom ARC capture scheduler: it is a documented,
smaller change with a plausible fourfold improvement in stall tolerance.

## Useful findings outside the LR2021 documentation

### SRAM ownership: lessons from ESP-SDR

[ESP-SDR's spectrum implementation](https://github.com/ESPARGOS/esp-sdr/blob/main/docs/spectrum.md)
describes bank ownership and reader coordination. On ESP32-C3, CPU reads while
the radio owns a bank can return a repeating bus pattern instead of samples.
That is not evidence of the same LR2021 behavior, but it makes coherent-access
testing essential: nonzero data or an attractive FFT is insufficient. Compare
live reads to stopped snapshots and use a sequence that exposes repetition.
Its hardware-specific ownership registers cannot be transplanted to LR2021.

### Decimation location matters

[ESP-SDR's continuous stream](https://github.com/ESPARGOS/esp-sdr/blob/main/docs/iq-stream.md)
filters and decimates on a separate core, with sample indices and explicit
loss metadata. Its offset-tuning and complex rotation also offer a useful
experiment for moving a channel away from DC leakage.

For LR2021, filtering on the P4 helps downstream CPU/storage bandwidth only:
the original samples have already crossed the radio FIFO and SPI. To reduce
that bottleneck, filtering, decimation or narrower sample packing must happen
inside the LR2021 before export. An ARC extension therefore needs a measured
cycle budget and an anti-alias filter. Dropping every Nth sample is not a
quality-preserving substitute. Offset tuning also needs verified capture
bandwidth, spectral orientation and filter response on this radio.

### Honest stream timing

[UHD receive metadata](https://files.ettus.com/manual/structuhd_1_1rx__metadata__t.html)
distinguishes overflow, sequence errors and timestamps. Apply that separation
to our stream: generation, first-sample index, count, and discontinuity reason.
A host receipt timestamp is not a hardware sample timestamp. If the exact
number of lost samples is unknown, report unknown loss and reset affected
decoder state instead of inventing consecutive indices. This also makes
FSK-versus-I/Q quality comparisons meaningful.

### Lower-bandwidth export may not be I/Q

[TI's CC1200 documentation](https://www.ti.com/product/CC1200) distinguishes
synchronous and transparent serial modes. Their existence on another radio
does not establish such a port on LR2021. The useful research question is
whether an LR2021 internal source exposes soft discriminator values, timing
information, or complex samples. Classify each route before calling it I/Q.
A scalar discriminator path may reduce C4FM export traffic, but it does not
preserve all the information needed for a general complex-signal receiver.

## Current conclusion

The extended FIFO is the next practical continuity experiment. P4 scheduling
and a bounded producer queue remain necessary. A coherent overlapping SRAM
ring or on-radio decimator remains a second path if FIFO servicing still
fails. No result here establishes gap-free capture, better P25 voice than the
FSK path, or a working continuous custom firmware image.
