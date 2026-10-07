/* SD work stays on the experiment worker, outside critical sections. */
#include "lr433_history.h"
#include "../ls_experiments.h"
#include "esp_attr.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include <dirent.h>
#include <errno.h>
#include <math.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#ifdef _WIN32
#include <direct.h>
#define mkdir(p, mode) _mkdir(p)
#endif

#ifndef LS_SENSOR_ROOT
#define LS_SENSOR_ROOT "/sdcard/lakeshark/sensors"
#endif
#define ROOT LS_SENSOR_ROOT
#define DISK_VERSION 1u
static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static EXT_RAM_BSS_ATTR sh_store_t s_store;
static EXT_RAM_BSS_ATTR sh_sensor_t s_disk;
static uint32_t s_dirty;
static bool s_loaded, s_ready, s_prefs_dirty;
static int64_t s_flush_at, s_prune_at;
/* A short outage retains pending CSV rows without allocation or capture I/O. */
#define CSV_PENDING 64
typedef struct {
    char id[16], name[SH_NAME];
    uint8_t proto;
    int8_t channel;
    bool own;
    sh_reading_t reading;
} csv_pending_t;
static EXT_RAM_BSS_ATTR csv_pending_t s_csv[CSV_PENDING];
static unsigned s_csv_head, s_csv_count;
static EXT_RAM_BSS_ATTR sh_sensor_t s_csv_sensor;
static int s_selected = -1;
static char s_text[SH_NAME];
static char s_selected_id[16];
static uint8_t s_selected_proto;
static int8_t s_selected_channel;
static char s_status[40] = "History: nothing saved yet";

int64_t lr433_history_now(void) { return (int64_t)time(NULL); }
const char *lr433_history_status(void) { return s_status; }

static void expire_locked(int64_t now)
{
    uint16_t before[SH_SENSORS]; uint32_t used = 0;
    for (int i = 0; i < SH_SENSORS; i++) {
        before[i] = s_store.sensor[i].count;
        if (s_store.sensor[i].used) used |= 1u << i;
    }
    sh_expire(&s_store, now);
    for (int i = 0; i < SH_SENSORS; i++)
        if (before[i] != s_store.sensor[i].count || !!(used & (1u << i)) != s_store.sensor[i].used)
            s_dirty |= 1u << i;
}

static void path(char *out, size_t n, int slot, bool tmp)
{
    snprintf(out, n, ROOT "/sensor-%02d.bin%s", slot, tmp ? ".tmp" : "");
}

static FILE *read_file(const char *p)
{
    FILE *f = fopen(p, "rb");
    if (f) return f;
    char bak[sizeof(ROOT) + 64]; snprintf(bak, sizeof(bak), "%s.bak", p);
    return fopen(bak, "rb");
}

static void ensure_directory(void)
{
#ifndef LS_SENSOR_HISTORY_TEST
    mkdir("/sdcard/lakeshark", 0775);
#endif
    mkdir(ROOT, 0775);
}

static bool replace_file(const char *tmp, const char *p)
{
    /* FAT and Windows refuse rename over an existing file. Keep the previous
       snapshot until the replacement has its final name, also across reboot. */
    char bak[sizeof(ROOT) + 64]; snprintf(bak, sizeof(bak), "%s.bak", p);
    struct stat st;
    bool previous = stat(p, &st) == 0;
    if (previous) {
        if (remove(bak) && errno != ENOENT) return false;
        if (rename(p, bak)) return false;
    }
    if (rename(tmp, p)) { if (previous) rename(bak, p); return false; }
    remove(bak);
    return true;
}

