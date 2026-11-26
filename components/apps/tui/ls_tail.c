/* The one "with you" rule and its setting (ls_tail.h). */
#include "ls_tail.h"
#include <string.h>

#ifdef ESP_PLATFORM
#include "nvs.h"
#include "ls_nvs_safe.h"
#endif

/* minutes << 16 | metres, read and written whole so readers never lock */
static uint32_t s_rule = (uint32_t)LS_TAIL_MINUTES_DEF << 16 | LS_TAIL_METRES_DEF;
static unsigned s_loaded;

static uint32_t pack(const ls_tail_rule_t *r) { return (uint32_t)r->minutes << 16 | r->metres; }
static uint32_t rule(void) { return __atomic_load_n(&s_rule, __ATOMIC_RELAXED); }

static bool valid(const ls_tail_rule_t *r)
{
    return r && r->minutes >= LS_TAIL_MINUTES_MIN && r->minutes <= LS_TAIL_MINUTES_MAX &&
           r->metres >= LS_TAIL_METRES_MIN && r->metres <= LS_TAIL_METRES_MAX;
}

void ls_tail_get(ls_tail_rule_t *out)
{
    if (!out) return;
    const uint32_t v = rule();
    out->minutes = (uint16_t)(v >> 16);
    out->metres = (uint16_t)v;
}

void ls_tail_defaults(void)
{
    __atomic_store_n(&s_rule, (uint32_t)LS_TAIL_MINUTES_DEF << 16 | LS_TAIL_METRES_DEF, __ATOMIC_RELAXED);
}

#ifdef ESP_PLATFORM
#define TAIL_MAGIC UINT32_C(0x7A11D00E)
typedef struct { uint32_t magic; ls_tail_rule_t rule; } tail_blob_t;
typedef struct { bool save; tail_blob_t blob; } tail_job_t;

static esp_err_t tail_job(void *arg)
{
    tail_job_t *j = arg;
    nvs_handle_t h;
    esp_err_t e = nvs_open("tail", j->save ? NVS_READWRITE : NVS_READONLY, &h);
    if (e != ESP_OK) return e;
    size_t n = sizeof(j->blob);
    if (j->save) {
        e = nvs_set_blob(h, "rule_v1", &j->blob, n);
        if (e == ESP_OK) e = nvs_commit(h);
    } else {
        e = nvs_get_blob(h, "rule_v1", &j->blob, &n);
        if (e == ESP_OK && (n != sizeof(j->blob) || j->blob.magic != TAIL_MAGIC)) e = ESP_ERR_INVALID_SIZE;
    }
    nvs_close(h);
    return e;
}
#endif

void ls_tail_load(void)
{
    unsigned e = 0;
    if (!__atomic_compare_exchange_n(&s_loaded, &e, 1, false, __ATOMIC_ACQ_REL, __ATOMIC_RELAXED)) return;
#ifdef ESP_PLATFORM
    tail_job_t j;
    memset(&j, 0, sizeof(j));
    if (ls_nvs_run(tail_job, &j, 0) == ESP_OK && valid(&j.blob.rule))
        __atomic_store_n(&s_rule, pack(&j.blob.rule), __ATOMIC_RELAXED);
#endif
}

bool ls_tail_set(const ls_tail_rule_t *in)
{
    if (!valid(in)) return false;
    __atomic_store_n(&s_rule, pack(in), __ATOMIC_RELAXED);
    __atomic_store_n(&s_loaded, 1, __ATOMIC_RELEASE);  /* a later load must not undo it */
#ifdef ESP_PLATFORM
    tail_job_t j = {.save = true, .blob = {.magic = TAIL_MAGIC, .rule = *in}};
    return ls_nvs_run(tail_job, &j, 0) == ESP_OK;
#else
    return true;
#endif
}

uint32_t ls_tail_ms(void) { return (rule() >> 16) * 60000u; }
float ls_tail_metres(void) { return (float)(rule() & 0xFFFFu); }
bool ls_tail_time_ok(uint32_t together_ms) { return together_ms >= ls_tail_ms(); }
bool ls_tail_moved_ok(float moved_m, unsigned places) { return moved_m >= ls_tail_metres() || places >= LS_TAIL_PLACES; }

bool ls_tail_with_you(uint32_t together_ms, float moved_m, unsigned places, bool gps)
{
    return ls_tail_time_ok(together_ms) && (!gps || ls_tail_moved_ok(moved_m, places));
}

float ls_tail_need_m(float range_m)
{
    const float m = ls_tail_metres();
    return range_m > m ? range_m : m;
}
