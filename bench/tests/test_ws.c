/* ls_ws: the web screen's HTTP and WebSocket pieces, against the RFC's own
   examples and what a phone's Safari actually sends. */

#include <stdio.h>
#include <string.h>

#include "ls_ws.h"

static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static void accept_key(void)
{
    char a[29];
    ls_ws_accept("dGhlIHNhbXBsZSBub25jZQ==", a);                 /* RFC 6455, 1.3 */
    CHECK(!strcmp(a, "s3pPLMBiTxaQ9kYGzzhZRbK+xOo="), "accept %s", a);
    ls_ws_accept("x3JJHMbDL1EzLkh9GBhXDw==", a);                 /* RFC 6455, 4.2.2's other example */
    CHECK(!strcmp(a, "HSmrc0sMlYUkAGmm5OPpG2HaGWk="), "accept %s", a);
}

static void frames_out(void)
{
    uint8_t h[10];
    CHECK(ls_ws_header(h, LS_WS_TEXT, 5) == 2 && h[0] == 0x81 && h[1] == 5, "short");
    CHECK(ls_ws_header(h, LS_WS_BINARY, 4000) == 4 && h[0] == 0x82 && h[1] == 126 && h[2] == 0x0f && h[3] == 0xa0, "medium");
    CHECK(ls_ws_header(h, LS_WS_BINARY, 70000) == 10 && h[1] == 127 && h[7] == 0x01 && h[8] == 0x11 && h[9] == 0x70, "long");
}

static void frames_in(void)
{
    /* RFC 6455, 5.7: a masked "Hello". */
    uint8_t b[64] = {0x81, 0x85, 0x37, 0xfa, 0x21, 0x3d, 0x7f, 0x9f, 0x4d, 0x51, 0x58};
    ls_ws_msg m;
    for (size_t n = 0; n < 11; n++) CHECK(ls_ws_parse(b, n, 100, &m) == 0, "partial %d", (int)n);
    CHECK(ls_ws_parse(b, 11, 100, &m) == 11, "whole");
    CHECK(m.op == LS_WS_TEXT && m.fin && m.n == 5 && !memcmp(m.data, "Hello", 5), "payload");

    /* Two frames back to back: the first one's length, then the second. */
    uint8_t two[32] = {0x89, 0x80, 1, 2, 3, 4, 0x81, 0x82, 0, 0, 0, 0, 'o', 'k'};
    CHECK(ls_ws_parse(two, 14, 100, &m) == 6 && m.op == LS_WS_PING && m.n == 0, "ping");
    CHECK(ls_ws_parse(two + 6, 8, 100, &m) == 8 && m.n == 2 && !memcmp(m.data, "ok", 2), "after ping");

    /* 126: a 16-bit length. */
    static uint8_t big[300];
    big[0] = 0x82; big[1] = 0x80 | 126; big[2] = 0; big[3] = 200;
    CHECK(ls_ws_parse(big, 8 + 200, 300, &m) == 208 && m.n == 200, "16-bit length");
    CHECK(ls_ws_parse(big, 8 + 200, 100, &m) == -1, "longer than the buffer allows");

    uint8_t bare[] = {0x81, 0x02, 'h', 'i'};
    CHECK(ls_ws_parse(bare, sizeof(bare), 100, &m) == -1, "unmasked from a client");
}

static void requests(void)
{
    /* What iOS Safari sent the board, give or take the key. */
    const char *req =
        "GET /ws?x=1 HTTP/1.1\r\n"
        "Host: 192.168.1.50\r\n"
        "Sec-WebSocket-Version: 13\r\n"
        "Upgrade: websocket\r\n"
        "Connection: keep-alive, Upgrade\r\n"
        "sec-websocket-key:   dGhlIHNhbXBsZSBub25jZQ==  \r\n"
        "If-None-Match: \"ab12\"\r\n"
        "\r\n";
    char method[8], path[32], v[64];
    CHECK(ls_http_request(req, method, sizeof(method), path, sizeof(path)), "request line");
    CHECK(!strcmp(method, "GET") && !strcmp(path, "/ws"), "%s %s", method, path);
    CHECK(ls_http_header(req, "Sec-WebSocket-Key", v, sizeof(v)) && !strcmp(v, "dGhlIHNhbXBsZSBub25jZQ=="), "key '%s'", v);
    CHECK(ls_http_header(req, "connection", v, sizeof(v)) && ls_http_has_token(v, "upgrade"), "connection");
    CHECK(!ls_http_has_token("keep-alive", "upgrade") && ls_http_has_token("Upgrade", "upgrade"), "tokens");
    CHECK(!ls_http_has_token("upgrade-ish", "upgrade"), "a longer token is another token");
    CHECK(ls_http_header(req, "If-None-Match", v, sizeof(v)) && !strcmp(v, "\"ab12\""), "etag");
    CHECK(!ls_http_header(req, "Origin", v, sizeof(v)), "absent");
    CHECK(!ls_http_header(req, "Sec-WebSocket-Key", v, 8), "longer than its room");
    CHECK(!ls_http_header("GET / HTTP/1.1\r\n\r\nHost: x\r\n", "Host", v, sizeof(v)), "past the head");
    CHECK(!ls_http_request("GARBAGE\r\n\r\n", method, sizeof(method), path, sizeof(path)), "no request line");
    CHECK(!ls_http_request("GET ws HTTP/1.1\r\n\r\n", method, sizeof(method), path, sizeof(path)), "not a path");
    CHECK(!ls_http_request("GET / FTP\r\n\r\n", method, sizeof(method), path, sizeof(path)), "not HTTP");
}

int main(void)
{
    accept_key();
    frames_out();
    frames_in();
    requests();
    if (fails) { printf("%d failed\n", fails); return 1; }
    printf("ws: all passed\n");
    return 0;
}
