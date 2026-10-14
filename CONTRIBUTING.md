# Contributing to LR2021-SDR

Bug reports, measurements on other boards, documentation fixes and code contributions are welcome. For a large change, a new board or new transmit work, open an issue first so we can agree on the scope.

Please follow the [code of conduct](CODE_OF_CONDUCT.md). Report vulnerabilities through the [security policy](SECURITY.md).

## Reporting bugs

Search the existing issues, then use the bug report template. Include the commit, the part involved, your board and LR2021 ROM version, the frequency and rate, the steps to reproduce and what happened.

Trim logs to the relevant section. Remove passwords, tokens and precise personal locations. Only share I/Q recordings you have permission to distribute, and never other people's private communications.

## Getting set up

The C library needs CMake 3.18+ and a C11 compiler; Windows builds use MinGW-w64. From the repository root:

```text
cmake -S . -B build
cmake --build build
ctest --test-dir build --output-on-failure
```

The ESP32-P4 capture firmware builds with ESP-IDF 5.5.4:

```text
idf.py -C firmware/esp32p4 set-target esp32p4 build
```

CI runs both on every push and pull request. See the [developer guide](docs/developer-guide.md) for the library API and the [getting started guide](docs/getting-started.md) for the firmware. A passing host test does not show that a receiver or transmitter works on live RF.

## Making a change

1. Create a branch from the current `main`.
2. Keep the change focused and follow the surrounding code style.
3. Add a test when it can reproduce the bug or verify new behaviour. Update the docs when behaviour, commands or measured results change.
4. Run the checks relevant to your change. For documentation edits, check links, paths and commands.
5. Open a pull request with the reason for the change, test results and any limits on testing.

## Measurements and transmit work

Say how a result was measured: the receiver, its tuning offset, the sample rate and the signal source. Keep raw evidence small and add it under `research/` with a short note on how to reproduce it.

Transmit only on frequencies and at powers you are permitted to use, into an antenna or a dummy load, never into an open port. Keep existing experimental labels until there is evidence to remove them, and mark untested rates, frequencies and boards clearly.

Do not commit build output, local caches or credentials. Preserve third-party copyright and license notices, and identify the source of any code you bring in.
