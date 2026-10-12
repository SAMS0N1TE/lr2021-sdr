# P25 Phase 1 voice-data transmission verified over RF

Measured 2026-10-12 on LR2021 ROM 1.24 with the existing v13 LakeShark TX
firmware. **Software-generated P25 C4FM voice-data bursts transmitted through
raw I/Q decoded in unmodified DSD-neo, and the recovered IMBE data synthesized
the expected 1,011 Hz test tone.** No firmware rebuild or reflash was needed.

This adds a concrete software-defined radio function to LakeShark: the LR2021
can transmit P25 voice-data test bursts even though its packet interface does
not provide a P25 framer or voice encoder. This demonstration uses pre-encoded
test-tone frames; continuous microphone speech is not implemented.

## Measured results

| Observation | First RF capture | Independent repeat |
| --- | ---: | ---: |
| NAC | `0x293` | `0x293` |
| Talkgroup / source ID | 4242 / 123456 | 4242 / 123456 |
| LDU1 / LDU2 / TDU decoded | 2 / 2 / 2 | 2 / 2 / 2 |
| Recovered IMBE voice frames | 36 | 36 |
| Match to software-reference IMBE data | All 36 exact | All 36 exact |
| IMBE logged error counts | All `[0] [0]` | All `[0] [0]` |
| DSD total header errors | 1 | 3 |
| DSD irrecoverable header errors | 0 | 0 |
| Recovered audio | 0.72 s | 0.72 s |
| Audio FFT peak | 1,011.11 Hz | 1,011.11 Hz |

Across both captures, **72 recovered 88-bit IMBE frames match the reference**.
Do not describe the complete RF stream as error-free: DSD reports the header
errors shown above. The post-FEC voice data matches, and no irrecoverable
header errors were reported. A received 0.5-second pre-burst noise segment
produced zero P25 or IMBE frames using the same processing.

Both radio runs transmitted 15,552 complex samples / 31,104 FIFO bytes at
19,200 samples/s, taking about 812 ms including overhead. Each uploaded
7,776-sample buffer was repeated twice. Final FIFO flags were `07`, FIFO level
zero, radio errors zero, and transmit/stop/recovery all returned `ESP_OK`.

## Signal and independent validation

The packed stream consists of **LDU1 + LDU2 + TDU**, with an unencrypted
standard 1,011 Hz IMBE test pattern. It uses the late-entry voice sequence;
there is no HDU in the successful RF fixture. OP25's existing `p25craft.py`
generated the protocol bits, including sync, NID, status symbols and FEC.
The local modulator maps dibits to nominal deviations of +600, +1800, -600
and -1800 Hz at 4,800 symbols/s, applies a finite C4FM transmit filter, and
integrates frequency into phase. The result is quantized signed I8/Q8 with
nominal radius 48. The upload includes 96 acquisition symbols and 48 trailing
symbols around the packet stream.

RF was **915 MHz, configured -9 dBm**. HackRF only received: LO 914.750 MHz,
2 Msamples/s, LNA gain 16 dB, VGA gain 32 dB, amplifier off. Complete
three-second captures are losslessly compressed in [samples](samples/).

The receiver performs fixed frequency translation, decimation, a 7 kHz FIR,
ordinary FM discrimination, fixed amplitude gating and conversion to 48 kHz
PCM. Stock **DSD-neo v2.10.0 (`ff5843f`), MBElib-neo 2.1.0** then decodes the
P25 stream. No expected payload, NAC, talkgroup, timing or bit corrections
are supplied to DSD. Reference comparison happens after decoding.

DSD's direct live WAV output was empty in this setup. For the audio check,
the tool packages the **actual recovered IMBE frames and their logged error
counts** into DSD's `.imb` file format and invokes stock DSD file playback.
That produces the archived 8 kHz stereo WAV files. This is a two-stage decoder
and vocoder test, not a claim that the live audio-output path is integrated.
No audio is played through the computer or board speakers.

An initial **offline HDU-containing baseline** misdecoded the header's
encryption metadata. Its [log](reference/hdu-baseline-failed.log) is retained;
it was not used as RF success evidence. The successful late-entry LDU2
decoded clear-audio algorithm ID `0x80`, key ID zero and message indicator zero.
HDU interoperability, arbitrary speech, continuous TX and spectral conformance
remain unverified. This fixed 915 MHz experiment does not establish operation
with a commercial P25 radio on another band.

## Reproduce the transmitted waveform

Install NumPy and follow the firmware/preflight instructions in
[TX playback](../../docs/tx-playback.md). From the repository root:

```text
python tools/lr2021_make_p25.py research/tx-p25/inputs/voice-tone.p25 p25.iq8
python tools/lr2021_tx_upload.py p25.iq8 --serial YOUR_USB_SERIAL --send 19200 --repeats 2
```

The first command exactly reproduces [voice-tone.iq8](inputs/voice-tone.iq8).
The second explicitly transmits an approximately 810 ms burst. The existing
upload limits and magnitude checks still apply. This tool modulates already
framed P25 bytes; it does not encode microphone audio.

## Replay the recordings without transmitting

Obtain the official [DSD-neo v2.10.0 Windows release](https://github.com/arancormonk/dsd-neo/releases/tag/v2.10.0)
and keep its DLLs beside the executable. Then:

```text
python research/tx-p25/replay.py --dsd PATH_TO/dsd-neo.exe
```

This checks both RF captures, recovered identifiers, all 72 IMBE frames, the
audio tone and the received-noise control. For saved decoder/audio artifacts:

```text
python tools/lr2021_decode_p25_capture.py research/tx-p25/samples/v13-p25-voice19200.iq8.gz --dsd PATH_TO/dsd-neo.exe --output p25-replay
```

Already-produced evidence is available for the
[first capture](evidence/v13-p25-voice19200/),
[repeat](evidence/v13-p25-voice19200-repeat/), and
[noise control](evidence/preburst-noise/).
The [recovered test-tone WAV](evidence/v13-p25-voice19200/recovered.wav)
contains decoded RF data, not the generator's original audio.

## Reference provenance

Protocol generator: upstream
[OP25 p25craft.py](https://github.com/boatbod/op25/blob/71abcd0ead32f86f51615ea6cc8a6a4dba4c949a/op25/gr-op25_repeater/apps/tx/p25craft.py).
Transmit response reference:
[op25_c4fm_mod.py](https://github.com/boatbod/op25/blob/71abcd0ead32f86f51615ea6cc8a6a4dba4c949a/op25/gr-op25_repeater/apps/tx/op25_c4fm_mod.py).
Generator and decoder SHA-256 values are in [provenance.json](reference/provenance.json);
RF archive hashes and receiver settings are in [manifest.json](manifest.json).
No third-party executable is bundled.

The original packed frames were generated with:

```text
python p25craft.py --ldu1 --1011 --nac 0x293 --tgid 4242 --src 123456 --output-file ldu1.bin
python p25craft.py --ldu2 --1011 --nac 0x293 --output-file ldu2.bin
python p25craft.py --stdu --nac 0x293 --output-file tdu.bin
```

Concatenate those three binary files in that order to reproduce
`inputs/voice-tone.p25`. The upstream generator's field reports and the
software-reference decoder trace are retained in [reference](reference/).
