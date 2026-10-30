static int agc_probe_bw_code(uint32_t bw)
{
    return bw==31250?2:bw==62500?3:bw==125000?4:bw==250000?5:-1;
}
static int agc_probe_restore(const lr2021_iq_hal *h)
{
    int e=h->standby(h->context);
    int c=lr2021_iq_dev_configure(radio_dev(),&radio_dev()->rx);
    return e?e:c;
}
static int agc_rssi_command(int argc, char **argv)
{
    unsigned count=argc>1?(unsigned)strtoul(argv[1],NULL,0):20;
    unsigned interval=argc>2?(unsigned)strtoul(argv[2],NULL,0):50;
    if(argc>3 || !count || count>10000 || interval>10000) {
        puts("usage: agcrssi [count 1..10000] [interval_ms 0..10000]");
        return 1;
    }
    const lr2021_iq_hal *h=radio_hal();
    h->lock(h->context);
    int e=h->receive(h->context);
    uint64_t t0=h->now_us(h->context);
    puts("agcrssi: t_ms,inst_b0,inst_b1,inst_raw,inst_dbm,field_raw,step,manual,gain_value,match");
    for(unsigned i=0;!e && i<count;i++) {
        uint8_t r[2]={0};
        uint32_t f=0,agc=0,value=0;
        e=h->read(h->context,0x020B,r,2);
        if(!e)e=h->read32(h->context,0xF301A8,&f);
        if(!e)e=h->read32(h->context,0xF30154,&agc);
        if(!e)e=h->read32(h->context,0xF3012C,&value);
        if(e)break;
        unsigned inst=((unsigned)r[0]<<1)|(r[1]>>7), field=(f>>18)&0x1FF;
        printf("agcrssi: %llu,%u,%u,%u,%.1f,%u,%lu,%lu,%lu,%d\n",
               (unsigned long long)((h->now_us(h->context)-t0)/1000),r[0],r[1],inst,-(double)inst/2,field,
               (unsigned long)((agc>>20)&15),(unsigned long)((agc>>16)&15),(unsigned long)((value>>18)&0x1FF),
               inst==field);
        if(interval)h->sleep_us(h->context,interval*1000u);
    }
    int r=agc_probe_restore(h);
    h->unlock(h->context);
    printf("agcrssi: done error=%d restore=%d\n",e,r);
    return e||r;
}
static int pkt_status_command(int argc, char **argv)
{
    unsigned seconds=argc>1?(unsigned)strtoul(argv[1],NULL,0):60;
    uint32_t freq=argc>2?(uint32_t)strtoul(argv[2],NULL,0):910525000u;
    uint32_t bw=argc>3?(uint32_t)strtoul(argv[3],NULL,0):62500u;
    unsigned sf=argc>4?(unsigned)strtoul(argv[4],NULL,0):7;
    unsigned sync=argc>5?(unsigned)strtoul(argv[5],NULL,0):0x12;
    unsigned gain=argc>6?(unsigned)strtoul(argv[6],NULL,0):0;
    unsigned preamble=argc>7?(unsigned)strtoul(argv[7],NULL,0):16;
    int code=agc_probe_bw_code(bw);
    if(argc>8 || !seconds || seconds>3600 || freq<150000000u || freq>960000000u || code<0 ||
       sf<5 || sf>12 || sync>255 || gain>13 || preamble<6 || preamble>65535) {
        puts("usage: pktstatus [seconds 60] [freq_hz 910525000] [bw 31250|62500|125000|250000] "
             "[sf 5..12] [sync 0x12] [gain 0=AGC|1..13] [preamble 16]");
        return 1;
    }
    const lr2021_iq_hal *h=radio_hal();
    void *c=h->context;
    const uint8_t type=0, step=(uint8_t)gain, sw=(uint8_t)sync, path[2]={0,0};
    const uint8_t f[4]={(uint8_t)(freq>>24),(uint8_t)(freq>>16),(uint8_t)(freq>>8),(uint8_t)freq};
    const uint8_t mod[2]={(uint8_t)((sf<<4)|(unsigned)code),(uint8_t)(0x10|(sf>=11 && bw<=125000?1:0))};
    const uint8_t pkt[4]={(uint8_t)(preamble>>8),(uint8_t)preamble,255,2};
    const uint8_t all[4]={0xFF,0xFF,0xFF,0xFF}, forever[3]={0xFF,0xFF,0xFF};
    h->lock(c);
    int e=h->standby(c);
    if(!e)e=h->write(c,0x0207,&type,1);
    if(!e)e=h->write(c,0x0200,f,4);
    if(!e)e=h->write(c,0x0201,path,2);
    if(!e)e=h->write(c,0x021A,&step,1);
    if(!e)e=h->write(c,0x0220,mod,2);
    if(!e)e=h->write(c,0x0221,pkt,4);
    if(!e)e=h->write(c,0x0223,&sw,1);
    if(!e)e=h->write(c,0x0116,all,4);
    if(!e)e=h->write(c,0x011E,NULL,0);
    if(!e)e=h->write(c,0x020C,forever,3);
    printf("pktstatus: %lu Hz bw %lu sf %u sync 0x%02X gain %u preamble %u for %u s\n",
           (unsigned long)freq,(unsigned long)bw,sf,sync,gain,preamble,seconds);
    puts("pktstatus: t_ms,irq,len,b0,b1,b2,b3,b4,b5,b6,b7,b8,b9,gain_b9,step_now,rssi_dbm,snr_db,fei_raw");
    uint64_t t0=h->now_us(c);
    unsigned packets=0;
    while(!e && h->now_us(c)-t0<(uint64_t)seconds*1000000u) {
        uint8_t q[4]={0};
        e=h->read(c,0x0117,q,4);
        uint32_t irq=((uint32_t)q[0]<<24)|((uint32_t)q[1]<<16)|((uint32_t)q[2]<<8)|q[3];
        if(e || !(irq&((1u<<18)|(1u<<22)|(1u<<9)))) {
            h->sleep_us(c,2000);
            continue;
        }
        uint8_t s[10]={0}, l[2]={0}, payload[256];
        uint32_t agc=0;
        e=h->read(c,0x022A,s,10);
        if(!e)e=h->read32(c,0xF30154,&agc);
        if(!e)e=h->read(c,0x0212,l,2);
        unsigned len=((unsigned)l[0]<<8)|l[1];
        if(!e && (irq&(1u<<18)) && len && len<=255 && h->read_fifo)e=h->read_fifo(c,payload,len);
        if(!e)e=h->write(c,0x011E,NULL,0);
        if(e)break;
        packets++;
        const unsigned rssi=((unsigned)s[3]<<1)|((s[5]>>1)&1u);
        const uint32_t fei=((uint32_t)s[6]<<16)|((uint32_t)s[7]<<8)|s[8];
        printf("pktstatus: %llu,0x%08lX,%u,%02X,%02X,%02X,%02X,%02X,%02X,%02X,%02X,%02X,%02X,%u,%lu,%.1f,%.2f,%lu\n",
               (unsigned long long)((h->now_us(c)-t0)/1000),(unsigned long)irq,len,
               s[0],s[1],s[2],s[3],s[4],s[5],s[6],s[7],s[8],s[9],s[9],(unsigned long)((agc>>20)&15),
               -(double)rssi/2,(double)(int8_t)s[2]/4,(unsigned long)fei);
    }
    int r=agc_probe_restore(h);
    h->unlock(c);
    printf("pktstatus: %u packets, error=%d restore=%d\n",packets,e,r);
    return e||r;
}
