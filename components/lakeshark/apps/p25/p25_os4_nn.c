#include "p25_os4_nn.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

#ifdef ESP_PLATFORM
#include "esp_attr.h"
#include "esp_cpu.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "sdkconfig.h"
#include <stdlib.h>
#endif

#if defined(ESP_PLATFORM) && CONFIG_IDF_TARGET_ESP32P4
#define NN_PIE 1
/* p25_os4_nn_pie.S */
void p25_os4_nn_l1_pie(const int16_t *const rows[P25_OS4_NN_CHUNKS], const int16_t *bias,
                       int16_t *out, int passes);
void p25_os4_nn_l2_pie(const int16_t *a, const int16_t *wt, int16_t *out, int n, int k, int shift);
int32_t p25_os4_nn_dot_pie(const int16_t *a, const int16_t *b, int n8, int shift);
#else
#define NN_PIE 0
#endif

#define CHUNK_MASK ((1u << P25_OS4_NN_CHUNK) - 1u)

typedef int16_t t1_row_t[1 << P25_OS4_NN_CHUNK][P25_OS4_NN_H1];

/* The tables a symbol reads: 4 KB of layer 1's 64 KB, scattered, and all
   16 KB of layer 2's. Generated as constants, so in flash, where a cache
   miss is a 128-byte read at 40 MB/s; with the screen's frames streaming
   through the same 256 KB cache they do not stay in it. On the chip they
   are read from a copy in PSRAM instead (p25_os4_nn_init). */
static const t1_row_t *s_t1 = p25_os4_nn_t1;
static const int16_t (*s_w2t)[P25_OS4_NN_H2] = p25_os4_nn_w2t;

#ifdef ESP_PLATFORM
static EXT_RAM_BSS_ATTR int16_t s_t1_ram[P25_OS4_NN_CHUNKS][1 << P25_OS4_NN_CHUNK][P25_OS4_NN_H1]
    __attribute__((aligned(16)));
static EXT_RAM_BSS_ATTR int16_t s_w2t_ram[P25_OS4_NN_H1][P25_OS4_NN_H2] __attribute__((aligned(16)));
static int s_in_ram = -1;

static void place(int in_ram)
{
    if (in_ram == s_in_ram) return;
    if (in_ram) {
        memcpy(s_t1_ram, p25_os4_nn_t1, sizeof(s_t1_ram));
        memcpy(s_w2t_ram, p25_os4_nn_w2t, sizeof(s_w2t_ram));
        s_t1 = (const t1_row_t *)s_t1_ram;
        s_w2t = (const int16_t (*)[P25_OS4_NN_H2])s_w2t_ram;
    } else {
        s_t1 = p25_os4_nn_t1;
        s_w2t = p25_os4_nn_w2t;
    }
    s_in_ram = in_ram;
}

void p25_os4_nn_init(void) { place(1); }
#else
void p25_os4_nn_init(void) {}
#endif

static int16_t sat16(int32_t v) { return (int16_t)(v > 32767 ? 32767 : v < -32768 ? -32768 : v); }

/* The kernels in plain C, with the same results: the bench runs these, and
   the chip checks its vector kernels against them (`p25 lr nnbench`). */
static void l1_c(const int16_t *const rows[P25_OS4_NN_CHUNKS], int16_t *out)
{
    for (int k = 0; k < P25_OS4_NN_H1; k++) {
        int32_t v = p25_os4_nn_b1[k];
        for (int j = 0; j < P25_OS4_NN_CHUNKS; j++) v = sat16(v + rows[j][k]);
        out[k] = (int16_t)v;
    }
}

static void l2_c(const int16_t *a, int16_t *out)
{
    for (int j = 0; j < P25_OS4_NN_H2; j++) {
        int64_t s = 0;
        for (int i = 0; i < P25_OS4_NN_H1; i++) s += (int32_t)a[i] * s_w2t[i][j];
        out[j] = sat16((int32_t)(s >> (15 - P25_OS4_NN_SHIFT2)));
    }
}

