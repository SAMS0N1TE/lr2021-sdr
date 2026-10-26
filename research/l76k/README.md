# L76K binary protocol checks

[Research index](../../docs/research.md) / L76K

The LakeShark GPS module is a **separate receiver** from the LR2021. It kept
a normal position fix and NMEA output during these tests. We looked past the
NMEA interface for per-satellite measurements or receiver diagnostics, not
for a new LR2021 mode.

The [L76K protocol specification](https://files.waveshare.com/upload/d/dd/Quectel_L76K_GNSS_Protocol_Specification_V1.1.pdf)
defines CASIC binary framing and configuration commands. Broader CASIC
message families in the [gpsd driver](https://gitlab.com/gpsd/gpsd/-/blob/master/drivers/driver_casic.c)
were leads to test, not promises that this module firmware implements them.

## Method

An experimental LakeShark build tapped UART2 bytes **before** the NMEA line
parser and passed bounded, allowlisted CASIC requests to the GPS module.
COM14 was the LakeShark console, not the GPS UART. The bridge checked CASIC
frame checksums and capture offsets, then returned to normal NMEA operation.
No persistent save, firmware write or GPS restart was sent. Location-bearing
UART captures remain local.

## Results on this board

The receiver reported a valid fix and normal GGA/GSA/GSV/RMC output with no
reported NMEA checksum errors. A passive 700 ms capture contained NMEA and no
CASIC frames. These results show ordinary GPS operation, not chipset identity
or a raw-observation interface.

| CASIC request | Result |
| --- | --- |
| Port `06/00` and update rate `06/04` | Checksum-valid replies and ACKs. The reported port was 115200 baud; the update period was 1000 ms. |
| Message-rate inventory `06/01` | 38 checksum-valid records. Candidate measurement messages `03/10` and `03/11` were disabled. |
| Empty version, hardware and navigation-clock polls | Explicit checksum-valid NAKs for the tested IDs and payload form. |
| Empty receiver-measurement `03/10` poll | Explicit NAK. |
| One-shot requests and bounded rate-one streams for `03/10` and `03/11` | Requests were ACKed, but no corresponding binary measurement frames appeared. Each stream test ran for 1.8 s, automatically stopped, and a follow-up inventory confirmed its rate returned to zero. |

The bridge reported zero dropped bytes in those capture windows. The GPS
module retained its fix and normal NMEA output. **No raw satellite
measurements, IQ export, hidden bootloader or arbitrary-frequency receiver
mode has been demonstrated.** An ACK or an inventory entry alone does not
show that a useful measurement payload exists.

## Next check

If a direct GPS UART tap becomes available, compare its untouched byte stream
with the on-device bridge, then test firmware-specific message variants one
at a time. The early useful result would be valid per-satellite observations
with understood units and epochs. Differential or RTK experiments would also
need carrier phase and a reference source; none is established here.

The binary bridge and its raw captures are in a local experimental LakeShark
build. They are not features of the public standalone LR2021 firmware.
