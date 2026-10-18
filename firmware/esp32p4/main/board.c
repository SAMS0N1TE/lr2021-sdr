#include "board.h"
#include <stdio.h>
#include <string.h>
#include "sdkconfig.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "soc/uart_pins.h"

static board_profile active, pending;
static bool storage_ready;
static const char *validate(const board_profile *p)
{
    int tx=-1,rx=-1;
#if CONFIG_ESP_CONSOLE_UART_DEFAULT
    tx=U0TXD_GPIO_NUM; rx=U0RXD_GPIO_NUM;
#elif CONFIG_ESP_CONSOLE_UART_CUSTOM
    tx=CONFIG_ESP_CONSOLE_UART_TX_GPIO; rx=CONFIG_ESP_CONSOLE_UART_RX_GPIO;
#endif
    return board_profile_validate(p,tx,rx);
}
const board_profile *board_active(void) { return &active; }
int board_load(void)
{
    storage_ready=false;
    board_profile_defaults(&active);
    pending=active;
    int e=nvs_flash_init();
    if(e) { printf("board: NVS unavailable (%d); defaults staged\n",e); return e; }
    storage_ready=true;
    nvs_handle_t h;
    e=nvs_open("board",NVS_READONLY,&h);
    if(e==ESP_ERR_NVS_NOT_FOUND) e=0;
    else if(!e) {
        uint32_t version=0;
        board_profile saved;
        size_t n=sizeof(saved);
        e=nvs_get_u32(h,"version",&version);
        if(!e && version!=1) e=ESP_ERR_INVALID_VERSION;
        if(!e) e=nvs_get_blob(h,"profile",&saved,&n);
        if(!e && n!=sizeof(saved)) e=ESP_ERR_INVALID_SIZE;
        if(!e) {
            const char *reason=validate(&saved);
            if(reason) { printf("board: saved profile refused: %s\n",reason); e=ESP_ERR_INVALID_ARG; }
        }
        if(!e) active=saved;
        nvs_close(h);
    }
    pending=active;
    const char *reason=validate(&active);
    if(reason) { printf("board: %s\n",reason); return ESP_ERR_INVALID_ARG; }
    if(e) printf("board: saved profile unavailable (%d); radio left off\n",e);
    return e;
}
static void show(void)
{
    const board_profile *p=&pending;
    puts("Pending board profile; board save then reboot to apply");
    if(p->xl9535) puts("reset xl9535 IO16 bit 14 low; rail IO0 low, antenna IO1 low");
    else printf("reset gpio %d %s\n",p->reset,p->reset_high?"high":"low");
    printf("busy %d sclk %d mosi %d miso %d cs %d sda %d scl %d\n",
        p->busy,p->sclk,p->mosi,p->miso,p->cs,p->sda,p->scl);
    if(p->irq<0) puts("irq none");
    else printf("irq gpio %d dio %d (reserved; capture uses polling)\n",p->irq,p->irq_dio);
    static const char *volts[]={"1.6","1.7","1.8","2.2","2.4","2.7","3.0","3.3"};
    printf("tcxo %s voltage_code %u ticks %lu\n",p->tcxo?volts[p->tcxo_voltage]:"off",
        p->tcxo_voltage,(unsigned long)p->tcxo_ticks);
    puts("rfsw dio standby rx_lf tx_lf rx_hf tx_hf pull");
    for(unsigned i=0;i<p->rfsw_count;i++) {
        const board_rfsw *r=&p->rfsw[i];
        printf("rfsw %u %u %u %u %u %u %u\n",r->dio,r->modes&1,(r->modes>>1)&1,
            (r->modes>>2)&1,(r->modes>>3)&1,(r->modes>>4)&1,r->pull);
    }
    printf("Pending differs from boot profile: %s\n",memcmp(&active,&pending,sizeof(active))?"yes":"no");
}
int board_command(int argc, char **argv)
{
    if(argc==1) { show(); return 0; }
    if(argc==2 && !strcmp(argv[1],"save")) {
        const char *reason=validate(&pending);
        if(reason) { printf("board: %s\n",reason); return 1; }
        if(!storage_ready) { puts("board: NVS unavailable; profile not saved"); return 1; }
        nvs_handle_t h;
        int e=nvs_open("board",NVS_READWRITE,&h);
        if(!e) {
            e=nvs_set_u32(h,"version",1);
            if(!e) e=nvs_set_blob(h,"profile",&pending,sizeof(pending));
            if(!e) e=nvs_commit(h);
            nvs_close(h);
        }
        if(e) { printf("board: save failed (%d)\n",e); return 1; }
        puts("Board profile saved; reboot to apply"); return 0;
    }
    board_profile next=pending;
    const char *reason;
    if(argc==2 && !strcmp(argv[1],"default")) {
        board_profile_defaults(&next); reason=NULL;
    } else reason=board_profile_edit(&next,argc-1,argv+1);
    if(!reason) reason=validate(&next);
    if(reason) { printf("board: %s\n",reason); return 1; }
    pending=next;
    puts("Board profile staged; board save then reboot to apply");
    return 0;
}
