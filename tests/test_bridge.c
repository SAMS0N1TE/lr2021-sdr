/* SPDX-License-Identifier: GPL-3.0-only */
#include "lr2021_bridge_server.h"
#include "lr2021_sim.h"
#include <stdio.h>
#include <string.h>
#define CHECK(x) do {if(!(x)){fprintf(stderr,"line %d: %s\n",__LINE__,#x);return 1;}}while(0)

typedef struct {
    lr2021_bridge_parser parser;
    lr2021_bridge_frame last; uint8_t last_payload[LR2021_BRIDGE_MAX_PAYLOAD];
    unsigned frames, data_frames, rx_end, tx_end, wakes;
    uint64_t pairs, next_pair; uint32_t last_value, gaps; unsigned shifted, formats, max_pairs;
    lr2021_bridge_rx_end end; lr2021_bridge_tx_end tx;
} host;
static void on_frame(void *ctx, const lr2021_bridge_frame *f)
{
    host *h=ctx; h->frames++;
    if(f->type==LR2021_BRIDGE_RX_DATA) {
        lr2021_bridge_rx_data d;
        static int16_t iq[2*LR2021_BRIDGE_RX_MAX_PAIRS];
        if(lr2021_bridge_unpack_rx_data(f->payload,f->length,&d) || d.first_pair!=h->next_pair ||
           lr2021_bridge_unpack_samples(iq,d.format,d.shift,f->payload+LR2021_BRIDGE_RX_DATA_HEADER,
               f->length-LR2021_BRIDGE_RX_DATA_HEADER,d.pairs)){ h->gaps++; return; }
        for(size_t i=0;i<d.pairs;i++) {
            uint32_t v=(uint16_t)iq[2*i]|(uint32_t)(uint16_t)iq[2*i+1]<<16;
            if(d.format==LR2021_BRIDGE_CS16 && h->pairs && v!=h->last_value+1)h->gaps++;
            h->last_value=v; h->pairs++;
        }
        if(d.format)h->shifted|=d.shift;
        h->formats|=1u<<d.format;
        if(d.pairs>h->max_pairs)h->max_pairs=d.pairs;
        h->next_pair=d.first_pair+d.pairs;
        h->data_frames++; return;
    }
    if(f->type==LR2021_BRIDGE_RX_END){ h->rx_end++; lr2021_bridge_unpack_rx_end(f->payload,f->length,&h->end); return; }
    if(f->type==LR2021_BRIDGE_TX_END){ h->tx_end++; lr2021_bridge_unpack_tx_end(f->payload,f->length,&h->tx); return; }
    h->last=*f; memcpy(h->last_payload,f->payload,f->length); h->last.payload=h->last_payload;
}
static int send_cb(void *ctx, const uint8_t *p, size_t n) { host *h=ctx; lr2021_bridge_parse(&h->parser,p,n,on_frame,h); return 0; }
static void wake_cb(void *ctx) { ((host *)ctx)->wakes++; }
static lr2021_iq_dev dev;
static int configure_cb(void *ctx, const lr2021_iq_rx *rx, uint16_t *errors)
{ (void)ctx; int e=lr2021_iq_dev_configure(&dev,rx); *errors=dev.errors; return e; }
static lr2021_bridge_server srv;
static int32_t call(host *h, uint8_t type, uint16_t seq, const uint8_t *p, size_t n)
{
    uint8_t frame[LR2021_BRIDGE_MAX_FRAME];
    size_t len=lr2021_bridge_encode(frame,sizeof(frame),type,seq,p,n);
    h->last.type=0;
    lr2021_bridge_server_input(&srv,frame,len);
    lr2021_bridge_server_work(&srv);
    while(lr2021_bridge_server_pump(&srv)){}
    if(h->last.type!=(type|0x80) || h->last.sequence!=seq || h->last.length<4)return 999;
    return (int32_t)lr2021_bridge_get32(h->last.payload);
}
int main(void)
{
    /* Codec. */
    CHECK(lr2021_bridge_crc32(0,(const uint8_t *)"123456789",9)==0xCBF43926u);
    uint8_t buf[LR2021_BRIDGE_MAX_FRAME], pay[LR2021_BRIDGE_MAX_PAYLOAD+1];
    for(unsigned i=0;i<sizeof(pay);i++)pay[i]=(uint8_t)(i*7);
    CHECK(!lr2021_bridge_encode(buf,sizeof(buf),1,1,pay,LR2021_BRIDGE_MAX_PAYLOAD+1));
    size_t len=lr2021_bridge_encode(buf,sizeof(buf),0x42,0x1234,pay,5);
    CHECK(len==17 && buf[0]=='L' && buf[1]=='R' && buf[2]==1 && buf[3]==0x42 && buf[4]==0x34 && buf[6]==5);
    static host h; lr2021_bridge_parser_init(&h.parser);
    /* Garbage, a corrupted copy and a split valid frame: exactly one frame decodes. */
    uint8_t stream[64]={'x','L','R',9,'L'}; size_t k=5;
    memcpy(stream+k,buf,len); stream[k+9]^=1; k+=len;
    memcpy(stream+k,buf,len); k+=len;
    lr2021_bridge_parse(&h.parser,stream,k-3,on_frame,&h);
    CHECK(h.frames==0);
    lr2021_bridge_parse(&h.parser,stream+k-3,3,on_frame,&h);
    CHECK(h.frames==1 && h.parser.crc_errors==1 && h.last.type==0x42 && h.last.sequence==0x1234);
    CHECK(h.last.length==5 && !memcmp(h.last.payload,pay,5));
    lr2021_bridge_hello hello={-2,1,3,1,24,2048,1000000,65534,"name","board-id"}, hello2;
    CHECK(lr2021_bridge_pack_hello(pay,&hello)==84 && !lr2021_bridge_unpack_hello(pay,84,&hello2));
    CHECK(hello2.status==-2 && hello2.rom_minor==24 && !strcmp(hello2.id,"board-id") && hello2.tx_capacity==65534);
    lr2021_bridge_tx_begin tb={10,0xDEADBEEF,125000,915000000,-18,3,0x0102030405060708ull,0,0}, tb2;
    tb.pa=1; tb.flags=1;
    CHECK(lr2021_bridge_pack_tx_begin(pay,&tb)==28 && !lr2021_bridge_unpack_tx_begin(pay,28,&tb2));
    CHECK(tb2.power_half_db==-18 && tb2.start_time==tb.start_time && tb2.crc32==tb.crc32 && tb2.pa==1 && tb2.flags==1);
    lr2021_bridge_rx_data rd={1,125000,5,6,7,3,LR2021_BRIDGE_CS12,2}, rd2;
    CHECK(lr2021_bridge_pack_rx_data(pay,&rd)==32 && !lr2021_bridge_unpack_rx_data(pay,32+9,&rd2));
    CHECK(rd2.pairs==3 && rd2.shift==2 && rd2.format==1 && lr2021_bridge_unpack_rx_data(pay,32+12,&rd2)==-1);
    /* CS12 block floating point: exact below 2048, else truncated by 2^shift. */
    const int16_t small[6]={2047,-2048,0,-1,1000,-999}, big[6]={32767,-32768,100,-100,16,-17};
    int16_t back[6]; uint8_t sh, packed[32]; uint8_t raw[24];
    for(unsigned i=0;i<6;i++)lr2021_bridge_put16(raw+2*i,(uint16_t)small[i]);
    CHECK(lr2021_bridge_pack_samples(packed,LR2021_BRIDGE_CS12,raw,3,&sh)==9 && sh==0);
    CHECK(!lr2021_bridge_unpack_samples(back,LR2021_BRIDGE_CS12,sh,packed,9,3) && !memcmp(back,small,12));
    for(unsigned i=0;i<6;i++)lr2021_bridge_put16(raw+2*i,(uint16_t)big[i]);
    CHECK(lr2021_bridge_pack_samples(packed,LR2021_BRIDGE_CS12,raw,3,&sh)==9 && sh==4);
    CHECK(!lr2021_bridge_unpack_samples(back,LR2021_BRIDGE_CS12,sh,packed,9,3));
    for(unsigned i=0;i<6;i++)CHECK(back[i]<=big[i] && big[i]-back[i]<16);
    CHECK(lr2021_bridge_unpack_samples(back,LR2021_BRIDGE_CS16,1,raw,12,3)==-1);

    /* Server over the simulator in virtual time, single-threaded. */
    static lr2021_sim sim; lr2021_sim_init(&sim);
    lr2021_iq_bus bus=lr2021_sim_bus(&sim);
    lr2021_iq_rx rx={154785000,31250,13,0,2,0,0,0};
    CHECK(!lr2021_iq_dev_open(&dev,&bus,NULL,&rx));
    memset(&h,0,sizeof(h)); lr2021_bridge_parser_init(&h.parser);
    lr2021_bridge_io io={&h,send_cb,NULL,NULL,wake_cb};
    lr2021_bridge_radio radio={NULL,&dev.hal,NULL,configure_cb,rx,1,24,
        LR2021_BRIDGE_CAP_RX|LR2021_BRIDGE_CAP_TX|LR2021_BRIDGE_CAP_LIVE_GAIN,"test","sim0"};
    static uint8_t txbuf[LR2021_BRIDGE_TX_CAPACITY];
    CHECK(!lr2021_bridge_server_init(&srv,&io,&radio,txbuf,sizeof(txbuf)));
    CHECK(lr2021_bridge_server_tune(&srv,1024,4096) && lr2021_bridge_server_tune(&srv,1020,32768));
    CHECK(!lr2021_bridge_server_tune(&srv,1020,4096));
    CHECK(lr2021_bridge_server_tune(&srv,1020,8192)); /* needs a larger buffer */
    static uint8_t stagebig[16384+1024];
    CHECK(lr2021_bridge_server_buffer(&srv,stagebig,100) && !lr2021_bridge_server_buffer(&srv,stagebig,sizeof(stagebig)));
    CHECK(!lr2021_bridge_server_tune(&srv,1020,16384) && lr2021_bridge_server_tune(&srv,1020,16388));
    CHECK(!lr2021_bridge_server_predict(&srv,3) && lr2021_bridge_server_predict(&srv,65));
    uint8_t p[64]={1,0};
    CHECK(call(&h,LR2021_BRIDGE_HELLO,7,p,2)==0 && !lr2021_bridge_unpack_hello(h.last.payload,h.last.length,&hello2));
    CHECK(hello2.version==1 && hello2.rom_minor==24 && !strcmp(hello2.id,"sim0") && (hello2.caps&2));
    CHECK(call(&h,0x33,8,NULL,0)==LR2021_BRIDGE_EUNKNOWN);
    CHECK(call(&h,LR2021_BRIDGE_SET_RX,9,p,3)==LR2021_BRIDGE_ELENGTH && h.wakes==1);
    lr2021_bridge_rx m={433920000,62500,12,7,2,0,0,0};
    len=lr2021_bridge_pack_rx(p,&m);
    CHECK(call(&h,LR2021_BRIDGE_SET_RX,10,p,len)==0 && sim.lora_bw==3 && sim.boost==7);
    lr2021_bridge_rx_ack ack;
    CHECK(!lr2021_bridge_unpack_rx_ack(h.last.payload,h.last.length,&ack) && ack.rate_hz==250000);
    m.frequency_hz=50000000; len=lr2021_bridge_pack_rx(p,&m);
    CHECK(call(&h,LR2021_BRIDGE_SET_RX,11,p,len)==LR2021_IQ_ARGUMENT && sim.freq==433920000);
    m.frequency_hz=433920000; m.bandwidth_hz=31250; len=lr2021_bridge_pack_rx(p,&m);
    CHECK(call(&h,LR2021_BRIDGE_SET_RX,12,p,len)==0);
    uint8_t g[2]={9,0};
    CHECK(call(&h,LR2021_BRIDGE_SET_GAIN,13,g,2)==0 && sim.step==9 && srv.radio.rx.gain_step==9);

    /* 20 ms bounded stream: 2500 pairs, 8 queued blocks, continuous counter. */
    lr2021_bridge_put16(p,1024); lr2021_bridge_put32(p+2,20);
    CHECK(call(&h,LR2021_BRIDGE_RX_START,14,p,6)==0);
    CHECK(h.rx_end==1 && !h.gaps && h.pairs==h.end.pairs && h.pairs>2000 && h.end.reason==0);
    CHECK(h.end.generation==1 && !h.end.capture_error && !h.end.recovery_error && !h.end.dropped);
    /* Longer stream overruns the 32-slot queue in a single thread: the device
     * keeps streaming and counts the drops; the host receives fewer pairs. */
    memset(&h.end,0,sizeof(h.end)); h.pairs=0; h.next_pair=0; h.gaps=0;
    lr2021_bridge_put32(p+2,200);
    CHECK(call(&h,LR2021_BRIDGE_RX_START,15,p,6)==0);
    CHECK(h.rx_end==2 && h.end.dropped>0 && h.pairs<h.end.pairs && h.end.generation==2 && !h.end.capture_error);
    /* CS12 stream: 3 bytes per pair; the counter's large values force a shift. */
    memset(&h.end,0,sizeof(h.end)); h.pairs=0; h.next_pair=0; h.gaps=0; h.shifted=0;
    lr2021_bridge_put32(p+2,20); p[6]=LR2021_BRIDGE_CS12;
    CHECK(call(&h,LR2021_BRIDGE_RX_START,17,p,7)==0);
    CHECK(h.rx_end==3 && !h.gaps && h.pairs==h.end.pairs && h.shifted && !h.end.dropped);
    p[6]=4; CHECK(call(&h,LR2021_BRIDGE_RX_START,18,p,7)==LR2021_IQ_ARGUMENT);
    /* Raw route over the bridge: the 17-byte RX record selects source 0, and
     * frames carry IQ8 with up to 960 pairs whatever format was asked for. */
    lr2021_bridge_rx rawrx={433920000,0,8,0,2,LR2021_IQ_ROUTE_SOURCE0,17,576000};
    CHECK(lr2021_bridge_pack_rx(p,&rawrx)==17);
    CHECK(call(&h,LR2021_BRIDGE_SET_RX,19,p,17)==0 && sim.packet_type==2);
    CHECK(!lr2021_bridge_unpack_rx_ack(h.last.payload,h.last.length,&ack) && ack.rate_hz==576000);
    memset(&h.end,0,sizeof(h.end)); h.pairs=0; h.next_pair=0; h.gaps=0; h.formats=0;
    lr2021_bridge_put16(p,1024); lr2021_bridge_put32(p+2,10); p[6]=LR2021_BRIDGE_CS12;
    CHECK(call(&h,LR2021_BRIDGE_RX_START,20,p,7)==0);
    CHECK(h.rx_end==4 && !h.gaps && h.pairs==h.end.pairs && h.formats==(1u<<LR2021_BRIDGE_IQ8) && h.max_pairs==960);
    lr2021_bridge_rx rxback;
    CHECK(!lr2021_bridge_unpack_rx(p,11,&rxback) && !rxback.route);
    m.route=0; len=lr2021_bridge_pack_rx(p,&m);
    CHECK(call(&h,LR2021_BRIDGE_SET_RX,21,p,len)==0);
    lr2021_bridge_put16(p,512);
    CHECK(call(&h,LR2021_BRIDGE_RX_START,16,p,6)==LR2021_IQ_ARGUMENT);

    /* TX upload and commit. */
    static int8_t iq[4000]; static uint8_t cap[8000];
    for(unsigned i=0;i<4000;i++)iq[i]=(int8_t)((int)(i%73)-36);
    sim.tx_capture=cap; sim.tx_capacity=sizeof(cap);
    lr2021_bridge_tx_begin b={4000,lr2021_bridge_crc32(0,(const uint8_t *)iq,4000),19200,433920000,-18,2,0,0,LR2021_IQ_TX_CHECKED};
    CHECK(call(&h,LR2021_BRIDGE_TX_COMMIT,20,NULL,0)==LR2021_BRIDGE_ESTATE);
    len=lr2021_bridge_pack_tx_begin(p,&b);
    CHECK(call(&h,LR2021_BRIDGE_TX_BEGIN,21,p,len)==0);
    static uint8_t chunk[1100];
    for(uint32_t off=0;off<4000;off+=1000) {
        lr2021_bridge_put32(chunk,off); memcpy(chunk+4,iq+off,1000);
        CHECK(call(&h,LR2021_BRIDGE_TX_DATA,(uint16_t)(22+off),chunk,1004)==0);
        CHECK(lr2021_bridge_get32(h.last.payload+4)==off+1000);
    }
    lr2021_bridge_put32(chunk,0);
    CHECK(call(&h,LR2021_BRIDGE_TX_DATA,30,chunk,6)==LR2021_BRIDGE_ERANGE);
    CHECK(call(&h,LR2021_BRIDGE_TX_COMMIT,31,NULL,0)==0);
    CHECK(h.tx_end==1 && !h.tx.error && !h.tx.recovery_error && h.tx.samples==4000 && !(h.tx.tx_flags&0x30));
    CHECK(sim.tx_captured==8000 && !memcmp(cap,iq,4000) && !memcmp(cap+4000,iq,4000));
    /* Resend the same upload; then a corrupted CRC and an over-range sample. */
    sim.tx_captured=0;
    CHECK(call(&h,LR2021_BRIDGE_TX_COMMIT,32,NULL,0)==0 && h.tx_end==2 && sim.tx_captured==8000);
    b.crc32^=1; len=lr2021_bridge_pack_tx_begin(p,&b);
    CHECK(call(&h,LR2021_BRIDGE_TX_BEGIN,33,p,len)==0);
    lr2021_bridge_put32(chunk,0); memcpy(chunk+4,iq,1000);
    for(uint32_t off=0;off<4000;off+=1000){ lr2021_bridge_put32(chunk,off); CHECK(call(&h,LR2021_BRIDGE_TX_DATA,34,chunk,1004)==0); }
    CHECK(call(&h,LR2021_BRIDGE_TX_COMMIT,35,NULL,0)==LR2021_BRIDGE_ECRC);
    iq[0]=57; b.bytes=2; b.crc32=lr2021_bridge_crc32(0,(const uint8_t *)iq,2); len=lr2021_bridge_pack_tx_begin(p,&b);
    CHECK(call(&h,LR2021_BRIDGE_TX_BEGIN,36,p,len)==0);
    lr2021_bridge_put32(chunk,0); memcpy(chunk+4,iq,2);
    CHECK(call(&h,LR2021_BRIDGE_TX_DATA,37,chunk,6)==0);
    CHECK(call(&h,LR2021_BRIDGE_TX_COMMIT,38,NULL,0)==LR2021_BRIDGE_ERANGE);
    /* Default mode: the same over-range sample at an untested rate is sent
     * and reported in TX_END.untested. */
    b.rate_hz=10000; b.flags=0; len=lr2021_bridge_pack_tx_begin(p,&b);
    CHECK(call(&h,LR2021_BRIDGE_TX_BEGIN,39,p,len)==0);
    CHECK(call(&h,LR2021_BRIDGE_TX_DATA,42,chunk,6)==0);
    sim.tx_captured=0;
    CHECK(call(&h,LR2021_BRIDGE_TX_COMMIT,43,NULL,0)==0 && h.tx_end==3 && !h.tx.error);
    CHECK(h.tx.untested==(LR2021_IQ_TX_UNTESTED_RATE|LR2021_IQ_TX_UNTESTED_RADIUS) && sim.tx_captured==4);
    b.pa=3; len=lr2021_bridge_pack_tx_begin(p,&b);
    CHECK(call(&h,LR2021_BRIDGE_TX_BEGIN,44,p,len)==LR2021_BRIDGE_ERANGE);

    CHECK(call(&h,LR2021_BRIDGE_GET_TIME,40,NULL,0)==0 && lr2021_bridge_get64(h.last.payload+4)>0);
    CHECK(call(&h,LR2021_BRIDGE_BYE,41,NULL,0)==0 && lr2021_bridge_server_exit_requested(&srv));
    puts("bridge codec, resync and server transactions passed");
    return 0;
}
