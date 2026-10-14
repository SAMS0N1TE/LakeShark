/* Standalone host test; no firmware, panel or hardware dependencies. */
#include "ls_ui_perf.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static ls_ui_perf_t state;
static ls_ui_perf_snapshot_t snap;
static int cmp(const void *a, const void *b)
{
    return (int)*(const uint16_t *)a - (int)*(const uint16_t *)b;
}

int main(void)
{
    ls_ui_perf_snapshot(&state, 0, &snap);
    assert(!snap.fps_x10 && !ls_ui_perf_percentile(&snap.frame, 95));
    uint16_t reference[256];
    for (unsigned i = 0; i < 700; i++) {
        unsigned us = (i * 7919u) % 9000000u;
        uint64_t end = (uint64_t)(i + 1) * 10000000;
        ls_ui_perf_frame(&state, end - us, end);
        reference[i % 256] = us >= 6553500 ? UINT16_MAX : (us + 50) / 100;
    }
    ls_ui_perf_snapshot(&state, state.last_frame_us, &snap);
    assert(snap.frame.count == 256);
    qsort(reference, 256, sizeof(*reference), cmp);
    for (unsigned p = 1; p <= 100; p++)
        assert(ls_ui_perf_percentile(&snap.frame, p) == reference[(256 * p + 99) / 100 - 1]);
    assert(ls_ui_perf_percentile(&snap.frame, 0) == reference[2]); /* clamps to p1 */
    assert(ls_ui_perf_percentile(&snap.frame, 101) == reference[255]);

    memset(&state, 0, sizeof(state));
    ls_ui_perf_frame(&state, 1, 50001);
    ls_ui_perf_frame(&state, 1, 50002);
    assert(state.late_frames == 1);
    ls_ui_perf_input(&state, 50002);
    ls_ui_perf_reset(&state);
    ls_ui_perf_snapshot(&state, 50003, &snap);
    assert(snap.late_frames == 1 && !snap.frame.count && !snap.input.count);
    assert(!snap.fps_x10 && !state.pending_count);
    ls_ui_perf_frame(&state, 100, 99);
    assert(!state.frame.count);
    ls_ui_perf_frame(&state, 0, 49);
    ls_ui_perf_snapshot(&state, 49, &snap);
    assert(snap.frame.count == 1 && !ls_ui_perf_percentile(&snap.frame, 95));
    ls_ui_perf_frame(&state, 0, 50);
    assert(state.frame.value[1] == 1);
    ls_ui_perf_frame(&state, 0, 6553500);
    assert(state.frame.value[2] == UINT16_MAX);
    ls_ui_perf_reset(&state);

    /* Multiple events per frame, retained across an unpresented draw. */
    ls_ui_perf_input(&state, 1000);
    ls_ui_perf_input(&state, 2000);
    ls_ui_perf_frame(&state, 3000, 10000);
    ls_ui_perf_snapshot(&state, 10000, &snap);
    assert(snap.input.count == 2);
    assert(ls_ui_perf_percentile(&snap.input, 50) == 80);
    assert(ls_ui_perf_percentile(&snap.input, 95) == 90);
    ls_ui_perf_frame(&state, 20000, 21000);
    assert(state.input.count == 2); /* No synthetic idle input samples. */
    ls_ui_perf_reset(&state);
    for (unsigned i = 0; i < 300; i++) ls_ui_perf_input(&state, i * 100);
    ls_ui_perf_frame(&state, 30000, 40000);
    ls_ui_perf_snapshot(&state, 40000, &snap);
    assert(snap.input.count == 256);
    assert(ls_ui_perf_percentile(&snap.input, 100) == 356);
    ls_ui_perf_input(&state, 1);
    ls_ui_perf_frame(&state, 9000000, 10000000);
    assert(state.input.value[0] == UINT16_MAX);

    /* Exact five-second count (1 ms resolution), independently of 256 timings.
     * Exercise timestamp wrap and a very long stopped session as well. */
    ls_ui_perf_reset(&state);
    uint64_t base = ((uint64_t)UINT32_MAX - 3000) * 1000;
    for (unsigned i = 1; i <= 2200; i++)
        ls_ui_perf_frame(&state, base + i * 5000 - 100, base + i * 5000);
    ls_ui_perf_snapshot(&state, base + 11000000, &snap);
    assert(snap.fps_x10 == 2000);
    ls_ui_perf_snapshot(&state, base + 13500000, &snap);
    assert(snap.fps_x10 == 1000);
    ls_ui_perf_snapshot(&state, base + 16000000, &snap);
    assert(!snap.fps_x10);
    uint64_t later = base + 11000000 + ((uint64_t)UINT32_MAX + 1) * 1000;
    ls_ui_perf_snapshot(&state, later, &snap);
    assert(!snap.fps_x10);
    ls_ui_perf_frame(&state, later, later + 100);
    ls_ui_perf_snapshot(&state, later + 100, &snap);
    assert(snap.fps_x10 == 2);

    /* Measured host recording cost, not a claim about the P4's PSRAM. */
    const unsigned n = 5000000;
    ls_ui_perf_reset(&state);
    clock_t start = clock();
    for (unsigned i = 1; i <= n; i++)
        ls_ui_perf_frame(&state, (uint64_t)i * 40000, (uint64_t)i * 40000 + 1000);
    double ns = (double)(clock() - start) * 1e9 / CLOCKS_PER_SEC / n;
    start = clock();
    for (unsigned i = 1; i <= n; i++) {
        ls_ui_perf_input(&state, (uint64_t)i * 40000);
        ls_ui_perf_frame(&state, (uint64_t)i * 40000, (uint64_t)i * 40000 + 1000);
    }
    double input_ns = (double)(clock() - start) * 1e9 / CLOCKS_PER_SEC / n;
    printf("PASS ui_perf: ring, all percentiles, saturation, reset, input, FPS/wrap\n");
    printf("record_ns=%.1f record_with_input_ns=%.1f state_bytes=%zu snapshot_bytes=%zu\n",
           ns, input_ns, sizeof(state), sizeof(snap));
    return 0;
}
