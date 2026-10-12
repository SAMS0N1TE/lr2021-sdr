/* Bounded RAM upload for the experimental LR2021 TX path. No RF here. */
#define TX_UPLOAD_CAPACITY 16384u
static uint8_t tx_upload_data[TX_UPLOAD_CAPACITY];
static uint32_t tx_upload_size, tx_upload_next, tx_upload_expected_crc;
static bool tx_upload_ready;
static uint32_t tx_upload_crc32(const uint8_t *p, size_t n)
{
    uint32_t crc=UINT32_MAX;
    while(n--) { crc^=*p++; for(unsigned b=0;b<8;++b) crc=(crc>>1)^((0u-(crc&1u))&0xEDB88320u); }
    return ~crc;
}
static int tx_upload_hex(char c)
{
    if(c>='0' && c<='9') return c-'0';
    if(c>='a' && c<='f') return c-'a'+10;
    if(c>='A' && c<='F') return c-'A'+10;
    return -1;
}
static int tx_upload_command(int argc,char **argv)
{
    if(argc==3 && !strcmp(argv[2],"abort")) {
        tx_upload_ready=false;tx_upload_size=tx_upload_next=0;
        puts("lrtx-upload: aborted");return 0;
    }
    if(argc==5 && !strcmp(argv[2],"begin")) {
        uint32_t size,crc;
        tx_upload_ready=false;tx_upload_size=tx_upload_next=0;
        if(!probe_number(argv[3],TX_UPLOAD_CAPACITY,&size) || !size || (size&1) ||
           !probe_number(argv[4],UINT32_MAX,&crc)) goto invalid;
        tx_upload_size=size;tx_upload_expected_crc=crc;
        printf("lrtx-upload: next=0 size=%lu\n",(unsigned long)size);return 0;
    }
    if(argc==5 && !strcmp(argv[2],"chunk")) {
        uint32_t offset;size_t len=strlen(argv[4]);uint8_t chunk[64];
        if(!tx_upload_size || tx_upload_ready || !probe_number(argv[3],TX_UPLOAD_CAPACITY,&offset) ||
           offset!=tx_upload_next || !len || len>128 || (len&3) || len/2>tx_upload_size-tx_upload_next) goto invalid;
        for(size_t i=0;i<len/2;++i) {
            int a=tx_upload_hex(argv[4][2*i]),b=tx_upload_hex(argv[4][2*i+1]);
            if(a<0 || b<0) goto invalid;
            chunk[i]=(uint8_t)((a<<4)|b);
        }
        memcpy(tx_upload_data+offset,chunk,len/2);tx_upload_next+=(uint32_t)(len/2);
        printf("lrtx-upload: next=%lu\n",(unsigned long)tx_upload_next);return 0;
    }
    if(argc==3 && !strcmp(argv[2],"commit")) {
        tx_upload_ready=false;
        if(!tx_upload_size || tx_upload_next!=tx_upload_size ||
           tx_upload_crc32(tx_upload_data,tx_upload_size)!=tx_upload_expected_crc) goto invalid;
        for(uint32_t i=0;i<tx_upload_size;i+=2) {
            int a=(int8_t)tx_upload_data[i],b=(int8_t)tx_upload_data[i+1];
            if(a*a+b*b>56*56) { puts("lrtx-upload: error magnitude exceeds 56");return 1; }
        }
        tx_upload_ready=true;
        printf("lrtx-upload: ready bytes=%lu crc32=%08lX\n",(unsigned long)tx_upload_size,(unsigned long)tx_upload_expected_crc);return 0;
    }
    if(argc==3 && !strcmp(argv[2],"status")) {
        printf("lrtx-upload: ready=%u bytes=%lu next=%lu\n",tx_upload_ready,(unsigned long)tx_upload_size,(unsigned long)tx_upload_next);return 0;
    }
invalid:
    puts("lrtx-upload: error invalid/incomplete upload");return 1;
}
