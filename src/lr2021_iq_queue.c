#include "lr2021_iq_queue.h"
#include <string.h>
bool lr2021_iq_queue_init(lr2021_iq_queue *q)
{
    if(!q)return false;
    atomic_init(&q->write_index,0);atomic_init(&q->read_index,0);
    return atomic_is_lock_free(&q->write_index) && atomic_is_lock_free(&q->read_index);
}
bool lr2021_iq_queue_push(lr2021_iq_queue *q,const uint8_t *data,size_t bytes,
                         uint64_t first_pair,uint32_t generation)
{
    if(!q || !data || !bytes || bytes>LR2021_IQ_BLOCK_BYTES || bytes%4)return false;
    unsigned w=atomic_load_explicit(&q->write_index,memory_order_relaxed);
    unsigned r=atomic_load_explicit(&q->read_index,memory_order_acquire);
    if(w-r>=LR2021_IQ_QUEUE_SLOTS)return false;
    lr2021_iq_block *b=&q->slots[w&(LR2021_IQ_QUEUE_SLOTS-1)];
    b->bytes=bytes;b->first_pair=first_pair;b->generation=generation;
    memcpy(b->data,data,bytes);
    atomic_store_explicit(&q->write_index,w+1,memory_order_release);
    return true;
}
const lr2021_iq_block *lr2021_iq_queue_peek(lr2021_iq_queue *q)
{
    if(!q)return NULL;
    unsigned r=atomic_load_explicit(&q->read_index,memory_order_relaxed);
    unsigned w=atomic_load_explicit(&q->write_index,memory_order_acquire);
    return w==r?NULL:&q->slots[r&(LR2021_IQ_QUEUE_SLOTS-1)];
}
void lr2021_iq_queue_release(lr2021_iq_queue *q)
{
    if(!q || !lr2021_iq_queue_peek(q))return;
    unsigned r=atomic_load_explicit(&q->read_index,memory_order_relaxed);
    atomic_store_explicit(&q->read_index,r+1,memory_order_release);
}
