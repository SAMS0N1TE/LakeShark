/* LS_TEST_SOURCES: ${APP}/fm/fm_rx_worker.c ${FW}/components/lakeshark/core/radio_decode_worker.c */
/* FM and P25 share one fixed cache-safe stack.  Reuse must be refused
 * before kernel-observed suspension, survive create failure, and preserve the
 * original core placement in both transition directions. */

#include "ls_test.h"

#include "fm_rx_worker.h"
#include "radio_decode_worker.h"
#include "freertos/task.h"

static unsigned s_fm_runs;
static unsigned s_p25_runs;

static void fm_worker(void *arg) { (void)arg; ++s_fm_runs; }
static void p25_worker(void *arg) { (void)arg; ++s_p25_runs; }
static void adsb_worker(void *arg) { (void)arg; }

LS_CASE(adsb_shares_decoder_storage_and_cannot_overlap_fm)
{
    ls_shim_task_reset();
    ls_shim_task_execute_on_create(pdTRUE);
    LS_CHECK(ls_radio_decode_worker_start(adsb_worker, "adsb_rx", 5, 1));
    StackType_t *reserved = ls_shim_task_last_static_stack();
    LS_CHECK(reserved != NULL);
    LS_EQ_UINT(ls_shim_task_last_stack_depth(), 16384u);
    LS_EQ_UINT(ls_shim_task_last_priority(), 5);
    LS_EQ_INT(ls_shim_task_last_core_id(), 1);
    ls_shim_task_set_state(eRunning);
    LS_CHECK(!fm_rx_worker_start(fm_worker));
    LS_CHECK(!ls_radio_decode_worker_release(0, 0));
    ls_shim_task_set_state(eSuspended);
    LS_CHECK(ls_radio_decode_worker_release(0, 0));
    LS_CHECK(fm_rx_worker_start(fm_worker));
    LS_CHECK(ls_shim_task_last_static_stack() == reserved);
    LS_CHECK(fm_rx_worker_release(0, 0));
    s_fm_runs = 0;
}

LS_CASE(shared_stack_covers_fm_p25_transitions_and_create_failure)
{
    ls_shim_task_reset();
    ls_shim_task_fail_create(pdTRUE);

    /* P25 decoder task creation failure leaves the shared owner retryable. */
    LS_CHECK(!ls_radio_decode_worker_start(p25_worker, "dsd_decode", 10, 0));
    StackType_t *reserved = ls_shim_task_last_static_stack();
    LS_CHECK(reserved != NULL);
    LS_EQ_UINT(ls_shim_task_last_stack_depth(), LS_RADIO_DECODE_STACK_BYTES);
    LS_EQ_UINT(LS_RADIO_DECODE_STACK_BYTES, 16384u);
    LS_EQ_UINT(ls_shim_task_static_create_count(), 1);
    LS_EQ_UINT(ls_shim_task_delete_count(), 0);
    LS_EQ_STR(ls_shim_task_last_name(), "dsd_decode");

    ls_shim_task_fail_create(pdFALSE);
    ls_shim_task_execute_on_create(pdTRUE);

    /* P25 -> FM: P25 stays on core 0 until fully suspended and deleted. */
    LS_CHECK(ls_radio_decode_worker_start(p25_worker, "dsd_decode", 10, 0));
    LS_CHECK(ls_shim_task_last_static_stack() == reserved);
    LS_EQ_UINT(s_p25_runs, 1);
    LS_EQ_INT(ls_shim_task_last_core_id(), 0);
    LS_EQ_UINT(ls_shim_task_last_priority(), 10);
    LS_EQ_UINT(ls_shim_task_static_create_count(), 2);

    /* A software owner flag is not enough: until the kernel says Suspended,
       release and reuse must both fail without deleting the old TCB. */
    ls_shim_task_set_state(eRunning);
    LS_CHECK(!ls_radio_decode_worker_release(0, 0));
    LS_CHECK(!fm_rx_worker_start(fm_worker));
    LS_EQ_UINT(ls_shim_task_static_create_count(), 2);
    LS_EQ_UINT(ls_shim_task_delete_count(), 0);
    ls_shim_task_set_state(eSuspended);
    LS_CHECK(ls_radio_decode_worker_release(0, 0));
    LS_EQ_UINT(ls_shim_task_delete_count(), 1);

    LS_CHECK(fm_rx_worker_start(fm_worker));
    LS_CHECK(ls_shim_task_last_static_stack() == reserved);
    LS_EQ_UINT(s_fm_runs, 1);
    LS_EQ_INT(ls_shim_task_last_core_id(), 1);
    LS_EQ_UINT(ls_shim_task_last_priority(), 6);
    LS_CHECK(fm_rx_worker_release(0, 0));

    /* FM -> P25 uses the same storage only after the FM wrapper quiesces. */
    LS_CHECK(fm_rx_worker_start(fm_worker));
    LS_EQ_UINT(s_fm_runs, 2);
    LS_CHECK(fm_rx_worker_release(0, 0));
    LS_CHECK(ls_radio_decode_worker_start(p25_worker, "dsd_decode", 10, 0));
    LS_CHECK(ls_shim_task_last_static_stack() == reserved);
    LS_EQ_UINT(s_p25_runs, 2);
    LS_CHECK(ls_radio_decode_worker_release(0, 0));

    LS_EQ_UINT(ls_shim_task_static_create_count(), 5);
    LS_EQ_UINT(ls_shim_task_delete_count(), 4);
    LS_EQ_UINT(ls_shim_task_delete_with_caps_count(), 0);
}