/* Where the vector unit writes: static, so in main SRAM. A task stack can
   sit in the low-power SRAM, which the vector unit cannot store to (four
   store faults on 2026-10-05 from buffers on the console task's stack).
   One set for the decoder's task and one for the console's bench. */
typedef struct {
    int16_t a1[P25_OS4_NN_H1] __attribute__((aligned(16)));
    int16_t a2[P25_OS4_NN_H2] __attribute__((aligned(16)));
} scratch_t;

static scratch_t s_dec;
#ifdef ESP_PLATFORM
static scratch_t s_bench;
#endif

static void rows_of(uint64_t win, const int16_t *rows[P25_OS4_NN_CHUNKS])
{
    for (int j = 0; j < P25_OS4_NN_CHUNKS; j++) {
        const unsigned v = (unsigned)(win >> (P25_OS4_NN_BITS - P25_OS4_NN_CHUNK * (j + 1))) & CHUNK_MASK;
        rows[j] = s_t1[j][v];
    }
}

/* The network in integers, as quant_nn.py (round8) checks it against the
   trained one: layer 1 from the chunk tables in int16 lanes, ReLU; layer 2
   a matrix-vector product shifted into int16, plus its bias, ReLU; layer 3
   into int64. On the P4 the first two run on its vector unit. */
static void logits(uint64_t win, float z[4], int vector, scratch_t *sc)
{
    const int16_t *rows[P25_OS4_NN_CHUNKS];
    int16_t *a1 = sc->a1, *a2 = sc->a2;
    rows_of(win, rows);
#if NN_PIE
    if (vector) p25_os4_nn_l1_pie(rows, p25_os4_nn_b1, a1, P25_OS4_NN_H1 / 32);
    else l1_c(rows, a1);
#else
    (void)vector;
    l1_c(rows, a1);
#endif
    for (int k = 0; k < P25_OS4_NN_H1; k++)
        if (a1[k] < 0) a1[k] = 0;
#if NN_PIE
    if (vector) p25_os4_nn_l2_pie(a1, &s_w2t[0][0], a2, P25_OS4_NN_H1, P25_OS4_NN_H2, P25_OS4_NN_SHIFT2);
    else l2_c(a1, a2);
#else
    l2_c(a1, a2);
#endif
    for (int i = 0; i < P25_OS4_NN_H2; i++) {
        const int32_t v = (int32_t)a2[i] + p25_os4_nn_b2[i];
        a2[i] = (int16_t)(v < 0 ? 0 : v > 32767 ? 32767 : v);
    }
    for (int c = 0; c < 4; c++) {
        int64_t s;
#if NN_PIE
        if (vector) {
            s = p25_os4_nn_dot_pie(a2, p25_os4_nn_w3[c], P25_OS4_NN_H2 / 8, P25_OS4_NN_SHIFT3);
        } else
#endif
        {
            s = 0;
            for (int i = 0; i < P25_OS4_NN_H2; i++) s += (int32_t)p25_os4_nn_w3[c][i] * a2[i];
            s >>= P25_OS4_NN_SHIFT3;
        }
        z[c] = (float)(s + p25_os4_nn_b3[c]) * (1.0f / P25_OS4_NN_S3);
    }
}

/* e^x for x <= 0, to about 0.01%: 2^(x log2 e), the fraction by a cubic.
   The doubts it feeds are 97/90/70% thresholds, and expf cost more than the
   rest of the network. */
static float fast_exp(float x)
{
    if (x < -60.0f) return 0.0f;
    const float t = x * 1.44269504f;
    int i = (int)t;
    if ((float)i > t) i--;
    const float f = t - (float)i;
    union { float f; int32_t i; } u = { 1.0f + f * (0.6951786f + f * (0.2261263f + f * 0.0782526f)) };
    u.i += i * (1 << 23);
    return u.f;
}

