/* SPDX-License-Identifier: GPL-3.0-only */
#include "lr2021_bridge.h"
#include <string.h>

void lr2021_bridge_put16(uint8_t *p, uint16_t v) { p[0]=(uint8_t)v; p[1]=(uint8_t)(v>>8); }
void lr2021_bridge_put32(uint8_t *p, uint32_t v)
{ for(unsigned i=0;i<4;i++)p[i]=(uint8_t)(v>>(8*i)); }
void lr2021_bridge_put64(uint8_t *p, uint64_t v)
{ for(unsigned i=0;i<8;i++)p[i]=(uint8_t)(v>>(8*i)); }
uint16_t lr2021_bridge_get16(const uint8_t *p) { return (uint16_t)(p[0]|p[1]<<8); }
uint32_t lr2021_bridge_get32(const uint8_t *p)
{ uint32_t v=0; for(unsigned i=0;i<4;i++)v|=(uint32_t)p[i]<<(8*i); return v; }
uint64_t lr2021_bridge_get64(const uint8_t *p)
{ uint64_t v=0; for(unsigned i=0;i<8;i++)v|=(uint64_t)p[i]<<(8*i); return v; }
#define P16 lr2021_bridge_put16
#define P32 lr2021_bridge_put32
#define P64 lr2021_bridge_put64
#define G16 lr2021_bridge_get16
#define G32 lr2021_bridge_get32
#define G64 lr2021_bridge_get64

uint32_t lr2021_bridge_crc32(uint32_t crc, const uint8_t *p, size_t n)
{
    crc=~crc;
    while(n--) {
        crc^=*p++;
        for(unsigned k=0;k<8;k++)crc=(crc>>1)^((0u-(crc&1u))&0xEDB88320u);
    }
    return ~crc;
}
size_t lr2021_bridge_encode(uint8_t *out, size_t cap, uint8_t type, uint16_t seq,
                           const uint8_t *payload, size_t n)
{
    if(n>LR2021_BRIDGE_MAX_PAYLOAD || cap<LR2021_BRIDGE_HEADER+n+4 || (n && !payload))return 0;
    out[0]='L'; out[1]='R'; out[2]=LR2021_BRIDGE_VERSION; out[3]=type;
    P16(out+4,seq); P16(out+6,(uint16_t)n);
    if(n)memmove(out+LR2021_BRIDGE_HEADER,payload,n);
    P32(out+LR2021_BRIDGE_HEADER+n,lr2021_bridge_crc32(0,out+2,LR2021_BRIDGE_HEADER-2+n));
    return LR2021_BRIDGE_HEADER+n+4;
}
void lr2021_bridge_parser_init(lr2021_bridge_parser *p) { memset(p,0,sizeof(*p)); }
static void drop(lr2021_bridge_parser *p, size_t n)
{
    memmove(p->buf,p->buf+n,p->used-n); p->used-=n;
}
void lr2021_bridge_parse(lr2021_bridge_parser *p, const uint8_t *data, size_t n,
                         lr2021_bridge_frame_cb cb, void *ctx)
{
    while(n || p->used) {
        size_t room=sizeof(p->buf)-p->used, take=n<room?n:room;
        memcpy(p->buf+p->used,data,take); p->used+=take; data+=take; n-=take;
        bool progress=true;
        while(progress && p->used) {
            progress=false;
            if(p->buf[0]!='L' || (p->used>1 && p->buf[1]!='R') ||
               (p->used>2 && p->buf[2]!=LR2021_BRIDGE_VERSION)) {
                drop(p,1); p->skipped_bytes++; progress=true; continue;
            }
            if(p->used<LR2021_BRIDGE_HEADER)break;
            size_t len=G16(p->buf+6), size=LR2021_BRIDGE_HEADER+len+4;
            if(len>LR2021_BRIDGE_MAX_PAYLOAD) { drop(p,1); p->skipped_bytes++; progress=true; continue; }
            if(p->used<size)break;
            if(lr2021_bridge_crc32(0,p->buf+2,LR2021_BRIDGE_HEADER-2+len)!=G32(p->buf+LR2021_BRIDGE_HEADER+len)) {
                drop(p,1); p->crc_errors++; p->skipped_bytes++; progress=true; continue;
            }
            lr2021_bridge_frame f={p->buf[3],G16(p->buf+4),(uint16_t)len,p->buf+LR2021_BRIDGE_HEADER};
            p->frames++;
            if(cb)cb(ctx,&f);
            drop(p,size); progress=true;
        }
        if(!n)break;
    }
}