void lr433_history_start(void)
{
    if (s_loaded) return;
    s_loaded = true;
    portENTER_CRITICAL(&s_lock);
    sh_init(&s_store);
    portEXIT_CRITICAL(&s_lock);
    ensure_directory();
    /* Versioned, bounded snapshots restore names, privacy and rings. CSV is
       the portable archive; snapshots are private to this firmware layout. */
    for (int i = 0; i < SH_SENSORS; i++) {
        char p[sizeof(ROOT) + 48]; path(p, sizeof(p), i, false);
        FILE *f = read_file(p);
        if (!f) continue;
        uint32_t version = 0;
        const bool ok = fread(&version, sizeof(version), 1, f) == 1 && version == DISK_VERSION &&
            fread(&s_disk, sizeof(s_disk), 1, f) == 1 && s_disk.proto < LR433_P_COUNT &&
            s_disk.head < SH_READINGS && s_disk.count <= SH_READINGS &&
            memchr(s_disk.id, 0, sizeof(s_disk.id)) && memchr(s_disk.name, 0, sizeof(s_disk.name));
        fclose(f);
        if (!ok) continue;
        sh_own(&s_disk, s_disk.own);
        s_disk.pending = false;
        portENTER_CRITICAL(&s_lock);
        s_store.sensor[i] = s_disk;
        portEXIT_CRITICAL(&s_lock);
    }
    FILE *f = read_file(ROOT "/options.bin");
    if (f) {
        unsigned char v[4];
        if (fread(v, 1, sizeof(v), f) == sizeof(v) && v[0] == DISK_VERSION && v[1] >= 1 && v[1] <= 30) {
            portENTER_CRITICAL(&s_lock);
            s_store.days = v[1]; s_store.show_strangers = !!v[2]; s_store.fahrenheit = !!v[3];
            portEXIT_CRITICAL(&s_lock);
        }
        fclose(f);
    }
    const int64_t now = lr433_history_now();
    portENTER_CRITICAL(&s_lock);
    expire_locked(now);
    portEXIT_CRITICAL(&s_lock);
    __atomic_store_n(&s_ready, true, __ATOMIC_RELEASE);
}

static void prune(void)
{
    DIR *dir = opendir(ROOT);
    if (!dir) return;
    const int64_t today = lr433_history_now() / SH_DAY;
    portENTER_CRITICAL(&s_lock); const int days = s_store.days; portEXIT_CRITICAL(&s_lock);
    struct dirent *e;
    while ((e = readdir(dir))) {
        long long day; int end = 0;
        if (sscanf(e->d_name, "day-%lld.csv%n", &day, &end) == 1 && end > 0 && !e->d_name[end] &&
            day >= 0 && today - day >= days && strlen(e->d_name) < 48) {
            char p[sizeof(ROOT) + 48]; snprintf(p, sizeof(p), ROOT "/%.47s", e->d_name); remove(p);
        }
    }
    closedir(dir);
}

void lr433_history_flush(void)
{
    /* One sensor per poll bounds work; dirty state survives a missing SD. */
    int slot = -1;
    portENTER_CRITICAL(&s_lock);
    for (int i = 0; i < SH_SENSORS; i++) if (s_dirty & (1u << i)) {
        slot = i; s_disk = s_store.sensor[i]; s_dirty &= ~(1u << i); break;
    }
    const bool prefs = s_prefs_dirty;
    s_prefs_dirty = false;
    unsigned char v[4] = { DISK_VERSION, s_store.days, s_store.show_strangers, s_store.fahrenheit };
    portEXIT_CRITICAL(&s_lock);
    bool ok = true;
    if (slot >= 0) {
        char p[sizeof(ROOT) + 48], tmp[sizeof(ROOT) + 48]; path(p, sizeof(p), slot, false); path(tmp, sizeof(tmp), slot, true);
        FILE *f = fopen(tmp, "wb");
        const uint32_t version = DISK_VERSION;
        if (!f) ok = false;
        else {
            ok = fwrite(&version, sizeof(version), 1, f) == 1 && fwrite(&s_disk, sizeof(s_disk), 1, f) == 1;
            if (fclose(f)) ok = false;
            if (ok) ok = replace_file(tmp, p);
        }
        if (!ok) { portENTER_CRITICAL(&s_lock); s_dirty |= 1u << slot; portEXIT_CRITICAL(&s_lock); }
    }
    if (prefs) {
        FILE *f = fopen(ROOT "/options.bin.tmp", "wb");
        bool saved = f != NULL;
        if (f) { saved = fwrite(v, 1, sizeof(v), f) == sizeof(v); if (fclose(f)) saved = false; }
        if (saved) saved = replace_file(ROOT "/options.bin.tmp", ROOT "/options.bin");
        if (!saved) { portENTER_CRITICAL(&s_lock); s_prefs_dirty = true; portEXIT_CRITICAL(&s_lock); ok = false; }
        if (saved) prune();
    }
    if (slot >= 0 || prefs) snprintf(s_status, sizeof(s_status), "%s", ok ? "History: SD saved" : "History: SD write failed");
}