static void probs(uint64_t win, float p[4], int vector, scratch_t *sc)
{
    float z[4];
    logits(win, z, vector, sc);
    float top = z[0];
    for (int c = 1; c < 4; c++)
        if (z[c] > top) top = z[c];
    float sum = 0.0f;
    for (int c = 0; c < 4; c++) {
        p[c] = fast_exp(z[c] - top);
        sum += p[c];
    }
    const float inv = 1.0f / sum;
    for (int c = 0; c < 4; c++) p[c] *= inv;
}

void p25_os4_nn_probs(uint64_t win, float p[4]) { probs(win, p, 1, &s_dec); }

/* the lookup's doubt scale (gen_lut.py): 0 when the bit is right at least
   97% of the time, 1 from 90%, 2 from 70%, 3 below */
static unsigned doubt(float q)
{
    return q >= 0.97f ? 0u : q >= 0.90f ? 1u : q >= 0.70f ? 2u : 3u;
}

static uint8_t byte_of(const float p[4])
{
    unsigned d = 0;
    for (unsigned c = 1; c < 4; c++)
        if (p[c] > p[d]) d = c;
    const float p_hi = (d >> 1) ? p[2] + p[3] : p[0] + p[1];   /* the sign bit right */
    const float p_lo = (d & 1) ? p[1] + p[3] : p[0] + p[2];    /* inner/outer right  */
    return (uint8_t)(d | doubt(p_lo) << 3 | doubt(p_hi) << 5);
}

uint8_t p25_os4_nn_byte(uint64_t win)
{
    float p[4];
    probs(win, p, 1, &s_dec);
    return byte_of(p);
}

#ifdef ESP_PLATFORM

/* `p25 lr nnbench`: what a symbol costs on this chip (the fastest and the
   median of single symbols in CPU cycles, so other tasks do not count), and
   whether the vector kernels give what the plain C does. */
static int cmp_u32(const void *a, const void *b)
{
    const uint32_t x = *(const uint32_t *)a, y = *(const uint32_t *)b;
    return x < y ? -1 : x > y;
}

static void cycles(int vector, uint32_t *best, uint32_t *median)
{
    enum { N = 401 };
    static uint32_t t[N];
    uint64_t w = 0x9E3779B97F4A7C15ull;
    for (int i = 0; i < N; i++) {
        w ^= w << 13; w ^= w >> 7; w ^= w << 17;
        float p[4];
        const uint32_t c0 = esp_cpu_get_cycle_count();
        probs(w, p, vector, &s_bench);
        t[i] = esp_cpu_get_cycle_count() - c0;
    }
    qsort(t, N, sizeof(t[0]), cmp_u32);
    *best = t[0];
    *median = t[N / 2];
}

#if NN_PIE
/* median cycles of layer 1 and of layer 2 alone, on the vector unit */
static void layer_cycles(uint32_t *l1, uint32_t *l2)
{
    enum { N = 201 };
    static uint32_t t1[N], t2[N];
    uint64_t w = 0x9E3779B97F4A7C15ull;
    const int16_t *rows[P25_OS4_NN_CHUNKS];
    for (int i = 0; i < N; i++) {
        w ^= w << 13; w ^= w >> 7; w ^= w << 17;
        rows_of(w, rows);
        uint32_t c0 = esp_cpu_get_cycle_count();
        p25_os4_nn_l1_pie(rows, p25_os4_nn_b1, s_bench.a1, P25_OS4_NN_H1 / 32);
        t1[i] = esp_cpu_get_cycle_count() - c0;
        for (int k = 0; k < P25_OS4_NN_H1; k++)
            if (s_bench.a1[k] < 0) s_bench.a1[k] = 0;
        c0 = esp_cpu_get_cycle_count();
        p25_os4_nn_l2_pie(s_bench.a1, &s_w2t[0][0], s_bench.a2, P25_OS4_NN_H1, P25_OS4_NN_H2,
                          P25_OS4_NN_SHIFT2);
        t2[i] = esp_cpu_get_cycle_count() - c0;
    }
    qsort(t1, N, sizeof(t1[0]), cmp_u32);
    qsort(t2, N, sizeof(t2[0]), cmp_u32);
    *l1 = t1[N / 2];
    *l2 = t2[N / 2];
}
#endif

