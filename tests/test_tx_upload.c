#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
static bool probe_number(const char *s,uint32_t limit,uint32_t *out)
{
    char *end; unsigned long n;
    if(!s || *s<'0' || *s>'9') return false;
    errno=0;n=strtoul(s,&end,0);
    if(errno || *end || n>limit) return false;
    *out=(uint32_t)n;return true;
}
#include "../research/tx-payload/lr2021_tx_upload.inc"
static int call(const char *action,const char *a,const char *b)
{
    char *args[]={"lora","lrtx-upload",(char *)action,(char *)a,(char *)b};
    return tx_upload_command(b?5:a?4:3,args);
}
int main(void)
{
    char crc[16];const uint8_t good[]={24,0};
    assert(tx_upload_crc32((const uint8_t *)"123456789",9)==0xCBF43926u);
    snprintf(crc,sizeof(crc),"0x%08lX",(unsigned long)tx_upload_crc32(good,2));
    assert(call("begin","2",crc)==0);
    assert(call("commit",NULL,NULL)!=0 && !tx_upload_ready);
    assert(call("chunk","2","1800")!=0 && tx_upload_next==0);
    assert(call("chunk","0","18zz")!=0 && tx_upload_next==0);
    assert(call("chunk","0","18000000")!=0 && tx_upload_next==0);
    assert(call("chunk","0","1800")==0);
    assert(call("commit",NULL,NULL)==0 && tx_upload_ready);
    assert(call("chunk","0","1800")!=0);
    assert(call("status",NULL,NULL)==0);
    assert(call("begin","16386",crc)!=0 && !tx_upload_ready);
    assert(call("begin","3",crc)!=0);
    assert(call("begin","2","0")==0);
    assert(call("chunk","0","1800")==0);
    assert(call("commit",NULL,NULL)!=0 && !tx_upload_ready);
    { const uint8_t high[]={57,0};snprintf(crc,sizeof(crc),"0x%08lX",(unsigned long)tx_upload_crc32(high,2)); }
    assert(call("begin","2",crc)==0);
    assert(call("chunk","0","3900")==0);
    assert(call("commit",NULL,NULL)!=0 && !tx_upload_ready);
    assert(call("abort",NULL,NULL)==0 && tx_upload_next==0 && tx_upload_size==0);
    puts("TX upload transaction tests passed");return 0;
}
