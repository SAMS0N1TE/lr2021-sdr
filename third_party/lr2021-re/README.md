# lr2021-re (third-party copy)

A copy of [beala/lr2021-re](https://github.com/beala/lr2021-re) by Alex Beal,
MIT licensed (see [`LICENSE`](LICENSE)), taken at upstream commit
`1bedd87662eb71f03ae96021d248bf9373b49b90` (2026-10-30). Its own README is
kept as [`README.upstream.md`](README.upstream.md).

lr2021-re credits this repository for the 0x0290 to 0x0293 capture and TX
commands; this copy brings its findings and tools back here.

| Folder | Contents |
| --- | --- |
| `tools/` | Software LoRa decoder, MeshCore parser, collided-packet recovery (SIC), capture and comparison tools, ROM analysis helpers, and their 52 unit tests |
| `captures/` | IQ test vectors used by the tests (adverts from the author's own node) |
| `analysis/` | Opcode table, register fingerprints, JLI map, response lengths, measurement JSON, SIC report data |
| `docs/` | The upstream write-ups |
| `firmware/` | The upstream MeshCore debug console headers, for reference |
| `firmware/pram/` | The Patch RAM program (sweep, on-chip FFT, benchmarks): source and the checked-in `pram.bin` from upstream commit `2a59dc6e0695a4a837aaf8a736265512a3dca5fe` (2026-11-01). The bridge firmware embeds `pram.bin` for `pram` and `wfrun` ([debug console](../../docs/debug-console.md#programs-on-the-radios-own-cpu)). Rebuilding needs upstream's patched ARC GCC (`build.sh`, `docker/`) |
| `docker/` | ARC binutils image for disassembling a ROM read from your own chip |
| `logs/` | Gain sweep register dumps and packet status logs |

Left out: the SIC demo's generated mixtures, residuals and waterfall images
(`analysis/sic_demo*/*.cf32`, `*.png`, `analysis/sic_sweep/recovery.png`).
`python tools/sic_demo.py --out <dir>` and `python tools/sic_sweep.py`
regenerate them. As upstream, there is no ROM image or disassembly here.

The tools run unchanged:

```bash
python -m unittest discover -s third_party/lr2021-re/tools -p "test_*.py"
python third_party/lr2021-re/tools/lora_decode.py third_party/lr2021-re/captures/iter/c02.bin 83333.333
```

CTest runs the same suite as `lr2021re_tools` (skipped without numpy and
scipy). The hardware tools in this folder talk to the upstream MeshCore debug
console; for this repository's bridge firmware use the adapters in
[`tools/`](../../tools/) described in
[the LoRa software receiver guide](../../docs/lora-software-receiver.md).
