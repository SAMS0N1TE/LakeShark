/* Bounded outbound TCP for LAP. App callbacks never enter the socket stack.
 * Rings are single producer/single consumer; publishing their counters hands
 * bytes to the other task. DNS callbacks never retain app memory. */
#include "ls_lap_net.h"
#include "ls_lap_api.h"
#include <stdatomic.h>
#include <errno.h>
#include <string.h>
#include <stdio.h>
#include <time.h>
#if defined(ESP_PLATFORM) && !defined(LS_LAP_HOST_TEST)
#include "esp_timer.h"
#include "esp_attr.h"
#include "esp_netif.h"
#include "lwip/sockets.h"
#include "lwip/dns.h"
#include "lwip/tcpip.h"
#include <fcntl.h>
#define SOCKET_CLOSE close
typedef int net_fd;
#elif defined(_WIN32)
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#define SOCKET_CLOSE closesocket
typedef SOCKET net_fd;
#else
#include <sys/socket.h>
#include <netdb.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <fcntl.h>
#define SOCKET_CLOSE close
typedef int net_fd;
#endif
#ifndef EXT_RAM_BSS_ATTR
#define EXT_RAM_BSS_ATTR
#endif

#define SLOTS 2
#define RING 4096u
enum { FREE, QUEUED, RESOLVING, CONNECTING, READY };
typedef struct {
    unsigned char bytes[RING];
    atomic_uint head, tail;
} ring_t;
typedef struct {
    atomic_int state;
    atomic_bool cancel, dns_pending;
    atomic_int dns_result;
    unsigned serial, generation;
    char host[254];
    uint16_t port;
    uint32_t timeout, started;
    struct sockaddr_in addr;
    net_fd fd;                 /* worker only */
    ring_t tx, rx;
} connection;
EXT_RAM_BSS_ATTR static connection sockets[SLOTS];
static unsigned generation = 1, serial;
static bool permitted;
static atomic_uint local_ip;
static void (*wake_io)(void);
static uint32_t clock_ms(void) {
#if defined(ESP_PLATFORM) && !defined(LS_LAP_HOST_TEST)
    return (uint32_t)(esp_timer_get_time() / 1000);
#elif defined(_WIN32)
    /* Host bench only. GetTickCount moves in 10-16 ms steps, so a 1 ms
       timeout could still read 0 ms elapsed after a 5 ms wait. */
    LARGE_INTEGER f, t;
    QueryPerformanceFrequency(&f); QueryPerformanceCounter(&t);
    return (uint32_t)((uint64_t)t.QuadPart * 1000 / (uint64_t)f.QuadPart);
#else
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint32_t)((uint64_t)t.tv_sec * 1000 + t.tv_nsec / 1000000);
#endif
}
void ls_lap_net_set_wake(void (*wake)(void)) { wake_io = wake; }
void ls_lap_net_end(void) {
    permitted = false;
    ++generation;
    for (unsigned i = 0; i < SLOTS; ++i)
        if (atomic_load(&sockets[i].state) != FREE) atomic_store(&sockets[i].cancel, true);
    if (wake_io) wake_io();
}
void ls_lap_net_begin(bool allowed) {
    ls_lap_net_end();
    permitted = allowed;
}
static connection *lookup(int handle) {
    if (!ls_lap_api_inside() || !permitted || handle < 0) return NULL;
    connection *c = &sockets[(unsigned)handle % SLOTS];
    if (atomic_load(&c->state) == FREE || c->generation != generation ||
        c->serial != (unsigned)handle / SLOTS || atomic_load(&c->cancel)) return NULL;
    return c;
}
int lap_net_status(lap_net_status_t *out) {
    if (!ls_lap_api_inside() || !ls_lap_api_owns(out, sizeof(*out))) return LAP_NET_EINVAL;
    memset(out, 0, sizeof(*out));
    uint32_t ip = atomic_load(&local_ip);
    out->connected = ip != 0;
    out->allowed = permitted;
    const unsigned char *b = (const unsigned char *)&ip;
    if (ip) snprintf(out->ip, sizeof(out->ip), "%u.%u.%u.%u", b[0], b[1], b[2], b[3]);
    if (wake_io) wake_io();
    return 0;
}
int lap_tcp_connect(const char *host, int port, uint32_t timeout_ms) {
    if (!ls_lap_api_inside()) return LAP_NET_EINVAL;
    if (!permitted) return LAP_NET_EACCES;
    /* Validate each byte before reading it: no string function on a foreign pointer. */
    if (!host) return LAP_NET_EINVAL;
    size_t n = 0;
    for (; n < sizeof(sockets[0].host); ++n) {
        if (!ls_lap_api_owns(host, n + 1)) return LAP_NET_EINVAL;
        if (!host[n]) break;
        if ((unsigned char)host[n] <= 32 || (unsigned char)host[n] >= 127) return LAP_NET_EINVAL;
    }
    if (!n || n == sizeof(sockets[0].host) || port < 1 || port > 65535 ||
        timeout_ms < 1 || timeout_ms > 30000) return LAP_NET_EINVAL;
    for (unsigned i = 0; i < SLOTS; ++i) {
        connection *c = &sockets[i];
        if (atomic_load(&c->state) != FREE) continue;
        c->serial = serial = serial % 0x1fffffff + 1;
        c->generation = generation;
        memcpy(c->host, host, n + 1);
        c->port = (uint16_t)port;
        c->timeout = timeout_ms;
        c->started = clock_ms();
        atomic_store(&c->cancel, false);
        atomic_store(&c->dns_pending, false);
        atomic_store(&c->dns_result, 0);
        atomic_store(&c->tx.head, 0); atomic_store(&c->tx.tail, 0);
        atomic_store(&c->rx.head, 0); atomic_store(&c->rx.tail, 0);
        atomic_store(&c->state, QUEUED);
        if (wake_io) wake_io();
        return (int)(c->serial * SLOTS + i);
    }
    return LAP_NET_EBUSY;
}
int lap_tcp_state(int handle) {
    connection *c = lookup(handle);
    if (!c) return LAP_NET_EINVAL;
    int s = atomic_load(&c->state);
    return s == READY ? LAP_TCP_CONNECTED : s < 0 ? s : LAP_TCP_PENDING;
}
int lap_tcp_send(int handle, const void *buf, size_t len) {
    connection *c = lookup(handle);
    if (!c || len > RING || (len && !ls_lap_api_owns(buf, len))) return LAP_NET_EINVAL;
    int s = atomic_load(&c->state);
    if (s != READY) return s < 0 ? s : LAP_NET_EAGAIN;
    unsigned head = atomic_load(&c->tx.head), tail = atomic_load(&c->tx.tail);
    size_t n = RING - (head - tail);
    if (n > len) n = len;
    const unsigned char *p = buf;
    for (size_t i = 0; i < n; ++i) c->tx.bytes[(head + i) % RING] = p[i];
    atomic_store(&c->tx.head, head + (unsigned)n);
    if (wake_io && n) wake_io();
    return n ? (int)n : len ? LAP_NET_EAGAIN : 0;
}
int lap_tcp_recv(int handle, void *buf, size_t len) {
    connection *c = lookup(handle);
    if (!c || len > RING || (len && !ls_lap_api_owns(buf, len))) return LAP_NET_EINVAL;
    if (!len) return 0;
    unsigned head = atomic_load(&c->rx.head), tail = atomic_load(&c->rx.tail);
    size_t n = head - tail;
    if (n > len) n = len;
    unsigned char *p = buf;
    for (size_t i = 0; i < n; ++i) p[i] = c->rx.bytes[(tail + i) % RING];
    atomic_store(&c->rx.tail, tail + (unsigned)n);
    if (n) return (int)n;
    int s = atomic_load(&c->state);
    return s == LAP_NET_EOF ? 0 : s < 0 ? s : LAP_NET_EAGAIN;
}
void lap_tcp_close(int handle) {
    connection *c = lookup(handle);
    if (c) { atomic_store(&c->cancel, true); if (wake_io) wake_io(); }
}
static bool again(void) {
#ifdef _WIN32
    int e = WSAGetLastError();
    return e == WSAEWOULDBLOCK || e == WSAEINPROGRESS || e == WSAEALREADY;
#else
    return errno == EAGAIN || errno == EWOULDBLOCK || errno == EINPROGRESS || errno == EALREADY;
#endif
}
static void finish(connection *c, int state) {
    if (c->fd != (net_fd)-1) { SOCKET_CLOSE(c->fd); c->fd = (net_fd)-1; }
    atomic_store(&c->state, state);
}
#if (defined(ESP_PLATFORM) && !defined(LS_LAP_HOST_TEST)) || defined(LS_LAP_NET_TEST_DNS)
static void dns_publish(void *arg, uint32_t addr) {
    connection *c = arg;
    if (addr) c->addr.sin_addr.s_addr = addr;
    atomic_store(&c->dns_result, addr ? 1 : -1);
    atomic_store(&c->dns_pending, false); /* final access to this slot */
}
#endif
#if defined(ESP_PLATFORM) && !defined(LS_LAP_HOST_TEST)
/* Runs on tcpip task. Storage cannot be reused until pending becomes false. */
static void dns_done(const char *name, const ip_addr_t *ip, void *arg) {
    (void)name;
    dns_publish(arg, ip && IP_IS_V4(ip) ? ip4_addr_get_u32(ip_2_ip4(ip)) : 0);
}
static void dns_start(void *arg) {
    connection *c = arg;
    if (atomic_load(&c->cancel) || atomic_load(&c->state) != RESOLVING) {
        dns_done(NULL, NULL, c); return;
    }
    ip_addr_t ip;
    err_t e = dns_gethostbyname(c->host, &ip, dns_done, c);
    if (e != ERR_INPROGRESS) dns_done(NULL, e == ERR_OK ? &ip : NULL, c);
}
#endif
static void resolve(connection *c) {
    memset(&c->addr, 0, sizeof(c->addr));
    c->addr.sin_family = AF_INET;
    c->addr.sin_port = htons(c->port);
    atomic_store(&c->state, RESOLVING);
#if defined(ESP_PLATFORM) && !defined(LS_LAP_HOST_TEST)
    atomic_store(&c->dns_pending, true);
    if (tcpip_callback(dns_start, c) != ERR_OK) {
        atomic_store(&c->dns_result, -1);
        atomic_store(&c->dns_pending, false);
    }
#elif defined(LS_LAP_NET_TEST_DNS)
    extern void ls_lap_net_test_dns_start(const char *, void *, void (*)(void *, uint32_t));
    atomic_store(&c->dns_pending, true);
    ls_lap_net_test_dns_start(c->host, c, dns_publish);
#else
    /* Host integration harness only; production always uses async lwIP DNS. */
    struct addrinfo hints = {0}, *result = NULL;
    hints.ai_family = AF_INET; hints.ai_socktype = SOCK_STREAM;
    int e = getaddrinfo(c->host, NULL, &hints, &result);
    if (!e && result) c->addr.sin_addr = ((struct sockaddr_in *)result->ai_addr)->sin_addr;
    atomic_store(&c->dns_result, !e && result ? 1 : -1);
    if (result) freeaddrinfo(result);
#endif
}
static void connect_start(connection *c) {
    c->fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (c->fd == (net_fd)-1) { finish(c, LAP_NET_EIO); return; }
#ifdef _WIN32
    u_long on = 1;
    if (ioctlsocket(c->fd, FIONBIO, &on)) { finish(c, LAP_NET_EIO); return; }
#else
    if (fcntl(c->fd, F_SETFL, O_NONBLOCK) < 0) { finish(c, LAP_NET_EIO); return; }
#endif
    int e = connect(c->fd, (struct sockaddr *)&c->addr, sizeof(c->addr));
    atomic_store(&c->state, !e ? READY : CONNECTING);
    if (e && !again()) finish(c, LAP_NET_EIO);
}
static void transfer(connection *c) {
    unsigned tail = atomic_load(&c->tx.tail), head = atomic_load(&c->tx.head);
    unsigned n = head - tail;
    if (n > RING - tail % RING) n = RING - tail % RING;
    if (n) {
#ifdef MSG_NOSIGNAL
        int r = send(c->fd, c->tx.bytes + tail % RING, n, MSG_NOSIGNAL);
#else
        int r = send(c->fd, (const char *)c->tx.bytes + tail % RING, n, 0);
#endif
        if (r > 0) atomic_store(&c->tx.tail, tail + (unsigned)r);
        else if (r == 0 || !again()) { finish(c, LAP_NET_EIO); return; }
    }
    head = atomic_load(&c->rx.head); tail = atomic_load(&c->rx.tail);
    n = RING - (head - tail);
    if (n > RING - head % RING) n = RING - head % RING;
    if (n) {
        int r = recv(c->fd, (char *)c->rx.bytes + head % RING, n, 0);
        if (r > 0) atomic_store(&c->rx.head, head + (unsigned)r);
        else if (!r) finish(c, LAP_NET_EOF);
        else if (!again()) finish(c, LAP_NET_EIO);
    }
}
bool ls_lap_net_service(void) {
#ifdef _WIN32
    static bool initialized;
    if (!initialized) { WSADATA w; if (WSAStartup(MAKEWORD(2, 2), &w)) return false; initialized = true; }
#endif
#if defined(ESP_PLATFORM) && !defined(LS_LAP_HOST_TEST)
    esp_netif_ip_info_t info = {0};
    /* LINK creates the standard station interface; AP-only has no joined
       network and must not be presented to apps as a connected station. */
    esp_netif_t *nif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    bool up = nif && esp_netif_is_netif_up(nif) && esp_netif_get_ip_info(nif, &info) == ESP_OK;
    atomic_store(&local_ip, up ? info.ip.addr : 0);
#else
    atomic_store(&local_ip, htonl(0x7f000001));
#endif
    bool busy = false;
    for (unsigned i = 0; i < SLOTS; ++i) {
        connection *c = &sockets[i];
        int s = atomic_load(&c->state);
        if (s == FREE) continue;
        if (s == QUEUED) c->fd = (net_fd)-1;
        if (atomic_load(&c->cancel)) {
            if (!atomic_load(&c->dns_pending)) finish(c, FREE);
            busy = true; continue;
        }
        if (s < 0) continue;
        busy = true;
        if (!atomic_load(&local_ip)) { finish(c, LAP_NET_ENETDOWN); continue; }
        if (s != READY && clock_ms() - c->started >= c->timeout) { finish(c, LAP_NET_ETIMEOUT); continue; }
        if (s == QUEUED) resolve(c);
        if (atomic_load(&c->state) == RESOLVING && !atomic_load(&c->dns_pending)) {
            if (atomic_load(&c->dns_result) < 0) finish(c, LAP_NET_EDNS);
            else connect_start(c);
        }
        if (atomic_load(&c->state) == CONNECTING) {
            fd_set wf, ef; FD_ZERO(&wf); FD_ZERO(&ef); FD_SET(c->fd, &wf); FD_SET(c->fd, &ef);
            struct timeval tv = {0};
            int r = select((int)c->fd + 1, NULL, &wf, &ef, &tv);
            if (r < 0) finish(c, LAP_NET_EIO);
            else if (r) {
                int error = 0; socklen_t len = sizeof(error);
                if (getsockopt(c->fd, SOL_SOCKET, SO_ERROR, (void *)&error, &len) || error) finish(c, LAP_NET_EIO);
                else atomic_store(&c->state, READY);
            }
        }
        if (atomic_load(&c->state) == READY) transfer(c);
    }
    return busy;
}
