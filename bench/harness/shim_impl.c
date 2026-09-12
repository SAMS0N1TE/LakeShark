/* Host implementations of the firmware primitives the decoders call. */
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include "esp_timer.h"
#include "audio_player.h"
#include "esp_random.h"
#include "esp_heap_caps.h"

extern int ls_shim_log_enabled;

static int64_t s_now_us = 0;

#define LS_SHIM_HEAP_CALL_MAX 32
static unsigned s_heap_calls;
static unsigned s_heap_fail_from;
static unsigned s_heap_caps[LS_SHIM_HEAP_CALL_MAX];
static size_t s_heap_sizes[LS_SHIM_HEAP_CALL_MAX];
static unsigned s_heap_outstanding;

static bool heap_should_fail(size_t size, unsigned caps)
{
    unsigned call = ++s_heap_calls;
    if (call <= LS_SHIM_HEAP_CALL_MAX) {
        s_heap_caps[call - 1] = caps;
        s_heap_sizes[call - 1] = size;
    }
    return s_heap_fail_from != 0 && call >= s_heap_fail_from;
}

void *heap_caps_malloc(size_t size, unsigned caps)
{
    if (heap_should_fail(size, caps)) return NULL;
    void *ptr = malloc(size);
    if (ptr) ++s_heap_outstanding;
    return ptr;
}

void *heap_caps_calloc(size_t count, size_t size, unsigned caps)
{
    if (count != 0 && size > SIZE_MAX / count) return NULL;
    if (heap_should_fail(count * size, caps)) return NULL;
    void *ptr = calloc(count, size);
    if (ptr) ++s_heap_outstanding;
    return ptr;
}

void heap_caps_free(void *ptr)
{
    if (!ptr) return;
    free(ptr);
    if (s_heap_outstanding != 0) --s_heap_outstanding;
}

size_t heap_caps_get_free_size(unsigned caps)
{
    (void)caps;
    return 0;
}

size_t heap_caps_get_largest_free_block(unsigned caps)
{
    (void)caps;
    return 0;
}

/* zeros, like the two above - there is no heap here to report on,
   and a fabricated figure would be a measurement. What matters is that the
   call exists, so code that reads the allocator's watermark links and runs. */
void heap_caps_get_info(multi_heap_info_t *info, unsigned caps)
{
    (void)caps;
    if (info) memset(info, 0, sizeof(*info));
}

void ls_shim_heap_reset(void)
{
    s_heap_calls = 0;
    s_heap_fail_from = 0;
    memset(s_heap_caps, 0, sizeof(s_heap_caps));
    memset(s_heap_sizes, 0, sizeof(s_heap_sizes));
    s_heap_outstanding = 0;
}

void ls_shim_heap_fail_from(unsigned call_index)
{
    s_heap_fail_from = call_index;
}

unsigned ls_shim_heap_call_count(void) { return s_heap_calls; }
unsigned ls_shim_heap_call_caps(unsigned call_index)
{
    return call_index > 0 && call_index <= s_heap_calls &&
           call_index <= LS_SHIM_HEAP_CALL_MAX
               ? s_heap_caps[call_index - 1] : 0;
}
size_t ls_shim_heap_call_size(unsigned call_index)
{
    return call_index > 0 && call_index <= s_heap_calls &&
           call_index <= LS_SHIM_HEAP_CALL_MAX
               ? s_heap_sizes[call_index - 1] : 0;
}
unsigned ls_shim_heap_outstanding(void) { return s_heap_outstanding; }

/* See esp_timer.h. Frozen unless something asks for a live clock. */
static int s_time_live;

#if defined(_WIN32)
#include <windows.h>
static int64_t host_us(void)
{
    LARGE_INTEGER f, c;
    if (!QueryPerformanceFrequency(&f) || f.QuadPart == 0) return 0;
    QueryPerformanceCounter(&c);
    return (int64_t)((c.QuadPart * 1000000LL) / f.QuadPart);
}
#else
#include <time.h>
static int64_t host_us(void)
{
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) return 0;
    return (int64_t)ts.tv_sec * 1000000LL + ts.tv_nsec / 1000;
}
#endif

int64_t esp_timer_get_time(void)
{
    return s_time_live ? host_us() : s_now_us;
}
void    ls_shim_time_set(int64_t us)  { s_now_us = us; }
void    ls_shim_time_advance(int64_t us) { s_now_us += us; }
void    ls_shim_time_live(int on)     { s_time_live = on ? 1 : 0; }

/* The ROM busy-wait. Real on the board, nothing on the host: the
   bench has no synthesiser to wait for and a test that actually slept would
   spend its time proving the host can sleep. */
void esp_rom_delay_us(uint32_t us) { (void)us; }

void esp_fill_random(void *buffer, size_t bytes)
{
    uint8_t *out = (uint8_t *)buffer;
    uint32_t state = 0x6c735478u;
    for (size_t i = 0; i < bytes; ++i) {
        state ^= state << 13;
        state ^= state >> 17;
        state ^= state << 5;
        out[i] = (uint8_t)state;
    }
}

/* diag.c drags in UART and FreeRTOS, so the bench substitutes it. */
#define DIAG_CAP_LINES 512
#define DIAG_CAP_LEN   256

static char s_diag[DIAG_CAP_LINES][DIAG_CAP_LEN];
static int  s_diag_n;

void  diag_init(void) {}
void  diag_emit_periodic(void) {}
void  diag_count_sync_attempt(int matched_exact, int best_hd) { (void)matched_exact; (void)best_hd; }
void  diag_count_bch_result(int ok, int ec) { (void)ok; (void)ec; }
void  diag_count_frame(const char *duid) { (void)duid; }
float diag_uptime_s(void) { return (float)s_now_us / 1e6f; }

void diag_dump_nid(const char *tag, const int *dibits33, int nac_raw,
                   const char *duid_raw, int ec, int verdict_ok,
                   const char *reason)
{
    (void)tag; (void)dibits33; (void)nac_raw; (void)duid_raw;
    (void)ec;  (void)verdict_ok; (void)reason;
}

void diag_vline(const char *tag, const char *fmt, va_list ap)
{
    char body[DIAG_CAP_LEN - 64];
    vsnprintf(body, sizeof(body), fmt, ap);
    if (s_diag_n < DIAG_CAP_LINES)
        snprintf(s_diag[s_diag_n++], DIAG_CAP_LEN, "%s: %s", tag, body);
    if (ls_shim_log_enabled)
        fprintf(stderr, "[diag] %s: %s\n", tag, body);
}

void diag_line(const char *tag, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    diag_vline(tag, fmt, ap);
    va_end(ap);
}

void        ls_diag_clear(void)  { s_diag_n = 0; }
int         ls_diag_count(void)  { return s_diag_n; }
const char *ls_diag_line(int i)  { return (i >= 0 && i < s_diag_n) ? s_diag[i] : ""; }

int ls_diag_contains(const char *needle)
{
    for (int i = 0; i < s_diag_n; i++)
        if (strstr(s_diag[i], needle)) return 1;
    return 0;
}

/* Audio player state - a test writes to it, the code under test reads it. */
static audio_player_state_t s_audio_state = AUDIO_PLAYER_STATE_IDLE;

audio_player_state_t audio_player_get_state(void)         { return s_audio_state; }
void ls_shim_audio_state_set(audio_player_state_t state)  { s_audio_state = state; }
