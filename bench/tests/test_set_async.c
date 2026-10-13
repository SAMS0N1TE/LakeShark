#include "ls_test.h"
#include <setjmp.h>
static size_t strlcpy(char *out, const char *in, size_t n)
{ size_t len = strlen(in); if (n) snprintf(out, n, "%s", in); return len; }
#include "../../components/lakeshark/core/settings.c"
#include "../../components/apps/tui/screens/ext/scr_settings.c"
#include "../../components/lakeshark/core/usb_autoreboot_pref.c"

static set_write_t queued[SET_Q_DEPTH];
static int queued_n, queued_read, commit_count;
static bool reject_write, live_ring, live_vibe, muted;
static int boot_saved, ring_saved, vibe_saved, volume, timeout;
static int lr_iq_saved = -1;
static int nvs_reads;
static jmp_buf worker_done;

BaseType_t xQueueSend(QueueHandle_t q, const void *item, TickType_t ticks)
{
    (void)q; (void)ticks;
    if (reject_write || queued_n == SET_Q_DEPTH) return pdFALSE;
    queued[queued_n++] = *(const set_write_t *)item;
    return pdTRUE;
}
BaseType_t xQueueReceive(QueueHandle_t q, void *item, TickType_t ticks)
{
    (void)q;
    if (queued_read < queued_n) {
        *(set_write_t *)item = queued[queued_read++];
        return pdTRUE;
    }
    if (ticks == portMAX_DELAY) longjmp(worker_done, 1);
    LS_EQ_UINT(pdMS_TO_TICKS(300), ticks);
    return pdFALSE;
}
esp_err_t nvs_set_u8(nvs_handle_t h, const char *key, uint8_t value)
{
    (void)h;
    if (!strcmp(key, "boot_snd")) boot_saved = value;
    if (!strcmp(key, "alert_ring")) ring_saved = value;
    if (!strcmp(key, "alert_vibe")) vibe_saved = value;
    if (!strcmp(key, "p25_lriq")) lr_iq_saved = value;
    return ESP_OK;
}
#define UNUSED_SET(t, c) \
    esp_err_t nvs_set_##t(nvs_handle_t h, const char *k, c v) \
    { (void)h; (void)k; (void)v; return ESP_OK; }
