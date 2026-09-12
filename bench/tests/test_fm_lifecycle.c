/* LS_TEST_SOURCES: ${APP}/fm/fm_lifecycle.c */
#include "ls_test.h"

#include "fm_lifecycle.h"

#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <string.h>

typedef enum {
    HOLD_IQ_READ = 0,
    HOLD_BEFORE_ACARS_GATE,
    HOLD_AFTER_ACARS_GATE,
    HOLD_AFTER_POINTER_CHECK,
    HOLD_IN_ACARS_PROCESS,
} hold_point_t;

typedef struct {
    pthread_mutex_t lock;
    pthread_cond_t changed;
    hold_point_t target;
    hold_point_t reached;
    bool at_target;
    bool released;
    bool iq_cancelled;
    bool task_done;
    atomic_bool decoder_destroyed;
    unsigned cancel_count;
    unsigned not_receiving_count;
    unsigned release_count;
    unsigned destroy_count;
    unsigned audio_reset_count;
    unsigned buffer_free_count;
    unsigned process_count;
    unsigned uaf_count;
    unsigned cleanup_while_live;
} scenario_t;

static void scenario_init(scenario_t *s, hold_point_t target)
{
    memset(s, 0, sizeof(*s));
    pthread_mutex_init(&s->lock, NULL);
    pthread_cond_init(&s->changed, NULL);
    s->target = target;
    atomic_init(&s->decoder_destroyed, false);
}

static void scenario_fini(scenario_t *s)
{
    pthread_cond_destroy(&s->changed);
    pthread_mutex_destroy(&s->lock);
}

static void hold_at(scenario_t *s, hold_point_t point)
{
    pthread_mutex_lock(&s->lock);
    s->reached = point;
    if (s->target == point) {
        s->at_target = true;
        pthread_cond_broadcast(&s->changed);
        while (!s->released) pthread_cond_wait(&s->changed, &s->lock);
    }
    pthread_mutex_unlock(&s->lock);
}

static void wait_for_target(scenario_t *s)
{
    pthread_mutex_lock(&s->lock);
    while (!s->at_target) pthread_cond_wait(&s->changed, &s->lock);
    pthread_mutex_unlock(&s->lock);
}

static void release_target(scenario_t *s)
{
    pthread_mutex_lock(&s->lock);
    s->released = true;
    pthread_cond_broadcast(&s->changed);
    pthread_mutex_unlock(&s->lock);
}

static void *fake_fm_task(void *arg)
{
    scenario_t *s = (scenario_t *)arg;

    pthread_mutex_lock(&s->lock);
    s->reached = HOLD_IQ_READ;
    if (s->target == HOLD_IQ_READ) {
        s->at_target = true;
        pthread_cond_broadcast(&s->changed);
        while (!s->iq_cancelled)
            pthread_cond_wait(&s->changed, &s->lock);
        pthread_mutex_unlock(&s->lock);
        ++s->buffer_free_count;
        fm_lifecycle_task_finished();
        pthread_mutex_lock(&s->lock);
        s->task_done = true;
        pthread_cond_broadcast(&s->changed);
        pthread_mutex_unlock(&s->lock);
        return NULL;
    }
    pthread_mutex_unlock(&s->lock);

    hold_at(s, HOLD_BEFORE_ACARS_GATE);
    if (!fm_lifecycle_acars_enter()) {
        ++s->buffer_free_count;
        fm_lifecycle_task_finished();
        pthread_mutex_lock(&s->lock);
        s->task_done = true;
        pthread_cond_broadcast(&s->changed);
        pthread_mutex_unlock(&s->lock);
        return NULL;
    }

    hold_at(s, HOLD_AFTER_ACARS_GATE);
    void *decoder = atomic_load_explicit(&s->decoder_destroyed,
                                         memory_order_acquire)
                        ? NULL : s;
    hold_at(s, HOLD_AFTER_POINTER_CHECK);
    hold_at(s, HOLD_IN_ACARS_PROCESS);
    if (decoder) {
        ++s->process_count;
        if (atomic_load_explicit(&s->decoder_destroyed,
                                 memory_order_acquire))
            ++s->uaf_count;
    }
    fm_lifecycle_acars_leave();
    ++s->buffer_free_count;
    fm_lifecycle_task_finished();
    pthread_mutex_lock(&s->lock);
    s->task_done = true;
    pthread_cond_broadcast(&s->changed);
    pthread_mutex_unlock(&s->lock);
    return NULL;
}

static void cancel_iq(void *user)
{
    scenario_t *s = (scenario_t *)user;
    pthread_mutex_lock(&s->lock);
    ++s->cancel_count;
    s->iq_cancelled = true;
    pthread_cond_broadcast(&s->changed);
    pthread_mutex_unlock(&s->lock);
}

static void report_not_receiving(void *user)
{
    scenario_t *s = (scenario_t *)user;
    LS_CHECK(fm_lifecycle_active());
    ++s->not_receiving_count;
}

static void release_session(void *user)
{
    scenario_t *s = (scenario_t *)user;
    if (fm_lifecycle_task_live()) ++s->cleanup_while_live;
    ++s->release_count;
}