/* A symbol with the cache emptied first (512 KB read through it), as on the
   air when the screen's frames have pushed the tables out: the median of
   single symbols, in cycles. */
static EXT_RAM_BSS_ATTR uint8_t s_evict[512 * 1024];

static uint32_t cold_cycles(void)
{
    enum { N = 101 };
    static uint32_t t[N];
    uint64_t w = 0x9E3779B97F4A7C15ull;
    volatile uint32_t sink = 0;
    for (int i = 0; i < N; i++) {
        w ^= w << 13; w ^= w >> 7; w ^= w << 17;
        for (size_t k = 0; k < sizeof(s_evict); k += 64) sink += s_evict[k];
        float p[4];
        const uint32_t c0 = esp_cpu_get_cycle_count();
        probs(w, p, 1, &s_bench);
        t[i] = esp_cpu_get_cycle_count() - c0;
    }
    (void)sink;
    qsort(t, N, sizeof(t[0]), cmp_u32);
    return t[N / 2];
}

/* Paced as the receive loop runs, 48 symbols every 10 ms with the rest of
   the system running between: the medians and the 90th percentiles of the
   first symbol of each batch (the coldest) and of all of them, in cycles. */
enum { PACED_BATCHES = 300, PACED_PER = 48 };
static EXT_RAM_BSS_ATTR uint32_t s_paced_first[PACED_BATCHES];
static EXT_RAM_BSS_ATTR uint32_t s_paced_all[PACED_BATCHES * PACED_PER];

static void paced_cycles(uint32_t first[2], uint32_t all[2])
{
    uint64_t w = 0x9E3779B97F4A7C15ull;
    for (int b = 0; b < PACED_BATCHES; b++) {
        vTaskDelay(pdMS_TO_TICKS(10));
        for (int i = 0; i < PACED_PER; i++) {
            w ^= w << 13; w ^= w >> 7; w ^= w << 17;
            float p[4];
            const uint32_t c0 = esp_cpu_get_cycle_count();
            probs(w, p, 1, &s_bench);
            const uint32_t c = esp_cpu_get_cycle_count() - c0;
            s_paced_all[b * PACED_PER + i] = c;
            if (!i) s_paced_first[b] = c;
        }
    }
    qsort(s_paced_first, PACED_BATCHES, sizeof(uint32_t), cmp_u32);
    qsort(s_paced_all, PACED_BATCHES * PACED_PER, sizeof(uint32_t), cmp_u32);
    first[0] = s_paced_first[PACED_BATCHES / 2];
    first[1] = s_paced_first[PACED_BATCHES * 9 / 10];
    all[0] = s_paced_all[PACED_BATCHES * PACED_PER / 2];
    all[1] = s_paced_all[PACED_BATCHES * PACED_PER * 9 / 10];
}

#if NN_PIE
/* The vector unit storing to PSRAM: the P25 receive task's stack is there,
   and so would its saved vector registers be. */
static EXT_RAM_BSS_ATTR int16_t s_psram_out[P25_OS4_NN_H1] __attribute__((aligned(16)));

static int psram_store_ok(void)
{
    const int16_t *rows[P25_OS4_NN_CHUNKS];
    rows_of(0x0123456789ABCDEFull, rows);
    p25_os4_nn_l1_pie(rows, p25_os4_nn_b1, s_psram_out, P25_OS4_NN_H1 / 32);
    p25_os4_nn_l1_pie(rows, p25_os4_nn_b1, s_bench.a1, P25_OS4_NN_H1 / 32);
    return memcmp(s_psram_out, s_bench.a1, sizeof(s_psram_out)) == 0;
}
#endif

