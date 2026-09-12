/* LS_TEST_LINK: -Wl,--wrap=malloc -Wl,--wrap=calloc -Wl,--wrap=free */
/* LS_TEST_SOURCES: ${FW}/components/apps/map_gui/map_tiles.c */
/**/

#include "ls_test.h"
#include "map_gui/map_tiles.h"
#include "driver/jpeg_decode.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* --- Wrapped libc allocator, so any allocation done underneath the module
       (including the mocks defined below, which malloc real memory) is
       accounted for and its release can be observed. -------------------- */

extern void *__real_malloc(size_t);
extern void *__real_calloc(size_t, size_t);
extern void  __real_free(void *);

#define TRACK_MAX 4096
static void  *g_ptrs[TRACK_MAX];
static size_t g_sizes[TRACK_MAX];
static long   g_bytes_live;

static void track_alloc(void *p, size_t n)
{
    if (!p) return;
    for (int i = 0; i < TRACK_MAX; i++) {
        if (!g_ptrs[i]) {
            g_ptrs[i]  = p;
            g_sizes[i] = n;
            g_bytes_live += (long)n;
            return;
        }
    }
}

static void track_free(void *p)
{
    if (!p) return;
    for (int i = 0; i < TRACK_MAX; i++) {
        if (g_ptrs[i] == p) {
            g_bytes_live -= (long)g_sizes[i];
            g_ptrs[i]  = NULL;
            g_sizes[i] = 0;
            return;
        }
    }
}

void *__wrap_malloc(size_t n)
{
    void *p = __real_malloc(n);
    track_alloc(p, n);
    return p;
}

void *__wrap_calloc(size_t a, size_t b)
{
    void *p = __real_calloc(a, b);
    track_alloc(p, a * b);
    return p;
}

void __wrap_free(void *p)
{
    track_free(p);
    __real_free(p);
}

/* --- Mocked JPEG decoder API.  Only enough behaviour to let the test flip
       specific allocations from success to failure and to notice whether the
       engine handle was released. --------------------------------------- */

static int  g_alloc_calls;
static int  g_fail_alloc_at;      /* 1-indexed; 0 = never fail */
static bool g_new_should_fail;
static int  g_engine_live;        /* +1 on new, -1 on del */

static void jpeg_mock_reset(void)
{
    g_alloc_calls     = 0;
    g_fail_alloc_at   = 0;
    g_new_should_fail = false;
    /* Deliberately do NOT reset g_engine_live: an imbalanced count is
       evidence that the module leaked the engine on a prior attempt. */
}

esp_err_t jpeg_new_decoder_engine(const jpeg_decode_engine_cfg_t *cfg,
                                  jpeg_decoder_handle_t *ret_decoder)
{
    (void)cfg;
    if (g_new_should_fail) return ESP_FAIL;
    /* Any non-NULL value works; the module only stores it and passes it
       back to jpeg_del_decoder_engine. */
    *ret_decoder = (jpeg_decoder_handle_t)(uintptr_t)0xBEEFCAFEu;
    g_engine_live++;
    return ESP_OK;
}

esp_err_t jpeg_del_decoder_engine(jpeg_decoder_handle_t decoder_engine)
{
    (void)decoder_engine;
    g_engine_live--;
    return ESP_OK;
}

void *jpeg_alloc_decoder_mem(size_t size,
                             const jpeg_decode_memory_alloc_cfg_t *mem_cfg,
                             size_t *allocated_size)
{
    (void)mem_cfg;
    g_alloc_calls++;
    if (g_fail_alloc_at && g_alloc_calls == g_fail_alloc_at) {
        if (allocated_size) *allocated_size = 0;
        return NULL;
    }
    void *p = malloc(size);
    if (allocated_size) *allocated_size = p ? size : 0;
    return p;
}

esp_err_t jpeg_decoder_process(jpeg_decoder_handle_t decoder_engine,
                               const jpeg_decode_cfg_t *decode_cfg,
                               const uint8_t *bit_stream, uint32_t stream_size,
                               uint8_t *decode_outbuf, uint32_t outbuf_size,
                               uint32_t *out_size)
{
    (void)decoder_engine; (void)decode_cfg;
    (void)bit_stream; (void)stream_size;
    (void)decode_outbuf; (void)outbuf_size;
    if (out_size) *out_size = 0;
    return ESP_FAIL;      /* nothing in these tests ever calls decode */
}

/**/
/* Two injection sites plus the retry.  Before the fix each failed attempt
   leaked the JPEG engine and any buffers acquired earlier in the sequence;
   after it, byte-live returns to its pre-attempt baseline and the retry
   comes up cleanly from that baseline. */
LS_CASE(init_leaks_nothing_and_can_retry_after_partial_failure)
{
    /* Phase 1: failure at the earliest allocation (the DMA input staging
       buffer).  The engine is up but nothing else, so teardown has to
       delete the engine even though no slot allocation happened yet. */
    jpeg_mock_reset();
    LS_EQ_INT(g_engine_live, 0);
    long b1 = g_bytes_live;
    g_fail_alloc_at = 1;                    /* input buffer is alloc #1 */

    LS_CHECK(!map_tiles_init());
    LS_CHECK(!map_tiles_ready());
    LS_EQ_INT(g_engine_live, 0);
    LS_EQ_INT(g_bytes_live, b1);

    /* Phase 2: failure part way through slot allocation.  Alloc #1 is the
       input buffer; allocs #2..#1+MAP_TILE_SLOTS are the tile slots.
       Fail on the fifth slot (call 1 + 5 = 6) so four slot buffers plus
       the input buffer are already live when the injected failure hits. */
    jpeg_mock_reset();
    LS_EQ_INT(g_engine_live, 0);
    long b2 = g_bytes_live;
    g_fail_alloc_at = 1 + 5;

    LS_CHECK(!map_tiles_init());
    LS_CHECK(!map_tiles_ready());
    LS_EQ_INT(g_engine_live, 0);
    LS_EQ_INT(g_bytes_live, b2);

    /* Phase 3: the retry in the task's done-when.  With allocations
       allowed, init has to come up clean and grow the live-bytes count -
       proving the two failed attempts left no ghost buffers behind. */
    jpeg_mock_reset();
    long b3 = g_bytes_live;

    LS_CHECK(map_tiles_init());
    LS_CHECK(map_tiles_ready());
    LS_EQ_INT(g_engine_live, 1);
    long delta = g_bytes_live - b3;
    LS_CHECK_MSG(delta > 0,
                 "successful init acquired no bytes (delta=%ld)", delta);
    /* leaving MAP must return its full cache and JPEG engine, and
     * returning to it must rebuild from the same baseline. */
    map_tiles_deinit();
    LS_CHECK(!map_tiles_ready());
    LS_EQ_INT(g_engine_live, 0);
    LS_EQ_INT(g_bytes_live, b3);
    map_tiles_deinit();
    LS_EQ_INT(g_bytes_live, b3);
    LS_CHECK(map_tiles_init());
    LS_CHECK(map_tiles_ready());
    LS_EQ_INT(g_bytes_live - b3, delta);
    map_tiles_deinit();
    LS_EQ_INT(g_bytes_live, b3);
}
