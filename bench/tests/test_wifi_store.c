#include "ls_test.h"
#include "ls_wifi_sta_core.h"
#include "esp_err.h"
#include <string.h>
#define ESP_ERR_NVS_NOT_FOUND 0x1102
#define NVS_READWRITE 1
#define LS_WIFI_NVS_KEY_SSID "ssid"
#define LS_WIFI_NVS_KEY_PASS "pass"
typedef unsigned nvs_handle_t;
static ls_wifi_saved_t s_saved[LS_WIFI_SAVED_MAX];
typedef struct { char key[16], text[65]; uint32_t number; bool used, numeric; } entry_t;
static entry_t disk[32];
static unsigned commits;
static bool fail_commit;
static entry_t *entry(const char *key,bool create) {
    entry_t *empty=NULL;
    for(unsigned i=0;i<32;i++) {if(disk[i].used && !strcmp(disk[i].key,key)) return &disk[i];if(!disk[i].used) empty=&disk[i];}
    if(!create || !empty) return NULL;
    memset(empty,0,sizeof(*empty));strcpy(empty->key,key);empty->used=true;return empty;
}
static esp_err_t nvs_get_u32(nvs_handle_t h,const char *key,uint32_t *out) {entry_t *e=entry(key,false);if(!e) return ESP_ERR_NVS_NOT_FOUND;*out=e->number;return ESP_OK;}
static esp_err_t nvs_set_u32(nvs_handle_t h,const char *key,uint32_t value) {entry_t *e=entry(key,true);e->number=value;e->numeric=true;return ESP_OK;}
static esp_err_t nvs_get_str(nvs_handle_t h,const char *key,char *out,size_t *n) {entry_t *e=entry(key,false);if(!e) return ESP_ERR_NVS_NOT_FOUND;size_t size=strlen(e->text)+1;if(size>*n) return ESP_ERR_INVALID_SIZE;memcpy(out,e->text,size);*n=size;return ESP_OK;}
static esp_err_t nvs_set_str(nvs_handle_t h,const char *key,const char *value) {entry_t *e=entry(key,true);strcpy(e->text,value);return ESP_OK;}
static esp_err_t nvs_erase_key(nvs_handle_t h,const char *key) {entry_t *e=entry(key,false);if(!e) return ESP_ERR_NVS_NOT_FOUND;e->used=false;return ESP_OK;}
static esp_err_t nvs_commit(nvs_handle_t h) {commits++;return fail_commit?ESP_FAIL:ESP_OK;}
static void nvs_close(nvs_handle_t h) {(void)h;}
static esp_err_t nvs_open_creds(int mode,nvs_handle_t *h) {*h=1;return ESP_OK;}
static void wipe(void *p,size_t n) {memset(p,0,n);}
#include "ls_wifi_store.inc"
static void reset(void) {memset(disk,0,sizeof(disk));memset(s_saved,0,sizeof(s_saved));commits=0;fail_commit=false;}
LS_CASE(slots_require_both_header_values_and_save_round_trips) {
    reset();store_job_t save={1,"home","password"},load={0};
    LS_EQ_INT(store_job(&save),ESP_OK);
    LS_EQ_UINT(entry("magic",false)->number,WIFI_STORE_MAGIC);
    LS_EQ_UINT(entry("build",false)->number,WIFI_STORE_BUILD);
    LS_EQ_INT(store_job(&load),ESP_OK);LS_CHECK(!strcmp(s_saved[0].ssid,"home"));
    char pass[64];store_job_t read={3,"home",NULL,pass};
    LS_EQ_INT(store_job(&read),ESP_OK);LS_CHECK(!strcmp(pass,"password"));
    for(int bad=0;bad<4;bad++) {
        nvs_set_u32(1,"magic",WIFI_STORE_MAGIC);nvs_set_u32(1,"build",WIFI_STORE_BUILD);
        if(bad==0) nvs_erase_key(1,"magic");
        if(bad==1) nvs_erase_key(1,"build");
        if(bad==2) nvs_set_u32(1,"magic",1);
        if(bad==3) nvs_set_u32(1,"build",1);
        LS_EQ_INT(store_job(&load),ESP_OK);for(int i=0;i<8;i++) LS_CHECK(!s_saved[i].ssid[0]);
    }
}
LS_CASE(legacy_network_migrates_before_keys_are_removed_and_stale_slots_stay_ignored) {
    reset();nvs_set_str(1,"ssid","legacy");nvs_set_str(1,"pass","legacy-pass");
    nvs_set_str(1,"s7","stale");store_job_t load={0};
    LS_EQ_INT(store_job(&load),ESP_OK);LS_CHECK(!strcmp(s_saved[0].ssid,"legacy"));
    LS_CHECK(!entry("ssid",false) && !entry("pass",false) && !entry("s7",false));
    LS_EQ_UINT(entry("magic",false)->number,WIFI_STORE_MAGIC);LS_EQ_UINT(entry("build",false)->number,WIFI_STORE_BUILD);
    char pass[64];store_job_t read={3,"legacy",NULL,pass};LS_EQ_INT(store_job(&read),ESP_OK);LS_CHECK(!strcmp(pass,"legacy-pass"));
    reset();nvs_set_str(1,"ssid","open");LS_EQ_INT(store_job(&load),ESP_OK);LS_CHECK(!strcmp(s_saved[0].ssid,"open"));
    reset();nvs_set_str(1,"ssid","retry");nvs_set_str(1,"pass","keep-me");fail_commit=true;
    LS_EQ_INT(store_job(&load),ESP_FAIL);LS_CHECK(entry("ssid",false) && entry("pass",false));
    fail_commit=false;LS_EQ_INT(store_job(&load),ESP_OK);LS_CHECK(!strcmp(s_saved[0].ssid,"retry"));
}
