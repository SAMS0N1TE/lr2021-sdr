/* Offline C11 inspection and timing tools. No serial, radio or write access. */
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <errno.h>
#include <math.h>

static uint32_t le32(const uint8_t *p)
{ return p[0]|(uint32_t)p[1]<<8|(uint32_t)p[2]<<16|(uint32_t)p[3]<<24; }
static uint32_t arc32(const uint8_t *p)
{ return (uint32_t)(p[0]|p[1]<<8)<<16 | p[2] | (uint32_t)p[3]<<8; }
static uint8_t *load(const char *path,size_t *size)
{
    FILE *f=fopen(path,"rb");if(!f){perror(path);return NULL;}
    if(fseek(f,0,SEEK_END)){fclose(f);return NULL;}
    long n=ftell(f);
    if(n<=0 || n>16*1024*1024 || fseek(f,0,SEEK_SET)){fclose(f);return NULL;}
    uint8_t *p=malloc((size_t)n);
    if(!p){fclose(f);return NULL;}
    if(fread(p,1,(size_t)n,f)!=(size_t)n || ferror(f)){free(p);fclose(f);return NULL;}
    fclose(f);*size=(size_t)n;return p;
}
static int number(const char *s,uint32_t *v)
{
    if(!s[0] || s[0]=='-')return 0;
    char *end;errno=0;unsigned long long n=strtoull(s,&end,0);
    if(errno || *end || n>UINT32_MAX)return 0;
    *v=(uint32_t)n;return 1;
}
static int known_rom(const uint8_t *p,size_t n)
{
    return n>=0x13f70 && le32(p+0x13bec)==0x5cec &&
        le32(p+0x13cd0+4*0x90)==0xf9a0;
}
static int commands(const uint8_t *p,size_t n)
{
    if(!known_rom(p,n)){fputs("Unsupported/truncated ROM table layout; refusing to label it ROM 1.24.\n",stderr);return 2;}
    puts("opcode,handler,shared_handlers,classification");
    const unsigned bases[]={0x13bec,0x13cd0},counts[]={0x37,0xa8},prefix[]={0x100,0x200};
    for(unsigned bank=0;bank<2;bank++)for(unsigned k=0;k<counts[bank];k++) {
        uint32_t target=le32(p+bases[bank]+4*k); unsigned shared=0;
        for(unsigned b=0;b<2;b++)for(unsigned j=0;j<counts[b];j++)
            shared+=le32(p+bases[b]+4*j)==target;
        printf("0x%04x,0x%06lx,%u,%s\n",prefix[bank]+k,(unsigned long)target,shared,
            target>=n || (target&1)?"invalid-pointer":
            target==0xd568 || target==0xd56c?"common-reject-candidate":"handler-candidate");
    }
    return 0;
}
static int refs(const uint8_t *p,size_t n,uint32_t value)
{
    puts("offset,encoding,value");
    size_t matches=0;
    for(size_t k=0;k+4<=n;k+=2) {
        if(le32(p+k)==value && matches++<4096)printf("0x%06zx,little-endian,0x%08lx\n",k,(unsigned long)value);
        if(arc32(p+k)==value && matches++<4096)printf("0x%06zx,ARC-halfword-order,0x%08lx\n",k,(unsigned long)value);
    }
    fprintf(stderr,"Pattern matches: %zu (listing capped at 4096).\n",matches);
    /* Matches are byte patterns, not proven instruction operands or xrefs. */
    return 0;
}
static int diff(const char *a,const char *b,uint32_t base)
{
    size_t na=0,nb=0;uint8_t *pa=load(a,&na),*pb=load(b,&nb);
    if(!pa || !pb || na!=nb || na%4 || (base&3) || na>UINT32_MAX-(uint64_t)base+1) {
        free(pa);free(pb);fputs("Diff requires equally sized aligned snapshots and a valid address range.\n",stderr);return 2;
    }
    puts("address,before,after,xor");size_t changed=0;
    for(size_t k=0;k<na;k+=4)if(le32(pa+k)!=le32(pb+k)) {
        if(changed<256)printf("0x%08lx,0x%08lx,0x%08lx,0x%08lx\n",
            (unsigned long)(base+k),(unsigned long)le32(pa+k),(unsigned long)le32(pb+k),
            (unsigned long)(le32(pa+k)^le32(pb+k)));
        changed++;
    }
    fprintf(stderr,"Changed words: %zu (listing capped at 256). Changes do not establish register semantics.\n",changed);
    free(pa);free(pb);return 0;
}
/* Optimistic FIFO model: instantaneous drain every service_us; a single
 * scheduling stall at 5 ms. Omits BUSY, SPI wire time and command overhead.
 * Failure even in this model rules out that schedule; success proves no board
 * performance. Loss invalidates the current 1024-byte validation block. */
