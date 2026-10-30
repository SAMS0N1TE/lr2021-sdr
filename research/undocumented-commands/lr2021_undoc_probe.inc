static int undoc_frame(lr2021_iq_dev *d, uint8_t *b, size_t n)
{
    uint64_t end=d->bus.now_us(d->bus.context)+200000;
    for(;;) {
        int busy=d->bus.busy(d->bus.context);
        if(busy<0)return LR2021_IQ_IO;
        if(!busy)break;
        if(d->bus.now_us(d->bus.context)>=end)return LR2021_IQ_TIMEOUT;
    }
    return d->bus.transfer(d->bus.context,b,n)?LR2021_IQ_IO:0;
}
static int undoc_read(uint16_t op, const uint8_t *a, size_t n, uint8_t *out, size_t m, unsigned *status)
{
    lr2021_iq_dev *d=radio_dev();
    uint8_t b[64]={(uint8_t)(op>>8),(uint8_t)op};
    if(n>sizeof(b)-2 || m>sizeof(b)-2)return LR2021_IQ_ARGUMENT;
    if(n)memcpy(b+2,a,n);
    int e=undoc_frame(d,b,n+2);
    if(e)return e;
    memset(b,0,m+2);
    e=undoc_frame(d,b,m+2);
    if(e)return e;
    *status=((unsigned)b[0]<<8)|b[1];
    memcpy(out,b+2,m);
    return 0;
}
static void undoc_print(const char *what, int e, unsigned status, const uint8_t *r, size_t m)
{
    printf("undocprobe: %s error=%d stat=0x%04X cmd=%u data=",what,e,status,(status>>9)&7);
    for(size_t i=0;i<m;i++)printf("%02X",r[i]);
    putchar('\n');
}
static int undoc_probe_command(int argc, char **argv)
{
    const char *what=argc>1?argv[1]:"all";
    bool all=!strcmp(what,"all");
    if(argc>2 || (!all && strcmp(what,"aux") && strcmp(what,"param") && strcmp(what,"getters"))) {
        puts("usage: undocprobe [all|aux|param|getters]");
        return 1;
    }
    const lr2021_iq_hal *h=radio_hal();
    h->lock(h->context);
    const bool was=radio_dev()->profile.receiving;
    lr2021_iq_profile p={0};
    int e=h->snapshot(h->context,&p);
    if(!e && (!p.lr2021 || p.fw_major!=1 || p.fw_minor!=24))e=LR2021_IQ_STATE;
    if(e) {
        h->unlock(h->context);
        printf("undocprobe: refused, ROM 1.24 only (error %d)\n",e);
        return 1;
    }
    uint8_t r[32]; unsigned st=0;
    if(all || !strcmp(what,"aux")) {
        const uint8_t a[4]={0,0,0x02,0x90};
        memset(r,0,sizeof(r));
        e=undoc_read(0x0103,a,4,r,4,&st);
        undoc_print("0x0103 aux 0x290 jli_base",e,st,r,4);
    }
    if(all || !strcmp(what,"param")) {
        for(uint8_t i=0x0B;i<=0x15;i++) {
            char label[40];
            memset(r,0,sizeof(r));
            e=undoc_read(0x0213,&i,1,r,1,&st);
            snprintf(label,sizeof(label),"0x0213 get 0x%02X",i);
            undoc_print(label,e,st,r,1);
        }
    }
    if(all || !strcmp(what,"getters")) {
        for(int pass=0;pass<2;pass++) {
            int x=pass?h->receive(h->context):h->standby(h->context);
            memset(r,0,sizeof(r));
            e=x?x:undoc_read(0x023E,NULL,0,r,6,&st);
            undoc_print(pass?"0x023E in RX":"0x023E in standby",e,st,r,6);
            memset(r,0,sizeof(r));
            e=x?x:undoc_read(0x023F,NULL,0,r,18,&st);
            undoc_print(pass?"0x023F in RX":"0x023F in standby",e,st,r,18);
        }
    }
    int s=h->standby(h->context);
    h->unlock(h->context);
    int c=lr2021_iq_dev_configure(radio_dev(),&radio_dev()->rx);
    if(!c && was) {
        h->lock(h->context);
        c=h->receive(h->context);
        h->unlock(h->context);
    }
    printf("undocprobe: done restore=%d\n",s?s:c);
    return s||c;
}
