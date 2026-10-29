/* See ls_decoded.h. Entries are numbered from 1; reader cursors are the
   number of the next entry they will look at. A critical section covers one
   entry's copy (about 200 bytes), or a reader looking at a few entries
   (NEXT_SCAN) and copying at most one, and nothing else. */
#include "ls_decoded.h"
#include <stdlib.h>
#include <string.h>
#if defined(ESP_PLATFORM) && !defined(LS_LAP_HOST_TEST)
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
#define LOCK() portENTER_CRITICAL(&s_mux)
#define UNLOCK() portEXIT_CRITICAL(&s_mux)
#define RING_ALLOC(n) heap_caps_calloc(1, (n), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)
#define RING_FREE(p) heap_caps_free(p)
#else
#define LOCK() ((void)0)            /* the bench is one thread */
#define UNLOCK() ((void)0)
#define RING_ALLOC(n) calloc(1, (n))
#define RING_FREE(p) free(p)
#endif

static ls_decoded_t *s_ring;         /* PSRAM, from the first reader on; never freed */
static uint32_t s_head;              /* entries published so far */
static struct {
    bool used;
    uint32_t kinds, cursor;
} s_reader[LS_DECODED_READERS];
static volatile int s_readers;

void ls_decoded_publish(const ls_decoded_t *d)
{
    if (!d || !s_readers || !s_ring) return;
    LOCK();
    s_ring[s_head % LS_DECODED_RING] = *d;
    s_head++;
    UNLOCK();
}

int ls_decoded_open(uint32_t kinds)
{
    if (!s_ring) {
        ls_decoded_t *r = RING_ALLOC(sizeof(ls_decoded_t) * LS_DECODED_RING);
        if (!r) return -1;
        LOCK();
        if (!s_ring) { s_ring = r; r = NULL; }
        UNLOCK();
        RING_FREE(r);                /* another reader got there first */
    }
    int id = -1;
    LOCK();
    for (int i = 0; i < LS_DECODED_READERS && id < 0; i++)
        if (!s_reader[i].used) {
            s_reader[i].used = true;
            s_reader[i].kinds = kinds;
            s_reader[i].cursor = s_head;
            s_readers++;
            id = i;
        }
    UNLOCK();
    return id;
}

void ls_decoded_kinds(int reader, uint32_t kinds)
{
    if (reader < 0 || reader >= LS_DECODED_READERS) return;
    LOCK();
    s_reader[reader].kinds = kinds;
    UNLOCK();
}

void ls_decoded_close(int reader)
{
    if (reader < 0 || reader >= LS_DECODED_READERS) return;
    LOCK();
    if (s_reader[reader].used) { s_reader[reader].used = false; s_readers--; }
    UNLOCK();
}

void ls_decoded_flush(int reader)
{
    if (reader < 0 || reader >= LS_DECODED_READERS) return;
    LOCK();
    s_reader[reader].cursor = s_head;
    UNLOCK();
}

/* Entries a reader looks at per critical section: a reader skipping a
   ring full of other kinds lets go of the spinlock between every few, so a
   publisher (a decoder's task, either core) never waits behind it. */
#define NEXT_SCAN 4

bool ls_decoded_next(int reader, ls_decoded_t *out, uint32_t *lost)
{
    if (reader < 0 || reader >= LS_DECODED_READERS || !out || !s_ring) return false;
    bool got = false, more = true;
    while (!got && more) {
        LOCK();
        if (!s_reader[reader].used) {
            more = false;
        } else {
            uint32_t *c = &s_reader[reader].cursor;
            if (s_head - *c > LS_DECODED_RING) {
                if (lost) *lost += s_head - *c - LS_DECODED_RING;
                *c = s_head - LS_DECODED_RING;
            }
            for (int n = 0; n < NEXT_SCAN && *c != s_head && !got; n++) {
                const ls_decoded_t *e = &s_ring[*c % LS_DECODED_RING];
                (*c)++;
                if (e->kind & s_reader[reader].kinds) { *out = *e; got = true; }
            }
            more = *c != s_head;
        }
        UNLOCK();
    }
    return got;
}
