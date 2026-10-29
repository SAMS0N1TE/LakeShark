/* The board's METERS store: one ls_meters store behind a lock, filled from
   rec_ism's sink on whichever task decoded the message, read by the screen
   as a copy taken only when something changed. The pins file is read and
   written on the experiments worker (ls_meters_upkeep), never on the UI. */

#include "ls_meters.h"
#include "ls_experiments.h"
#include "rec_ism.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#ifdef _WIN32
#include <direct.h>
#define mkdir(p, mode) _mkdir(p)
#endif

#include "esp_attr.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "ls_textpack.h"

/* A mutex, not a critical section: the store is 20 KB and a copy of it
   must not run with interrupts off. Every caller is a task. */
static portMUX_TYPE s_init_lock = portMUX_INITIALIZER_UNLOCKED;
static StaticSemaphore_t s_mutex_mem;
static SemaphoreHandle_t s_mutex;
static void lock(void)
{
    if (!s_mutex) {
        portENTER_CRITICAL(&s_init_lock);
        if (!s_mutex) s_mutex = xSemaphoreCreateMutexStatic(&s_mutex_mem);
        portEXIT_CRITICAL(&s_init_lock);
    }
    xSemaphoreTake(s_mutex, portMAX_DELAY);
}
static void unlock(void) { xSemaphoreGive(s_mutex); }
static EXT_RAM_BSS_ATTR lsm_store_t s_store;
static bool s_begun, s_loaded;

int64_t ls_meters_now_s(void) { return esp_timer_get_time() / 1000000 + 1; }

void ls_meters_inject(const lr433_msg_t *msg, uint32_t hz, float rssi, uint8_t source, int64_t now_s)
{
    if (!msg || lr433_proto_class(msg->proto) != LR433_METER) return;
    lock();
    lsm_receive(&s_store, msg, hz, rssi, source, now_s);
    unlock();
}

static void sink(const rec_ism_msg_t *m, void *ctx)
{
    (void)ctx;
    ls_meters_inject(&m->m, m->hz, m->rssi, m->source, ls_meters_now_s());
}

void ls_meters_begin(void)
{
    if (s_begun) return;
    s_begun = true;
    rec_ism_subscribe(sink, NULL);
    /* The pins file is read on the worker. */
    ls_exp_hw_wake();
}

bool ls_meters_snapshot(lsm_store_t *out, uint32_t *seq)
{
    if (!out || !seq) return false;
    lock();
    const bool changed = s_store.seq != *seq;
    if (changed) { memcpy(out, &s_store, sizeof(*out)); *seq = s_store.seq; }
    unlock();
    return changed;
}

static bool edit(uint8_t proto, const char *id, int what, bool pin, const char *name)
{
    lock();
    const int slot = lsm_find(&s_store, proto, id);
    const bool ok = slot >= 0 && (what ? lsm_name(&s_store, slot, name) : lsm_pin(&s_store, slot, pin));
    unlock();
    if (ok) ls_exp_hw_wake();
    return ok;
}

bool ls_meters_pin(uint8_t proto, const char *id, bool pin) { return edit(proto, id, 0, pin, NULL); }
bool ls_meters_name(uint8_t proto, const char *id, const char *name) { return edit(proto, id, 1, false, name); }

void ls_meters_clear(void)
{
    lock();
    lsm_clear(&s_store);
    unlock();
}

#define FILE_MAX 4096

