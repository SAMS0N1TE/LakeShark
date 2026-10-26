/* Actual loopback TCP, including DNS, partial queues, EOF and app teardown. */
#include "ls_lap_net.h"
#include "ls_lap_api.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
typedef SOCKET fd_t;
#define close_fd closesocket
#define pause_ms(n) Sleep(n)
#else
#include <sys/socket.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include <unistd.h>
typedef int fd_t;
#define close_fd close
#define pause_ms(n) usleep((n)*1000)
#endif
static bool inside = true;
#ifdef LS_LAP_NET_TEST_DNS
static bool delay_dns;
static struct { void *ctx; void (*done)(void *, uint32_t); } dns[2];
void ls_lap_net_test_dns_start(const char *host, void *ctx, void (*done)(void *, uint32_t)) {
    assert(!strcmp(host, "localhost"));
    if (!delay_dns) { done(ctx, htonl(INADDR_LOOPBACK)); return; }
    for (int i = 0; i < 2; ++i) if (!dns[i].ctx) {
        dns[i].ctx = ctx; dns[i].done = done; return;
    }
    assert(!"unbounded DNS request");
}
#endif
static struct {
    char host[254], tx[4096], rx[4096];
    lap_net_status_t status;
} app;
bool ls_lap_api_inside(void) { return inside; }
bool ls_lap_api_owns(const void *p, size_t n) {
    uintptr_t a = (uintptr_t)p, b = (uintptr_t)&app;
    return p && a >= b && a - b <= sizeof(app) && n <= sizeof(app) - (a - b);
}
static void nonblock(fd_t fd) {
#ifdef _WIN32
    u_long on = 1; assert(!ioctlsocket(fd, FIONBIO, &on));
#else
    assert(!fcntl(fd, F_SETFL, O_NONBLOCK));
#endif
}
static int wait_state(int h) {
    for (int i = 0; i < 1000; ++i) {
        ls_lap_net_service();
        int s = lap_tcp_state(h);
        if (s != LAP_TCP_PENDING) return s;
        pause_ms(1);
    }
    assert(!"connect failed to complete in one second"); return 0;
}
int main(void) {
#ifdef _WIN32
    WSADATA data; assert(!WSAStartup(MAKEWORD(2,2), &data));
#endif
    strcpy(app.host, "localhost");
    strcpy(app.tx, "hello from the app");
    fd_t listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    assert(listener != (fd_t)-1);
    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET; addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    assert(!bind(listener, (void *)&addr, sizeof(addr)));
    socklen_t size = sizeof(addr); assert(!getsockname(listener, (void *)&addr, &size));
    assert(!listen(listener, 2)); nonblock(listener);
    int port = ntohs(addr.sin_port);
    ls_lap_net_begin(false);
    assert(lap_tcp_connect(app.host, port, 1000) == LAP_NET_EACCES);
    ls_lap_net_begin(true);
#ifdef LS_LAP_NET_TEST_DNS
    /* A late resolver callback may arrive after repeated close/reopen, or
       after a timeout. It must never target a new app's reused slot. */
    delay_dns = true;
    for (int round = 0; round < 3; ++round) {
        int d1 = lap_tcp_connect(app.host, port, 1), d2 = lap_tcp_connect(app.host, port, 1000);
        assert(d1 >= 0 && d2 >= 0); ls_lap_net_service();
        assert(dns[0].ctx && dns[1].ctx);
        pause_ms(5); ls_lap_net_service();
        assert(lap_tcp_state(d1) == LAP_NET_ETIMEOUT);
        ls_lap_net_end(); ls_lap_net_begin(true); ls_lap_net_service();
        assert(lap_tcp_connect(app.host, port, 1000) == LAP_NET_EBUSY);
        for (int i = 0; i < 2; ++i) { dns[i].done(dns[i].ctx, i ? 0 : htonl(INADDR_LOOPBACK)); dns[i].ctx = NULL; }
        ls_lap_net_service();
        assert(lap_tcp_state(d1) == LAP_NET_EINVAL && lap_tcp_state(d2) == LAP_NET_EINVAL);
    }
    delay_dns = false;
#endif
    assert(lap_tcp_connect("foreign", port, 1000) == LAP_NET_EINVAL);
    assert(lap_tcp_connect(app.host, 0, 1000) == LAP_NET_EINVAL);
    assert(lap_tcp_connect(app.host, port, 30001) == LAP_NET_EINVAL);
    inside = false; assert(lap_tcp_connect(app.host, port, 1000) == LAP_NET_EINVAL); inside = true;
    int h = lap_tcp_connect(app.host, port, 1000); assert(h >= 0);
    assert(lap_tcp_state(h) == LAP_TCP_PENDING);
    assert(lap_tcp_send(h, app.tx, 5) == LAP_NET_EAGAIN);
    assert(wait_state(h) == LAP_TCP_CONNECTED);
    fd_t peer = accept(listener, NULL, NULL); assert(peer != (fd_t)-1); nonblock(peer);
    assert(lap_net_status(&app.status) == 0 && app.status.allowed && app.status.connected);
    assert(!strcmp(app.status.ip, "127.0.0.1"));
    char foreign[10];
    assert(lap_net_status((lap_net_status_t *)foreign) == LAP_NET_EINVAL);
    assert(lap_tcp_send(h, foreign, 1) == LAP_NET_EINVAL);
    assert(lap_tcp_recv(h, foreign, 1) == LAP_NET_EINVAL);
    assert(lap_tcp_send(h, app.tx, 4097) == LAP_NET_EINVAL);
    assert(lap_tcp_recv(h, app.rx, 10) == LAP_NET_EAGAIN);
    /* Fill the entire queue, then force a partial acceptance across its wrap. */
    for (unsigned i = 0; i < sizeof(app.tx); ++i) app.tx[i] = (char)(i % 251);
    assert(lap_tcp_send(h, app.tx, 4000) == 4000);
    assert(lap_tcp_send(h, app.tx, 200) == 96);
    assert(lap_tcp_send(h, app.tx, 1) == LAP_NET_EAGAIN);
    char received[4096]; int got = 0;
    for (int i = 0; got < 4096 && i < 1000; ++i) {
        ls_lap_net_service();
        int r = recv(peer, received + got, 4096 - got, 0);
        if (r > 0) got += r;
        pause_ms(1);
    }
    assert(got == 4096 && !memcmp(received, app.tx, 4000) && !memcmp(received + 4000, app.tx, 96));
    /* Several rounds prove both rings wrap without losing or reordering bytes. */
    for (int round = 0; round < 4; ++round) {
        assert(send(peer, received, 4096, 0) == 4096);
        int total = 0;
        for (int i = 0; total < 4096 && i < 1000; ++i) {
            ls_lap_net_service();
            int r = lap_tcp_recv(h, app.rx + total, 4096 - total);
            if (r > 0) total += r; else assert(r == LAP_NET_EAGAIN);
            pause_ms(1);
        }
        assert(total == 4096 && !memcmp(received, app.rx, 4096));
    }
    /* Peer closes with unread data: drain data first, then EOF. */
    assert(send(peer, "bye", 3, 0) == 3); close_fd(peer);
    for (int i = 0; i < 1000 && lap_tcp_state(h) >= 0; ++i) { ls_lap_net_service(); pause_ms(1); }
    assert(lap_tcp_state(h) == LAP_NET_EOF);
    assert(lap_tcp_recv(h, app.rx, 4096) == 3 && !memcmp(app.rx, "bye", 3));
    assert(lap_tcp_recv(h, app.rx, 4096) == 0);
    lap_tcp_close(h); assert(lap_tcp_state(h) == LAP_NET_EINVAL); ls_lap_net_service();
    int timed = lap_tcp_connect(app.host, port, 1); assert(timed >= 0);
    pause_ms(5); ls_lap_net_service(); assert(lap_tcp_state(timed) == LAP_NET_ETIMEOUT);
    lap_tcp_close(timed); ls_lap_net_service();
    int a = lap_tcp_connect(app.host, port, 1000), b = lap_tcp_connect(app.host, port, 1000);
    assert(a >= 0 && b >= 0 && a != b);
    assert(lap_tcp_connect(app.host, port, 1000) == LAP_NET_EBUSY);
    ls_lap_net_end();
    assert(lap_tcp_state(a) == LAP_NET_EINVAL && lap_tcp_state(b) == LAP_NET_EINVAL);
    ls_lap_net_begin(true); ls_lap_net_service();
    int newer = lap_tcp_connect(app.host, port, 1000); assert(newer >= 0 && newer != a && newer != b);
    assert(lap_tcp_state(a) == LAP_NET_EINVAL);
    /* Closing with an established socket closes the actual peer, too. */
    assert(wait_state(newer) == LAP_TCP_CONNECTED);
    peer = accept(listener, NULL, NULL); assert(peer != (fd_t)-1); nonblock(peer);
    ls_lap_net_end(); ls_lap_net_service();
    assert(recv(peer, received, sizeof(received), 0) == 0);
    close_fd(peer); close_fd(listener);
#ifdef _WIN32
    WSACleanup();
#endif
    puts("LAP TCP: permission, DNS/loopback, queue bounds/wrap, EOF, timeout, stale handles and teardown passed");
    return 0;
}
