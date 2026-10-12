# TX research evidence — 2026-10-12

See [the TX discovery report](../../docs/tx-discovery.md).

`lr2021_tx_probe.inc` is a LakeShark driver include, not a standalone library
or drop-in addition to this repository's ESP32-P4 RX firmware. Its enclosing
driver supplies SPI, radio ownership, PA configuration and reset recovery.
Integrate before `lr20xx_clock_command` and dispatch `argv[1] == "lrtx-probe"`
to `tx_probe_command(argc, argv)`.

`evidence/` contains measured summaries and console register/FIFO results.
Raw receiver recordings are retained locally, not included in Git. Scripts
expect those recordings in `rf/` under their original capture names and require
Python plus NumPy. `test_rf_analysis.py` generates its own synthetic input.
Run it with `python test_rf_analysis.py` from this directory.

Mode names `raw0`, `raw1`, `raw2` are experimental labels, not Semtech names.
`result=ESP_OK` does not by itself establish waveform correctness. The first
`probe-raw0` trial underflowed; later v2/v3 trials supplied 128 bytes for N=64
and did not. The diagnostic has not been promoted into the portable TX API.

Original firmware images, flash backups, hardware serial identifiers and
machine-specific setup scripts are deliberately excluded from this package.