static unsigned simulate(unsigned service_us,unsigned stall_us,unsigned capacity,
                         unsigned *accepted,unsigned *rejected)
{
    unsigned level=0,lost=0,pending=0;
    int bad=0;*accepted=*rejected=0;
    unsigned next=service_us;
    for(unsigned t=2;t<=10000;t+=2) {
        if(level<capacity)level++;else {lost++;bad=1;}
        if(t>=next) {
            unsigned drain=level&~3u;level-=drain;pending+=drain;
            if(pending>=1024) {
                if(bad)*rejected+=pending;else *accepted+=pending;
                pending=0;bad=0;
            }
            next+=service_us;
            if(t<5000 && next>=5000)next+=stall_us;
        }
    }
    if(bad)*rejected+=pending;else *accepted+=pending;
    return lost;
}
static int model(void)
{
    puts("Optimistic model: 125000 IQ16 pairs/s, 256-byte FIFO, 10 ms, instantaneous drains.");
    puts("service_us,stall_us,lost_bytes,qualified_bytes,rejected_bytes");
    const unsigned waits[]={0,200,400,600,1000};
    for(unsigned k=0;k<sizeof(waits)/sizeof(waits[0]);k++) {
        unsigned good,bad,lost=simulate(100,waits[k],256,&good,&bad);
        printf("100,%u,%u,%u,%u\n",waits[k],lost,good,bad);
    }
    puts("occupancy_bytes,remaining_time_us");
    for(unsigned k=0;k<=256;k+=64)printf("%u,%u\n",k,2*(256-k));
    puts("A host RAM queue cannot extend this chip-FIFO deadline. A 16 MHz SPI data-only ceiling is 2 MB/s;");
    puts("actual service includes BUSY, status commands, validation and host scheduling.");
    return 0;
}
static int hex_digit(char c)
{
    if(c>='0' && c<='9')return c-'0';
    if(c>='a' && c<='f')return c-'a'+10;
    if(c>='A' && c<='F')return c-'A'+10;
    return -1;
}
static int parse_route(const char *hex,uint8_t bytes[11])
{
    if(strlen(hex)!=22)return 0;
    for(unsigned k=0;k<11;k++) {
        int a=hex_digit(hex[2*k]),b=hex_digit(hex[2*k+1]);
        if(a<0 || b<0)return 0;
        bytes[k]=(uint8_t)(16*a+b);
    }
    return 1;
}
static int route(const char *hex)
{
    uint8_t p[11];if(!parse_route(hex,p))return 2;
    unsigned source=p[5]&7,mode=(p[5]>>3)&3,bank=p[5]>>5;
    unsigned count=(unsigned)p[8]*256+p[9];
    puts("Static interpretation of ROM 1.24 handler 0x00f9a0 -> 0x00cf7c. No commands sent.");
    printf("source=%u mode=%u bank=%u delay_field=%u limit_index=%u last_byte=%u\n",
        source,mode,bank,(unsigned)p[6]*256+p[7],count,p[10]);
    if(mode==1)puts("FIFO branch. Source 2 IQ16 is the measured route; other source meanings are unverified.");
    else if(mode==2 && bank>=1 && bank<=6) {
        const unsigned offsets[]={0,0x1000,0x1000,0x1000,0x2000,0x2000,0x4000};
        const unsigned limits[]={0,1023,3071,5119,2047,4095,2047};
        unsigned chosen=count && count<limits[bank]?count:limits[bank];
        printf("SRAM branch: inferred base=0x%06x maximum_index=%u selected_index=%u\n",
            0x800000+offsets[bank],limits[bank],chosen);
        puts("This does not prove circular buffering or safe SRAM ownership. Banks may overlap firmware/patch memory.");
    } else puts("Other/disabled/unsupported branch; interpretation incomplete.");
    return 0;
}
static double s16(const uint8_t *p)
{ unsigned v=p[0]|(unsigned)p[1]<<8;return v<32768?(double)v:(double)v-65536; }
static unsigned phase_steps(const uint8_t *p,size_t bytes,double rate,double tone,
                            double threshold,double *worst,size_t *tested)
{
    size_t n=bytes/4;double mi=0,mq=0,power=0;
    for(size_t k=0;k<n;k++){mi+=s16(p+4*k);mq+=s16(p+4*k+2);}
    mi/=n;mq/=n;
    for(size_t k=0;k<n;k++) {
        double i=s16(p+4*k)-mi,q=s16(p+4*k+2)-mq;power+=i*i+q*q;
    }
    double floor_power=.0025*power/n,expected=6.283185307179586*tone/rate;
    unsigned bad=0;*worst=0;*tested=0;
    for(size_t k=1;k<n;k++) {
        double ai=s16(p+4*(k-1))-mi,aq=s16(p+4*(k-1)+2)-mq;
        double bi=s16(p+4*k)-mi,bq=s16(p+4*k+2)-mq;
        if(ai*ai+aq*aq<=floor_power || bi*bi+bq*bq<=floor_power)continue;
        double error=fabs(remainder(atan2(ai*bq-aq*bi,ai*bi+aq*bq)-expected,6.283185307179586))*57.29577951308232;
        (*tested)++;if(error>*worst)*worst=error;if(error>threshold)bad++;
    }
    return bad;
}
static int continuity(const char *path,const char *rate_arg,const char *tone_arg)
{
    uint32_t rate;if(!number(rate_arg,&rate) || !rate)return 2;
    char *end;errno=0;double tone=strtod(tone_arg,&end);
    if(errno || !tone_arg[0] || *end || !isfinite(tone) || tone==0 || fabs(tone)>=rate*.5)return 2;
    size_t n=0;uint8_t *p=load(path,&n);if(!p)return 2;
    if(n<16 || n%4){free(p);return 2;}
    double worst;size_t tested;unsigned bad=phase_steps(p,n,rate,tone,30,&worst,&tested);
    printf("pairs=%zu tested_steps=%zu phase_outliers=%u max_error_deg=%.3f threshold_deg=30\n",n/4,tested,bad,worst);
    puts("Known single CW tone only; skips near-zero amplitude after DC removal. Noise, fading, clipping and");
    puts("wrong rate/frequency can create outliers. Whole-tone-cycle losses are invisible. Not proof of continuity.");
    free(p);return tested?0:1;
}
static int selftest(void)
{
    const uint8_t p[]={0xf3,0,0x44,0};
    if(arc32(p)!=0xf30044 || le32(p)!=0x004400f3)return 1;
    uint8_t *rom=calloc(0x14000,1);if(!rom)return 2;
    if(known_rom(rom,0x14000)){free(rom);return 1;}
    rom[0x13bec]=0xec;rom[0x13bed]=0x5c;
    rom[0x13f10]=0xa0;rom[0x13f11]=0xf9;
    int ok=known_rom(rom,0x14000) && !known_rom(rom,0x13f6f);free(rom);
    unsigned good,bad;
    if(simulate(100,0,256,&good,&bad) || bad || good!=5000)return 1;
    if(!simulate(100,1000,256,&good,&bad) || !bad)return 1;
    uint8_t route_bytes[11];
    if(!parse_route("FF0000000092000000FF00",route_bytes) || route_bytes[5]!=0x92 || route_bytes[9]!=255)return 1;
    if(parse_route("FF",route_bytes) || parse_route("FF0000000092000000FF0Z",route_bytes))return 1;
    uint8_t iq[4000];
    for(unsigned k=0;k<1000;k++) {
        double a=6.283185307179586*10000*k/125000;
        int i=(int)lround(16000*cos(a)),q=(int)lround(16000*sin(a));
        iq[4*k]=(uint8_t)i;iq[4*k+1]=(uint8_t)((unsigned)i>>8);
        iq[4*k+2]=(uint8_t)q;iq[4*k+3]=(uint8_t)((unsigned)q>>8);
    }
    double worst;size_t tested;
    if(phase_steps(iq,sizeof(iq),125000,10000,30,&worst,&tested) || tested!=999)return 1;
    memmove(iq+2000,iq+2020,1980);
    if(phase_steps(iq,3980,125000,10000,30,&worst,&tested)!=1)return 1;
    return ok?0:1;
}
int main(int argc,char **argv)
{
    if(argc==2 && !strcmp(argv[1],"selftest"))return selftest();
    if(argc==2 && !strcmp(argv[1],"model"))return model();
    if(argc==3 && !strcmp(argv[1],"route"))return route(argv[2]);
    if(argc==5 && !strcmp(argv[1],"continuity"))return continuity(argv[2],argv[3],argv[4]);
    uint32_t value;
    if(argc==5 && !strcmp(argv[1],"diff") && number(argv[4],&value))return diff(argv[2],argv[3],value);
    if((argc==3 && !strcmp(argv[1],"commands")) ||
       (argc==4 && !strcmp(argv[1],"refs") && number(argv[3],&value))) {
        size_t n=0;uint8_t *p=load(argv[2],&n);if(!p)return 2;
        int rc=argc==3?commands(p,n):refs(p,n,value);free(p);return rc;
    }
    fputs("lr2021_lab commands rom.bin\nlr2021_lab refs image.bin 0xADDRESS\n"
          "lr2021_lab diff before.bin after.bin 0xBASE\nlr2021_lab route 22_HEX_DIGITS\n"
          "lr2021_lab continuity iq16.bin rate_hz known_tone_hz\nlr2021_lab model|selftest\n"
          "Offline only. Pattern matches are candidates, never permission to execute unknown commands.\n",stderr);
    return 2;
}
