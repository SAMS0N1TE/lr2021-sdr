#include "lr2021_iq.h"
#include "lriq.h"

/* Supply board callbacks and configured LoRa RX before calling. */
int capture_tone(const lr2021_iq_hal *radio, lriq_tone *tone)
{
    uint32_t words[256],scratch[256];
    lr2021_iq_result result;
    int e=lr2021_iq_capture(radio,words,scratch,256,&result);
    if(e)return e;
    lriq_capture capture={0}; capture.pairs=256;
    for(unsigned i=0;i<256;i++)
        for(unsigned b=0;b<4;b++)capture.data[4*i+b]=(uint8_t)(words[i]>>(8*b));
    return lriq_measure_tone(&capture,tone);
}
