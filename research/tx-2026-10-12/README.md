# TX research evidence — 2026-10-12

See [the TX discovery report](../../docs/tx-discovery.md).

`lr2021_tx_probe.inc` is a LakeShark driver include, not a standalone library
or drop-in addition to this repository's ESP32-P4 RX firmware. Its enclosing
driver supplies SPI, radio ownership, PA configuration and reset recovery.
Integrate before `lr20xx_clock_command` and dispatch `argv[1] == "lrtx-probe"`
to `tx_probe_command(argc, argv)`.

`evidence/` contains measured summaries and console register/FIFO results.
Full receiver recordings are retained locally; selected lossless excerpts
are included under `samples/`. Scripts
expect those recordings in `rf/` under their original capture names and require
Python plus NumPy. `test_rf_analysis.py` generates its own synthetic input.
Run it with `python test_rf_analysis.py` from this directory.

Mode names `raw0`, `raw1`, `raw2` are experimental labels, not Semtech names.
`result=ESP_OK` does not by itself establish waveform correctness. The first
`probe-raw0` trial underflowed; later v2/v3 trials supplied 128 bytes for N=64
and did not. The diagnostic has not been promoted into the portable TX API.

Original firmware images, flash backups, hardware serial identifiers and
machine-specific setup scripts are deliberately excluded from this package.

## Follow-up: phase reversals

Pattern 5 holds each of 16 random I/Q points for four sample clocks.
Tested at 4,800, 9,600 and 19,200 samples/s, without FIFO underflow.

`samples/` now contains lossless short excerpts of six actual HackRF captures,
including these follow-up trials. `manifest.json` records the original sample
offset and full/cropped SHA256 hashes. Crop boundaries are multiples of eight
samples, preserving downmix phase. The excerpts contain the complete tested
burst plus margins. Copy a sample into a local `rf/` folder before analysis:

```sh
mkdir rf
cp samples/v4-raw0-held4800.iq8 rf/
python analyze_hold.py v4-raw0-held4800
python analyze_phase_jumps.py v4-raw0-held4800
```

Analysis timestamps are relative to the supplied file; add the manifest offset
to compare cropped-file times with original evidence. Fits are exploratory,
not calibrated EVM or proof of reliable symbol transmission.
