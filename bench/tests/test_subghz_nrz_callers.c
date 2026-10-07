#include "ls_test.h"
#include "subghz_nrz.h"
#include <pthread.h>
#include <string.h>

enum { CALLERS = 4, EDGES = 1536, RUNS = 2000 };
typedef struct {
    int32_t pulses[EDGES];
    subghz_nrz_t expected;
    int failures;
} caller_t;
static caller_t callers[CALLERS];
static pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t ready = PTHREAD_COND_INITIALIZER;
static int waiting;
static bool start;

static void *decode_many(void *arg)
{
    caller_t *caller = arg;
    pthread_mutex_lock(&mutex);
    waiting++;
    pthread_cond_broadcast(&ready);
    while (!start) pthread_cond_wait(&ready, &mutex);
    pthread_mutex_unlock(&mutex);
    for (int i = 0; i < RUNS; i++) {
        subghz_nrz_t got;
        if (!subghz_nrz_decode(caller->pulses, EDGES, &got) ||
            memcmp(&got, &caller->expected, sizeof(got))) caller->failures++;
    }
    return NULL;
}

LS_CASE(concurrent_callers_keep_their_own_payload)
{
    pthread_t threads[CALLERS];
    waiting = 0;
    start = false;
    for (int c = 0; c < CALLERS; c++) {
        caller_t *caller = &callers[c];
        ls_rng_t rng;
        ls_rng_seed(&rng, 713 + c * 319);
        for (int i = 0; i < 96; i++) {
            int sign = i && (ls_rng_u32(&rng) & 1) ? 1 : -1;
            for (int j = i; j < EDGES; j += 96) caller->pulses[j] = sign * (300 + c * 100);
        }
        caller->failures = 0;
        LS_CHECK(subghz_nrz_decode(caller->pulses, EDGES, &caller->expected));
    }
    int created = 0;
    for (int c = 0; c < CALLERS; c++) {
        int rc = pthread_create(&threads[c], NULL, decode_many, &callers[c]);
        LS_EQ_INT(rc, 0);
        if (rc) break;
        created++;
    }
    pthread_mutex_lock(&mutex);
    while (waiting < created) pthread_cond_wait(&ready, &mutex);
    start = true;
    pthread_cond_broadcast(&ready);
    pthread_mutex_unlock(&mutex);
    for (int c = 0; c < created; c++) {
        LS_EQ_INT(pthread_join(threads[c], NULL), 0);
        LS_EQ_INT(callers[c].failures, 0);
    }
}
