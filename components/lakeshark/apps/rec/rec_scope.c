#include "rec_scope.h"
#include "esp_timer.h"
#include <math.h>
#include <stdatomic.h>
#include <string.h>

static uint8_t s_ring[REC_SCOPE_N];
static atomic_uint s_head;          /* columns written, ever            */
static int64_t s_last_us;
/* The bucket being filled. */
static float    s_peak_db = -1000.0f;
static uint32_t s_us;
/* The floor, followed down quickly and up slowly, so a burst does not
   become the floor and a quieter band is found again within a second. */
static float s_floor = 0.0f;
static bool  s_floor_set;
static float s_now_over;
static bool  s_dbm;

static void push(float db)
{
    if (!s_floor_set) { s_floor = db; s_floor_set = true; }
    float over = db - s_floor;
    if (over < 6.0f) s_floor += (db - s_floor) * (over < 0.0f ? 0.25f : 0.02f);
    else             s_floor += 0.005f;   /* a carrier that never ends is the floor in the end */
    if (over < 0.0f) over = 0.0f;
    if (over > 127.0f) over = 127.0f;
    s_now_over = over;
    uint8_t col = (uint8_t)(over + 0.5f);
    if (over >= REC_SCOPE_HOT_DB) col |= REC_SCOPE_HOT;
    const unsigned h = atomic_load_explicit(&s_head, memory_order_relaxed);
    s_ring[h % REC_SCOPE_N] = col;
    atomic_store_explicit(&s_head, h + 1, memory_order_release);
    s_last_us = esp_timer_get_time();
}

void rec_scope_level(float db, uint32_t span_us)
{
    if (db > s_peak_db) s_peak_db = db;
    s_us += span_us;
    while (s_us >= REC_SCOPE_BUCKET_US) {
        push(s_peak_db);
        s_us -= REC_SCOPE_BUCKET_US;
        /* A reading longer than a bucket fills each one it covers. */
        if (s_us < REC_SCOPE_BUCKET_US) s_peak_db = -1000.0f;
    }
}

void rec_scope_iq16(const uint8_t *iq, size_t bytes, uint32_t us_per_sample)
{
    if (!iq || !us_per_sample) return;
    const uint32_t per = REC_SCOPE_BUCKET_US / us_per_sample;
    static int32_t peak;
    static uint32_t n;
    for (size_t i = 0; i + 3 < bytes; i += 4) {
        const int16_t si = (int16_t)(iq[i] | (iq[i + 1] << 8));
        const int16_t sq = (int16_t)(iq[i + 2] | (iq[i + 3] << 8));
        const int32_t mag = (si < 0 ? -(int32_t)si : si) + (sq < 0 ? -(int32_t)sq : sq);
        if (mag > peak) peak = mag;
        if (++n >= per) {
            /* log once a bucket, not once a sample */
            push(20.0f * log10f((float)(peak > 0 ? peak : 1)));
            peak = 0; n = 0;
        }
    }
}

void rec_scope_reset(void)
{
    s_floor_set = false;
    s_peak_db = -1000.0f;
    s_us = 0;
}

int rec_scope_read(uint8_t *out, int n)
{
    const unsigned h = atomic_load_explicit(&s_head, memory_order_acquire);
    if (n > REC_SCOPE_N) n = REC_SCOPE_N;
    if ((unsigned)n > h) n = (int)h;
    for (int i = 0; i < n; i++) out[i] = s_ring[(h - (unsigned)n + (unsigned)i) % REC_SCOPE_N];
    return n;
}

int32_t rec_scope_age_ms(void)
{
    if (!s_last_us) return INT32_MAX;
    const int64_t age = (esp_timer_get_time() - s_last_us) / 1000;
    return age > INT32_MAX ? INT32_MAX : (int32_t)age;
}

float rec_scope_now_db(void) { return s_now_over; }
float rec_scope_floor_db(void) { return s_floor; }
bool  rec_scope_dbm(void) { return s_dbm; }
void  rec_scope_set_dbm(bool dbm) { s_dbm = dbm; }
