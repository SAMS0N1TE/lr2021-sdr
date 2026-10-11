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
} lr2021_iq_profile;

/* All I/O hooks return 0 on success and must validate radio command status.
 * snapshot is called under lock; exclusive_lora excludes TX and other owners.
 * reset must clear raw routing without restoring RX. restore reapplies config.
 * fast(true) owns the SPI bus at 16 MHz; fast(false) releases/restores it.
 * Hooks have bounded timeouts. No hook may transmit RF. */
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
} lr2021_iq_hal;

typedef struct {
    size_t bytes;
    uint32_t status, nominal_rate_hz, max_fifo_level, polls;
    uint64_t elapsed_us;
    uint8_t fifo_flags;
    bool stopped_by_sink;
    int capture_error, restore_error, recovery_error;
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
#ifdef __cplusplus
}
#endif
#endif
