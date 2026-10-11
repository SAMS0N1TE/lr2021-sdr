#include "lriq.h"
#include <stdio.h>
#include <string.h>

void lriq_fifo_init(lriq_fifo_parser *p, uint8_t *data, size_t capacity)
{
    memset(p, 0, sizeof(*p));
    p->data=data; p->capacity=capacity; p->crc=0xFFFFFFFFu;
}
static int nibble(char c)
{
    if(c>='0' && c<='9')return c-'0';
    if(c>='A' && c<='F')return c-'A'+10;
    return -1;
}
int lriq_fifo_line(lriq_fifo_parser *p, const char *line)
{
    if(p->failed)return -1;
    if(!strncmp(line,"lrfifo-hex:",11)) {
        unsigned offset; char hex[130]; int end=0;
        if(p->complete || p->checksum_seen ||
           sscanf(line,"lrfifo-hex: %u %129s%n",&offset,hex,&end)!=2 || line[end] ||
           offset!=p->bytes)goto invalid;
        size_t len=strlen(hex);
        if(!len || len>128 || len%8 || !p->data || p->bytes>p->capacity ||
           len/2>p->capacity-p->bytes)goto invalid;
        for(size_t i=0;i<len;i+=2) {
            int a=nibble(hex[i]),b=nibble(hex[i+1]);
            if(a<0 || b<0)goto invalid;
            uint8_t v=(uint8_t)(a*16+b);
            p->data[p->bytes++]=v; p->crc^=v;
            for(unsigned bit=0;bit<8;bit++)p->crc=(p->crc>>1)^((p->crc&1)?0xEDB88320u:0);
        }
    } else if(!strncmp(line,"lrfifo-crc32:",13)) {
        unsigned crc,bytes; int end=0;
        if(p->complete || p->checksum_seen ||
           sscanf(line,"lrfifo-crc32: %8X bytes=%u encoding=iq16le%n",&crc,&bytes,&end)!=2 ||
           !end || line[end] || !bytes || bytes!=p->bytes || crc!=~p->crc)goto invalid;
        p->checksum_seen=1;
    } else if(!strncmp(line,"lrfifo: bytes=",14)) {
        unsigned bytes,level,polls,flags; long long elapsed;
        char result[40],recovery[40]; int end=0;
        if(p->complete || !p->checksum_seen ||
           sscanf(line,"lrfifo: bytes=%u max_level=%u polls=%u elapsed_us=%lld flags=0x%X result=%39s recovery=%39s%n",
             &bytes,&level,&polls,&elapsed,&flags,result,recovery,&end)!=7 ||
           !end || line[end] || bytes!=p->bytes || (flags&0x30) ||
           strcmp(result,"ESP_OK") || strcmp(recovery,"ESP_OK"))goto invalid;
        p->complete=1;
    }
    return p->complete ? 1 : 0;
invalid:
    p->failed=1; return -1;
}
