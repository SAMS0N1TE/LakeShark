#include "ls_lap_context.h"
/* Worker-owned nonblocking datagrams. No socket calls on the screen task. */
#include "ls_lap_net_audio.h"
#include <errno.h>
#if LAP_DEVICE
#include "lwip/sockets.h"
#include <fcntl.h>
#define UDP_CLOSE close
typedef int udp_fd;
#elif defined(_WIN32)
#include <winsock2.h>
#include <ws2tcpip.h>
#define UDP_CLOSE closesocket
typedef SOCKET udp_fd;
#else
#include <arpa/inet.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <unistd.h>
#define UDP_CLOSE close
typedef int udp_fd;
#endif
#if !LAP_DEVICE
#include <stdlib.h>
static bool sim_drop;
void ls_lap_udp_script(const char *c) {
    if (!strncmp(c, "udpdrop=", 8))
        sim_drop = atoi(c + 8) != 0;
}
#endif
#define PACKETS 4
#define DATAGRAM 1472
static atomic_flag lock = ATOMIC_FLAG_INIT;
typedef struct {
    lap_socket_address_t addr;
    uint16_t len;
    uint8_t data[DATAGRAM];
} packet;
static EXT_RAM_BSS_ATTR struct udp {
    int id;
    unsigned epoch;
    bool opened;
    udp_fd fd;
    lap_socket_address_t local;
    lap_operation_t status;
    packet tx[PACKETS], rx[PACKETS];
    unsigned th, tt, rh, rt;
    char group[16];
    int membership;
} p4_sockets_slots[LS_LAP_CONTEXTS][4];
#define p4_sockets (p4_sockets_slots[ls_lap_context_index()])

static unsigned serial;
static LS_LAP_POOL atomic_int p4_canceled_slots[LS_LAP_CONTEXTS][4], p4_published_slots[LS_LAP_CONTEXTS][4];
#define p4_canceled (p4_canceled_slots[ls_lap_context_index()])
#define p4_published (p4_published_slots[ls_lap_context_index()])

