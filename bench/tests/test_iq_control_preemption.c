#include "ls_test.h"
#include "iq_app_control.h"
#include <pthread.h>
#include <time.h>
#include <errno.h>

static pthread_mutex_t critical = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t scheduler = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t changed = PTHREAD_COND_INITIALIZER;
static _Thread_local int role;
static int low_entered, high_attempted, entries, release_violations;
static ls_iq_control_t control;
static ls_iq_control_request_t taken;
static bool high_took_request;

/* Backend operations are outside this scheduler regression. */
ls_radio_err_t ls_radio_iq_configure(ls_radio_session_t *s,
    const ls_radio_iq_config_t *r, ls_radio_iq_config_t *a)
{ (void)s; (void)r; (void)a; return LS_RADIO_ERR_UNAVAILABLE; }
ls_radio_err_t ls_radio_iq_set_gain(ls_radio_session_t *s,
    ls_radio_gain_mode_t m, int g, int *a)
{ (void)s; (void)m; (void)g; (void)a; return LS_RADIO_ERR_UNAVAILABLE; }
ls_radio_err_t ls_radio_iq_retune(ls_radio_session_t *s,
    uint64_t f, bool fast, uint64_t *a)
{ (void)s; (void)f; (void)fast; (void)a; return LS_RADIO_ERR_UNAVAILABLE; }

void iq_test_critical_enter(int *mux)
{
    (void)mux;
    if (role == 2) {
        pthread_mutex_lock(&scheduler);
        high_attempted = 1;
        pthread_cond_broadcast(&changed);
        pthread_mutex_unlock(&scheduler);
    }
    pthread_mutex_lock(&critical);
    ++entries;
    if (role == 1) {
        /* Model a higher-priority receiver becoming runnable while the
         * lower-priority telemetry/control task owns its critical section. */
        pthread_mutex_lock(&scheduler);
        low_entered = 1;
        pthread_cond_broadcast(&changed);
        while (!high_attempted) pthread_cond_wait(&changed, &scheduler);
        pthread_mutex_unlock(&scheduler);
    }
}
void iq_test_critical_exit(int *mux)
{
    (void)mux;
    if (control.writer_lock) ++release_violations;
    pthread_mutex_unlock(&critical);
}
static void *low(void *arg)
{ (void)arg; role = 1; ls_iq_control_request_tune(&control, 100, false); return NULL; }
static void *high(void *arg)
{ (void)arg; role = 2; high_took_request = ls_iq_control_take(&control, &taken); return NULL; }

LS_CASE(receiver_waits_until_lower_priority_owner_releases_control)
{
    ls_iq_control_reset(&control);
    pthread_t low_task, high_task;
    LS_EQ_INT(pthread_create(&low_task, NULL, low, NULL), 0);
    struct timespec deadline;
    clock_gettime(CLOCK_REALTIME, &deadline); ++deadline.tv_sec;
    pthread_mutex_lock(&scheduler);
    int rc = 0;
    while (!low_entered && rc == 0)
        rc = pthread_cond_timedwait(&changed, &scheduler, &deadline);
    pthread_mutex_unlock(&scheduler);
    LS_CHECK_MSG(low_entered, "control owner did not enter a scheduler critical section");
    if (!low_entered) { pthread_join(low_task, NULL); return; }
    LS_EQ_INT(pthread_create(&high_task, NULL, high, NULL), 0);
    pthread_join(low_task, NULL);
    pthread_join(high_task, NULL);
    ls_iq_control_request_t request;
    LS_CHECK(high_took_request);
    LS_EQ_INT(taken.flags, LS_IQ_CONTROL_TUNE);
    LS_EQ_INT(taken.center_hz, 100);
    LS_EQ_INT(taken.tune_generation, 1);
    LS_CHECK(!ls_iq_control_take(&control, &request));
    LS_CHECK(entries >= 4);
    LS_EQ_INT(release_violations, 0);
}
