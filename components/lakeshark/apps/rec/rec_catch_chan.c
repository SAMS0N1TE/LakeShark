/* See rec_catch_chan.h. */
#include "rec_catch_chan.h"
#include <string.h>
#ifdef ESP_PLATFORM
#include "freertos/FreeRTOS.h"
#include "freertos/portmacro.h"
#include "nvs.h"
#include "ls_nvs_safe.h"
static portMUX_TYPE guard = portMUX_INITIALIZER_UNLOCKED;
static void lock(void) { portENTER_CRITICAL(&guard); }
static void unlock(void) { portEXIT_CRITICAL(&guard); }
#else
static unsigned guard;
static void lock(void) { while (__atomic_exchange_n(&guard, 1, __ATOMIC_ACQUIRE)) {} }
static void unlock(void) { __atomic_store_n(&guard, 0, __ATOMIC_RELEASE); }
#endif

static const uint32_t PRESET_HZ[CATCH_PRESETS] = {433920000u, 315000000u, 868350000u, 915000000u};

unsigned catch_watch_count(const catch_watch_t *w)
{
    unsigned n = 0;
    for (int i = 0; i < CATCH_CHAN_MAX; i++) if (w->chan[i].centre) n++;
    return n;
}
bool catch_watch_filtering(const catch_watch_t *w)
{ return w->mode == CATCH_WATCH_CHOSEN && catch_watch_count(w) > 0; }
const char *catch_watch_note(const catch_watch_t *w)
{ return w->mode == CATCH_WATCH_CHOSEN && !catch_watch_count(w) ? "CHOSEN is empty: watching ALL" : NULL; }
bool catch_watch_allows(const catch_watch_t *w, uint32_t hz)
{
    if (!catch_watch_filtering(w)) return true;
    for (int i = 0; i < CATCH_CHAN_MAX; i++) {
        const catch_chan_t *c = &w->chan[i];
        if (!c->centre) continue;
        int64_t half = c->width / 2, d = (int64_t)hz - (int64_t)c->centre;
        if (d >= -half && d <= half) return true;
    }
    return false;
}
static bool chan_ok(const catch_chan_t *c)
{
    if (!c->centre && !c->width) return true;
    return c->centre >= CATCH_CHAN_MIN_HZ && c->centre <= CATCH_CHAN_MAX_HZ &&
           c->width >= CATCH_CHAN_MIN_WIDTH && c->width <= CATCH_CHAN_MAX_WIDTH;
}
bool catch_watch_valid(const catch_watch_t *w)
{
    if (w->mode > CATCH_WATCH_CHOSEN) return false;
    for (int i = 0; i < CATCH_CHAN_MAX; i++) if (!chan_ok(&w->chan[i])) return false;
    return true;
}
int catch_watch_add(catch_watch_t *w, uint32_t centre, uint32_t width)
{
    catch_chan_t c = {centre, width};
    if (!centre || !chan_ok(&c)) return -1;
    int free_slot = -1;
    for (int i = 0; i < CATCH_CHAN_MAX; i++) {
        if (w->chan[i].centre == centre) { w->chan[i].width = width; return i; }
        if (!w->chan[i].centre && free_slot < 0) free_slot = i;
    }
    if (free_slot < 0) return -2;
    w->chan[free_slot] = c;
    return free_slot;
}
bool catch_watch_remove(catch_watch_t *w, unsigned slot)
{
    if (slot >= CATCH_CHAN_MAX || !w->chan[slot].centre) return false;
    w->chan[slot] = (catch_chan_t){0, 0};
    return true;
}
bool catch_chan_in_slice(uint32_t slice_centre, uint32_t hz)
{
    int64_t d = (int64_t)hz - (int64_t)slice_centre;
    return d >= -(int64_t)(CATCH_SLICE_HZ / 2) && d <= (int64_t)(CATCH_SLICE_HZ / 2);
}
unsigned catch_watch_presets(uint32_t slice_centre, uint32_t *out, unsigned cap)
{
    unsigned n = 0;
    for (unsigned i = 0; i < CATCH_PRESETS && n < cap; i++)
        if (catch_chan_in_slice(slice_centre, PRESET_HZ[i])) out[n++] = PRESET_HZ[i];
    return n;
}
/* Column of a frequency on a strip of `cols` (<= 4096) cells; clamped to it. */
int catch_slice_col(uint32_t slice_centre, uint32_t hz, int cols)
{
    int64_t d = (int64_t)hz - ((int64_t)slice_centre - CATCH_SLICE_HZ / 2);
    if (d < 0) return 0;
    if (d >= CATCH_SLICE_HZ) return cols - 1;
    return (int)((uint32_t)d * (uint32_t)cols / CATCH_SLICE_HZ);
}
bool catch_chan_cols(uint32_t slice_centre, const catch_chan_t *c, int cols, int *left, int *right)
{
    if (!c->centre || cols <= 0) return false;
    uint32_t lo = c->centre - c->width / 2, hi = c->centre + c->width / 2;   /* centre >= 25.2 MHz > width */
    if (!catch_chan_in_slice(slice_centre, hi) && !catch_chan_in_slice(slice_centre, lo) &&
        !(lo < slice_centre && hi > slice_centre)) return false;
    *left = catch_slice_col(slice_centre, lo, cols);
    *right = catch_slice_col(slice_centre, hi, cols);
    return true;
}

