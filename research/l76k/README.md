# L76K: look beyond the NMEA interface

[Research index](../../docs/research.md)

2026-10-26. This is research on the **separate GNSS module** in LakeShark,
not another LR2021 mode. No raw GNSS observations, IQ export, hidden bootloader
or arbitrary-frequency receiver mode has been demonstrated.

## What is already established

The installed LakeShark reports UART2 RX GPIO22/TX GPIO23 at 115200 baud.
Its GNSS receiver had a valid fix, tracked 11 satellites in the initial snapshot
and emitted NMEA GGA/GSA/GSV/RMC with no reported checksum errors. The receiver
was configured for GPS, BeiDou and GLONASS at 1 Hz. Location-bearing logs remain
local. These observations establish normal GNSS operation, not chipset identity
or a raw-data interface.

LakeShark's driver is configured as an L76K and uses `PCAS` commands. The
[Quectel L76K protocol V1.1](https://files.waveshare.com/upload/d/dd/Quectel_L76K_GNSS_Protocol_Specification_V1.1.pdf)
also defines CASIC binary framing and configuration queries. A line-based NMEA
parser does not preserve arbitrary binary responses; `gps raw` is a window of
framed text, not a byte-for-byte UART recorder.

## The useful protocol lead

The chipset family's broader protocol, and the [gpsd CASIC header](https://gitlab.com/gpsd/gpsd/-/blob/master/include/driver_casic.h),
name additional message families. The [gpsd driver](https://gitlab.com/gpsd/gpsd/-/blob/master/drivers/driver_casic.c)
decodes several navigation messages and polls version/port information. This
is a concrete lead beyond the smaller L76K manual, **not evidence that every
family command works in the module's firmware**.

| Probe | Class, ID | Interpretation and current status |
| --- | --- | --- |
| Current port | 06,00 | L76K-documented query; inspect protocol mask without changing it |
| Current update rate | 06,04 | L76K-documented query; establish baseline |
| Version | 0A,04 | Broader-family candidate; identify software/hardware if answered |
| Hardware diagnostics | 0A,09 | Broader-family candidate; preserve unknown fields |
| Navigation clock | 01,11 | Broader-family candidate; clock bias/drift possibilities need a verified layout |
| Receiver measurements | 03,10 | RXM-MEASX candidate; the most interesting lead for raw observations |

All live polls in `tools/casic_probe.py` have empty payloads. It does not send
configuration setters, persistent saves, firmware writes, restart requests or
arbitrary numeric command IDs. Broader-family polls are optional named probes,
not proven side-effect-free operations on every firmware. Start with passive
capture and documented port/rate polls. A missing response can mean disabled
binary input/output, firmware limitations, wiring or capture loss; it is not
proof that hardware lacks the feature.

## The important distinctions

1. **Binary navigation** is a richer version of position, velocity, clock or
   satellite status. It is not raw RF.
2. **Raw observables**, if RXM-MEASX actually supplies them on this firmware,
   might support pseudorange/Doppler/carrier experiments. A recognizable message
   ID alone does not prove its fields, units, validity or ambiguity handling.
3. **Raw I/Q** would require an ADC/baseband export mechanism. No command or
   verified route for that is known here. A GNSS chip's RF front end is not
   automatically a tunable general-purpose SDR.

The most useful early win would be valid per-satellite measurements or clock
diagnostics. Differential positioning/RTK would additionally need usable carrier
phase, synchronized epochs and reference data; none is established by NMEA.
GNSS RF fingerprinting or a low-cost interference monitor could begin with
per-satellite C/N0 and clock residuals, but those are not independent interference
or spoofing detections.

## How to test without discarding the response

COM14 is LakeShark's **console**, not the GPS UART. Sending a CASIC frame there
does not send it to the module. The current console exposes `gps`, `gps sky`,
`gps raw`, `gps baud` and `gps reset`; it has no arbitrary binary UART bridge.
The live session used status/text observation only. It did **not** send binary
polls to the onboard GNSS receiver.

For a direct 3.3 V UART connection, keep one UART transmitter/reader owner and
avoid connecting a second output driver to the module RX while the P4 drives it.
Use a byte tap/logic analyzer for passive capture first. On a direct GNSS UART:

```text
python tools/casic_probe.py query version
python tools/casic_probe.py listen --port DIRECT_GNSS_UART --seconds 5 --out local-passive
python tools/casic_probe.py listen --port DIRECT_GNSS_UART --poll port --seconds 5 --out local-port
python tools/casic_probe.py listen --port DIRECT_GNSS_UART --poll version --seconds 5 --out local-version
python tools/casic_probe.py inspect local-version/uart.bin
```

For an on-device follow-up, add a bounded **binary byte tap before text
framing** and queue named queries to the existing GPS reader task. That task
must remain UART owner. Capture response bytes and overflow counts for a fixed
window, preserve NMEA parsing, and print/export only after the window closes.
Do not replace `gps raw` or race a second `uart_read_bytes` against the parser.
This firmware addition has not been implemented or flashed in this change.

The host tool recognizes `BA CE`, validates aligned bounded payload length and
the little-endian word checksum, and retains unknown payload bytes. It reports
truncation/checksum errors and never labels a packet as proven raw observations.
Captures and decoded payloads can contain location; review before publication.

## Investigation ladder

1. Record exact module markings, board revision and any boot TXT/version response.
   Do not assume all Quectel modules or all similarly named products use the same
   chipset/protocol. The vendor lists the AT6558R family separately
   ([manufacturer](https://www.icofchina.com/)).
2. Collect an untouched UART stream; identify binary frames and checksum-valid
   replies before enabling any new stream.
3. Query documented baseline settings, then optional version/hardware/clock and
   measurement candidates individually. Compare NMEA cadence and fix health
   before/after. Preserve an explicit no-response outcome.
4. If measurements exist, validate against known stationary geometry and an
   independent GNSS receiver; derive units/epochs from a matching protocol
   revision and verify invalid-value behavior. Do not infer carrier phase from
   a changing integer field.
5. Investigate PPS routing from the exact schematic. The current LakeShark board
   definition exposes RX/TX and wake, not a PPS input. UART arrival time is not
   a precision PPS timestamp and cannot directly discipline the LR2021 clock.
6. Only after identification and a recoverable setup, inspect matching public
   firmware/update formats or board test pads for an alternate diagnostic
   interface. There is currently no demonstrated undocumented IQ entry point.

The broader CASIC spec is published by the [chipset vendor](https://icofchina.com/d/file/xiazai/2020-09-22/20f1b42b3a11ac52089caf3603b43fb5.pdf),
but its PDF could not be retrieved during this session. Candidate IDs above
were checked against gpsd source, not against an unseen copy of that PDF.
