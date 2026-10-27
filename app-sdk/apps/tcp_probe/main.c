/* Change host/port/request for your own echo service. Return to LINK to join
   Wi-Fi first. This app never asks for credentials or starts its own network. */
#include "lakeshark_app.h"
#include <stdio.h>
#include <string.h>
LAP_ICON_PX("....................",
            "....................",
            "..6666666666666666..",
            "..6..............6..",
            "..6..ff..........6..",
            "..6....ff........6..",
            "..6......ff......6..",
            "..6....ff........6..",
            "..6..ff....ffff..6..",
            "..6..............6..",
            "..6..............6..",
            "..6666666666666666..",
            "....................",
            "........6666........",
            "......66666666......",
            "....................",
            "....................",
            "....................");
static const char host[] = "example.com";
static const int port = 80;
static const char request[] = "GET / HTTP/1.0\r\nHost: example.com\r\n\r\n";
static lap_net_status_t net;
static int socket_handle = -1, error, connected, finished, status_done;
static size_t sent, received, status_len;
static char incoming[512], status[48];
static const char *why(int e) {
    switch (e) {
    case LAP_NET_EACCES: return "network access denied";
    case LAP_NET_EBUSY: return "no free connection";
    case LAP_NET_EDNS: return "name not found";
    case LAP_NET_ETIMEOUT: return "timed out";
    case LAP_NET_ENETDOWN: return "Wi-Fi dropped";
    case LAP_NET_EIO: return "connection refused or reset";
    default: return "error";
    }
}
static void finish(int e) {
    if (socket_handle >= 0) lap_tcp_close(socket_handle);
    socket_handle = -1; error = e; finished = 1;
}
static void enter(void) {
    sent = received = status_len = 0; status[0] = 0;
    error = connected = finished = status_done = 0;
    socket_handle = lap_tcp_connect(host, port, 10000);
    if (socket_handle < 0) finish(socket_handle);
}
static void leave(void) { if (socket_handle >= 0) lap_tcp_close(socket_handle); socket_handle = -1; }
static void poll(void) {
    int state = lap_tcp_state(socket_handle);
    if (state < 0 && state != LAP_NET_EOF) { finish(state); return; }
    if (state == LAP_TCP_CONNECTED || state == LAP_NET_EOF) connected = 1;
    if (connected && sent < sizeof(request) - 1) {
        int n = lap_tcp_send(socket_handle, request + sent, sizeof(request) - 1 - sent);
        if (n > 0) sent += (size_t)n;
        else if (n != LAP_NET_EAGAIN) { finish(n); return; }
    }
    /* Drain what has arrived before treating end of stream as done. */
    for (;;) {
        int n = lap_tcp_recv(socket_handle, incoming, sizeof(incoming));
        if (n == LAP_NET_EAGAIN) return;
        if (n == 0 || n == LAP_NET_EOF) { finish(0); return; }
        if (n < 0) { finish(n); return; }
        /* Keep the reply's first line, e.g. "HTTP/1.0 200 OK". */
        for (int i = 0; i < n && !status_done; ++i) {
            if (incoming[i] == '\r' || incoming[i] == '\n' || status_len == sizeof(status) - 1) status_done = 1;
            else if (incoming[i] >= 32 && incoming[i] < 127) status[status_len++] = incoming[i];
        }
        status[status_len] = 0;
        received += (size_t)n;
    }
}
static void row(int y, const char *label, const char *value, uint8_t colour) {
    lap_put_str(1, y, label, LAP_ATTR(LAP_WHITE, LAP_BLACK));
    lap_put_str(11, y, value, LAP_ATTR(colour, LAP_BLACK));
}
static void draw(void) {
    char line[64];
    lap_net_status(&net);
    if (socket_handle >= 0) poll();
    lap_clear(LAP_ATTR(LAP_WHITE, LAP_BLACK));
    lap_put_str(1, 1, "TCP PROBE", LAP_ATTR(LAP_CYAN | LAP_BRIGHT, LAP_BLACK));
    row(3, "Wi-Fi", net.connected ? net.ip : "not joined, use LINK", net.connected ? LAP_YELLOW : LAP_RED);
    snprintf(line, sizeof(line), "%s:%d", host, port);
    row(4, "Server", line, LAP_YELLOW);
    row(6, "Connect", connected ? "OK" : finished ? "failed" : "...", connected ? LAP_GREEN : finished ? LAP_RED : LAP_WHITE);
    snprintf(line, sizeof(line), "%u of %u bytes", (unsigned)sent, (unsigned)(sizeof(request) - 1));
    row(7, "Sent", connected ? line : "-", LAP_WHITE);
    row(8, "Reply", status_len ? status : "-", LAP_WHITE);
    snprintf(line, sizeof(line), "%u bytes", (unsigned)received);
    row(9, "Received", received ? line : "-", LAP_WHITE);
    if (!finished) lap_put_str(1, 11, "Working...", LAP_ATTR(LAP_WHITE, LAP_BLACK));
    else if (!error && status_len) lap_put_str(1, 11, "PASS  TCP works", LAP_ATTR(LAP_GREEN | LAP_BRIGHT, LAP_BLACK));
    else {
        snprintf(line, sizeof(line), "FAIL  %s", error ? why(error) : "no reply");
        lap_put_str(1, 11, line, LAP_ATTR(LAP_RED | LAP_BRIGHT, LAP_BLACK));
    }
}
LAP_APP(.id = "tcp_probe", .name = "TCP PROBE", .sub = "network test",
        .version = "1.1", .author = "LakeShark", .icon = LAP_ICON_TOWER,
        .color = LAP_CYAN, .flags = LAP_CAP_NETWORK,
        .enter = enter, .leave = leave, .draw = draw);
