/* Actual catalog worker/action adapter; fake HTTPS, NVS and GPS producers. */
#include "ls_test.h"
#include "../p25pack/shims/posix.h"
#include "../../main/ls_p25_packs.c"
static char response[2048];
static size_t offset;
static unsigned opened;
static bool trusted = true;
static char stored_token[192];
struct esp_http_client { int unused; } client;
esp_http_client_handle_t ls_ota_open_data(const char *url, const char *auth, int64_t deadline, int64_t *length, int *status)
{ (void)url; (void)auth; (void)deadline; opened++; offset = 0; *length = strlen(response); *status = 200; return &client; }
int esp_http_client_read(esp_http_client_handle_t c, char *out, int cap)
{ (void)c; size_t n = strlen(response) - offset; if (n > (size_t)cap) n = (size_t)cap; memcpy(out, response + offset, n); offset += n; return (int)n; }
esp_err_t esp_http_client_close(esp_http_client_handle_t c) { (void)c; return ESP_OK; }
esp_err_t esp_http_client_cleanup(esp_http_client_handle_t c) { (void)c; return ESP_OK; }
bool ls_ota_signed_by_us(const char *text, const ls_ota_manifest_t *m) { (void)text; (void)m; return trusted; }
void p25_sites_init(void) {}
bool p25_sites_reload(void) { return true; }
bool p25_sites_set(bool on) { (void)on; return true; }
bool p25_sites_resume(void) { return true; }
void p25_sites_status(p25_sites_status_t *s) { memset(s, 0, sizeof(*s)); }
esp_err_t ls_nvs_run(ls_nvs_fn_t fn, void *ctx, unsigned stack) { (void)stack; return fn(ctx); }
esp_err_t nvs_open(const char *ns, int mode, nvs_handle_t *out) { (void)ns; (void)mode; *out = 1; return ESP_OK; }
void nvs_close(nvs_handle_t n) { (void)n; }
esp_err_t nvs_set_str(nvs_handle_t n, const char *k, const char *v) { (void)n; (void)k; strcpy(stored_token, v); return ESP_OK; }
esp_err_t nvs_get_str(nvs_handle_t n, const char *k, char *v, size_t *len) { (void)n; (void)k; snprintf(v, *len, "%s", stored_token); return ESP_OK; }
esp_err_t nvs_commit(nvs_handle_t n) { (void)n; return ESP_OK; }
void mbedtls_sha256_init(mbedtls_sha256_context *c) { (void)c; }
void mbedtls_sha256_free(mbedtls_sha256_context *c) { (void)c; }
int mbedtls_sha256_starts(mbedtls_sha256_context *c, int b) { (void)c; (void)b; return 0; }
int mbedtls_sha256_update(mbedtls_sha256_context *c, const unsigned char *b, size_t n) { (void)c; (void)b; (void)n; return 0; }
int mbedtls_sha256_finish(mbedtls_sha256_context *c, unsigned char *b) { (void)c; memset(b, 0, 32); return 0; }
static void fixture(void)
{
    const char *sha = "0000000000000000000000000000000000000000000000000000000000000000";
    snprintf(response, sizeof(response), "LAKESHARK-OTA 1\nboard=P25-sites\nchannel=catalog\nschema=p25-sites-1\nversion=1.0.0\nbuild=sites-1\nsize=1\nsha256=%s\nurl=catalog.txt\nentry=MA|Fictional MA|MA.txt|12|%s|1.0.0|plus\nsig=3006020101020101\n", sha, sha);
    trusted = true; token[0] = 0; opened = 0; ls_shim_time_set(1000000);
}
LS_CASE(catalog_worker_publishes_and_rejects_bad_signature_without_losing_trusted_catalog)
{
    fixture(); refresh(); ls_p25_pack_info_t e;
    LS_EQ_INT(1, ls_p25_pack_entry(0, &e)); LS_EQ_STR("MA", e.pack.state);
    LS_EQ_STR("1.0.0", e.pack.version); LS_CHECK(!e.installed);
    LS_CHECK(strstr(entries_text, "MA|Fictional MA|plus|absent|1.0.0") != NULL);
    trusted = false; refresh();
    LS_EQ_INT(1, ls_p25_pack_entry(0, &e)); LS_EQ_STR("Fictional MA", e.pack.name);
    LS_CHECK(strstr(stage, "signature") != NULL);
}
LS_CASE(plus_download_is_refused_before_network_without_entitlement)
{
    fixture(); refresh(); unsigned before = opened;
    download("MA"); LS_EQ_INT(before, opened);
    LS_CHECK(strstr(stage, "entitlement token") != NULL);
    download("NH"); LS_EQ_INT(before, opened); LS_CHECK(strstr(stage, "not listed") != NULL);
    request_t r = {.op = 't'}; strcpy(r.arg, "disposable-entitlement");
    LS_EQ_INT(ESP_OK, token_nvs(&r)); LS_EQ_STR("disposable-entitlement", stored_token);
    r.op = 0; token[0] = 0; LS_EQ_INT(ESP_OK, token_nvs(&r));
    LS_EQ_STR(stored_token, token); token[0] = 0;
}
LS_CASE(registry_and_requests_are_cached_and_validate_input)
{
    ls_p25_packs_publish(); fixture(); refresh(); unsigned before = opened;
    ls_val_t v; LS_CHECK(ls_value_read("p25.pack.count", &v, NULL)); LS_EQ_INT(1, v.i);
    LS_CHECK(ls_value_read("p25.pack.state", &v, NULL)); LS_EQ_INT(before, opened);
    ls_args_t a = {.n = 1, .v = {{.kind = LS_VAL_TEXT, .s = "../MA"}}};
    LS_EQ_INT(LS_ACT_BADARG, ls_action_call("p25.pack.get", &a, &v, LS_CAP_STORE));
    LS_CHECK(!request('t', "bad\r\nheader"));
    LS_CHECK(!request('g', "ZZ"));
    LS_CHECK(request('g', "MA")); LS_CHECK(!request('f', NULL)); LS_EQ_INT(before, opened);
    request_t r; LS_EQ_INT(pdTRUE, xQueueReceive(queue, &r, 0)); LS_EQ_INT('g', r.op);
    LS_EQ_STR("MA", r.arg);
}

LS_CASE(verified_pack_replaces_existing_file_and_failed_rename_rolls_back)
{
    mkdir(TEST_PACK_EVIDENCE_DIR, 0775);
    const char *path = TEST_PACK_EVIDENCE_DIR "/install-test.txt";
    const char *tmp = TEST_PACK_EVIDENCE_DIR "/install-test.tmp";
    const char *missing = TEST_PACK_EVIDENCE_DIR "/install-missing.tmp";
    remove(path); remove(tmp); remove(missing);
    FILE *f = fopen(path, "wb"); LS_CHECK(f != NULL); if (!f) return;
    fputs("old verified pack", f); fclose(f);
    f = fopen(tmp, "wb"); LS_CHECK(f != NULL); if (!f) return;
    fputs("new verified pack", f); fclose(f);
    LS_CHECK(install_pack(tmp, path));
    char text[32] = {0}; f = fopen(path, "rb"); LS_CHECK(f != NULL); if (!f) return;
    fread(text, 1, sizeof(text) - 1, f); fclose(f); LS_EQ_STR("new verified pack", text);
    LS_CHECK(!install_pack(missing, path));
    memset(text, 0, sizeof(text)); f = fopen(path, "rb"); LS_CHECK(f != NULL); if (!f) return;
    fread(text, 1, sizeof(text) - 1, f); fclose(f); LS_EQ_STR("new verified pack", text);
    remove(path);
}
