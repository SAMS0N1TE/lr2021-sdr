#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
typedef int esp_err_t;
#define ESP_OK 0
#define ESP_ERR_INVALID_ARG -1
static uint8_t sent[1024];static unsigned sent_count,calls,fail_call;
static esp_err_t lr20xx_write_tx_fifo(const uint8_t *p,unsigned n)
{
    assert(n>0 && n<=128 && sent_count+n<=sizeof(sent));
    if(++calls==fail_call)return -9;
    memcpy(sent+sent_count,p,n);sent_count+=n;return ESP_OK;
}
#include "../research/tx-payload/lr2021_tx_fifo.inc"
int main(void)
{
    const uint8_t frame[]={1,2,3,4,5,6};unsigned queued=0,writes=0;
    assert(tx_iq_enqueue(frame,6,600,&queued,240,&writes)==0);
    assert(queued==240 && sent_count==240);
    assert(tx_iq_enqueue(frame,6,600,&queued,128,&writes)==0);
    assert(queued==368);
    assert(tx_iq_enqueue(frame,6,600,&queued,900,&writes)==0);
    assert(queued==600 && sent_count==600);
    for(unsigned i=0;i<600;++i)assert(sent[i]==frame[i%6]);
    assert(tx_iq_enqueue(frame,6,600,&queued,128,&writes)==0 && queued==600);
    sent_count=calls=queued=writes=0;fail_call=2;
    assert(tx_iq_enqueue(frame,6,60,&queued,60,&writes)==-9);
    assert(queued==6 && writes==1 && sent_count==6);
    fail_call=0;
    assert(tx_iq_enqueue(frame,6,60,&queued,60,&writes)==0 && queued==60);
    for(unsigned i=0;i<60;++i)assert(sent[i]==frame[i%6]);
    assert(tx_iq_enqueue(frame,0,60,&queued,60,&writes)==ESP_ERR_INVALID_ARG);
    puts("TX FIFO wrap and failed-write tests passed");return 0;
}
