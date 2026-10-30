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
