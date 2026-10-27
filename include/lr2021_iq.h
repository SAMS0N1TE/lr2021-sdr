#ifndef LR2021_IQ_H
#define LR2021_IQ_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

#define LR2021_IQ_WORDS 256u
enum {
    LR2021_IQ_OK=0, LR2021_IQ_ARGUMENT=-1, LR2021_IQ_STATE=-2,
    LR2021_IQ_TIMEOUT=-3, LR2021_IQ_DATA=-4, LR2021_IQ_IO=-5
};
typedef struct {
    bool lr2021, configured, exclusive_lora, receiving;
    unsigned fw_major, fw_minor, spreading_factor;
    uint32_t frequency_hz, bandwidth_hz;
    /* Receive path (0 LF, 1 HF), gain step (0 = chip AGC, 1..13) and boost 0..7. */
    uint8_t rx_path, gain_step, rx_boost;
    /* Session receive route: 0 LoRa source 2 (IQ16), 1 source 0, 2 source 1,
     * 3 source 6 (two-byte pairs from an FSK setup with explicit output rate). */
    uint8_t route, input_code;
    uint32_t route_rate_hz;
} lr2021_iq_profile;

/* All I/O hooks return 0 on success and must validate radio command status.
 * snapshot is called under lock; exclusive_lora excludes TX and other owners.
 * reset must clear raw routing without restoring RX. restore reapplies config.
 * fast(true) owns the SPI bus at 16 MHz; fast(false) releases/restores it.
 * Hooks have bounded timeouts. No hook may transmit RF.
 * write_fifo is optional and only used by lr2021_iq_transmit (lr2021_iq_tx.h):
 * it writes 1..1020 bytes to the TX FIFO with command 0x0002 in one frame
 * (one CS assertion). read_fifo takes 1..256 bytes, or up to the
 * session's max_read when an adapter supports larger frames. */
typedef struct {
    void *context;
    void (*lock)(void *);
    void (*unlock)(void *);
    int (*snapshot)(void *, lr2021_iq_profile *);
    int (*read32)(void *, uint32_t, uint32_t *);
    int (*write32)(void *, uint32_t, uint32_t);
    int (*write)(void *, uint16_t, const uint8_t *, size_t);
    int (*read)(void *, uint16_t, uint8_t *, size_t);
    int (*standby)(void *);
    int (*receive)(void *);
    int (*reset)(void *);
    int (*restore)(void *, const lr2021_iq_profile *);
    int (*fast)(void *, bool);
    int (*clear_fifo)(void *);
    int (*read_fifo)(void *, uint8_t *, size_t);
    uint64_t (*now_us)(void *);
    void (*sleep_us)(void *, unsigned);
    int (*write_fifo)(void *, const uint8_t *, size_t);
} lr2021_iq_hal;

typedef struct {
    size_t bytes;
    uint32_t status, nominal_rate_hz, max_fifo_level, polls;
    uint64_t elapsed_us;
    uint8_t fifo_flags;
    bool stopped_by_sink;
    int capture_error, restore_error, recovery_error;
    /* Checked stream only: bytes published after a successful FIFO flag check.
     * A later failure invalidates the unpublished tail, not these earlier blocks. */
    unsigned fifo_capacity;
    size_t validated_bytes;
    uint64_t max_service_gap_us;
    /* Session API only: now_us() when sample 0 was retained, gain changes applied. */
    uint64_t start_us;
    unsigned gain_changes;
} lr2021_iq_result;

/* Two separate buffers of >=256 words. Host-order words contain I low/Q high.
 * Output is provisional until return 0, including reset and SRAM verification. */
int lr2021_iq_capture(const lr2021_iq_hal *, uint32_t *words, uint32_t *scratch,
                     size_t capacity_words, lr2021_iq_result *);
/* Synchronous sink: copy/process immediately, never block or call radio APIs.
 * Returning false cancels normally. All blocks remain provisional until return 0. */
typedef bool (*lr2021_iq_sink)(void *, const uint8_t *, size_t, uint64_t first_pair);
int lr2021_iq_stream(const lr2021_iq_hal *, unsigned duration_ms,
                    lr2021_iq_sink, void *sink_context, lr2021_iq_result *);
