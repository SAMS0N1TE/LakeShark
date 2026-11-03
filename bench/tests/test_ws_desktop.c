/* Host-only production framing, bounded queue and transport budget tests. */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "ls_ws.h"
#include "ls_ws_tx.h"
#include "ls_rterm_budget.h"

static uint8_t received[LS_WS_TX_LIMIT + 32];
static size_t received_n, calls;
static int mode;
static int writer(void *ctx, const void *data, size_t n)
{
    assert(ctx == &mode); calls++;
    if (mode == -1) return -1;
    if (!mode || calls % 3 == 0) return 0; /* arbitrarily slow client */
    if (n > 733) n = 733;
    memcpy(received + received_n, data, n); received_n += n;
    return (int)n;
}
static void *oom(void *p, size_t n) { (void)p; (void)n; return NULL; }

static void frames(void)
{
    char accept[29];
    ls_ws_accept("dGhlIHNhbXBsZSBub25jZQ==", accept);
    assert(!strcmp(accept, "s3pPLMBiTxaQ9kYGzzhZRbK+xOo="));
    const size_t sizes[] = {0, 1, 125, 126, 4096, 65535, 65536, 2097152};
    for (size_t i = 0; i < sizeof(sizes)/sizeof(sizes[0]); i++) {
        uint8_t h[10]; size_t n = sizes[i];
        size_t head = ls_ws_header(h, LS_WS_BINARY, n);
        assert(h[0] == 0x82 && !(h[1] & 128));
        size_t decoded = h[1];
        if (n >= 126) {
            decoded = 0;
            for (size_t j = 2; j < head; j++) decoded = decoded * 256 + h[j];
        }
        assert(decoded == n && head == (n < 126 ? 2 : n < 65536 ? 4 : 10));
        /* Same lengths masked as a browser; every partial frame is incomplete
         * and remains untouched until all mask and payload bytes arrive. */
        uint8_t *b = malloc(head + 4 + n); assert(b);
        memcpy(b, h, head); b[1] |= 128;
        const uint8_t mask[4] = {0xde, 0xad, 0xbe, 0xef};
        memcpy(b + head, mask, 4);
        for (size_t j = 0; j < n; j++) b[head+4+j] = (uint8_t)j ^ mask[j%4];
        ls_ws_msg msg;
        size_t total = head + 4 + n;
        for (size_t k = 0; k < total; k += (n > 4096 ? 997 : 1))
            assert(ls_ws_parse(b, k, n, &msg) == 0);
        if (n) assert(ls_ws_parse(b, total, n-1, &msg) == -1);
        assert(ls_ws_parse(b, total, n, &msg) == (int)total);
        assert(msg.fin && msg.op == LS_WS_BINARY && msg.n == n);
        for (size_t j = 0; j < n; j++) assert(msg.data[j] == (uint8_t)j);
        free(b);
    }
    uint8_t bad[][10] = {
        {0x81,0},                    /* unmasked */
        {0xc1,0x80},                 /* RSV */
        {0x01,0x80},                 /* fragmented */
        {0x80,0x80},                 /* continuation without reassembly */
        {0x83,0x80},                 /* reserved opcode */
        {0x89,0xfe,0,126},           /* oversized control */
        {0x88,0x81},                 /* invalid close payload */
        {0x81,0xfe,0,125},           /* noncanonical length */
        {0x81,0xff,0x80,0,0,0,0,0,0,0}, /* invalid 64-bit length */
    };
    ls_ws_msg msg;
    for (size_t i=0; i<sizeof(bad)/sizeof(bad[0]); i++)
        assert(ls_ws_parse(bad[i], sizeof(bad[i]), LS_WS_TX_LIMIT, &msg) == -1);
}
static void queue(void)
{
    ls_ws_tx q = {0};
    uint8_t *payload = malloc(LS_WS_TX_LIMIT); assert(payload);
    for (size_t i=0; i<LS_WS_TX_LIMIT; i++) payload[i]=(uint8_t)(i*31);
    uint8_t h[10]; size_t n = 1048576, hn = ls_ws_header(h, LS_WS_BINARY, n);
    assert(ls_ws_tx_push(&q, h, hn, realloc));
    assert(ls_ws_tx_push(&q, payload, n, realloc));
    mode = 0; calls = 0; received_n = 0;
    assert(ls_ws_tx_flush(&q, writer, &mode, 65536) == 0 && calls == 1);
    assert(ls_ws_tx_pending(&q) == n + hn && received_n == 0);
    mode = 1;
    assert(ls_ws_tx_flush(&q, writer, &mode, 17) == 0 && received_n == 17);
    /* FIFO ordering survives partial header/body sends and later appends. */
    assert(ls_ws_tx_push(&q, "tail", 4, realloc));
    while (ls_ws_tx_pending(&q)) assert(ls_ws_tx_flush(&q, writer, &mode, 4096) >= 0);
    assert(received_n == hn+n+4 && !memcmp(received,h,hn));
    assert(!memcmp(received+hn,payload,n) && !memcmp(received+hn+n,"tail",4));
    assert(!q.at && !q.end && q.cap <= LS_WS_TX_LIMIT);
    assert(ls_ws_tx_push(&q, payload, LS_WS_TX_LIMIT, realloc));
    assert(!ls_ws_tx_push(&q, "overflow", 8, realloc));
    assert(ls_ws_tx_pending(&q) == LS_WS_TX_LIMIT);
    mode = -1; assert(ls_ws_tx_flush(&q, writer, &mode, 4096) == -1);
    assert(ls_ws_tx_pending(&q) == LS_WS_TX_LIMIT);
    free(q.data); q=(ls_ws_tx){0};
    assert(!ls_ws_tx_push(&q, "oom", 3, oom) && !q.data && !q.end);
    /* Compaction keeps unsent bytes before appended bytes. */
    assert(ls_ws_tx_push(&q, payload, 4096, realloc));
    q.at=4000;
    assert(ls_ws_tx_push(&q, payload, 4000, realloc));
    assert(q.at==0 && q.end==4096 && !memcmp(q.data,payload+4000,96));
    assert(!memcmp(q.data+96,payload,4000));
    free(payload); free(q.data);
}
int main(void)
{
    frames(); queue();
    assert(ls_rterm_picture_budget(false) == 524288);
    assert(ls_rterm_picture_budget(true) == 2097152);
    assert(ls_rterm_picture_budget(true) + 43008u*6u + 32768u < LS_WS_TX_LIMIT);
    puts("desktop framing, partial sends, backpressure, queue bounds/OOM and budgets: OK");
}
