"""Strict host NVS adapter: every raw call requires the DRAM worker context."""
NVS_SHIM = r"""
typedef int nvs_handle_t;
typedef int esp_err_t;
#define ESP_OK 0
#define NVS_READONLY 0
#define NVS_READWRITE 1
static unsigned char persisted[1024],staged[1024];
static size_t persisted_size,staged_size;
static bool commit_fail,open_fail,allow_nvs;
static unsigned nvs_calls,dispatches;
static esp_err_t nvs_open(const char *name,int mode,nvs_handle_t *h) {
    assert(allow_nvs);nvs_calls++;
    assert(!strcmp(name,"cartocore"));assert(mode==0 || mode==1);*h=1;return open_fail?-1:ESP_OK;
}
static void nvs_close(nvs_handle_t h) { assert(allow_nvs && h==1);nvs_calls++; }
static esp_err_t nvs_get_blob(nvs_handle_t h,const char *key,void *p,size_t *n) {
    assert(allow_nvs && h==1 && !strcmp(key,"view"));nvs_calls++;
    if(!persisted_size || *n<persisted_size) return -1;
    memcpy(p,persisted,persisted_size);*n=persisted_size;return ESP_OK;
}
static esp_err_t nvs_set_blob(nvs_handle_t h,const char *key,const void *p,size_t n) {
    assert(allow_nvs && h==1 && !strcmp(key,"view") && n<=sizeof(staged));nvs_calls++;
    memcpy(staged,p,n);staged_size=n;return ESP_OK;
}
static esp_err_t nvs_commit(nvs_handle_t h) {
    assert(allow_nvs && h==1);nvs_calls++;if(commit_fail) return -1;
    memcpy(persisted,staged,staged_size);persisted_size=staged_size;return ESP_OK;
}
static const char *esp_err_to_name(int e) { (void)e;return "injected"; }
static esp_err_t ls_nvs_call(esp_err_t (*fn)(void *),void *ctx,unsigned bytes) {
    assert(!bytes);bool old=allow_nvs;allow_nvs=true;dispatches++;
    esp_err_t e=fn(ctx);allow_nvs=old;return e;
}
static esp_err_t ls_nvs_run(esp_err_t (*fn)(void *),void *ctx,unsigned bytes) {
    return ls_nvs_call(fn,ctx,bytes);
}
"""
