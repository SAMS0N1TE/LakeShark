/* lr_wf_session's lock: a stats reader or a new start on another task waits
   while a stop is freeing the session, instead of reading freed memory. */
#include "ls_test.h"
#include <pthread.h>
#include <string.h>
#include <time.h>
#include "../../components/lakeshark/radio/lr_wf_session.c"

static ls_lora_cfg_t cfg = {.freq_hz = 915000000, .sf = 9, .bw_hz = 125000};
const ls_lora_cfg_t *ls_lora_cfg(void) { return &cfg; }
void ls_lora_cfg_default(ls_lora_cfg_t *out) { *out = cfg; }
uint32_t ls_lora_caps(void) { return 0; }
ls_lora_chip_t ls_lora_chip(void) { return LS_LORA_CHIP_LR20XX; }
bool ls_lora_fsk_active(void) { return false; }
bool ls_lora_scanning(void) { return false; }
bool lr20xx_stream_source2_active(void) { return false; }
esp_err_t ls_lora_configure(const ls_lora_cfg_t *c) { cfg = *c; return ESP_OK; }
esp_err_t lr20xx_set_agc_gain(uint8_t gain) { (void)gain; return ESP_OK; }
esp_err_t ls_lora_receive(void) { return ESP_OK; }
esp_err_t lr20xx_stream_source2_live(lr20xx_iq_sink_t sink, void *arg, lr20xx_stream_result_t *out)
{
    (void)sink; (void)arg; memset(out, 0, sizeof(*out)); return ESP_OK;
}

static void nap_ms(int ms)
{
    struct timespec t = {.tv_sec = 0, .tv_nsec = (long)ms * 1000000L};
    nanosleep(&t, NULL);
}

static atomic_bool stop_done, read_done, start_done;
static lr_wf_stats read_stats;
static esp_err_t start_err;

static void *stopper(void *arg) { (void)arg; lr_wf_stop(); atomic_store(&stop_done, true); return NULL; }
static void *reader(void *arg) { (void)arg; lr_wf_get_stats(&read_stats); atomic_store(&read_done, true); return NULL; }
static void *starter(void *arg)
{
    (void)arg; start_err = lr_wf_start(916000000); atomic_store(&start_done, true); return NULL;
}

LS_CASE(reader_and_starter_wait_for_a_stop_in_progress)
{
    /* The host task shim does not run the owner: the session stays live
       until the test says its owner is done. */
    LS_EQ_INT(ESP_OK, lr_wf_start(915000000));
    session_t *s = session;
    LS_CHECK(s != NULL);
    atomic_store(&s->lost_pairs, 7);
    atomic_store(&stop_done, false); atomic_store(&read_done, false); atomic_store(&start_done, false);

    pthread_t a, b, c;
    pthread_create(&a, NULL, stopper, NULL);
    nap_ms(20);                        /* the stop now waits on done, holding the lock */
    LS_CHECK(!atomic_load(&stop_done));
    pthread_create(&b, NULL, reader, NULL);
    pthread_create(&c, NULL, starter, NULL);
    nap_ms(50);
    LS_CHECK(!atomic_load(&read_done));
    LS_CHECK(!atomic_load(&start_done));

    atomic_store_explicit(&s->done, true, memory_order_release);   /* owner finished */
    pthread_join(a, NULL); pthread_join(b, NULL); pthread_join(c, NULL);
    LS_CHECK(atomic_load(&stop_done));
    /* Either order after the stop is safe: the reader never saw the freed
       session's 7 lost pairs, and the start made a fresh session. */
    LS_EQ_UINT(0, read_stats.lost_pairs);
    LS_EQ_INT(ESP_OK, start_err);
    LS_CHECK(session != NULL);
    LS_EQ_UINT(916000000, session->hz);
    atomic_store_explicit(&session->done, true, memory_order_release);
    lr_wf_stop();
    LS_CHECK(session == NULL);
}

static lr_wf_stats stop_stats;
static void *stopper_stats(void *arg) { (void)arg; lr_wf_stop_stats(&stop_stats); atomic_store(&stop_done, true); return NULL; }

/* An owner that never finishes (stuck in the radio driver): the stop gives
   up after STOP_WAIT_US instead of holding the lock, and every caller behind
   it, forever. The session is not freed under the owner; starts are refused
   until that owner ends, and it frees the session then. */
LS_CASE(stop_gives_up_on_a_stuck_owner_and_the_owner_frees_it_later)
{
    LS_EQ_INT(ESP_OK, lr_wf_start(915000000));
    session_t *s = session;
    LS_CHECK(s != NULL);
    atomic_store(&stop_done, false); atomic_store(&read_done, false);
    pthread_t a, b;
    pthread_create(&a, NULL, stopper_stats, NULL);
    nap_ms(20);
    LS_CHECK(!atomic_load(&stop_done));
    ls_shim_time_advance(STOP_WAIT_US + 1000);   /* the host clock moves only when told */
    pthread_join(a, NULL);
    LS_CHECK(atomic_load(&stop_done));
    LS_CHECK(stop_stats.done); LS_EQ_INT(ESP_ERR_TIMEOUT, stop_stats.error);
    LS_CHECK(session == NULL);
    LS_CHECK(atomic_load(&s->orphaned));
    LS_CHECK(atomic_load(&s->stop));
    /* The lock is free again: readers return at once. */
    pthread_create(&b, NULL, reader, NULL);
    pthread_join(b, NULL);
    LS_CHECK(atomic_load(&read_done));
    /* The stuck owner still holds the radio: no new session over it. */
    LS_EQ_INT(ESP_ERR_INVALID_STATE, lr_wf_start(916000000));
    /* It ends at last (owner's exit: done, then its reference). */
    atomic_store_explicit(&s->done, true, memory_order_release);
    session_put(s);
    LS_EQ_UINT(0, atomic_load(&s_orphans));
    LS_EQ_INT(ESP_OK, lr_wf_start(916000000));
    atomic_store_explicit(&session->done, true, memory_order_release);
    lr_wf_stop();
    LS_CHECK(session == NULL);
}
