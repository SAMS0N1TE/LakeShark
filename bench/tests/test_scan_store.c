#include "ls_test.h"
#include "../../components/lakeshark/core/scan_channels.c"
static uint8_t saved[4096], pending[4096];
static size_t saved_size, pending_size;
static int commits;
static bool fail;
static int safe_dispatches;
esp_err_t ls_nvs_call(ls_nvs_fn_t fn,void *context,unsigned stack) { ++safe_dispatches; return fn(context); }
esp_err_t nvs_open(const char *name,int mode,nvs_handle_t *h) { *h=1; return ESP_OK; }
esp_err_t nvs_get_blob(nvs_handle_t h,const char *key,void *out,size_t *size) {
    if (!saved_size || *size < saved_size) return ESP_FAIL;
    memcpy(out,saved,saved_size); *size=saved_size; return ESP_OK;
}
esp_err_t nvs_set_blob(nvs_handle_t h,const char *key,const void *value,size_t size) {
    if (size>sizeof(pending)) return ESP_FAIL;
    memcpy(pending,value,size); pending_size=size; return ESP_OK;
}
esp_err_t nvs_commit(nvs_handle_t h) {
    if(fail) return ESP_FAIL;
    ++commits; memcpy(saved,pending,pending_size); saved_size=pending_size; return ESP_OK;
}
LS_CASE(legacy_channel_lists_migrate_without_losing_names_or_flags)
{
    scan_store_hdr_t header={SCANLIST_MAGIC,1,1,SCANLIST_BUILD};
    scan_channel_t old={.name="Old Channel",.freq_hz=154785000,.mode=SCAN_MODE_P25,.flags=SCAN_FLAG_LOCKOUT,.zone=3};
    memcpy(saved,&header,sizeof(header)); memcpy(saved+sizeof(header),&old,24); saved_size=sizeof(header)+24;
    scan_channels_init();
    LS_EQ_INT(scan_channels_count(),1); LS_EQ_INT(scan_channel_get(0)->radius_m,0);
    LS_EQ_INT(scan_channel_get(0)->flags,SCAN_FLAG_LOCKOUT);
    LS_CHECK(!strcmp(scan_channel_get(0)->name,"Old Channel"));
}
LS_CASE(import_commit_failure_keeps_the_previous_live_list)
{
    fail=false; saved_size=0; commits=0; scan_channels_init();
    scan_channel_t first={.name="First",.freq_hz=154785000,.mode=SCAN_MODE_P25,.flags=SCAN_FLAG_ENABLED};
    scan_channel_t replacement=first; strcpy(replacement.name,"Second"); replacement.radius_m=30000;
    LS_CHECK(scan_channels_replace(&first,1)); LS_EQ_INT(commits,1);
    fail=true; LS_CHECK(!scan_channels_replace(&replacement,1));
    LS_CHECK(!strcmp(scan_channel_get(0)->name,"First")); LS_EQ_INT(commits,1);
    fail=false; LS_CHECK(scan_channels_replace(&replacement,1)); LS_EQ_INT(commits,2);
    scan_channels_init(); LS_EQ_INT(scan_channel_get(0)->radius_m,30000);
}
LS_CASE(channel_clear_and_edits_use_the_cache_safe_worker)
{
    fail=false; saved_size=0; safe_dispatches=0; scan_channels_init();
    LS_CHECK(scan_channel_add("Test",154785000,SCAN_MODE_P25,0)>=0);
    LS_EQ_INT(safe_dispatches,1);
    scan_channels_clear(); LS_EQ_INT(safe_dispatches,2); LS_EQ_INT(scan_channels_count(),0);
}

bool scan_sd_available(void) { return false; }
bool scan_sd_name(const char *name) { return true; }
int scan_sd_read(const char *name, scan_channel_t *rows) { return -1; }
bool scan_sd_write(const char *name, const scan_channel_t *rows, int count) { return false; }
LS_CASE(large_lists_require_sd_and_keep_existing_list_without_it)
{
    static scan_channel_t many[65];
    fail=false; saved_size=0; scan_channels_init();
    for(int i=0;i<65;i++) { many[i]=(scan_channel_t){.name="Channel",.freq_hz=154785000,.flags=SCAN_FLAG_ENABLED}; }
    LS_CHECK(scan_channels_replace(many,1));
    LS_CHECK(!scan_channels_replace(many,65));
    LS_EQ_INT(scan_channels_count(),1);
}