void lr433_history_receive(const lr433_msg_t *m)
{
    bool appended;
    const int64_t now = lr433_history_now();
    portENTER_CRITICAL(&s_lock);
    expire_locked(now);
    int slot = sh_receive(&s_store, m, now, &appended);
    if (slot >= 0) {
        s_dirty |= 1u << slot;
        if (appended && s_csv_count < CSV_PENDING) {
            const sh_sensor_t *d = &s_store.sensor[slot];
            csv_pending_t *q = &s_csv[(s_csv_head + s_csv_count) % CSV_PENDING];
            snprintf(q->id, sizeof(q->id), "%s", d->id);
            snprintf(q->name, sizeof(q->name), "%s", d->name);
            q->proto = d->proto; q->channel = d->channel; q->own = d->own; q->reading = d->raw;
            s_csv_count++;
        } else if (appended) snprintf(s_status, sizeof(s_status), "History: CSV queue full (ring kept)");
    }
    portEXIT_CRITICAL(&s_lock);
}

static bool csv_flush(void)
{
    if (!s_csv_count) return true;
    const csv_pending_t *q = &s_csv[s_csv_head];
    snprintf(s_csv_sensor.id, sizeof(s_csv_sensor.id), "%s", q->id);
    snprintf(s_csv_sensor.name, sizeof(s_csv_sensor.name), "%s", q->name);
    s_csv_sensor.proto = q->proto; s_csv_sensor.channel = q->channel; s_csv_sensor.own = q->own;
    char p[sizeof(ROOT) + 48];
    snprintf(p, sizeof(p), ROOT "/day-%lld.csv", (long long)(q->reading.time / SH_DAY));
    FILE *f = fopen(p, "a+");
    bool ok = f != NULL;
    if (f) {
        if (fseek(f, 0, SEEK_END)) ok = false;
        if (ok && ftell(f) == 0)
            ok = fprintf(f, "utc_seconds,protocol,id,channel,name,temp_C,humidity_pct,rain_mm,wind_m_s,pressure_kPa,battery_low\n") > 0;
        if (ok) ok = sh_csv(f, &s_csv_sensor, &q->reading);
        if (fclose(f)) ok = false;
    }
    snprintf(s_status, sizeof(s_status), "%s", ok ? "History: SD saved" : "History: CSV failed (ring kept)");
    if (ok) { s_csv_head = (s_csv_head + 1) % CSV_PENDING; s_csv_count--; }
    return ok;
}

int lr433_history_list(uint8_t order[SH_SENSORS])
{
    const int64_t now = lr433_history_now();
    portENTER_CRITICAL(&s_lock);
    uint32_t was_dirty = s_dirty;
    expire_locked(now);
    bool changed = was_dirty != s_dirty;
    int n = sh_order(&s_store, order);
    portEXIT_CRITICAL(&s_lock);
    if (changed) ls_exp_hw_wake();
    return n;
}
bool lr433_history_copy(int slot, sh_sensor_t *out)
{
    if (slot < 0 || slot >= SH_SENSORS) return false;
    portENTER_CRITICAL(&s_lock);
    bool used = s_store.sensor[slot].used;
    if (used) *out = s_store.sensor[slot];
    portEXIT_CRITICAL(&s_lock);
    return used;
}
bool lr433_history_alert(char *name, size_t n, float *c)
{
    bool found = false;
    portENTER_CRITICAL(&s_lock);
    for (int i = 0; i < SH_SENSORS; i++) {
        sh_sensor_t *d = &s_store.sensor[i];
        if (!d->pending) continue;
        d->pending = false;
        if (d->hidden || !d->name[0] || !d->alert_on) continue;
        snprintf(name, n, "%s", d->name); *c = d->last.temp_c; found = true; break;
    }
    portEXIT_CRITICAL(&s_lock);
    return found;
}