static void bench_body(void)
{
    uint32_t vb, vm, cb, cm;
    cycles(1, &vb, &vm);
    cycles(0, &cb, &cm);
    const double mhz = (double)CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ;
    int differ = 0;
    float worst = 0.0f;
    uint64_t w = 0x9E3779B97F4A7C15ull;
    for (int i = 0; i < 2000; i++) {
        w ^= w << 13; w ^= w >> 7; w ^= w << 17;
        float a[4], b[4];
        probs(w, a, 1, &s_bench);
        probs(w, b, 0, &s_bench);
        differ += byte_of(a) != byte_of(b);
        for (int c = 0; c < 4; c++) {
            const float e = fabsf(a[c] - b[c]);
            if (e > worst) worst = e;
        }
    }
    printf("P25LR nnbench %s: best %lu cycles %.1f us, median %.1f us a symbol (%.1f%% of a core at 4800 a second); "
           "plain C best %.1f us median %.1f us; vector vs C: %d of 2000 bytes differ, largest probability difference %.5f\n",
           NN_PIE ? "vector" : "plain C", (unsigned long)vb, vb / mhz, vm / mhz, vm / mhz * 4800.0 / 1e4,
           cb / mhz, cm / mhz, differ, (double)worst);
#if NN_PIE
    printf("P25LR nnbench psram store: %s\n", psram_store_ok() ? "ok" : "WRONG");
    uint32_t l1, l2;
    layer_cycles(&l1, &l2);
    printf("P25LR nnbench layers: layer 1 %lu cycles, layer 2 %lu cycles, the rest %ld cycles of the median symbol\n",
           (unsigned long)l1, (unsigned long)l2, (long)vm - (long)l1 - (long)l2);
#endif
    const int was = s_in_ram;
    place(0);
    const uint32_t cold_flash = cold_cycles();
    place(1);
    const uint32_t cold_ram = cold_cycles();
    printf("P25LR nnbench cold (cache emptied before each symbol): weights in flash %.1f us, in PSRAM %.1f us "
           "a symbol (median)\n", cold_flash / mhz, cold_ram / mhz);
    for (int in_ram = 0; in_ram < 2; in_ram++) {
        uint32_t first[2], all[2];
        place(in_ram);
        paced_cycles(first, all);
        printf("P25LR nnbench paced (48 symbols every 10 ms, weights in %s): first of a batch median %.1f us, "
               "90%% %.1f us; all median %.1f us, 90%% %.1f us\n", in_ram ? "PSRAM" : "flash",
               first[0] / mhz, first[1] / mhz, all[0] / mhz, all[1] / mhz);
    }
    place(was == 0 ? 0 : 1);
}

/* Never on the console task: its stack is in the LP SRAM, slow and
   uncached (every first measurement there was five to ten times too slow),
   and FreeRTOS keeps a task's vector registers at the bottom of its stack,
   which the vector unit cannot store to there (a hardware watchdog when the
   next task took the unit over). The timing runs in a task with its stack
   in PSRAM, where the P25 receive task's is, on core 0: the receive task
   owns the unit on core 1. */
static EXT_RAM_BSS_ATTR StackType_t s_bench_stack[8192 / sizeof(StackType_t)];
static StaticTask_t s_bench_tcb;
static SemaphoreHandle_t s_bench_done;
static StaticSemaphore_t s_bench_done_buf;

static void bench_task(void *arg)
{
    (void)arg;
    bench_body();
    xSemaphoreGive(s_bench_done);
    vTaskSuspend(NULL);
}

void p25_os4_nn_bench(void)
{
    if (!s_bench_done) s_bench_done = xSemaphoreCreateBinaryStatic(&s_bench_done_buf);
    TaskHandle_t t = xTaskCreateStaticPinnedToCore(bench_task, "nnbench", sizeof(s_bench_stack) / sizeof(StackType_t),
                                                   NULL, 2, s_bench_stack, &s_bench_tcb, 0);
    if (!t) { printf("P25LR nnbench: no task\n"); return; }
    xSemaphoreTake(s_bench_done, pdMS_TO_TICKS(30000));
    vTaskDelete(t);
}
#endif
