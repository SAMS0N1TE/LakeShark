#include "ls_test.h"
#include "p25_iq_capture.h"
static bool stop_during_copy;
static void *capture_copy(void *dst, const void *src, size_t n)
{
    if (stop_during_copy && n == P25_IQ_CAPTURE_BLOCK_BYTES) {
        stop_during_copy = false;
        p25_iq_capture_receiver(false);
    }
    return memcpy(dst, src, n);
}
/* Exercise the production ownership guard as well as its public API. */
#define memcpy capture_copy
#include "../../components/lakeshark/apps/p25/p25_iq_capture.c"
#undef memcpy

static uint8_t block[P25_IQ_CAPTURE_BLOCK_BYTES];
static p25_iq_capture_meta_t meta;

static void reset_capture(void)
{
    LS_EQ_INT(p25_iq_capture_free(), P25_IQ_CAPTURE_OK);
    p25_iq_capture_receiver(true);
    ls_shim_heap_reset();
    meta = (p25_iq_capture_meta_t){154785000, 240000, 12000, 7, 252, 0, -9000, 100};
    for (unsigned i = 0; i < sizeof(block); ++i) block[i] = (uint8_t)(i * 17);
}

static p25_iq_capture_status_t status_at(uint32_t now)
{
    p25_iq_capture_status_t s;
    LS_CHECK(p25_iq_capture_status(&s, now));
    return s;
}