/* Checked receive session, 1..60000 ms, one setup and one final recovery.
 * No periodic standby/reset. Publishes approximately 1 KiB blocks only after
 * checking sticky FIFO error flags. On loss, aborts and discards the pending
 * tail. Already published blocks keep their continuity qualification even if
 * a later block or final recovery fails. Never append a restarted session to
 * them without a downstream gap/reset. No radio calls/DSP/waits in the sink;
 * copy to a bounded queue and return false if it cannot accept more data.
 * Unlike the legacy sink contract, false rejects this block and stops; only
 * blocks accepted with true contribute to validated_bytes.
 * Synchronous, holds the radio lock throughout. The adapter must meet the
 * selected FIFO service deadline. See docs/extended-fifo.md for board results. */
int lr2021_iq_stream_checked(const lr2021_iq_hal *, unsigned duration_ms,
                            lr2021_iq_sink, void *, lr2021_iq_result *);
/* Explicit FIFO selection: 256 (legacy) or nominal 1024 bytes. Extended mode
 * uses 0x804400..0x8047FF exclusively; exclude SRAM capture and CalibFE until
 * final reset/restore. Read chunks remain <=256 bytes. Mapping is verified
 * before/after RX. On failure the session aborts and recovers, never silently
 * falls back. reset must restore default mapping and restore must recalibrate
 * as required by the saved radio profile. No warm-sleep retention is used. */
int lr2021_iq_stream_checked_fifo(const lr2021_iq_hal *, unsigned duration_ms,
                                 unsigned fifo_capacity, lr2021_iq_sink,
                                 void *, lr2021_iq_result *);
/* Open-ended checked session for host streaming. Same contract as the checked
 * stream: false from the sink rejects that block and ends the session. Route 0
 * is LoRa source 2 at 4 x bandwidth for BW 31.25, 62.5, 125 and 250 kHz (125k
 * to 1M pairs/s, four bytes per pair); whether the bus keeps up above 250k is
 * a property of the adapter. Routes 1..3 stream two-byte pairs from sources 0,
 * 1 and 6 at the profile's route rate. first_pair counts pairs of the route's
 * size. duration_ms 0 runs until
 * the sink stops it; otherwise 1..60000. start_us is written before the first
 * block reaches the sink. gain, when set, is polled between FIFO reads and may
 * request a new gain step (0 AGC, 1..13) and boost (0..7); the library applies
 * it with SetAgcGainManual/SetRxPath without restarting reception. The
 * callback must not call radio functions itself. The loop busy-polls the
 * FIFO: on an RTOS, run it on a core whose idle task is not watched by a task
 * watchdog. The bus clock is the adapter's choice (16 MHz is the datasheet
 * figure; faster clocks are board measurements). */
typedef struct {
    unsigned duration_ms, fifo_capacity;
    bool (*gain)(void *context, uint8_t *step, uint8_t *boost);
    void *gain_context;
    volatile uint64_t start_us;
    /* Optional throughput tuning, 0 = default. max_read: bytes per read_fifo
     * call, multiple of 4, up to 1020 (default 256; the adapter's read_fifo
     * must accept it). check_bytes: published block size, one FIFO flag check
     * each, multiple of 4 and at least max_read (default 1024). buffer: staging
     * of at least check_bytes + max_read bytes (default: 1280 on the stack). */
    unsigned max_read, check_bytes;
    uint8_t *buffer;
    size_t buffer_size;
    /* level_every N > 1: read the FIFO level on every Nth pass only and, in
     * between, read 90% of what the nominal rate delivered since. A wrong
     * estimate shows as FIFO underflow and ends the session; no data passes
     * unchecked. 0 or 1 reads the level on every pass. */
    unsigned level_every;
} lr2021_iq_session;
int lr2021_iq_stream_session(const lr2021_iq_hal *, lr2021_iq_session *,
                            lr2021_iq_sink, void *, lr2021_iq_result *);
#ifdef __cplusplus
}
#endif
#endif
