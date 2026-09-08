#ifndef FM_LIFECYCLE_H
#define FM_LIFECYCLE_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*fm_lifecycle_hook_t)(void *user);
typedef void (*fm_lifecycle_delay_t)(void *user, uint32_t delay_ms);

typedef struct {
    fm_lifecycle_hook_t report_not_receiving;
    fm_lifecycle_hook_t cancel_iq;
    fm_lifecycle_hook_t release_session;
    fm_lifecycle_hook_t destroy_acars;
    fm_lifecycle_hook_t reset_audio;
    fm_lifecycle_delay_t delay;
    void *user;
} fm_lifecycle_hooks_t;

/* Start reserves the worker slot before task creation.  This closes the
 * create-to-task-entry window in which an exit could previously see no live
 * task and tear resources down under the newly scheduled worker. */
bool fm_lifecycle_start(void);
void fm_lifecycle_task_failed(void);
bool fm_lifecycle_start_failed(const fm_lifecycle_hooks_t *hooks);
void fm_lifecycle_task_finished(void);
bool fm_lifecycle_active(void);
bool fm_lifecycle_task_live(void);

/* ACARS processing is admitted separately from the worker loop.  Once stop
 * begins, a worker that has not crossed this gate cannot enter the decoder.
 * Every successful enter must have one matching leave. */
bool fm_lifecycle_acars_enter(void);
void fm_lifecycle_acars_leave(void);

/* Returns true only after the worker and every admitted ACARS call have
 * drained and cleanup has run.  A false result retains all owned resources so
 * the caller can safely retry without allowing the next radio owner to start. */
bool fm_lifecycle_stop(const fm_lifecycle_hooks_t *hooks,
                       unsigned wait_iterations, uint32_t wait_ms);

/* Host lifecycle cases use this between scenarios.  Firmware never needs it. */
void fm_lifecycle_test_reset(void);

#ifdef __cplusplus
}
#endif

#endif
