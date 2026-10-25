/* The HTTP and WebSocket the web screen speaks (ls_rterm.c): a request's
   line and headers, the handshake's answer, and frames each way. No sockets
   and no IDF here, so the bench runs it on the PC. */

#ifndef LS_WS_H
#define LS_WS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum { LS_WS_TEXT = 1, LS_WS_BINARY = 2, LS_WS_CLOSE = 8, LS_WS_PING = 9, LS_WS_PONG = 10 };

/* Sec-WebSocket-Accept for a client's Sec-WebSocket-Key: 28 characters and a NUL. */
void ls_ws_accept(const char *key, char out[29]);

/* The header of one whole, unmasked frame from the board: 2, 4 or 10 bytes. */
size_t ls_ws_header(uint8_t out[10], int opcode, size_t n);

typedef struct {
    int op;
    bool fin;
    uint8_t *data;                  /* in the caller's buffer, unmasked in place */
    size_t n;
} ls_ws_msg;

/* The frame at the start of buf: the bytes it took, 0 while it is still
   coming, -1 for one no browser sends (unmasked, or longer than max). */
int ls_ws_parse(uint8_t *buf, size_t n, size_t max, ls_ws_msg *m);

/* A request's head, NUL-terminated: method and path from its first line
   (the query cut off); false when it is not one. */
bool ls_http_request(const char *req, char *method, size_t mcap, char *path, size_t pcap);

/* A header's value, any case of its name, spaces trimmed; false when absent. */
bool ls_http_header(const char *req, const char *name, char *out, size_t cap);

/* A comma list header (Connection: keep-alive, Upgrade) has this token. */
bool ls_http_has_token(const char *value, const char *token);

#ifdef __cplusplus
}
#endif

#endif