/* Selected sensor is pinned by the options context until it closes. */
static sh_sensor_t *selected(void)
{
    if (s_selected < 0) return NULL;
    sh_sensor_t *d = &s_store.sensor[s_selected];
    return d->used && d->proto == s_selected_proto && d->channel == s_selected_channel &&
        !strcmp(d->id, s_selected_id) ? d : NULL;
}
static int get(const ls_opt_t *o)
{
    portENTER_CRITICAL(&s_lock);
    sh_sensor_t *d = selected();
    int v = o->arg == 0 ? s_store.show_strangers : o->arg == 1 ? s_store.fahrenheit :
        !d ? 0 : o->arg == 2 ? d->hidden : o->arg == 3 ? d->own : d->alert_on;
    portEXIT_CRITICAL(&s_lock); return v;
}
static void set(const ls_opt_t *o, int v)
{
    if (!__atomic_load_n(&s_ready, __ATOMIC_ACQUIRE)) { ls_exp_hw_wake(); return; }
    portENTER_CRITICAL(&s_lock);
    sh_sensor_t *d = selected();
    if (o->arg == 0) s_store.show_strangers = !!v;
    else if (o->arg == 1) s_store.fahrenheit = !!v;
    else if (d) {
        if (o->arg == 2) { d->hidden = !!v; d->pending = false; }
        else if (o->arg == 3) sh_own(d, !!v);
        else { d->alert_on = !!v && d->name[0]; d->alarm = d->pending = false; }
        s_dirty |= 1u << s_selected;
    }
    s_prefs_dirty = true;
    portEXIT_CRITICAL(&s_lock);
    ls_exp_hw_wake();
}
static double number(const ls_opt_t *o)
{
    portENTER_CRITICAL(&s_lock);
    double v = o->arg == 0 ? s_store.days : selected() ? selected()->threshold_c : -10;
    if (o->arg && s_store.fahrenheit) v = v * 1.8 + 32;
    portEXIT_CRITICAL(&s_lock); return v;
}
static void set_number(const ls_opt_t *o, double v)
{
    if (!__atomic_load_n(&s_ready, __ATOMIC_ACQUIRE)) { ls_exp_hw_wake(); return; }
    const int64_t now = lr433_history_now();
    portENTER_CRITICAL(&s_lock);
    if (!o->arg) {
        s_store.days = (uint8_t)v;
        expire_locked(now);
        s_prefs_dirty = true;
        for (int i = 0; i < SH_SENSORS; i++) if (s_store.sensor[i].used) s_dirty |= 1u << i;
    }
    else if (selected()) {
        selected()->threshold_c = s_store.fahrenheit ? (v - 32) / 1.8 : v;
        selected()->alarm = selected()->pending = false;
        s_dirty |= 1u << s_selected;
    }
    portEXIT_CRITICAL(&s_lock);
    ls_exp_hw_wake();
}
static const char *text(const ls_opt_t *o)
{
    (void)o;
    portENTER_CRITICAL(&s_lock);
    snprintf(s_text, sizeof(s_text), "%s", selected() ? selected()->name : "");
    portEXIT_CRITICAL(&s_lock); return s_text;
}
static void set_text(const ls_opt_t *o, const char *v)
{
    (void)o;
    if (!__atomic_load_n(&s_ready, __ATOMIC_ACQUIRE)) { ls_exp_hw_wake(); return; }
    portENTER_CRITICAL(&s_lock);
    if (selected()) { snprintf(selected()->name, SH_NAME, "%s", v); s_dirty |= 1u << s_selected; }
    portEXIT_CRITICAL(&s_lock);
    ls_exp_hw_wake();
}
static const char *const UNITS[] = { "C", "F" };
static void show_tpms(const ls_opt_t *o, char *out, size_t n)
{
    (void)o; snprintf(out, n, "MY IDS ONLY");
}
static const char *sensor_why(const ls_opt_t *o)
{
    if (!__atomic_load_n(&s_ready, __ATOMIC_ACQUIRE)) return "History is loading";
    const char *why = NULL;
    portENTER_CRITICAL(&s_lock);
    sh_sensor_t *d = selected();
    if (!d) why = "Sensor has aged out";
    else if (o->arg == 3 && lr433_proto_class(d->proto) != LR433_TPMS) why = "TPMS only";
    else if (o->arg == 4 && !d->name[0]) why = "NAME sensor first";
    portEXIT_CRITICAL(&s_lock);
    return why;
}
static void unhide(const ls_opt_t *o)
{
    (void)o;
    portENTER_CRITICAL(&s_lock);
    for (int i = 0; i < SH_SENSORS; i++) if (s_store.sensor[i].hidden) {
        s_store.sensor[i].hidden = false; s_dirty |= 1u << i;
    }
    portEXIT_CRITICAL(&s_lock); ls_exp_hw_wake();
}
static const ls_opt_t GLOBAL[] = {
    { .label = "SHOW STRANGERS", .kind = LS_OPT_TOGGLE, .arg = 0, .get = get, .set = set },
    { .label = "KEEP DAYS", .kind = LS_OPT_LEVEL, .arg = 0, .num = number, .set_num = set_number, .lo = 1, .hi = 30, .step = 1 },
    { .label = "UNITS", .kind = LS_OPT_CYCLE, .arg = 1, .names = UNITS, .n = 2, .get = get, .set = set },
    { .label = "TPMS HISTORY", .kind = LS_OPT_ACTION, .show = show_tpms },
    { .label = "UNHIDE SENSORS", .kind = LS_OPT_ACTION, .act = unhide },
};
static const ls_opt_ctx_t GLOBAL_CTX = { .name = "HISTORY", .job = -1, .radio = LS_RSEL_NONE, LS_OPT_ROWS(GLOBAL) };
static const ls_opt_t SENSOR[] = {
    { .label = "NAME", .kind = LS_OPT_TEXT, .text = text, .set_text = set_text, .max = SH_NAME - 1 },
    { .label = "HIDE", .kind = LS_OPT_TOGGLE, .arg = 2, .get = get, .set = set },
    { .label = "MY TPMS ID", .kind = LS_OPT_TOGGLE, .arg = 3, .get = get, .set = set, .why_not = sensor_why },
    { .label = "ALERT ABOVE", .kind = LS_OPT_TOGGLE, .arg = 4, .get = get, .set = set, .why_not = sensor_why },
    { .label = "THRESHOLD", .kind = LS_OPT_LEVEL, .arg = 1, .num = number, .set_num = set_number, .lo = -100, .hi = 250, .step = 1 },
    { .label = "HISTORY", .kind = LS_OPT_MENU, .sub = &GLOBAL_CTX },
};
static const ls_opt_ctx_t SENSOR_CTX = { .name = "SENSOR", .job = -1, .radio = LS_RSEL_NONE, LS_OPT_ROWS(SENSOR) };
const ls_opt_ctx_t *lr433_history_options(int slot)
{
    portENTER_CRITICAL(&s_lock);
    s_selected = slot >= 0 && slot < SH_SENSORS ? slot : -1;
    if (s_selected >= 0) {
        sh_sensor_t *d = &s_store.sensor[s_selected];
        snprintf(s_selected_id, sizeof(s_selected_id), "%s", d->id);
        s_selected_proto = d->proto; s_selected_channel = d->channel;
    }
    portEXIT_CRITICAL(&s_lock);
    return s_selected >= 0 ? &SENSOR_CTX : &GLOBAL_CTX;
}