/* Saved form: 'C' 'W' version 0, the struct as it is, additive check. */
static uint16_t sum16(const uint8_t *p, size_t n) { uint16_t s = 0x5A5A; while (n--) s = (uint16_t)(s + *p++); return s; }
size_t catch_watch_encode(const catch_watch_t *w, uint8_t out[CATCH_WATCH_BLOB])
{
    out[0] = 'C'; out[1] = 'W'; out[2] = 1; out[3] = 0;
    memcpy(out + 4, w, sizeof(*w));
    uint16_t s = sum16(out, CATCH_WATCH_BLOB - 2);
    out[CATCH_WATCH_BLOB - 2] = (uint8_t)s; out[CATCH_WATCH_BLOB - 1] = (uint8_t)(s >> 8);
    return CATCH_WATCH_BLOB;
}
bool catch_watch_decode(const uint8_t *in, size_t n, catch_watch_t *w)
{
    if (!in || n != CATCH_WATCH_BLOB || in[0] != 'C' || in[1] != 'W' || in[2] != 1 || in[3]) return false;
    if ((uint16_t)(in[n - 2] | in[n - 1] << 8) != sum16(in, n - 2)) return false;
    catch_watch_t t;
    memcpy(&t, in + 4, sizeof(t));
    if (!catch_watch_valid(&t)) return false;
    *w = t;
    return true;
}

static catch_watch_t live;
static bool loaded;
#ifdef ESP_PLATFORM
typedef struct { uint8_t blob[CATCH_WATCH_BLOB]; size_t len; bool save; } blob_job_t;
static esp_err_t blob_job(void *arg)
{
    blob_job_t *j = arg; nvs_handle_t h;
    esp_err_t e = nvs_open("catch", j->save ? NVS_READWRITE : NVS_READONLY, &h);
    if (e != ESP_OK) return e;
    if (j->save) { e = nvs_set_blob(h, "watch_v1", j->blob, j->len); if (e == ESP_OK) e = nvs_commit(h); }
    else { j->len = sizeof(j->blob); e = nvs_get_blob(h, "watch_v1", j->blob, &j->len); }
    nvs_close(h);
    return e;
}
static bool store_read(uint8_t *out, size_t *n)
{
    blob_job_t j = {0};
    if (ls_nvs_run(blob_job, &j, 0) != ESP_OK) return false;
    memcpy(out, j.blob, j.len); *n = j.len;
    return true;
}
static void store_write(const uint8_t *b, size_t n)
{
    blob_job_t j = {.len = n, .save = true};
    memcpy(j.blob, b, n);
    (void)ls_nvs_run(blob_job, &j, 0);
}
#else
static uint8_t host_blob[CATCH_WATCH_BLOB + 8];
static size_t host_len;
static bool store_read(uint8_t *out, size_t *n)
{ if (!host_len) return false; memcpy(out, host_blob, host_len); *n = host_len; return true; }
static void store_write(const uint8_t *b, size_t n) { memcpy(host_blob, b, n); host_len = n; }
void catch_watch_host_reboot(void) { lock(); live = (catch_watch_t){0}; loaded = false; unlock(); }
uint8_t *catch_watch_host_blob(size_t *len) { if (len) *len = host_len; return host_blob; }
#endif

void catch_watch_load(void)
{
    lock(); bool done = loaded; unlock();
    if (done) return;
    catch_watch_t w = {0}; uint8_t b[CATCH_WATCH_BLOB + 8]; size_t n = 0;
    /* Missing, short, damaged or out-of-range: start from ALL, nothing chosen. */
    if (!store_read(b, &n) || !catch_watch_decode(b, n, &w)) w = (catch_watch_t){0};
    lock(); live = w; loaded = true; unlock();
}
void catch_watch_get(catch_watch_t *out)
{ lock(); *out = live; unlock(); }
bool catch_watch_set(const catch_watch_t *w)
{
    if (!catch_watch_valid(w)) return false;
    uint8_t b[CATCH_WATCH_BLOB];
    lock(); live = *w; loaded = true; unlock();
    store_write(b, catch_watch_encode(w, b));
    return true;
}
bool catch_watch_passes(uint32_t hz)
{ lock(); bool ok = catch_watch_allows(&live, hz); unlock(); return ok; }
