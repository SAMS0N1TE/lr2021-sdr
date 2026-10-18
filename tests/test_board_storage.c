#include "board.h"
#include "nvs.h"
#include <stdio.h>
#include <string.h>
#define CHECK(x) do {if(!(x)){fprintf(stderr,"line %d: %s\n",__LINE__,#x);return 1;}}while(0)

static board_profile disk, staged;
static uint32_t version, staged_version;
static bool exists;
static int init_error, commit_error;
static unsigned commits, handles;
static size_t disk_size=sizeof(disk);
int nvs_flash_init(void) { return init_error; }
int nvs_open(const char *name, int mode, nvs_handle_t *h)
{
    if(strcmp(name,"board")) return -1;
    if(mode==NVS_READONLY && !exists) return ESP_ERR_NVS_NOT_FOUND;
    handles++; *h=1; return 0;
}
void nvs_close(nvs_handle_t h) { (void)h; handles--; }
int nvs_get_u32(nvs_handle_t h, const char *key, uint32_t *v)
{ (void)h; (void)key; *v=version; return 0; }
int nvs_get_blob(nvs_handle_t h, const char *key, void *p, size_t *n)
{
    (void)h; (void)key;
    memcpy(p,&disk,*n); *n=disk_size; return 0;
}
int nvs_set_u32(nvs_handle_t h, const char *key, uint32_t v)
{ (void)h; (void)key; staged_version=v; return 0; }
int nvs_set_blob(nvs_handle_t h, const char *key, const void *p, size_t n)
{
    (void)h; (void)key;
    if(n!=sizeof(staged)) return -1;
    memcpy(&staged,p,n); return 0;
}
int nvs_commit(nvs_handle_t h)
{
    (void)h;
    if(commit_error) return commit_error;
    disk=staged; version=staged_version; exists=true; commits++; return 0;
}
int main(void)
{
    CHECK(!board_load() && !handles);
    board_profile stock=*board_active();
    char *reset[]={"board","set","reset","gpio","5","low"};
    char *busy[]={"board","set","busy","10"};
    char *save[]={"board","save"};
    char *defaults[]={"board","default"};
    CHECK(!board_command(6,reset) && !board_command(4,busy));
    CHECK(!memcmp(&stock,board_active(),sizeof(stock)) && !commits);
    CHECK(!board_command(2,save) && commits==1 && !handles);
    CHECK(!memcmp(&stock,board_active(),sizeof(stock)));
    CHECK(!board_load() && board_active()->busy==10 && !board_active()->xl9535);
    busy[3]="26"; CHECK(board_command(4,busy));
    CHECK(!board_command(2,save) && disk.busy==10);
    CHECK(!board_command(2,defaults) && !board_active()->xl9535);
    commit_error=-9;
    CHECK(board_command(2,save) && !handles && !disk.xl9535);
    commit_error=0;
    CHECK(!board_load() && !board_active()->xl9535);
    CHECK(!board_command(2,defaults) && !board_command(2,save));
    CHECK(!board_load() && !memcmp(&stock,board_active(),sizeof(stock)));
    disk.busy=27; CHECK(board_load() && !handles);
    CHECK(!board_command(2,defaults) && !board_command(2,save) && !board_load());
    version=2; CHECK(board_load() && !handles);
    version=1; disk_size--; CHECK(board_load() && !handles); disk_size++;
    init_error=-8; CHECK(board_load()); CHECK(board_command(2,save));
    CHECK(!handles);
    puts("board staging, NVS reload, defaults and storage failures passed");
    return 0;
}