UNUSED_SET(i8, int8_t)
UNUSED_SET(u32, uint32_t)
UNUSED_SET(i32, int32_t)
UNUSED_SET(u64, uint64_t)
esp_err_t (nvs_get_u8)(nvs_handle_t h, const char *key, uint8_t *out)
{
    (void)h; nvs_reads++;
    if (strcmp(key, "p25_lriq") || lr_iq_saved < 0) return ESP_ERR_NVS_NOT_FOUND;
    *out = (uint8_t)lr_iq_saved;
    return ESP_OK;
}
#define UNUSED_GET(t, c) \
    esp_err_t (nvs_get_##t)(nvs_handle_t h, const char *k, c *v) \
    { (void)h; (void)k; (void)v; return ESP_ERR_NVS_NOT_FOUND; }
UNUSED_GET(i8, int8_t)
UNUSED_GET(u32, uint32_t)
UNUSED_GET(i32, int32_t)
UNUSED_GET(u64, uint64_t)
esp_err_t nvs_commit(nvs_handle_t h) { (void)h; commit_count++; return ESP_OK; }
esp_err_t ls_nvs_run(ls_nvs_fn_t fn, void *ctx, unsigned bytes)
{ (void)bytes; return fn(ctx); }
void ls_notify_set_alerts(bool r, bool v) { live_ring = r; live_vibe = v; }
bool ls_notify_ring(void) { return live_ring; }
bool ls_notify_vibe(void) { return live_vibe; }
bool speech_available(void) { return true; }
int audio_volume_get(void) { return volume; }
void audio_volume_set(int v) { volume = v; }
bool audio_is_muted(void) { return muted; }
void audio_toggle_mute(void) { muted = !muted; }
int display_ctl_autodim_timeout(void) { return timeout; }
void display_ctl_set_autodim_timeout(int v) { timeout = v; }
bool app_usb_autoreboot(void) { return usb_autoreboot_pref_get(); }
void app_set_usb_autoreboot(bool en) { usb_autoreboot_pref_set(en); }

static void prepare(void)
{
    s_nvs_ok = true;
    s_wq = (QueueHandle_t)queued;
    queued_n = queued_read = commit_count = 0;
    reject_write = false;
    s_boot_sound = boot_saved = 1;
    s_alert_ring = s_alert_vibe = true;
    live_ring = live_vibe = true;
    ring_saved = vibe_saved = 1;
}
static void flush(void)
{
    if (!setjmp(worker_done)) set_worker(NULL);
}

LS_CASE(p25_lr_iq_ignores_legacy_fsk_without_nvs_access_or_writes)
{
    prepare(); lr_iq_saved = -1; nvs_reads = 0;
    LS_CHECK(settings_get_p25_lr_iq());
    LS_EQ_INT(0, queued_n); LS_EQ_INT(0, commit_count);
    for (int choice = 0; choice <= 1; choice++) {
        prepare();
        lr_iq_saved = choice; /* A boot after either legacy saved mode. */
        LS_CHECK(settings_get_p25_lr_iq());
        LS_CHECK(!settings_set_p25_lr_iq(false));
        LS_CHECK(settings_set_p25_lr_iq(true));
        LS_EQ_INT(0, nvs_reads); LS_EQ_INT(0, queued_n);
        LS_EQ_INT(0, commit_count);
        LS_EQ_INT(choice, lr_iq_saved);
        LS_CHECK(settings_get_p25_lr_iq());
        LS_EQ_INT(1, boot_saved); LS_EQ_INT(1, ring_saved);
    }
    prepare(); reject_write = true;
    LS_CHECK(!settings_set_p25_lr_iq(false));
    LS_CHECK(settings_get_p25_lr_iq());
    lr_iq_saved = 7; LS_CHECK(settings_get_p25_lr_iq());
    s_nvs_ok = false; LS_CHECK(settings_get_p25_lr_iq());
    prepare();
}

LS_CASE(rapid_alert_edits_preserve_live_flags_before_delayed_commit)
{
    prepare();
    ring_next();
    LS_CHECK(!settings_get_alert_ring() && !live_ring && live_vibe);
    vibe_next();
    LS_CHECK(!settings_get_alert_vibe() && !live_ring && !live_vibe);
    LS_EQ_INT(1, ring_saved);
    LS_EQ_INT(1, vibe_saved);
    LS_EQ_INT(0, commit_count);
    flush();
    LS_EQ_INT(0, ring_saved);
    LS_EQ_INT(0, vibe_saved);
    LS_EQ_INT(1, commit_count);
    LS_CHECK(!live_ring && !live_vibe);
}
LS_CASE(rapid_double_toggles_and_boot_cycles_use_accepted_choices)
{
    prepare();
    ring_next(); ring_next(); vibe_next(); vibe_next();
    LS_CHECK(settings_get_alert_ring() && live_ring);
    LS_CHECK(settings_get_alert_vibe() && live_vibe);
    boot_next(); LS_EQ_INT(2, settings_get_boot_sound());
    boot_next(); LS_EQ_INT(0, settings_get_boot_sound());
    LS_EQ_INT(1, boot_saved);
    flush();
    LS_EQ_INT(1, ring_saved);
    LS_EQ_INT(1, vibe_saved);
    LS_EQ_INT(0, boot_saved);
}
LS_CASE(rejected_persistence_keeps_displayed_and_live_choices)
{
    prepare();
    reject_write = true;
    ring_next(); vibe_next(); boot_next();
    LS_CHECK(settings_get_alert_ring() && live_ring);
    LS_CHECK(settings_get_alert_vibe() && live_vibe);
    LS_EQ_INT(1, settings_get_boot_sound());
    LS_EQ_INT(0, queued_n);
}
LS_CASE(usb_toggle_changes_live_policy_before_persistence)
{
    prepare();
    usb_autoreboot_pref_init(settings_set_usb_autoreboot, false);
    usb_next();
    LS_CHECK(app_usb_autoreboot());
    char value[8]; usb_show(value, sizeof(value));
    LS_CHECK(!strcmp(value, "on"));
    usb_next();
    LS_CHECK(!app_usb_autoreboot());
    LS_EQ_INT(2, queued_n);
    LS_CHECK(!strcmp(queued[0].key, "usb_autoreb"));
    LS_EQ_INT(1, queued[0].v.u8);
    LS_EQ_INT(0, queued[1].v.u8);
    LS_EQ_INT(0, commit_count);
}
LS_CASE(volume_edits_preserve_explicit_mute_at_every_endpoint)
{
    muted = true; volume = 75;
    vol_step(-1); LS_EQ_INT(70, volume); LS_CHECK(muted);
    vol_step(1); LS_EQ_INT(75, volume); LS_CHECK(muted);
    volume = 0; vol_step(-1); LS_EQ_INT(0, volume); LS_CHECK(muted);
    volume = 100; vol_next(); LS_EQ_INT(100, volume); LS_CHECK(muted);
    mute_next(); LS_CHECK(!muted);
}
LS_CASE(dim_timeout_steps_clamp_and_handle_intermediate_saved_values)
{
    timeout = 15; dimt_step(-1); LS_EQ_INT(15, timeout);
    const int steps[] = {30, 60, 120, 240};
    for (unsigned i = 0; i < sizeof(steps) / sizeof(steps[0]); i++) {
        dimt_step(1); LS_EQ_INT(steps[i], timeout);
    }
    dimt_next(); LS_EQ_INT(240, timeout);
    for (int i = 2; i >= 0; i--) { dimt_step(-1); LS_EQ_INT(steps[i], timeout); }
    dimt_step(-1); LS_EQ_INT(15, timeout);
    timeout = 45; dimt_step(-1); LS_EQ_INT(30, timeout);
    timeout = 45; dimt_step(1); LS_EQ_INT(60, timeout);
    LS_CHECK(DISPLAY_ITEMS[5].step == dimt_step);
}

LS_CASE(waterfall_preferences_are_ram_only_on_the_ui_path)
{
    const int before = nvs_reads;
    s_wf_display = UINT32_MAX;
    LS_EQ_UINT(settings_get_waterfall(123), 123);
    s_wf_display = 456; /* boot-loaded value */
    for (int visit = 0; visit < 20; ++visit) LS_EQ_UINT(settings_get_waterfall(123), 456);
    LS_EQ_INT(nvs_reads, before);
}
