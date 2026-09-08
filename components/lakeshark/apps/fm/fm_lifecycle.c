#include "fm_lifecycle.h"

#include <stdatomic.h>
#include <stddef.h>

/* LS-706: FM used s_active/s_running as an informal cross-core join.  The
 * worker set s_running only after allocating its IQ buffer, so exit could run
 * in the task-start window, conclude that nothing was live, and free ACARS;
 * a delayed worker could also cross `if (s_acars)` while exit timed out and
 * destroyed that same context.  Reserve the worker before task creation,
 * close ACARS admission as the first stop action, and publish worker finish
 * with release/acquire ordering before cleanup. */
static atomic_bool s_active       = ATOMIC_VAR_INIT(false);
static atomic_bool s_task_live    = ATOMIC_VAR_INIT(false);
static atomic_bool s_stopping     = ATOMIC_VAR_INIT(false);
static atomic_bool s_cleanup_done = ATOMIC_VAR_INIT(true);
static atomic_uint s_acars_users  = ATOMIC_VAR_INIT(0);

bool fm_lifecycle_start(void)
{
    if (!atomic_load_explicit(&s_cleanup_done, memory_order_acquire) ||
        atomic_load_explicit(&s_task_live, memory_order_acquire) ||
        atomic_load_explicit(&s_active, memory_order_acquire))
        return false;

    atomic_store_explicit(&s_stopping, false, memory_order_release);
    atomic_store_explicit(&s_cleanup_done, false, memory_order_release);
    atomic_store_explicit(&s_task_live, true, memory_order_release);
    atomic_store_explicit(&s_active, true, memory_order_release);
    return true;
}

void fm_lifecycle_task_failed(void)
{
    atomic_store_explicit(&s_active, false, memory_order_release);
    atomic_store_explicit(&s_task_live, false, memory_order_release);
}

bool fm_lifecycle_start_failed(const fm_lifecycle_hooks_t *hooks)
{
    /* LS-725: publishing the selected submode is not evidence that its worker
     * exists.  Report the receiver failure before releasing the reserved task
     * slot and decoder ownership, so every concurrent snapshot says NO RX. */
    if (hooks && hooks->report_not_receiving)
        hooks->report_not_receiving(hooks->user);
    fm_lifecycle_task_failed();
    return fm_lifecycle_stop(hooks, 0, 0);
}

void fm_lifecycle_task_finished(void)
{
    atomic_store_explicit(&s_task_live, false, memory_order_release);
}

bool fm_lifecycle_active(void)
{
    return atomic_load_explicit(&s_active, memory_order_acquire);
}

bool fm_lifecycle_task_live(void)
{
    return atomic_load_explicit(&s_task_live, memory_order_acquire);
}

bool fm_lifecycle_acars_enter(void)
{
    if (atomic_load_explicit(&s_stopping, memory_order_acquire)) return false;

    atomic_fetch_add_explicit(&s_acars_users, 1, memory_order_acq_rel);
    if (atomic_load_explicit(&s_stopping, memory_order_acquire)) {
        atomic_fetch_sub_explicit(&s_acars_users, 1, memory_order_release);
        return false;
    }
    return true;
}

void fm_lifecycle_acars_leave(void)
{
    atomic_fetch_sub_explicit(&s_acars_users, 1, memory_order_release);
}

static bool drained(void)
{
    return !atomic_load_explicit(&s_task_live, memory_order_acquire) &&
           atomic_load_explicit(&s_acars_users, memory_order_acquire) == 0;
}

bool fm_lifecycle_stop(const fm_lifecycle_hooks_t *hooks,
                       unsigned wait_iterations, uint32_t wait_ms)
{
    atomic_store_explicit(&s_active, false, memory_order_release);
    bool already_stopping = atomic_exchange_explicit(
        &s_stopping, true, memory_order_acq_rel);

    if (!already_stopping && hooks && hooks->cancel_iq)
        hooks->cancel_iq(hooks->user);

    for (unsigned i = 0; i < wait_iterations && !drained(); ++i) {
        if (hooks && hooks->delay) hooks->delay(hooks->user, wait_ms);
    }
    if (!drained()) return false;

    bool was_clean = atomic_exchange_explicit(
        &s_cleanup_done, true, memory_order_acq_rel);
    if (!was_clean && hooks) {
        if (hooks->release_session) hooks->release_session(hooks->user);
        if (hooks->destroy_acars) hooks->destroy_acars(hooks->user);
        if (hooks->reset_audio) hooks->reset_audio(hooks->user);
    }
    return true;
}

void fm_lifecycle_test_reset(void)
{
    atomic_store_explicit(&s_active, false, memory_order_relaxed);
    atomic_store_explicit(&s_task_live, false, memory_order_relaxed);
    atomic_store_explicit(&s_stopping, false, memory_order_relaxed);
    atomic_store_explicit(&s_cleanup_done, true, memory_order_relaxed);
    atomic_store_explicit(&s_acars_users, 0, memory_order_relaxed);
}