static bool address(const lap_socket_address_t *a, bool local) {
    if (a->size != sizeof(*a) || a->version != 1 || a->reserved || !memchr(a->ip, 0, sizeof(a->ip)) ||
        (!local && !a->port))
        return false;
    struct in_addr ip;
    return (local && !a->ip[0]) || inet_pton(AF_INET, a->ip, &ip) == 1;
}
static int index_of(int h) {
    for (int i = 0; i < 4; i++)
        if (p4_sockets[i].id == h && p4_sockets[i].epoch == atomic_load(&ls_lap_av_epoch) &&
            atomic_load(&p4_canceled[i]) != h)
            return i;
    return -1;
}
int lap_udp_open(const lap_socket_address_t *local) {
    int e = ls_lap_av_access(LAP_HW_WIFI, true);
    if (e)
        return e;
    if (!ls_lap_api_readable(local, sizeof(*local)) || !address(local, true))
        return LAP_IO_EINVAL;
    if (!av_take(&lock))
        return LAP_IO_EBUSY;
    int h = LAP_IO_EBUSY;
    for (int i = 0; i < 4; i++)
        if (!p4_sockets[i].id) {
            struct udp *s = &p4_sockets[i];
            atomic_store(&p4_published[i], 0);
            memset(s, 0, sizeof(*s));
            serial = serial % 0xfffffff + 1;
            s->id = h = serial * 4 + i;
            atomic_store(&p4_published[i], h);
            s->epoch = atomic_load(&ls_lap_av_epoch);
            s->local = *local;
            s->fd = (udp_fd)-1;
            s->status = av_status(LAP_OPERATION_PENDING, 0);
            break;
        }
    av_give(&lock);
    ls_lap_av_wake();
    return h;
}
int lap_udp_send(int h, const lap_socket_address_t *to, const void *data, size_t len) {
    int e = ls_lap_av_access(LAP_HW_WIFI, true);
    if (e)
        return e;
    if (len > DATAGRAM)
        return LAP_IO_ELIMIT;
    if (!ls_lap_api_readable(to, sizeof(*to)) || !address(to, false) ||
        (len && !ls_lap_api_readable(data, len)))
        return LAP_IO_EINVAL;
    if (!av_take(&lock))
        return LAP_IO_EBUSY;
    int i = index_of(h), r = LAP_IO_ECLOSED;
    if (i >= 0) {
        struct udp *s = &p4_sockets[i];
        r = s->status.error;
        if (!r && s->th - s->tt == PACKETS)
            r = LAP_IO_EAGAIN;
        if (!r) {
            packet *p = &s->tx[s->th++ % PACKETS];
            p->addr = *to;
            p->len = len;
            if (len)
                memcpy(p->data, data, len);
            r = len;
        }
    }
    av_give(&lock);
    ls_lap_av_wake();
    return r;
}
int lap_udp_recv(int h, lap_socket_address_t *from, void *data, size_t cap) {
    int e = ls_lap_av_access(LAP_HW_WIFI, true);
    if (e)
        return e;
    if (cap > DATAGRAM || !ls_lap_api_readable(from, sizeof(*from)) ||
        (cap && !ls_lap_api_readable(data, cap)))
        return LAP_IO_EINVAL;
    if (!av_take(&lock))
        return LAP_IO_EBUSY;
    int i = index_of(h), r = LAP_IO_ECLOSED;
    if (i >= 0) {
        struct udp *s = &p4_sockets[i];
        r = s->status.error ? s->status.error : LAP_IO_EAGAIN;
        if (s->rh != s->rt) {
            packet *p = &s->rx[s->rt % PACKETS];
            r = p->len > cap ? LAP_IO_ELIMIT : p->len;
            if (r >= 0) {
                *from = p->addr;
                if (p->len)
                    memcpy(data, p->data, p->len);
                s->rt++;
            }
        }
    }
    av_give(&lock);
    return r;
}
int lap_udp_join(int h, const char *group, bool on) {
    int e = ls_lap_av_access(LAP_HW_WIFI, true);
    if (e)
        return e;
    char name[16];
    struct in_addr a;
    if (!ls_lap_av_string(name, sizeof(name), group) || inet_pton(AF_INET, name, &a) != 1 ||
        (ntohl(a.s_addr) & 0xf0000000) != 0xe0000000)
        return LAP_IO_EINVAL;
    if (!av_take(&lock))
        return LAP_IO_EBUSY;
    int i = index_of(h), r = LAP_IO_ECLOSED;
    if (i >= 0) {
        if (p4_sockets[i].membership)
            r = LAP_IO_EBUSY;
        else {
            strcpy(p4_sockets[i].group, name);
            p4_sockets[i].membership = on ? 1 : -1;
            r = 0;
        }
    }
    av_give(&lock);
    ls_lap_av_wake();
    return r;
}
int lap_udp_status(int h, lap_operation_t *out) {
    int e = ls_lap_av_access(0, true);
    if (e)
        return e;
    if (!ls_lap_api_readable(out, sizeof(*out)))
        return LAP_IO_EINVAL;
    if (!av_take(&lock))
        return LAP_IO_EBUSY;
    int i = index_of(h);
    if (i >= 0)
        *out = p4_sockets[i].status;
    av_give(&lock);
    return i < 0 ? LAP_IO_ECLOSED : 0;
}
void lap_udp_close(int h) {
    if (!ls_lap_api_inside())
        return;
    if (h > 0)
        av_close(&p4_canceled[(unsigned)h % 4], &p4_published[(unsigned)h % 4], h);
    ls_lap_av_wake();
}
static bool would_block(void) {
#ifdef _WIN32
    return WSAGetLastError() == WSAEWOULDBLOCK;
#else
    return errno == EAGAIN || errno == EWOULDBLOCK;
#endif
}
bool ls_lap_udp_service(void) {
#if defined(_WIN32) && !LAP_DEVICE
    /* UDP works even when no legacy TCP call has initialized Winsock. */
    static bool initialized;
    if (!initialized) {
        WSADATA data;
        if (WSAStartup(MAKEWORD(2, 2), &data))
            return false;
        initialized = true;
    }
#endif
    if (!av_take(&lock))
        return true;
    bool busy = false;
    unsigned epoch = atomic_load(&ls_lap_av_epoch);
    for (int i = 0; i < 4; i++)
        if (p4_sockets[i].id) {
            struct udp *s = &p4_sockets[i];
            if (s->epoch != epoch || atomic_load(&p4_canceled[i]) == s->id) {
                if (s->fd != (udp_fd)-1)
                    UDP_CLOSE(s->fd);
                atomic_store(&p4_published[i], 0);
                memset(s, 0, sizeof(*s));
                continue;
            }
            busy = true;
            if (s->status.error)
                continue;
            if (!s->opened) {
                s->fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
                if (s->fd == (udp_fd)-1) {
                    s->status.error = LAP_IO_EIO;
                    continue;
                }
                int on = 1;
                setsockopt(s->fd, SOL_SOCKET, SO_BROADCAST, (void *)&on, sizeof(on));
                unsigned char ttl = 1;
                setsockopt(s->fd, IPPROTO_IP, IP_MULTICAST_TTL, (void *)&ttl, sizeof(ttl));
#ifdef _WIN32
                u_long nb = 1;
                int r = ioctlsocket(s->fd, FIONBIO, &nb);
#else
                int r = fcntl(s->fd, F_SETFL, O_NONBLOCK);
#endif
                struct sockaddr_in a = {0};
                a.sin_family = AF_INET;
                a.sin_port = htons(s->local.port);
                if (s->local.ip[0])
                    inet_pton(AF_INET, s->local.ip, &a.sin_addr);
                if (r || bind(s->fd, (void *)&a, sizeof(a))) {
                    s->status.error = LAP_IO_EIO;
                    continue;
                }
                s->opened = true;
                s->status.state = LAP_OPERATION_READY;
            }
            if (s->membership) {
                struct ip_mreq req = {0};
                inet_pton(AF_INET, s->group, &req.imr_multiaddr);
                if (setsockopt(s->fd, IPPROTO_IP, s->membership > 0 ? IP_ADD_MEMBERSHIP : IP_DROP_MEMBERSHIP,
                               (void *)&req, sizeof(req)))
                    s->status.error = LAP_IO_EIO;
                s->membership = 0;
            }
#if !LAP_DEVICE
            if (sim_drop && s->th != s->tt) {
                s->tt++;
                s->status.dropped++;
            } else
#endif
                if (s->th != s->tt) {
                packet *p = &s->tx[s->tt % PACKETS];
                struct sockaddr_in a = {0};
                a.sin_family = AF_INET;
                a.sin_port = htons(p->addr.port);
                inet_pton(AF_INET, p->addr.ip, &a.sin_addr);
                int n = sendto(s->fd, (char *)p->data, p->len, 0, (void *)&a, sizeof(a));
                if (n >= 0) {
                    s->status.transferred += n;
                    s->tt++;
                } else if (!would_block())
                    s->status.error = LAP_IO_EIO;
            }
            /* Keep consuming even when full so overflow is visible. */
            packet *p = &s->rx[s->rh % PACKETS];
            struct sockaddr_in a;
            socklen_t size = sizeof(a);
            uint8_t buf[DATAGRAM + 1];
            int n = recvfrom(s->fd, (char *)buf, sizeof(buf), 0, (void *)&a, &size);
            if (n >= 0) {
                if (s->rh - s->rt == PACKETS || n > DATAGRAM)
                    s->status.dropped++;
                else {
                    p->len = n;
                    if (n)
                        memcpy(p->data, buf, n);
                    p->addr = (lap_socket_address_t){
                        .size = sizeof(p->addr), .version = 1, .port = ntohs(a.sin_port)};
                    inet_ntop(AF_INET, &a.sin_addr, p->addr.ip, sizeof(p->addr.ip));
                    s->rh++;
                }
            } else if (!would_block())
                s->status.error = LAP_IO_EIO;
        }
    av_give(&lock);
    return busy;
}

#include "ls_lap_usage.h"
ls_lap_usage_t ls_lap_udp_usage(unsigned s) {
    ls_lap_usage_t v={sizeof(p4_sockets_slots[s]),0};
    for(int i=0;i<4;i++) v.handles+=atomic_load(&p4_published_slots[s][i])>0;
    return v;
}
