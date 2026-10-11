#ifndef LRIQ_H
#define LRIQ_H
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
/* Portable C11 capture decoder and tone measurement. No allocation, OS, ESP-IDF
 * or Python dependency. Source 2 is experimental; no assumed sample clock. */
#define LRIQ_PAIRS 256
#define LRIQ_BYTES (LRIQ_PAIRS * 4)
typedef struct {
    uint8_t data[LRIQ_BYTES]; /* explicitly little endian, low component first */
    uint32_t frequency_hz, status;
    uint32_t bandwidth_hz, sample_rate_hz; /* nominal, 0 = unknown */
    int iq_validated; /* profile checked with RF tones; not amplitude calibration */
    size_t pairs;
} lriq_capture;
typedef struct {
    lriq_capture capture;
    size_t next;
    int failed, complete;
} lriq_parser;
/* Feed complete console lines without CR/LF. 0=more, 1=complete, -1=invalid.
 * Ignore unrelated console noise, reject malformed/duplicate capture records.
 * Consume capture only after return 1. One parser per transaction. */
void lriq_parser_init(lriq_parser *parser);
int lriq_parser_line(lriq_parser *parser, const char *line);
/* Compact FIFO console export. Caller owns storage; data is provisional until
 * complete. One transaction only; CRC, offsets, length and radio errors checked. */
typedef struct {
    uint8_t *data;
    size_t capacity, bytes;
    uint32_t crc;
    int checksum_seen, complete, failed;
} lriq_fifo_parser;
void lriq_fifo_init(lriq_fifo_parser *parser, uint8_t *data, size_t capacity);
int lriq_fifo_line(lriq_fifo_parser *parser, const char *line);
void lriq_components(const lriq_capture *capture, size_t index, int16_t *a, int16_t *b);
typedef struct {
    double cycles_per_pair; /* signed, low component + j*high component */
    double peak_to_mean_db; /* spectral prominence, not calibrated RF SNR */
    double rms_a, rms_b;
} lriq_tone;
/* Hann-windowed complex DFT, DC removed. Returns -1 for empty/constant input. */
int lriq_measure_tone(const lriq_capture *capture, lriq_tone *tone);
/* Two known RF tones in one non-aliased passband: signed rate indicates
 * orientation. Caller must establish tone prominence and unwrap aliases. */
int lriq_estimate_rate(double rf_delta_hz, double cycles_delta, double *signed_rate_hz);
#ifdef __cplusplus
}
#endif
#endif
