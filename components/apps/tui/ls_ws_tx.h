/* Bounded queue owned by the terminal worker. Finish each delta, then encode
 * the newest snapshot. Callers skip rendering while pending. No IDF/sockets. */
#ifndef LS_WS_TX_H
#define LS_WS_TX_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#define LS_WS_TX_LIMIT (3u * 1024u * 1024u)
typedef struct { uint8_t *data; size_t cap, at, end; } ls_ws_tx;
static inline size_t ls_ws_tx_pending(const ls_ws_tx *q) { return q->end - q->at; }
static inline bool ls_ws_tx_push(ls_ws_tx *q, const void *data, size_t n,
                               void *(*resize)(void *, size_t))
{
    size_t pending = ls_ws_tx_pending(q);
    if (n > LS_WS_TX_LIMIT - pending) return false;
    if (q->at && n > q->cap - q->end) {
        memmove(q->data, q->data + q->at, pending);
        q->at = 0; q->end = pending;
    }
    if (n > q->cap - q->end) {
        size_t cap = q->cap ? q->cap : 4096;
        while (cap < q->end + n && cap < LS_WS_TX_LIMIT) cap *= 2;
        if (cap > LS_WS_TX_LIMIT) cap = LS_WS_TX_LIMIT;
        void *p = resize(q->data, cap);
        if (!p) return false;
        q->data = p; q->cap = cap;
    }
    if (n) memcpy(q->data + q->end, data, n);
    q->end += n;
    return true;
}
/* write: >0 accepted, 0 would block, -1 fatal. No waiting. Returns 1 drained,
 * 0 pending, -1 fatal; bounded work even for a continuously writable socket. */
static inline int ls_ws_tx_flush(ls_ws_tx *q, int (*write)(void *, const void *, size_t),
                                 void *ctx, size_t budget)
{
    while (q->at < q->end && budget) {
        size_t n = q->end - q->at;
        if (n > budget) n = budget;
        int k = write(ctx, q->data + q->at, n);
        if (k < 0) return -1;
        if (!k) return 0;
        if ((size_t)k > n) return -1;
        q->at += (size_t)k; budget -= (size_t)k;
    }
    if (q->at != q->end) return 0;
    q->at = q->end = 0;
    return 1;
}
#endif