static void destroy_acars(void *user)
{
    scenario_t *s = (scenario_t *)user;
    ++s->destroy_count;
    atomic_store_explicit(&s->decoder_destroyed, true, memory_order_release);
}

static void reset_audio(void *user)
{
    ++((scenario_t *)user)->audio_reset_count;
}

static void delay_ms(void *user, uint32_t ms)
{
    (void)ms;
    scenario_t *s = (scenario_t *)user;
    if (s->target != HOLD_IQ_READ) return;
    pthread_mutex_lock(&s->lock);
    while (!s->task_done) pthread_cond_wait(&s->changed, &s->lock);
    pthread_mutex_unlock(&s->lock);
}

static fm_lifecycle_hooks_t hooks_for(scenario_t *s)
{
    const fm_lifecycle_hooks_t hooks = {
        .report_not_receiving = report_not_receiving,
        .cancel_iq = cancel_iq,
        .release_session = release_session,
        .destroy_acars = destroy_acars,
        .reset_audio = reset_audio,
        .delay = delay_ms,
        .user = s,
    };
    return hooks;
}

static void run_hold_case(hold_point_t point)
{
    fm_lifecycle_test_reset();
    scenario_t s;
    scenario_init(&s, point);
    fm_lifecycle_hooks_t hooks = hooks_for(&s);

    LS_CHECK(fm_lifecycle_start());
    pthread_t task;
    LS_EQ_INT(pthread_create(&task, NULL, fake_fm_task, &s), 0);
    wait_for_target(&s);

    bool stopped = fm_lifecycle_stop(&hooks, 3, 1);
    if (point == HOLD_IQ_READ) {
        LS_CHECK(stopped);
    } else {
        LS_CHECK(!stopped);
        LS_EQ_UINT(s.release_count, 0);
        LS_EQ_UINT(s.destroy_count, 0);
        LS_EQ_UINT(s.audio_reset_count, 0);
        release_target(&s);
    }

    LS_EQ_INT(pthread_join(task, NULL), 0);
    LS_CHECK(fm_lifecycle_stop(&hooks, 0, 0));
    LS_CHECK(fm_lifecycle_stop(&hooks, 0, 0));

    LS_EQ_UINT(s.cancel_count, 1);
    LS_EQ_UINT(s.release_count, 1);
    LS_EQ_UINT(s.destroy_count, 1);
    LS_EQ_UINT(s.audio_reset_count, 1);
    LS_EQ_UINT(s.uaf_count, 0);
    LS_EQ_UINT(s.cleanup_while_live, 0);
    if (point == HOLD_IQ_READ) {
        LS_EQ_UINT(s.buffer_free_count, 1);
        LS_EQ_UINT(s.process_count, 0);
    } else {
        LS_EQ_UINT(s.buffer_free_count, 1);
        LS_EQ_UINT(s.process_count,
                   point == HOLD_BEFORE_ACARS_GATE ? 0 : 1);
    }
    scenario_fini(&s);
}

LS_CASE(stalled_iq_and_every_acars_pointer_boundary_are_safe)
{
    run_hold_case(HOLD_IQ_READ);
    run_hold_case(HOLD_BEFORE_ACARS_GATE);
    run_hold_case(HOLD_AFTER_ACARS_GATE);
    run_hold_case(HOLD_AFTER_POINTER_CHECK);
    run_hold_case(HOLD_IN_ACARS_PROCESS);
}

LS_CASE(task_start_and_iq_buffer_allocation_failures_are_retryable)
{
    fm_lifecycle_test_reset();
    scenario_t s;
    scenario_init(&s, HOLD_IQ_READ);
    fm_lifecycle_hooks_t hooks = hooks_for(&s);

    /* xTaskCreate failure: the reserved slot is returned without a worker. */
    LS_CHECK(fm_lifecycle_start());
    LS_CHECK(fm_lifecycle_start_failed(&hooks));
    LS_CHECK(fm_lifecycle_stop(&hooks, 0, 0));
    LS_EQ_UINT(s.not_receiving_count, 1);
    LS_EQ_UINT(s.destroy_count, 1);
    LS_EQ_UINT(s.release_count, 1);
    LS_EQ_UINT(s.audio_reset_count, 1);

    /* IQ-buffer failure happens in the worker after task creation and uses
       the same failure publication.  A clean stop permits re-entry. */
    LS_CHECK(fm_lifecycle_start());
    LS_CHECK(fm_lifecycle_start_failed(&hooks));
    LS_EQ_UINT(s.not_receiving_count, 2);
    LS_EQ_UINT(s.destroy_count, 2);
    LS_EQ_UINT(s.release_count, 2);
    LS_EQ_UINT(s.audio_reset_count, 2);

    LS_CHECK(fm_lifecycle_start());
    LS_CHECK(fm_lifecycle_start_failed(&hooks));
    LS_EQ_UINT(s.not_receiving_count, 3);
    LS_EQ_UINT(s.destroy_count, 3);
    scenario_fini(&s);
}
