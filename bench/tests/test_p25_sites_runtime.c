/* Exercise the actual owner handoff and cached status without SD or hardware. */
#include "ls_test.h"
#include "../../components/lakeshark/apps/p25/p25_sites.c"
static p25_program_t program;
static ls_gps_state_t fix;
static bool running = true, accept_tune = true, fail_nvs;
static unsigned tunes;
static uint8_t stored = 1;
const p25_program_t *p25_program_session(void) { return &program; }
bool p25_program_survey_active_now(void) { return false; }
bool p25_program_site_now(const p25_profile_t *p, unsigned c)
{ (void)p; (void)c; if (!accept_tune) return false; tunes++; return true; }
void p25_program_geo_resume_now(void) { program.geo_paused = false; }
bool p25_owner_post(void (*fn)(uint32_t), uint32_t arg) { fn(arg); return true; }
bool ls_gps_running(void) { return running; }
void ls_gps_get(ls_gps_state_t *out) { *out = fix; }
esp_err_t ls_nvs_run(ls_nvs_fn_t fn, void *ctx, unsigned stack) { (void)stack; return fn(ctx); }
esp_err_t nvs_open(const char *ns, int mode, nvs_handle_t *out)
{ (void)ns; (void)mode; *out = 1; return fail_nvs ? ESP_FAIL : ESP_OK; }
void nvs_close(nvs_handle_t n) { (void)n; }
esp_err_t nvs_get_u8(nvs_handle_t n, const char *key, uint8_t *v) { (void)n; (void)key; *v = stored; return ESP_OK; }
esp_err_t nvs_set_u8(nvs_handle_t n, const char *key, uint8_t v) { (void)n; (void)key; stored = v; return ESP_OK; }
esp_err_t nvs_commit(nvs_handle_t n) { (void)n; return ESP_OK; }
static void reset(void)
{
    heap_caps_free(active); active = NULL;
    heap_caps_free(atomic_exchange(&pending, NULL));
    memset(&program, 0, sizeof(program)); memset(&fix, 0, sizeof(fix));
    memset(&geo, 0, sizeof(geo)); memset(&status, 0, sizeof(status));
    atomic_store(&enabled, true); tunes = 0; running = accept_tune = true; fail_nvs = false;
    ls_shim_time_set(1000000); fix.fix = true; fix.last_fix_us = 1000000;
    fix.lat_deg = 42; fix.lon_deg = -71;
}
static void handoff(void)
{
    p25_pack_db_t *db = heap_caps_calloc(1, sizeof(*db), MALLOC_CAP_SPIRAM);
    db->count = db->packs = db->sites = 1;
    p25_profile_t *p = &db->records[0];
    p->control_count = 1; p->control_has_geo[0] = true;
    p->control_lat_e7[0] = 420000000; p->control_lon_e7[0] = -710000000;
    p->control_radius_m[0] = 10000;
    strcpy(p->site_name, "Fictional hill"); strcpy(p->system_name, "Test system");
    atomic_store(&pending, db);
}
LS_CASE(owner_handoff_publishes_status_and_never_consumes_a_busy_selection)
{
    reset(); handoff(); p25_sites_status_t s;
    LS_EQ_INT(-1, p25_sites_poll(false)); LS_EQ_INT(0, tunes);
    accept_tune = false; LS_EQ_INT(-1, p25_sites_poll(true)); LS_CHECK(!geo.have_answer);
    accept_tune = true; LS_EQ_INT(0, p25_sites_poll(true)); LS_EQ_INT(1, tunes);
    p25_sites_status(&s); LS_EQ_STR("following", s.state); LS_EQ_STR("Fictional hill", s.site);
    LS_EQ_STR("Test system", s.system); LS_EQ_INT(1, s.packs); LS_EQ_INT(1, s.sites);
    LS_EQ_INT(-1, p25_sites_poll(true)); LS_EQ_INT(1, tunes);
    reset();
}
LS_CASE(disabled_paused_and_stale_fix_do_not_tune_and_resume_clears_manual_pause)
{
    reset(); handoff(); p25_sites_status_t s;
    program.geo_paused = true; LS_EQ_INT(-1, p25_sites_poll(true));
    p25_sites_status(&s); LS_EQ_STR("paused", s.state); LS_EQ_INT(0, tunes);
    LS_CHECK(p25_sites_resume()); LS_CHECK(!program.geo_paused);
    atomic_store(&enabled, false); LS_EQ_INT(-1, p25_sites_poll(true)); LS_EQ_INT(0, tunes);
    atomic_store(&enabled, true); ls_shim_time_set(7000000);
    LS_EQ_INT(-1, p25_sites_poll(true)); p25_sites_status(&s); LS_EQ_STR("no fix", s.state);
    fix.last_fix_us = 7000000; LS_EQ_INT(0, p25_sites_poll(true));
    reset(); LS_EQ_INT(-1, p25_sites_poll(true)); p25_sites_status(&s); LS_EQ_STR("no packs", s.state);
}
LS_CASE(preference_only_changes_after_successful_persistence)
{
    reset(); job_t j = {1, 0}; fail_nvs = true;
    LS_CHECK(preference(&j) != ESP_OK); LS_CHECK(atomic_load(&enabled));
    fail_nvs = false; LS_EQ_INT(ESP_OK, preference(&j)); LS_CHECK(!atomic_load(&enabled));
    atomic_store(&enabled, true); j.op = 0;
    LS_EQ_INT(ESP_OK, preference(&j)); LS_CHECK(!atomic_load(&enabled)); reset();
}
