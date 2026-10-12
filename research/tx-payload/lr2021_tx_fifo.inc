/* Queue a bounded slice of a repeated IQ buffer, preserving frame boundaries.
 * All lengths and queue positions are bytes. Called while radio lock is held. */
static esp_err_t tx_iq_enqueue(const uint8_t *frame, unsigned frame_len,
                               unsigned total, unsigned *queued,
                               unsigned limit, unsigned *writes)
{
    if(!frame || !frame_len || !queued || !writes || *queued>total)
        return ESP_ERR_INVALID_ARG;
    unsigned remaining=total-*queued;
    if(limit>remaining) limit=remaining;
    while(limit) {
        unsigned offset=*queued%frame_len;
        unsigned count=frame_len-offset;
        if(count>limit) count=limit;
        if(count>128) count=128;
        esp_err_t err=lr20xx_write_tx_fifo(frame+offset,count);
        if(err!=ESP_OK) return err;
        *queued+=count;limit-=count;++*writes;
    }
    return ESP_OK;
}
