# LR2021-SDR research wiki

- [TX discovery](TX-Discovery): RF-verified FIFO-fed magnitude/phase IQ under OOK setup; frequency modes also tested.
- [RX protocol](https://github.com/SAMS0N1TE/lr2021-sdr/blob/main/docs/protocol.md).
- [Live RX results](https://github.com/SAMS0N1TE/lr2021-sdr/blob/main/docs/live-streaming.md).

The portable library remains RX-only. The experimental LakeShark TX integration
now supports uploaded I8/Q8 playback with decoded payloads through 125 ksample/s.
[Using TX playback](https://github.com/SAMS0N1TE/lr2021-sdr/blob/main/docs/tx-playback.md).

## Existing sensor format independently decoded

Stock rtl_433 25.12 decoded synthetic Acurite-606TX readings of 23.4 C,
-12.3 C and 37.8 C from HackRF recordings of LR2021 uploaded I/Q TX.
IDs, channels, battery flags and checksums matched. The deliberately bad
checksum produced no accepted reading; a half-amplitude acquisition failure
is also archived. Tests used 915 MHz, not the sensor's normal RF band, so
physical display compatibility is not established.

[Generator, results and replayable evidence](https://github.com/SAMS0N1TE/lr2021-sdr/tree/main/research/tx-temperature/).
