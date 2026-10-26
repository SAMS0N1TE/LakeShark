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
static const char request[] = "GET / HTTP/1.0\r\nHost: example.com\r\n\r\n";
static lap_net_status_t net;
static int socket_handle = -1, state;
static size_t sent;
static char incoming[512], display[512];
static const char *message;
static void enter(void) {
    sent = 0; display[0] = 0;
    socket_handle = lap_tcp_connect(host, 80, 10000);
    message = socket_handle < 0 ? "Connect request rejected" : "Resolving / connecting...";
}
static void leave(void) { if (socket_handle >= 0) lap_tcp_close(socket_handle); }
static void draw(void) {
    lap_net_status(&net);
    lap_clear(LAP_ATTR(LAP_WHITE, LAP_BLACK));
    lap_put_str(1, 1, "TCP PROBE", LAP_ATTR(LAP_CYAN | LAP_BRIGHT, LAP_BLACK));
    lap_put_str(1, 3, net.connected ? net.ip : "Join Wi-Fi in LINK first", LAP_ATTR(LAP_YELLOW, LAP_BLACK));
    if (socket_handle >= 0) {
        state = lap_tcp_state(socket_handle);
        if (state == LAP_TCP_CONNECTED && sent < sizeof(request) - 1) {
            int n = lap_tcp_send(socket_handle, request + sent, sizeof(request) - 1 - sent);
            if (n > 0) sent += (size_t)n;
            else if (n != LAP_NET_EAGAIN) message = "Send failed";
        }
        int n = lap_tcp_recv(socket_handle, incoming, sizeof(incoming) - 1);
        if (n > 0) {
            /* Render only printable text; real apps should use a bounded terminal. */
            for (int i = 0; i < n; ++i) display[i] = incoming[i] >= 32 && incoming[i] < 127 ? incoming[i] : ' ';
            display[n] = 0; message = "Received data";
        } else if (!n) message = "Peer closed";
        else if (n != LAP_NET_EAGAIN) message = "Connection failed (see state)";
    } else state = socket_handle;
    char line[64]; snprintf(line, sizeof(line), "state %d / queued %u bytes", state, (unsigned)sent);
    lap_put_str(1, 5, line, LAP_ATTR(LAP_GREEN, LAP_BLACK));
    lap_put_str(1, 7, message, LAP_ATTR(LAP_WHITE, LAP_BLACK));
    /* Clip to rows/columns so the response stays inside the app area. */
    size_t pos = 0;
    for (int y = 9; y < lap_rows() - 1 && display[pos]; ++y) {
        int width = lap_cols() - 2; if (width > 63) width = 63; if (width < 1) break;
        snprintf(line, sizeof(line), "%.*s", width, display + pos);
        lap_put_str(1, y, line, LAP_ATTR(LAP_WHITE, LAP_BLACK));
        pos += strlen(line);
    }
}
LAP_APP(.id = "tcp_probe", .name = "TCP PROBE", .sub = "outbound TCP example",
        .version = "1.0", .author = "LakeShark", .icon = LAP_ICON_TOWER,
        .color = LAP_CYAN, .flags = LAP_CAP_NETWORK,
        .enter = enter, .leave = leave, .draw = draw);
