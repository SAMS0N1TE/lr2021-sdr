# LR2021 debug console (MeshCore)

`LR2021DebugCLI.h` and `LR2021IQStream.h` add raw chip-access commands to the serial console
of MeshCore's `simple_repeater`. They were used for everything in this repository. They are
**debug-only**: some commands overwrite chip RAM, and capture modes hold the mesh loop.

## Commands

| Command | What it does |
|---|---|
| `mr <addr> [words]` | ReadRegMem32: read words at a hex address, printed 8 per line (`AAAAAA: w0 ... w7`) |
| `mw <addr> <value>` | WriteRegMem32: write one word |
| `rc <op> <nread> [bytes..]` | Raw command: hex opcode, number of reply bytes, hex parameter bytes. Prints `rc st=<status> <reply bytes>` |
| `err` | Print and clear the chip error register |
| `romprobe` | Read a few words at candidate ROM bases (how the ROM was located) |
| `hold on\|off` | Stop/resume the mesh loop, so MeshCore doesn't touch the radio during experiments |
| `cap <cfg> <words> <ms> [trig]` | One-shot 0x0290 capture into RAM 0x804000, then dump. `cfg` = size[7:5]\|mode[4:3]\|src[2:0], e.g. `D0` |
| `capwait <cfg> <ratio> <ms>` | Repeat one-shot captures until the power exceeds ratio × the first capture's power, then dump (set packet type 0x0B first: `rc 207 0 0B`) |
| `stream <src> <x> <ms> <prekb> [mhz] [dec]` | FIFO IQ streaming into a 128 KB ring. Triggers on a power rise × `x` or an AGC gain drop, keeps `prekb` KB of history; `x` = 0 for a timed capture |
| `streamdump` | Print the summary, then `STREAMBIN <n>` and n raw bytes |
| `streamstats` | Per-4 KB-block power, actual / manual gain, int8 rail counts of the last stream |
| `iqstream <ms> [mhz] [dec] [bits] [rawrate] [rxdelay]` | Continuous LRIQ binary stream (see [docs/raw-iq.md](../docs/raw-iq.md)); `ms` = 0 runs until ETX (0x03). Defaults: 16 MHz, decimation 3, 8 bits, 250000 |

Examples:

```
mr 0 32768                 dump ROM + zeros to 0x1FFFF
mr F30154 1                AGC gain register; bits 23:20 = current step
rc 21A 0 05                SetAgcGainManual(5)
rc 22A 10                  GetLoraPacketStatus, all 10 bytes (byte 9 = gain step)
rc 103 4 00 00 00 04       read aux IDENTITY -> 00 00 00 46
```

Use `tools/sc.py` to script these from the host.

## Integration

The sources come from branch `lr2021-rom-dump` of
[beala/MeshCore](https://github.com/beala/MeshCore), on top of the AB Xiao nRF52 LR2021
variant. To add them to another MeshCore tree:

1. Copy both headers to `src/helpers/radiolib/`.
2. Add these public helpers to `CustomLR2021` (in `CustomLR2021.h`). `SPIcommand` and
   `writeCommon` are protected members of RadioLib's `LR2021` class:

   ```cpp
   // memRead: n <= 32 words; reply words are big-endian on the wire.
   int16_t memRead(uint32_t addr, uint32_t* out, uint8_t n) {
     if (n == 0 || n > 32) return RADIOLIB_ERR_SPI_CMD_INVALID;
     uint8_t req[4] = { (uint8_t)(addr >> 16), (uint8_t)(addr >> 8), (uint8_t)addr, n };
     uint8_t rpl[128];
     int16_t state = SPIcommand(RADIOLIB_LR2021_CMD_READ_REG_MEM_32, false, rpl, 4 * n, req, sizeof(req));
     for (uint8_t i = 0; i < n; i++)
       out[i] = ((uint32_t)rpl[4*i] << 24) | ((uint32_t)rpl[4*i+1] << 16) | ((uint32_t)rpl[4*i+2] << 8) | rpl[4*i+3];
     return state;
   }
   int16_t memWrite(uint32_t addr, uint32_t val) {
     return writeCommon(RADIOLIB_LR2021_CMD_WRITE_REG_MEM_32, addr, &val, 1, false);
   }
   // outLen == 0: write in[] as parameters; else send in[] then read outLen bytes.
   int16_t rawCmd(uint16_t op, const uint8_t* in, size_t inLen, uint8_t* out, size_t outLen) {
     if (outLen == 0) return SPIcommand(op, true, (uint8_t*)in, inLen);
     return SPIcommand(op, false, out, outLen, in, inLen);
   }
   int16_t getChipErrors(uint16_t* err) {
     uint8_t buff[2] = { 0 };
     int16_t state = SPIcommand(RADIOLIB_LR2021_CMD_GET_ERRORS, false, buff, sizeof(buff));
     if (err) *err = ((uint16_t)buff[0] << 8) | buff[1];
     return state;
   }
   int16_t clearChipErrors() { return SPIcommand(RADIOLIB_LR2021_CMD_CLEAR_ERRORS, true, NULL, 0); }
   ```

3. Hook the console into `examples/simple_repeater/main.cpp`:

   ```cpp
   #ifdef LR2021_DEBUG_CLI
     #include <helpers/radiolib/LR2021DebugCLI.h>
     extern CustomLR2021 radio;
   #endif
   ...
   // where serial commands are handled:
   #if defined(LR2021_DEBUG_CLI)
       if (!lr2021dbg::handle(radio, command, Serial)) {
         the_mesh.handleCommand(0, command, reply);
       }
   #else
       the_mesh.handleCommand(0, command, reply);
   #endif
   ...
   // in loop():
   #ifdef LR2021_DEBUG_CLI
     if (!lr2021dbg::hold)
   #endif
     the_mesh.loop();
   ```

4. Add `-D LR2021_DEBUG_CLI` to the repeater env. The streaming commands drive SPI directly
   and need `P_LORA_NSS` and `P_LORA_BUSY` defined, which every LR2021 variant already has.

### Per-packet RXSTAT line (optional)

To print the full packet status for every received packet, override `recvRaw` in
`CustomLR2021Wrapper`:

```cpp
#ifdef LR2021_DEBUG_CLI
  int recvRaw(uint8_t* bytes, int sz) override {
    int len = RadioLibWrapper::recvRaw(bytes, sz);
    if (len > 0) {
      uint8_t st[10] = { 0 };
      int16_t s = ((CustomLR2021 *)_radio)->rawCmd(RADIOLIB_LR2021_CMD_GET_LORA_PACKET_STATUS, NULL, 0, st, sizeof(st));
      Serial.printf("RXSTAT len=%d st=%d rssi=%.1f snr=%.2f raw=", len, s, getLastRSSI(), getLastSNR());
      for (int i = 0; i < 10; i++) Serial.printf("%02X ", st[i]);
      Serial.printf("gain=%u\r\n", st[9]);
    }
    return len;
  }
#endif
```

`tools/listen.py` logs these lines.

## Notes

- nRF52 USB CDC: output can stall while the host is silent. The host tools send periodic
  newlines to keep it flowing.
- After `stream`, `iqstream` or `capwait`, the radio is in packet type 0x0B with a modified
  FIFO route. Reboot to restore normal MeshCore operation.
- Tested on the nRF52840 (Seeed XIAO, Adafruit core). `iqstream` uses SPIM3 at 16 MHz;
  32 MHz did not work.