static size_t bounded(const char *s, size_t max)
{ size_t n=0; while(n<max && s[n])n++; return n; }
size_t lr2021_bridge_pack_hello(uint8_t *p, const lr2021_bridge_hello *m)
{
    P32(p,(uint32_t)m->status); P16(p+4,m->version); P16(p+6,m->caps);
    p[8]=m->rom_major; p[9]=m->rom_minor; P16(p+10,m->max_payload);
    P32(p+12,m->clock_hz); P32(p+16,m->tx_capacity);
    memset(p+20,0,64);
    memcpy(p+20,m->name,bounded(m->name,31)); memcpy(p+52,m->id,bounded(m->id,31));
    return 84;
}
int lr2021_bridge_unpack_hello(const uint8_t *p, size_t n, lr2021_bridge_hello *m)
{
    if(n!=84)return -1;
    m->status=(int32_t)G32(p); m->version=G16(p+4); m->caps=G16(p+6);
    m->rom_major=p[8]; m->rom_minor=p[9]; m->max_payload=G16(p+10);
    m->clock_hz=G32(p+12); m->tx_capacity=G32(p+16);
    memcpy(m->name,p+20,32); m->name[31]=0; memcpy(m->id,p+52,32); m->id[31]=0;
    return 0;
}
size_t lr2021_bridge_pack_rx(uint8_t *p, const lr2021_bridge_rx *m)
{
    P32(p,m->frequency_hz); P32(p+4,m->bandwidth_hz);
    p[8]=m->gain_step; p[9]=m->boost; p[10]=m->path; return 11;
}
int lr2021_bridge_unpack_rx(const uint8_t *p, size_t n, lr2021_bridge_rx *m)
{
    if(n!=11)return -1;
    m->frequency_hz=G32(p); m->bandwidth_hz=G32(p+4);
    m->gain_step=p[8]; m->boost=p[9]; m->path=p[10]; return 0;
}
size_t lr2021_bridge_pack_rx_ack(uint8_t *p, const lr2021_bridge_rx_ack *m)
{ P32(p,(uint32_t)m->status); P16(p+4,m->radio_errors); P32(p+6,m->rate_hz); return 10; }
int lr2021_bridge_unpack_rx_ack(const uint8_t *p, size_t n, lr2021_bridge_rx_ack *m)
{
    if(n!=10)return -1;
    m->status=(int32_t)G32(p); m->radio_errors=G16(p+4); m->rate_hz=G32(p+6); return 0;
}
size_t lr2021_bridge_pack_tx_begin(uint8_t *p, const lr2021_bridge_tx_begin *m)
{
    P32(p,m->bytes); P32(p+4,m->crc32); P32(p+8,m->rate_hz); P32(p+12,m->frequency_hz);
    p[16]=(uint8_t)m->power_half_db; p[17]=m->repeats; P64(p+18,m->start_time); return 26;
}
int lr2021_bridge_unpack_tx_begin(const uint8_t *p, size_t n, lr2021_bridge_tx_begin *m)
{
    if(n!=26)return -1;
    m->bytes=G32(p); m->crc32=G32(p+4); m->rate_hz=G32(p+8); m->frequency_hz=G32(p+12);
    m->power_half_db=(int8_t)p[16]; m->repeats=p[17]; m->start_time=G64(p+18); return 0;
}
size_t lr2021_bridge_pack_rx_data(uint8_t *p, const lr2021_bridge_rx_data *m)
{
    P32(p,m->generation); P32(p+4,m->rate_hz); P64(p+8,m->first_pair);
    P64(p+16,m->start_time); P32(p+24,m->dropped); return LR2021_BRIDGE_RX_DATA_HEADER;
}
int lr2021_bridge_unpack_rx_data(const uint8_t *p, size_t n, lr2021_bridge_rx_data *m)
{
    if(n<LR2021_BRIDGE_RX_DATA_HEADER+4 || n>LR2021_BRIDGE_RX_DATA_HEADER+LR2021_BRIDGE_RX_MAX_BYTES ||
       (n-LR2021_BRIDGE_RX_DATA_HEADER)%4)return -1;
    m->generation=G32(p); m->rate_hz=G32(p+4); m->first_pair=G64(p+8);
    m->start_time=G64(p+16); m->dropped=G32(p+24); return 0;
}
size_t lr2021_bridge_pack_rx_end(uint8_t *p, const lr2021_bridge_rx_end *m)
{
    P32(p,m->generation); P32(p+4,(uint32_t)m->capture_error); P32(p+8,(uint32_t)m->recovery_error);
    p[12]=m->reason; p[13]=m->fifo_flags; P16(p+14,m->max_fifo_level);
    P32(p+16,m->dropped); P32(p+20,m->gain_changes); P64(p+24,m->pairs);
    P64(p+32,m->start_time); P64(p+40,m->elapsed_us); P64(p+48,m->max_service_gap_us);
    return 56;
}
int lr2021_bridge_unpack_rx_end(const uint8_t *p, size_t n, lr2021_bridge_rx_end *m)
{
    if(n!=56)return -1;
    m->generation=G32(p); m->capture_error=(int32_t)G32(p+4); m->recovery_error=(int32_t)G32(p+8);
    m->reason=p[12]; m->fifo_flags=p[13]; m->max_fifo_level=G16(p+14);
    m->dropped=G32(p+16); m->gain_changes=G32(p+20); m->pairs=G64(p+24);
    m->start_time=G64(p+32); m->elapsed_us=G64(p+40); m->max_service_gap_us=G64(p+48);
    return 0;
}
size_t lr2021_bridge_pack_tx_end(uint8_t *p, const lr2021_bridge_tx_end *m)
{
    P32(p,(uint32_t)m->error); P32(p+4,(uint32_t)m->stop_error); P32(p+8,(uint32_t)m->recovery_error);
    p[12]=m->tx_flags; P16(p+13,m->final_level); P16(p+15,m->min_level);
    P32(p+17,m->irq); P32(p+21,m->samples); P64(p+25,m->start_time);
    P64(p+33,m->elapsed_us); P64(p+41,m->max_service_gap_us); return 49;
}
int lr2021_bridge_unpack_tx_end(const uint8_t *p, size_t n, lr2021_bridge_tx_end *m)
{
    if(n!=49)return -1;
    m->error=(int32_t)G32(p); m->stop_error=(int32_t)G32(p+4); m->recovery_error=(int32_t)G32(p+8);
    m->tx_flags=p[12]; m->final_level=G16(p+13); m->min_level=G16(p+15);
    m->irq=G32(p+17); m->samples=G32(p+21); m->start_time=G64(p+25);
    m->elapsed_us=G64(p+33); m->max_service_gap_us=G64(p+41); return 0;
}
const char *lr2021_bridge_strerror(int e)
{
    switch(e) {
    case 0: return "ok";
    case -1: return "invalid argument";
    case -2: return "radio state or ROM version refused";
    case -3: return "timeout";
    case -4: return "radio data, status or FIFO error";
    case -5: return "bus I/O error";
    case LR2021_BRIDGE_EBUSY: return "device busy";
    case LR2021_BRIDGE_EUNKNOWN: return "unknown request";
    case LR2021_BRIDGE_ELENGTH: return "bad payload length";
    case LR2021_BRIDGE_ESTATE: return "wrong protocol state";
    case LR2021_BRIDGE_EUNSUPPORTED: return "unsupported";
    case LR2021_BRIDGE_ECRC: return "upload CRC mismatch";
    case LR2021_BRIDGE_ERANGE: return "outside the TX contract";
    default: return "error";
    }
}
