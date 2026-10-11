#ifndef LR2021_IQ_QUEUE_H
#define LR2021_IQ_QUEUE_H
#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#define LR2021_IQ_QUEUE_SLOTS 8u
#define LR2021_IQ_BLOCK_BYTES 1280u
typedef struct {
    uint64_t first_pair;
    uint32_t generation;
    size_t bytes;
    uint8_t data[LR2021_IQ_BLOCK_BYTES];
} lr2021_iq_block;
typedef struct {
    atomic_uint write_index, read_index;
    lr2021_iq_block slots[LR2021_IQ_QUEUE_SLOTS];
} lr2021_iq_queue;
/* One producer, one consumer. Caller owns storage; no allocation or waits.
 * Init/reset only while both owners are stopped. Returns false if indices are
 * not lock-free on this platform; such a queue is unsuitable for the FIFO sink.
 * A full queue rejects new data; it never overwrites queued samples. Stop the
 * capture and mark a new generation on restart. Consumer resets DSP/acquisition
 * on a changed generation or nonconsecutive first_pair; never bridges gaps. */
bool lr2021_iq_queue_init(lr2021_iq_queue *q);
bool lr2021_iq_queue_push(lr2021_iq_queue *q,const uint8_t *data,size_t bytes,
                         uint64_t first_pair,uint32_t generation);
/* peek pointer remains valid until this consumer calls release. */
const lr2021_iq_block *lr2021_iq_queue_peek(lr2021_iq_queue *q);
void lr2021_iq_queue_release(lr2021_iq_queue *q);
#endif