bool ls_meters_upkeep(void)
{
    if (!s_begun) return false;
    if (!s_loaded) {
        s_loaded = true;
        FILE *f = fopen(LS_METERS_FILE, "rb");
        if (f) {
            char *buf = heap_caps_malloc(FILE_MAX, MALLOC_CAP_SPIRAM);
            if (buf) {
                const size_t n = fread(buf, 1, FILE_MAX - 1, f);
                buf[n] = 0;
                lock();
                const bool dirty = s_store.pins_dirty;
                lsm_pins_load(&s_store, buf);
                s_store.pins_dirty = dirty;
                unlock();
                heap_caps_free(buf);
            }
            fclose(f);
        }
    }
    lock();
    const bool dirty = s_store.pins_dirty;
    s_store.pins_dirty = false;
    unlock();
    if (!dirty) return false;
    char *buf = heap_caps_malloc(FILE_MAX, MALLOC_CAP_SPIRAM);
    lsm_store_t *copy = heap_caps_malloc(sizeof(*copy), MALLOC_CAP_SPIRAM);
    bool ok = buf && copy;
    if (ok) {
        lock();
        memcpy(copy, &s_store, sizeof(*copy));
        unlock();
        const size_t n = lsm_pins_json(copy, buf, FILE_MAX);
        mkdir("/sdcard/lakeshark", 0775);
        FILE *f = n ? fopen(LS_METERS_FILE ".tmp", "wb") : NULL;
        ok = f && fwrite(buf, 1, n, f) == n;
        if (f && fclose(f)) ok = false;
        if (ok) {
            remove(LS_METERS_FILE);
            ok = rename(LS_METERS_FILE ".tmp", LS_METERS_FILE) == 0;
        }
    }
    heap_caps_free(buf);
    heap_caps_free(copy);
    /* No card: keep the pins in memory and try again on the next change. */
    if (!ok) { lock(); s_store.pins_dirty = true; unlock(); }
    return false;
}

/* ------------------------------------------------------------- console -- */

static int find_id(const char *id)
{
    for (int i = 0; i < LSM_MAX; i++)
        if (s_store.m[i].used && !strcmp(s_store.m[i].id, id)) return i;
    return -1;
}

int ls_meters_command(int argc, char **argv)
{
    ls_meters_begin();
    if (argc >= 3 && (!strcmp(argv[1], "pin") || !strcmp(argv[1], "unpin") || !strcmp(argv[1], "name"))) {
        lock();
        const int slot = find_id(argv[2]);
        const uint8_t proto = slot >= 0 ? s_store.m[slot].proto : 0;
        unlock();
        bool ok = slot >= 0;
        if (ok && argv[1][0] == 'n') ok = ls_meters_name(proto, argv[2], argc > 3 ? argv[3] : "");
        else if (ok) ok = ls_meters_pin(proto, argv[2], argv[1][0] == 'p');
        printf("meters: %s\n", ok ? "ok" : "no meter with that ID");
        return ok ? 0 : 1;
    }
    if (argc >= 2 && strcmp(argv[1], "list")) {
        printf("meters [list] | pin <id> | unpin <id> | name <id> <text>\n");
        return 1;
    }
    lsm_store_t *s = heap_caps_malloc(sizeof(*s), MALLOC_CAP_SPIRAM);
    if (!s) return 1;
    uint32_t seq = UINT32_MAX;
    ls_meters_snapshot(s, &seq);
    uint8_t order[LSM_MAX];
    const int n = lsm_order(s, order);
    const int64_t now = ls_meters_now_s();
    uint32_t pub = 0, dup = 0;
    rec_ism_counts(&pub, &dup);
    LS_PRINTF(t_ls_meters_rt_meters_d_ism_published, "meters: %d (ISM published %lu, duplicates %lu)\n", n, (unsigned long)pub, (unsigned long)dup);
    for (int i = 0; i < n; i++) {
        const lsm_meter_t *m = &s->m[order[i]];
        const double r = lsm_rate_per_hour(m);
        char rate[24] = "--";
        if (!isnan(r)) snprintf(rate, sizeof(rate), "%.2f", r);
        LS_PRINTF(t_ls_meters_rt_c_9s_10s_12s, "%c %-9s %-10s %-12s %10lld  %s/h  heard %lu  %llds ago  %.3f MHz\n", m->pinned ? '*' : ' ',
               lr433_proto_label(m->proto), m->id, m->name[0] ? m->name : "-", (long long)m->value,
               rate, (unsigned long)m->heard, (long long)(m->last_s ? now - m->last_s : -1), m->hz / 1e6);
    }
    heap_caps_free(s);
    return 0;
}
