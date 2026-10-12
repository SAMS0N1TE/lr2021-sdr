# Custom LR2021 firmware research

Custom RAM code executed on the tested LR2021 ROM 1.24.
LakeShark already runs continuous raw-I/Q reception and decodes
[P25 voice](voice-capture.md) using the existing ROM route and P4 scheduling.
A custom on-radio capture extension is a separate research path for buffering
or decimation. Its target is a small RAM-resident extension to the ROM, with the host reloading it
after reset. A full replacement of the radio firmware is unnecessary for this
goal and would require substantially more reverse engineering.

## Evidence and limits

Semtech's public [patch driver](https://github.com/Lora-net/usp/blob/master/smtc_rac_lib/radio_drivers/lr20xx_driver/src/lr20xx_patch.c)
uploads words through the memory-write API and activates PRAM using command
0x012D with payload 0. It defines load address 0x801000, active marker 0x800FF8
and magic 0x600DB002. Its loader supports Semtech patch distribution; it is not
a published SDK for arbitrary on-radio applications.

Independent [research by radioegor146](https://b.re146.dev/lora-under-attack/)
explicitly reports LR2021 ROM dumping and custom code execution over SPI, and
states that the chip has no flash. Most of that article concerns LR11xx;
its flash-update procedures, addresses and security findings must not be
transferred to LR2021. The tested PRAM path does not use those procedures.

A 100-byte uploaded image computed 0x13579BDF XOR 0x2468ACE0 = 0x373F373F inside
the radio, wrote the result through its mapped alias, and returned. The hardware test verified execution, exact restoration of the 25 touched
words, radio recovery and zero radio errors. The harness restores memory
again after reset.
This proves a bounded entry point, not sustained processing, arbitrary memory
ownership, or compatibility with an active Semtech patch.

The proof refuses an existing active patch. Its routine uses no stack, calls,
interrupt hooks or radio register writes. It is not evidence that a custom
capture patch and Semtech's normal patch can coexist yet.

An offline rebuild of the original assembly matches the saved hardware-proof
binary byte for byte. SHA256:
`5d2cd5ceb025690bff433cb4c3be8c06137b348635b0b0b30b262efa954a2321`.
No board was accessed during this research.

## Loader and function-hook mechanism

These observations come from the saved ROM hash recorded in
[Capture engine investigation](capture-engine.md), plus the locally saved
560-word Semtech patch from USP v1.2.1. They are version-specific static analysis.
The little-endian serialization of that vendor word array has SHA256
`a828f5bac9f1309bc02b06f0885d5061ebd91510f40ade951b34c4b23d6edc3f`.

- ROM loader 0x24FC selects an instruction mapping and checks the header magic
  at 0x101000, with a fallback check at 0x102000. It reads the entry pointer at
  header offset 8 and calls it when the low byte of header word 1 is nonzero.
  A return value of 2 sets a runtime flag. This inspected path does not contain
  signature verification; successful custom execution is the relevant proof
  for the tested silicon/version.
- The test image is written at data/SPI address 0x801000 but executes through
  alias 0x101000. Once mapped, its result is read at 0x101060. The loader and readback paths use different aliases.
- The vendor image has primary entry 0x10104C and another pointer at header
  offset 12 to 0x101018. The minimal proof does not need the second callback;
  its broader lifecycle is not fully characterized.
- The vendor initialization path calls ROM helper 0xB3A4 with 460 entries and
  destination 0x1018C0. That helper resolves the old JLI branch entries, emits
  new entries and changes the ARC `jli_base` auxiliary register.
- Vendor calls to 0xBC88 replace selected JLI slots. The underlying helper
  0xBC60 writes an encoded branch into the slot; these are instruction words,
  not an ordinary array of function pointers. Direct ROM calls can bypass JLI
  hooks, so replacing a slot does not intercept every call to a routine.

The memory budget matters: 560 words = 2240 bytes = 0x8C0, ending at 0x1018C0.
The generated 460-entry table consumes another 1840 bytes = 0x730, ending at
0x101FF0. Together they occupy 4080 of the first 4096 bytes. Appending custom
code after the uploaded blob would collide with the generated table.
Relocating that table, extending code into another bank, or replacing selected
vendor routines each requires a verified memory plan. The fallback loader
address is not evidence of an automatically available second patch slot.

## What a capture extension could do

A capture extension would need a bounded service that keeps SPI commands
responsive and reports sequence numbers, producer position and overruns.

| Candidate | Potential benefit | Missing evidence |
| --- | --- | --- |
| Service an internal sample route into a RAM ring | Give the P4 milliseconds instead of a sub-millisecond FIFO deadline | ARC access to a continuous source, cycle budget and safe RAM |
| Switch SRAM capture banks locally | Remove host SPI round trips from switching | Hardware still sampling across the switch; coherent access to the other bank |
| Filter and decimate before export | Reduce SPI traffic and P4 workload | Core clock, supported arithmetic, filter cost and receiver coexistence |
| Export block sequence/status metadata | Detect loss and avoid joining unrelated samples | Stable command hook and host/producer ownership protocol |

Local rearming can shorten a gap but cannot remove a hardware blind interval
if the capture engine stops before it can be rearmed. A patch can reduce or
avoid losses only if it has access to the samples during that interval.

At 125k complex pairs/s, IQ16 produces 500 kB/s. A hypothetical 5:1 decimator
would export 25k pairs/s or 100 kB/s. It needs an actual anti-alias filter;
dropping four out of five samples is not an adequate P25 channel filter.
Neither that decimator nor the required on-chip processing budget has been
demonstrated. Keep vocoding, UI and the main decoder on the P4 initially.

## C implementation path

[GNU GCC supports ARC EM](https://gcc.gnu.org/onlinedocs/gcc/ARC-Options.html),
and [prebuilt ARC toolchains](https://github.com/foss-for-mips-arc-processors/toolchain)
are available. The existing assembly proof builds with `-mcpu=arcem`.
That successful instruction subset does not identify every optional core
extension, clock frequency or interrupt feature.

An offline GCC 15.2.0 check compiled a freestanding C function that XORs two
arguments into a volatile result pointer and returns 2. It produced eight bytes:
`xor_s`, `st_s`, `mov_s`, `j_s [blink]`, with no relocations, stack use or helper
calls in the inspected object. This verifies the minimal C toolchain path only;
the C-generated routine has not been run on the radio.

Use freestanding C for buffer management and counters, with a small assembly
entry/hook wrapper for register preservation and auxiliary-register access.
Start with integer code, explicit storage, no libc or allocation, and no assumed
floating-point, SIMD or atomic extensions. Determine the stack, GP convention,
interrupt context and return contract before calling C from a ROM hook. Inspect
linked code for compiler helper calls and unresolved symbols. A compiler build
or simulator result cannot validate Semtech-specific peripherals.

## Development sequence

1. Pin ROM and vendor patch hashes. Verify actual active patch state rather
   than assuming a patch was loaded. Current capture APIs deliberately reject
   the active vendor marker, so integration must account for that guard.
2. Map patch code, generated jump table, stack and receiver RAM. Preserve the
   vendor behavior explicitly; do not overwrite the proven 4 KiB patch region
   with capture buffers.
3. Establish a bounded C callback that increments a counter and returns while
   ordinary SPI commands and receive configuration continue working. Begin
   with one identified hook and a version-checked rollback path.
4. Instrument capture pointer/status transitions and measure callback execution
   time. Avoid the ROM's unbounded capture-stop wait in a custom service.
5. Add a small ring or bank switching only after the source/mapping behavior is
   known. Publish payload before metadata and enforce host acknowledgement;
   never overwrite unread samples silently.
6. Test long known sequences under P4 UI/audio/storage load. Require matching
   sample counts and explicit overrun detection before calling it continuous.
   Only then evaluate on-chip decimation and paired P25 reception quality.

The sources examined supplied no on-chip application SDK or ready-to-use
custom ARC capture extension. The working [LakeShark FIFO integration](live-streaming.md)
does not require that extension. The included [ESP32-P4 capture firmware](../firmware/esp32p4/README.md)
provides a standalone console for bounded capture tests.