void lr433_history_name(const char *name) { set_text(NULL, name); }

bool lr433_history_fahrenheit(void)
{
    portENTER_CRITICAL(&s_lock); bool v = s_store.fahrenheit; portEXIT_CRITICAL(&s_lock); return v;
}

bool lr433_history_owned(const lr433_msg_t *m)
{
    bool own = false;
    portENTER_CRITICAL(&s_lock);
    for (int i = 0; i < SH_SENSORS; i++) {
        const sh_sensor_t *d = &s_store.sensor[i];
        if (d->used && d->proto == m->proto && d->channel == m->channel && !strcmp(d->id, m->id)) {
            own = d->own && !d->hidden; break;
        }
    }
    portEXIT_CRITICAL(&s_lock);
    return own;
}

bool lr433_history_service(void)
{
    lr433_history_start();
    const int64_t now = esp_timer_get_time();
    if (now >= s_prune_at) {
        const int64_t stamp = lr433_history_now();
        portENTER_CRITICAL(&s_lock); expire_locked(stamp); portEXIT_CRITICAL(&s_lock);
        prune(); s_prune_at = now + 3600000000LL;
    }
    portENTER_CRITICAL(&s_lock);
    bool pending = s_dirty || s_prefs_dirty || s_csv_count;
    portEXIT_CRITICAL(&s_lock);
    if (!pending) return false;
    if (now < s_flush_at) return true;
    if (strstr(s_status, "failed")) ensure_directory();
    if (!csv_flush()) { s_flush_at = now + 2000000; return false; }
    lr433_history_flush();
    s_flush_at = now + 100000;
    /* Failed storage is retried on the next wake or running receiver poll. */
    return strncmp(s_status, "History: SD write failed", 23) != 0;
}

#ifdef LS_SENSOR_HISTORY_TEST
void lr433_history_test_reset(void)
{
    sh_init(&s_store);
    s_loaded = s_ready = s_prefs_dirty = false;
    s_dirty = 0; s_flush_at = s_prune_at = 0;
    s_csv_head = s_csv_count = 0;
    s_selected = -1;
}
#endif