LS_CASE(single_psram_allocation_and_byte_exact_bounded_export)
{
    reset_capture();
    LS_EQ_INT(p25_iq_capture_start(2, 100), P25_IQ_CAPTURE_OK);
    LS_EQ_UINT(ls_shim_heap_call_count(), 1);
    LS_EQ_UINT(ls_shim_heap_call_caps(1), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    LS_EQ_UINT(ls_shim_heap_call_size(1), sizeof(capture_t) + 2*sizeof(block));
    p25_iq_capture_feed(block, sizeof(block), &meta);
    LS_EQ_INT(status_at(100).phase, P25_IQ_CAPTURE_ACTIVE);
    meta.now_ms++;
    p25_iq_capture_feed(block, sizeof(block), &meta);
    LS_EQ_INT(status_at(101).phase, P25_IQ_CAPTURE_DONE);
    uint8_t out[256]; size_t n;
    for (uint32_t offset = 0; offset < 2*sizeof(block); offset += sizeof(out)) {
        LS_EQ_INT(p25_iq_capture_read(offset, out, sizeof(out), &n, 101), P25_IQ_CAPTURE_OK);
        LS_EQ_UINT(n, sizeof(out));
        LS_CHECK(!memcmp(out, block + offset % sizeof(block), sizeof(out)));
    }
    LS_EQ_UINT(ls_shim_heap_call_count(), 1);
    LS_EQ_INT(p25_iq_capture_read(2*sizeof(block), out, 1, &n, 101), P25_IQ_CAPTURE_OK);
    LS_EQ_UINT(n, 0);
    LS_EQ_INT(p25_iq_capture_read(0, out, 257, &n, 101), P25_IQ_CAPTURE_ARGUMENT);
    LS_EQ_INT(p25_iq_capture_read(2*sizeof(block)+1, out, 1, &n, 101), P25_IQ_CAPTURE_ARGUMENT);
    LS_EQ_INT(p25_iq_capture_free(), P25_IQ_CAPTURE_OK);
    LS_EQ_UINT(ls_shim_heap_outstanding(), 0);
}

LS_CASE(allocation_failure_and_invalid_bounds_never_fall_back_to_internal)
{
    reset_capture();
    LS_EQ_INT(p25_iq_capture_start(0, 100), P25_IQ_CAPTURE_ARGUMENT);
    LS_EQ_INT(p25_iq_capture_start(257, 100), P25_IQ_CAPTURE_ARGUMENT);
    ls_shim_heap_fail_from(1);
    LS_EQ_INT(p25_iq_capture_start(32, 100), P25_IQ_CAPTURE_NO_MEMORY);
    LS_EQ_UINT(ls_shim_heap_call_count(), 1);
    LS_EQ_INT(status_at(100).phase, P25_IQ_CAPTURE_FAILED);
    p25_iq_capture_receiver(false);
    LS_EQ_INT(p25_iq_capture_start(1, 100), P25_IQ_CAPTURE_NOT_RUNNING);
    LS_EQ_UINT(ls_shim_heap_outstanding(), 0);
}

LS_CASE(cancel_free_and_writer_ownership_do_not_wait_or_free_in_use)
{
    reset_capture();
    LS_EQ_INT(p25_iq_capture_start(32, 100), P25_IQ_CAPTURE_OK);
    p25_iq_capture_feed(block, sizeof(block), &meta);
    LS_CHECK(take());
    LS_EQ_INT(p25_iq_capture_cancel(), P25_IQ_CAPTURE_BUSY);
    LS_EQ_INT(p25_iq_capture_free(), P25_IQ_CAPTURE_BUSY);
    LS_EQ_UINT(ls_shim_heap_outstanding(), 1);
    release();
    LS_EQ_INT(p25_iq_capture_cancel(), P25_IQ_CAPTURE_OK);
    LS_EQ_INT(status_at(100).phase, P25_IQ_CAPTURE_ABORTED);
    LS_EQ_UINT(status_at(100).captured_bytes, sizeof(block));
    p25_iq_capture_receiver(false);
    LS_EQ_INT(p25_iq_capture_free(), P25_IQ_CAPTURE_OK);
    LS_EQ_UINT(ls_shim_heap_outstanding(), 0);
    p25_iq_capture_feed(block, sizeof(block), &meta);
    LS_EQ_INT(status_at(100).phase, P25_IQ_CAPTURE_EMPTY);
}

LS_CASE(session_stop_while_console_owns_guard_is_reconciled_without_rx)
{
    reset_capture();
    LS_EQ_INT(p25_iq_capture_start(32, 100), P25_IQ_CAPTURE_OK);
    LS_CHECK(take());
    p25_iq_capture_receiver(false);
    release();
    LS_EQ_INT(status_at(100).reason, P25_IQ_CAPTURE_SESSION_CHANGED);
    p25_iq_capture_receiver(true);
    LS_EQ_INT(status_at(100).phase, P25_IQ_CAPTURE_ABORTED);
}

LS_CASE(stop_crossing_final_copy_is_aborted_not_done_and_can_free_after_park)
{
    reset_capture();
    LS_EQ_INT(p25_iq_capture_start(1, 100), P25_IQ_CAPTURE_OK);
    stop_during_copy = true;
    p25_iq_capture_feed(block, sizeof(block), &meta);
    LS_CHECK(!stop_during_copy);
    LS_EQ_INT(status_at(100).phase, P25_IQ_CAPTURE_ABORTED);
    LS_EQ_INT(status_at(100).reason, P25_IQ_CAPTURE_SESSION_CHANGED);
    LS_EQ_INT(p25_iq_capture_free(), P25_IQ_CAPTURE_OK);
    LS_EQ_UINT(ls_shim_heap_outstanding(), 0);
}

LS_CASE(maximum_capture_is_bounded_and_can_cancel_before_first_block)
{
    reset_capture();
    LS_EQ_INT(p25_iq_capture_start(256, 100), P25_IQ_CAPTURE_OK);
    LS_EQ_UINT(ls_shim_heap_call_size(1), sizeof(capture_t) + 4*1024*1024);
    LS_EQ_INT(p25_iq_capture_cancel(), P25_IQ_CAPTURE_OK);
    LS_EQ_INT(status_at(100).reason, P25_IQ_CAPTURE_CANCELLED);
    LS_EQ_UINT(status_at(100).captured_bytes, 0);
    LS_EQ_INT(p25_iq_capture_free(), P25_IQ_CAPTURE_OK);
}

LS_CASE(retune_config_timeout_and_missing_block_never_claim_complete)
{
    for (int kind = 0; kind < 5; ++kind) {
        reset_capture();
        LS_EQ_INT(p25_iq_capture_start(2, 100), P25_IQ_CAPTURE_OK);
        p25_iq_capture_feed(block, sizeof(block), &meta);
        if (kind == 0) meta.tune_generation++;
        if (kind == 1) meta.demod_mode++;
        if (kind == 2) meta.now_ms += P25_IQ_CAPTURE_TIMEOUT_MS;
        if (kind == 3) { LS_CHECK(take()); p25_iq_capture_feed(block, sizeof(block), &meta); release(); }
        if (kind == 4) { LS_CHECK(take()); p25_iq_capture_interrupt(P25_IQ_CAPTURE_TUNE_CHANGED); release(); }
        p25_iq_capture_feed(block, sizeof(block), &meta);
        p25_iq_capture_status_t s = status_at(meta.now_ms);
        LS_EQ_UINT(s.captured_bytes, sizeof(block));
        LS_CHECK(s.phase == P25_IQ_CAPTURE_ABORTED || s.phase == P25_IQ_CAPTURE_FAILED);
        const int reasons[] = {P25_IQ_CAPTURE_TUNE_CHANGED,P25_IQ_CAPTURE_CONFIG_CHANGED,
            P25_IQ_CAPTURE_TIMEOUT,P25_IQ_CAPTURE_GAP,P25_IQ_CAPTURE_TUNE_CHANGED};
        LS_EQ_INT(s.reason, reasons[kind]);
    }
    reset_capture();
    LS_EQ_INT(p25_iq_capture_start(1, UINT32_MAX-10), P25_IQ_CAPTURE_OK);
    LS_EQ_INT(status_at(P25_IQ_CAPTURE_TIMEOUT_MS-11).reason, P25_IQ_CAPTURE_TIMEOUT);
}

LS_CASE(command_parser_rejects_overflow_extra_arguments_and_preserves_capture)
{
    reset_capture();
    char *start[] = {"start"};
    LS_EQ_INT(p25_iq_capture_command(1, start, 100), 0);
    LS_EQ_UINT(status_at(100).requested_bytes, 32*sizeof(block));
    const char *bad[] = {"-1", "0", "257", "4294967296", "1x", ""};
    for (unsigned i = 0; i < sizeof(bad)/sizeof(bad[0]); ++i) {
        char *cmd[] = {"start", (char *)bad[i]};
        LS_EQ_INT(p25_iq_capture_command(2, cmd, 100), 1);
    }
    char *extra[] = {"free", "extra"};
    LS_EQ_INT(p25_iq_capture_command(2, extra, 100), 1);
    LS_EQ_INT(status_at(100).phase, P25_IQ_CAPTURE_ARMED);
    LS_EQ_INT(p25_iq_capture_free(), P25_IQ_CAPTURE_OK);
}
