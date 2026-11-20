/* Control surface of the fault shims (HTTP server, network, storage). */
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include "ls_fault_fs.h"

typedef struct {
    int open_status;        /* 0 ok; <0 transport (-1,-2,-3); >0 HTTP error status */
    const char *fail_url;   /* only URLs containing this fail (NULL: all) */
    const uint8_t *body; size_t body_len;
    long claim_len;         /* Content-Length announced; -2: body_len; -1: unknown (chunked) */
    bool range_ok;          /* honour Range: 206 and the tail; else 200 and everything */
    long drop_after;        /* read returns -1 once this many bytes of the answer went out; -1 never */
    long end_early;         /* body ends silently (read 0, incomplete) after this many; -1 never */
    long stall_after;       /* from here on reads give EAGAIN; -1 never */
    int64_t read_us;        /* clock advance per read */
    void (*on_read)(size_t served);   /* hook before each read */
    bool (*route)(const char *url, const uint8_t **body, size_t *len);   /* per-URL answer; false: 404 */
    /* record */
    int opens, cleanups, last_from;
    char urls[4][240];
    int nurls;
} lf_srv_t;
extern lf_srv_t lf_srv;
extern bool lf_online;
extern bool lf_rf_busy;
void lf_net_reset(void);
extern size_t lf_heap_free;   /* what heap_caps_get_free_size reports */
void lf_task_pump(void);                       /* the downloader task, until its queue is empty */
void lf_task_pump_n(const char *name, int allow);   /* a named task for `allow` rounds */
