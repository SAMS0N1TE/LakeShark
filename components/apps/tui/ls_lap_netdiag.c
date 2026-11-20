#include "ls_lap_context.h"
/* API 2.8 network diagnostics: ping/traceroute, DNS, ARP and interface info.
 *
 * App calls only validate, copy and queue; the LAP worker owns every slot
 * transition under one try-lock. On the board ICMP goes through one lwIP raw
 * PCB living in the tcpip thread: requests are sent and answers timestamped
 * there, so a round trip is measured in microseconds although the worker
 * wakes every 20 ms. ARP requests and table reads run there too. No socket
 * descriptor is used, so apps keep their TCP/UDP budget. (ESP-IDF's
 * ping_sock was not used: it drops ICMP time-exceeded, which traceroute
 * needs, and it spends a task per session.)
 *
 * The host build answers from a fixed LAN (see lakeshark_app.h) on the
 * virtual clock, so lssim renders and bench tests are deterministic. */
#include "ls_lap_net_audio.h"
#include "ls_lap_usage.h"
#if LAP_DEVICE
#include "esp_netif.h"
#include "esp_timer.h"
#include "lwip/dns.h"
#include "lwip/etharp.h"
#include "lwip/inet_chksum.h"
#include "lwip/raw.h"
#include "lwip/tcpip.h"
#else
#include <stdlib.h>
#endif

#define PINGS 4
#define LOOKUPS 4
#define QUEUE 8       /* answers waiting per ping; also the most in flight */
#define WINDOW 16     /* sequence numbers tracked per ping */
#define PROBES 16     /* ARP requests waiting, per app */
#define CACHE 32      /* ARP pairs remembered for lap_arp_table */
#define NAME 254
#define SLOTS (LS_LAP_CONTEXTS * PINGS)

typedef struct {
    char name[NAME];
    bool asked;
    atomic_bool pending;        /* a tcpip callback still owns this resolver */
    atomic_int status;          /* 0 waiting, 1 found, -1 not found */
    atomic_uint addr;           /* host order */
    uint32_t started, timeout;
} resolver_t;

typedef struct {
    int id;
    unsigned epoch;
    int error;
    lap_ping_config_t cfg;
    resolver_t dns;
    uint32_t addr, next_at;
    uint16_t sent, base, pending, done_mask;  /* done_mask bit seq%WINDOW: answered */
    uint32_t sent_at[WINDOW];
#if !LAP_DEVICE
    lap_ping_reply_t plan[WINDOW];            /* what the fixture LAN will answer */
#endif
    lap_ping_reply_t q[QUEUE];
    unsigned qh, qt;
} ping_t;

typedef struct {
    int id;
    unsigned epoch;
    int result;                 /* LAP_IO_EAGAIN until final */
    uint32_t addr, done_at;
    resolver_t dns;
} lookup_t;

static atomic_flag lock = ATOMIC_FLAG_INIT;
static unsigned serial;
static EXT_RAM_BSS_ATTR ping_t pings[LS_LAP_CONTEXTS][PINGS];
static EXT_RAM_BSS_ATTR lookup_t lookups[LS_LAP_CONTEXTS][LOOKUPS];
static EXT_RAM_BSS_ATTR struct { uint32_t ip[PROBES]; unsigned head, tail; } probes[LS_LAP_CONTEXTS];
static LS_LAP_POOL atomic_int ping_cancel[LS_LAP_CONTEXTS][PINGS], ping_pub[LS_LAP_CONTEXTS][PINGS];
static LS_LAP_POOL atomic_int dns_cancel[LS_LAP_CONTEXTS][LOOKUPS], dns_pub[LS_LAP_CONTEXTS][LOOKUPS];
static EXT_RAM_BSS_ATTR struct { uint32_t ip, seen; uint8_t mac[6]; } cache[CACHE];
/* Interface snapshot, double-buffered: the worker writes the hidden copy. */
static EXT_RAM_BSS_ATTR lap_net_info_t info[2];
static atomic_uint info_pub;
static uint32_t info_at, touched_at, probed_at;
#if LAP_DEVICE
static bool info_valid;
#endif

/* ---- addresses ------------------------------------------------------------ */
static bool parse_ip(const char *s, uint32_t *out) {
    uint32_t a = 0;
    for (int part = 0; part < 4; part++) {
        unsigned v = 0, digits = 0;
        while (*s >= '0' && *s <= '9' && digits < 4) v = v * 10 + (unsigned)(*s++ - '0'), digits++;
        if (!digits || digits > 3 || v > 255 || (part < 3 && *s++ != '.')) return false;
        a = a << 8 | v;
    }
    if (*s) return false;
    *out = a;
    return true;
}
static void fmt_ip(char out[16], uint32_t a) {
    if (!a) { out[0] = 0; return; }
    snprintf(out, 16, "%u.%u.%u.%u", (unsigned)(a >> 24), (unsigned)(a >> 16 & 255), (unsigned)(a >> 8 & 255),
             (unsigned)(a & 255));
}
static bool copy_name(char *out, const char *in) {
    if (!ls_lap_av_string(out, NAME, in) || !out[0]) return false;
    for (const char *p = out; *p; p++)
        if ((unsigned char)*p <= 32 || (unsigned char)*p >= 127) return false;
    return true;
}
static const lap_net_info_t *current(void) { return &info[atomic_load(&info_pub) & 1]; }
static uint32_t own_ip(uint32_t *mask) {
    const lap_net_info_t *i = current();
    uint32_t a = 0, m = 0;
    if (!i->link_up || !parse_ip(i->ip, &a) || !parse_ip(i->mask, &m)) return 0;
    if (mask) *mask = m;
    return a;
}

/* ---- the network: board ---------------------------------------------------- */
#if LAP_DEVICE
static esp_netif_t *sta(void) { return esp_netif_get_handle_from_ifkey("WIFI_STA_DEF"); }
static void info_refresh(lap_net_info_t *o) {
    memset(o, 0, sizeof(*o));
    o->size = sizeof(*o); o->version = 1;
    esp_netif_t *n = sta();
    if (!n) return;
    o->kind = LAP_NETIF_WIFI;
    esp_netif_get_mac(n, o->mac);
    esp_netif_ip_info_t ip = {0};
    if (!esp_netif_is_netif_up(n) || esp_netif_get_ip_info(n, &ip) != ESP_OK || !ip.ip.addr) return;
    o->link_up = 1;
    fmt_ip(o->ip, lwip_ntohl(ip.ip.addr));
    fmt_ip(o->mask, lwip_ntohl(ip.netmask.addr));
    fmt_ip(o->gateway, lwip_ntohl(ip.gw.addr));
    esp_netif_dns_info_t dns = {0};
    if (esp_netif_get_dns_info(n, ESP_NETIF_DNS_MAIN, &dns) == ESP_OK && dns.ip.type == ESP_IPADDR_TYPE_V4)
        fmt_ip(o->dns, lwip_ntohl(dns.ip.u_addr.ip4.addr));
}

/* tcpip thread: DNS. The resolver's storage outlives the callback. */
static void dns_done(const char *name, const ip_addr_t *ip, void *arg) {
    (void)name;
    resolver_t *r = arg;
    bool ok = ip && IP_IS_V4(ip);
    if (ok) atomic_store(&r->addr, lwip_ntohl(ip4_addr_get_u32(ip_2_ip4(ip))));
    atomic_store(&r->status, ok ? 1 : -1);
    atomic_store(&r->pending, false);   /* last access */
}
static void dns_start(void *arg) {
    resolver_t *r = arg;
    ip_addr_t ip;
    err_t e = dns_gethostbyname(r->name, &ip, dns_done, r);
    if (e != ERR_INPROGRESS) dns_done(NULL, e == ERR_OK ? &ip : NULL, r);
}
static bool dns_ask(resolver_t *r) {
    atomic_store(&r->pending, true);
    if (tcpip_try_callback(dns_start, r) == ERR_OK) return true;
    atomic_store(&r->pending, false);
    return false;
}

/* Worker -> tcpip: requests to send and ARP requests. tcpip -> worker:
   answers. Single producer, single consumer rings. */
typedef struct { uint32_t dst; uint16_t id, seq, size; uint8_t ttl, g; } tx_t;
typedef struct { uint32_t from, rtt_us; uint16_t id, seq; uint8_t type, code, ttl; } rx_t;
static EXT_RAM_BSS_ATTR tx_t txq[32];
static EXT_RAM_BSS_ATTR rx_t rxq[32];
static EXT_RAM_BSS_ATTR uint32_t arpq[16];
static atomic_uint tx_head, tx_tail, rx_head, rx_tail, arp_head, arp_tail;
static atomic_bool posted, want_pcb, pcb_open, snap_want, snap_ready;
static EXT_RAM_BSS_ATTR struct { uint32_t ip; uint8_t mac[6]; } snap[ARP_TABLE_SIZE];
static unsigned snap_n;
/* tcpip thread only */
static struct raw_pcb *pcb;
static EXT_RAM_BSS_ATTR uint32_t sent_us[SLOTS][WINDOW];
static EXT_RAM_BSS_ATTR uint16_t sent_key[SLOTS][WINDOW], sent_seq[SLOTS][WINDOW];

static u8_t icmp_recv(void *arg, struct raw_pcb *p_, struct pbuf *p, const ip_addr_t *addr) {
    (void)arg; (void)p_; (void)addr;
    uint8_t b[64];
    int n = pbuf_copy_partial(p, b, sizeof(b), 0);
    if (n < 28 || (b[0] >> 4) != 4) return 0;
    int ihl = (b[0] & 15) * 4;
    if (n < ihl + 8) return 0;
    const uint8_t *icmp = b + ihl, *echo = icmp;
    if (icmp[0] == 11 || icmp[0] == 3) {        /* time exceeded / unreachable: our request inside */
        const uint8_t *in = icmp + 8;
        int iihl = (in[0] & 15) * 4;
        if (n < ihl + 8 + iihl + 8 || in[9] != 1 || in[iihl] != 8) return 0;
        echo = in + iihl;
    } else if (icmp[0] != 0)
        return 0;
    uint16_t id = (uint16_t)(echo[4] << 8 | echo[5]), seq = (uint16_t)(echo[6] << 8 | echo[7]);
    unsigned g = id >> 8 & 0x7f, w = seq % WINDOW;
    if (!(id & 0x8000) || g >= SLOTS || sent_key[g][w] != id || sent_seq[g][w] != seq) return 0;
    sent_key[g][w] = 0;                          /* one answer per request */
    unsigned h = atomic_load(&rx_head);
    if (h - atomic_load(&rx_tail) < 32) {
        rxq[h % 32] = (rx_t){.from = (uint32_t)b[12] << 24 | b[13] << 16 | b[14] << 8 | b[15],
                             .rtt_us = (uint32_t)esp_timer_get_time() - sent_us[g][w], .id = id, .seq = seq,
                             .type = icmp[0] == 0 ? LAP_PING_REPLY : icmp[0] == 11 ? LAP_PING_TTL_EXCEEDED
                                                                                    : LAP_PING_UNREACHABLE,
                             .code = icmp[1], .ttl = b[8]};
        atomic_store(&rx_head, h + 1);
    }
    return 0;                                    /* not eaten: lwIP keeps its own ICMP handling */
}
static void tcpip_work(void *arg) {
    (void)arg;
    atomic_store(&posted, false);
    if (atomic_load(&want_pcb) && !pcb && (pcb = raw_new(IP_PROTO_ICMP)) != NULL) {
        raw_recv(pcb, icmp_recv, NULL);
        raw_bind(pcb, IP_ADDR_ANY);
    }
    atomic_store(&pcb_open, pcb != NULL);
    for (unsigned t = atomic_load(&tx_tail); t != atomic_load(&tx_head); atomic_store(&tx_tail, ++t)) {
        tx_t *x = &txq[t % 32];
        struct pbuf *p = pcb ? pbuf_alloc(PBUF_IP, 8 + x->size, PBUF_RAM) : NULL;
        if (!p) continue;                        /* it times out */
        uint8_t *d = p->payload;
        d[0] = 8; d[1] = d[2] = d[3] = 0;
        d[4] = x->id >> 8; d[5] = x->id; d[6] = x->seq >> 8; d[7] = x->seq;
        for (unsigned i = 0; i < x->size; i++) d[8 + i] = (uint8_t)('a' + i % 23);
        uint16_t sum = inet_chksum(d, 8 + x->size);
        memcpy(d + 2, &sum, 2);
        ip_addr_t dst = IPADDR4_INIT(lwip_htonl(x->dst));
        pcb->ttl = x->ttl;
        unsigned w = x->seq % WINDOW;
        sent_key[x->g][w] = x->id; sent_seq[x->g][w] = x->seq;
        sent_us[x->g][w] = (uint32_t)esp_timer_get_time();
        raw_sendto(pcb, p, &dst);
        pbuf_free(p);
    }
    struct netif *nif = netif_default;
    bool up = nif && netif_is_up(nif) && !ip4_addr_isany_val(*netif_ip4_addr(nif));
    for (unsigned t = atomic_load(&arp_tail); t != atomic_load(&arp_head); atomic_store(&arp_tail, ++t)) {
        ip4_addr_t ip;
        ip4_addr_set_u32(&ip, lwip_htonl(arpq[t % 16]));
        if (up && ip4_addr_netcmp(&ip, netif_ip4_addr(nif), netif_ip4_netmask(nif))) etharp_request(nif, &ip);
    }
    if (atomic_load(&snap_want) && !atomic_load(&snap_ready)) {
        snap_n = 0;
        for (size_t i = 0; i < ARP_TABLE_SIZE; i++) {
            ip4_addr_t *ip;
            struct netif *on;
            struct eth_addr *mac;
            if (etharp_get_entry(i, &ip, &on, &mac) && on == nif) {
                snap[snap_n].ip = lwip_ntohl(ip4_addr_get_u32(ip));
                memcpy(snap[snap_n++].mac, mac->addr, 6);
            }
        }
        atomic_store(&snap_want, false);
        atomic_store(&snap_ready, true);
    }
    if (!atomic_load(&want_pcb) && pcb) { raw_remove(pcb); pcb = NULL; }
    atomic_store(&pcb_open, pcb != NULL);
}
static void post(void) {
    if (!atomic_exchange(&posted, true) && tcpip_try_callback(tcpip_work, NULL) != ERR_OK)
        atomic_store(&posted, false);
}
static bool send_request(ping_t *p, unsigned g, uint16_t seq) {
    unsigned h = atomic_load(&tx_head);
    if (!atomic_load(&pcb_open) || h - atomic_load(&tx_tail) >= 32) return false;
    txq[h % 32] = (tx_t){.dst = p->addr, .id = (uint16_t)(0x8000u | g << 8 | ((unsigned)p->id / PINGS & 0xff)),
                         .seq = seq, .size = p->cfg.payload_size, .ttl = p->cfg.ttl, .g = (uint8_t)g};
    atomic_store(&tx_head, h + 1);
    return true;
}
static bool arp_send(uint32_t ip) {
    unsigned h = atomic_load(&arp_head);
    if (h - atomic_load(&arp_tail) >= 16) return false;
    arpq[h % 16] = ip;
    atomic_store(&arp_head, h + 1);
    return true;
}

/* ---- the network: the simulator's fixed LAN -------------------------------- */
#else
static bool sim_down, sim_dnsfail;
static bool info_valid;
void ls_lap_netdiag_script(const char *c) {
    if (!strncmp(c, "netdown=", 8)) sim_down = atoi(c + 8) != 0, info_valid = false;
    if (!strncmp(c, "dnsfail=", 8)) sim_dnsfail = atoi(c + 8) != 0;
}
#define LAN 0xC0000200u             /* 192.0.2.0/24 */
static const struct { uint8_t host, mac[6]; } lan[] = {
    {1, {0x50, 0xC7, 0xBF, 0x4C, 0x53, 0x01}},    /* gateway */
    {3, {0x24, 0x0A, 0xC4, 0x4C, 0x53, 0x03}},    /* sim.local, the mDNS fixture */
    {10, {0xDC, 0xA6, 0x32, 0x4C, 0x53, 0x0A}},
    {23, {0x00, 0x17, 0x88, 0x4C, 0x53, 0x17}},
    {42, {0x00, 0x11, 0x32, 0x4C, 0x53, 0x2A}},
};
static const uint32_t routers[3] = {LAN | 1, 0xCB007101u, 0xCB007109u};   /* .1, 203.0.113.1, .9 */
static const uint8_t *lan_mac(uint32_t a) {
    for (unsigned i = 0; i < sizeof(lan) / sizeof(lan[0]); i++)
        if (a == (LAN | lan[i].host)) return lan[i].mac;
    return NULL;
}
static void sim_seen(uint32_t a, uint32_t now);
static void info_refresh(lap_net_info_t *o) {
    memset(o, 0, sizeof(*o));
    o->size = sizeof(*o); o->version = 1;
    if (!(ls_lap_sys_caps() & LAP_HW_WIFI)) return;
    o->kind = LAP_NETIF_WIFI;
    memcpy(o->mac, (const uint8_t[]){0x02, 0x4C, 0x53, 0x00, 0x00, 0x02}, 6);
    if (sim_down) return;
    o->link_up = 1;
    strcpy(o->ip, "192.0.2.2"); strcpy(o->mask, "255.255.255.0");
    strcpy(o->gateway, "192.0.2.1"); strcpy(o->dns, "192.0.2.1");
}
static bool dns_ask(resolver_t *r) {
    uint32_t a = 0, h = 2166136261u;
    for (const char *p = r->name; *p; p++) h = (h ^ (uint8_t)*p) * 16777619u;
    size_t n = strlen(r->name);
    bool fail = sim_dnsfail || (n >= 8 && !strcmp(r->name + n - 8, ".invalid"));
    if (!strcmp(r->name, "localhost")) a = 0x7F000001u;
    else if (!strcmp(r->name, "gateway.lan") || !strcmp(r->name, "router.lan")) a = LAN | 1;
    else if (!strcmp(r->name, "sim.local")) a = LAN | 3;
    else a = 0xC6336400u | (10 + h % 200);             /* 198.51.100.x */
    atomic_store(&r->addr, a);
    atomic_store(&r->status, fail ? -1 : 1);
    return true;
}
static bool send_request(ping_t *p, unsigned g, uint16_t seq) {
    (void)g;
    lap_ping_reply_t *r = &p->plan[seq % WINDOW];
    memset(r, 0, sizeof(*r));
    uint32_t a = p->addr, now = ls_lap_av_now();
    unsigned hop = 0;                                   /* 0: no answer */
    if (a == 0x7F000001u) { r->rtt_us = 40; hop = 1; }
    else if ((a & 0xFFFFFF00u) == LAN) {
        if (lan_mac(a)) { r->rtt_us = 900 + (a & 255) * 53 + seq % 5 * 90; hop = 1; sim_seen(a, now); }
        else if (a == (LAN | 2)) { r->rtt_us = 60; hop = 1; }
    } else if ((a & 0xFFFFFF00u) != 0xCB007100u || a == routers[1] || a == routers[2]) {
        hop = 4;
        sim_seen(LAN | 1, now);
        for (unsigned i = 1; i < 3; i++) if (a == routers[i]) hop = i + 1;
        if (p->cfg.ttl < hop) {
            hop = p->cfg.ttl;
            r->type = LAP_PING_TTL_EXCEEDED;
            a = routers[hop - 1];
        }
        r->rtt_us = 1100 + hop * 4200 + seq % 3 * 170;
    }
    if (!hop) { r->size = 0; return true; }             /* a dead address: it times out */
    r->size = sizeof(*r); r->version = 1; r->seq = seq;
    r->ttl = (uint8_t)(64 - (hop - 1));
    fmt_ip(r->from, a);
    return true;
}
static bool arp_send(uint32_t ip) { sim_seen(ip, ls_lap_av_now()); return true; }
#endif

/* ---- ARP cache -------------------------------------------------------------- */
static void cache_put(uint32_t ip, const uint8_t mac[6], uint32_t now) {
    int at = -1;
    for (int i = 0; i < CACHE && at < 0; i++) if (cache[i].ip == ip) at = i;
    for (int i = 0; i < CACHE && at < 0; i++) if (!cache[i].ip) at = i;
    if (at < 0) {
        at = 0;
        for (int i = 1; i < CACHE; i++) if ((int32_t)(cache[i].seen - cache[at].seen) < 0) at = i;
    }
    cache[at].ip = ip; cache[at].seen = now;
    memcpy(cache[at].mac, mac, 6);
}
#if !LAP_DEVICE
static void sim_seen(uint32_t a, uint32_t now) {
    const uint8_t *m = lan_mac(a);
    if (m && !sim_down) cache_put(a, m, now);
}
#endif

/* ---- resolver ---------------------------------------------------------------- */
static void resolve_begin(resolver_t *r, uint32_t now, uint32_t timeout) {
    r->asked = false; r->started = now; r->timeout = timeout;
    atomic_store(&r->status, 0);
    uint32_t a;
    if (parse_ip(r->name, &a)) { atomic_store(&r->addr, a); atomic_store(&r->status, 1); r->asked = true; }
}
/* 1 found, -1 not found, -2 timed out, 0 waiting */
static int resolve_poll(resolver_t *r, uint32_t now) {
    if (!r->asked) r->asked = dns_ask(r);
    int s = atomic_load(&r->pending) ? 0 : atomic_load(&r->status);
    if (s) return s;
    return now - r->started >= r->timeout ? -2 : 0;
}

/* ---- the worker ---------------------------------------------------------------- */
static bool answered(const ping_t *p, uint16_t seq) { return p->done_mask >> (seq % WINDOW) & 1; }
static void answer(ping_t *p, const lap_ping_reply_t *r) {
    if (r->seq < p->base || r->seq >= p->sent || answered(p, r->seq) || p->qh - p->qt >= QUEUE) return;
    p->done_mask |= (uint16_t)(1u << r->seq % WINDOW);
    p->pending--;
    p->q[p->qh++ % QUEUE] = *r;
}
static void ping_step(ping_t *p, unsigned g, uint32_t now, bool up) {
    (void)g;
    if (p->error) return;
    if (!up) { p->error = LAP_IO_ENETDOWN; return; }
    if (!p->addr) {
        int s = resolve_poll(&p->dns, now);
        if (s < 0) p->error = LAP_IO_ENOTFOUND;
        if (s != 1) return;
        p->addr = atomic_load(&p->dns.addr);
        p->next_at = now;
    }
#if !LAP_DEVICE
    for (uint16_t s = p->base; s < p->sent; s++) {
        lap_ping_reply_t *r = &p->plan[s % WINDOW];
        if (r->size && !answered(p, s) && now - p->sent_at[s % WINDOW] >= (r->rtt_us + 999) / 1000) answer(p, r);
    }
#endif
    for (uint16_t s = p->base; s < p->sent; s++)
        if (!answered(p, s) && now - p->sent_at[s % WINDOW] >= p->cfg.timeout_ms)
            answer(p, &(lap_ping_reply_t){.size = sizeof(lap_ping_reply_t), .version = 1, .seq = s,
                                          .type = LAP_PING_TIMEOUT});
    while (p->base < p->sent && answered(p, p->base)) p->done_mask &= (uint16_t)~(1u << p->base++ % WINDOW);
    while (p->sent < p->cfg.count && (int32_t)(now - p->next_at) >= 0 && p->qh - p->qt + p->pending < QUEUE &&
           p->sent - p->base < WINDOW && send_request(p, g, p->sent)) {
        p->sent_at[p->sent % WINDOW] = now;
        p->done_mask &= (uint16_t)~(1u << p->sent % WINDOW);
        p->sent++; p->pending++;
        p->next_at = now + p->cfg.interval_ms;
    }
}
static void lookup_step(lookup_t *l, uint32_t now, bool up) {
    if (l->result != LAP_IO_EAGAIN) return;
    int s = up ? resolve_poll(&l->dns, now) : -3;
    if (!s) return;
    l->result = s == 1 ? 0 : s == -1 ? LAP_IO_ENOTFOUND : s == -2 ? LAP_IO_ETIMEOUT : LAP_IO_ENETDOWN;
    l->addr = atomic_load(&l->dns.addr);
    l->done_at = now;
}
bool ls_lap_netdiag_service(void) {
    if (!av_take(&lock)) return true;
    uint32_t now = ls_lap_av_now();
    bool busy = false, pinging = false;
    if (!info_valid || now - info_at >= 500) {
        unsigned hidden = (atomic_load(&info_pub) + 1) & 1;
        info_refresh(&info[hidden]);
        atomic_store(&info_pub, hidden);
        info_at = now; info_valid = true;
    }
    bool up = current()->link_up;
#if LAP_DEVICE
    for (unsigned t = atomic_load(&rx_tail); t != atomic_load(&rx_head); atomic_store(&rx_tail, ++t)) {
        rx_t *x = &rxq[t % 32];
        unsigned g = x->id >> 8 & 0x7f;
        ping_t *p = &pings[g / PINGS][g % PINGS];
        if (p->id && ((unsigned)p->id / PINGS & 0xff) == (x->id & 0xffu)) {
            lap_ping_reply_t r = {.size = sizeof(r), .version = 1, .seq = x->seq, .type = x->type,
                                  .ttl = x->ttl, .rtt_us = x->rtt_us ? x->rtt_us : 1, .code = x->code};
            fmt_ip(r.from, x->from);
            answer(p, &r);
        }
    }
#endif
    for (unsigned c = 0; c < LS_LAP_CONTEXTS; c++) {
        unsigned epoch = atomic_load(&ls_lap_av_epochs[c]);
        for (unsigned i = 0; i < PINGS; i++) {
            ping_t *p = &pings[c][i];
            if (!p->id) continue;
            if (p->epoch != epoch || atomic_load(&ping_cancel[c][i]) == p->id) {
                if (atomic_load(&p->dns.pending)) { busy = true; continue; }
                atomic_store(&ping_pub[c][i], 0);
                p->id = 0;
                continue;
            }
            ping_step(p, c * PINGS + i, now, up);
            pinging = pinging || p->addr;
            busy = true;
        }
        for (unsigned i = 0; i < LOOKUPS; i++) {
            lookup_t *l = &lookups[c][i];
            if (!l->id) continue;
            bool gone = l->epoch != epoch || atomic_load(&dns_cancel[c][i]) == l->id ||
                        (l->result != LAP_IO_EAGAIN && now - l->done_at > 10000);
            if (gone) {
                if (atomic_load(&l->dns.pending)) { busy = true; continue; }
                atomic_store(&dns_pub[c][i], 0);
                l->id = 0;
                continue;
            }
            lookup_step(l, now, up);
            busy = true;
        }
        if (probes[c].head != probes[c].tail) touched_at = probed_at = now;
        while (probes[c].head != probes[c].tail && arp_send(probes[c].ip[probes[c].tail % PROBES]))
            probes[c].tail++;
    }
    for (int i = 0; i < CACHE; i++)
        if (cache[i].ip && now - cache[i].seen > 300000) cache[i].ip = 0;
#if LAP_DEVICE
    bool watching = pinging || now - touched_at < 60000;
    if (atomic_load(&snap_ready)) {
        for (unsigned i = 0; i < snap_n; i++) cache_put(snap[i].ip, snap[i].mac, now);
        atomic_store(&snap_ready, false);
    }
    static uint32_t snap_at;
    /* The stack's table holds ten pairs: copy it often while a sweep's answers arrive. */
    if (watching && up && now - snap_at >= (now - probed_at < 2000 ? 100u : 500u) && !atomic_load(&snap_want)) {
        snap_at = now;
        atomic_store(&snap_want, true);
    }
    atomic_store(&want_pcb, pinging);
    if (atomic_load(&tx_head) != atomic_load(&tx_tail) || atomic_load(&arp_head) != atomic_load(&arp_tail) ||
        atomic_load(&snap_want) || pinging != atomic_load(&pcb_open))
        post();
    busy = busy || watching;
#else
    (void)pinging;
#endif
    av_give(&lock);
    return busy;
}

/* ---- the app's calls ------------------------------------------------------------- */
int lap_net_info(int iface, lap_net_info_t *out) {
    int e = ls_lap_av_access(0, true);
    if (e) return e;
    if (!ls_lap_api_readable(out, sizeof(*out)) || iface < LAP_NETIF_ACTIVE || iface > LAP_NETIF_ETH)
        return LAP_IO_EINVAL;
    ls_lap_av_wake();
    if (iface == LAP_NETIF_ETH) {
        memset(out, 0, sizeof(*out));
        out->size = sizeof(*out); out->version = 1;
        return LAP_IO_ENOTSUP;
    }
    *out = *current();
    out->size = sizeof(*out); out->version = 1;
    return iface == LAP_NETIF_WIFI && out->kind != LAP_NETIF_WIFI ? LAP_IO_ENOTSUP : 0;
}
int lap_ping_start(const char *host, const lap_ping_config_t *cfg) {
    int e = ls_lap_av_access(0, true);
    if (e) return e;
    lap_ping_config_t c = {.size = sizeof(c), .version = 1};
    if (cfg) {
        if (!ls_lap_api_readable(cfg, sizeof(*cfg))) return LAP_IO_EINVAL;
        c = *cfg;
    }
    if (c.size != sizeof(c) || c.version != 1 || c.reserved || c.reserved2) return LAP_IO_EINVAL;
    if (!c.count) c.count = 4;
    if (!c.interval_ms) c.interval_ms = 1000;
    if (!c.timeout_ms) c.timeout_ms = 1000;
    if (!c.ttl) c.ttl = 64;
    if (!c.payload_size) c.payload_size = 32;
    if (c.count > 1000 || c.interval_ms < 20 || c.interval_ms > 10000 || c.timeout_ms < 100 ||
        c.timeout_ms > 10000 || c.payload_size > 1400)
        return LAP_IO_EINVAL;
    char name[NAME];
    if (!copy_name(name, host)) return LAP_IO_EINVAL;
    if (!av_take(&lock)) return LAP_IO_EBUSY;
    unsigned ctx = ls_lap_context_index();
    int h = LAP_IO_EBUSY;
    for (unsigned i = 0; i < PINGS; i++) {
        ping_t *p = &pings[ctx][i];
        if (p->id) continue;
        memset(p, 0, sizeof(*p));
        serial = serial % 0xfffffff + 1;
        p->id = h = (int)(serial * PINGS + i);
        p->epoch = atomic_load(&ls_lap_av_epochs[ctx]);
        p->cfg = c;
        memcpy(p->dns.name, name, sizeof(name));
        resolve_begin(&p->dns, ls_lap_av_now(), 5000);
        atomic_store(&ping_pub[ctx][i], h);
        break;
    }
    av_give(&lock);
    ls_lap_av_wake();
    return h;
}
static ping_t *ping_of(int h, unsigned ctx) {
    if (h <= 0) return NULL;
    ping_t *p = &pings[ctx][(unsigned)h % PINGS];
    return p->id == h && p->epoch == atomic_load(&ls_lap_av_epochs[ctx]) &&
                   atomic_load(&ping_cancel[ctx][(unsigned)h % PINGS]) != h
               ? p
               : NULL;
}
int lap_ping_next(int h, lap_ping_reply_t *out) {
    int e = ls_lap_av_access(0, true);
    if (e) return e;
    if (!ls_lap_api_readable(out, sizeof(*out))) return LAP_IO_EINVAL;
    if (!av_take(&lock)) return LAP_IO_EAGAIN;
    ping_t *p = ping_of(h, ls_lap_context_index());
    int r = LAP_IO_ECLOSED;
    if (p) {
        if (p->qh != p->qt) { *out = p->q[p->qt++ % QUEUE]; r = 1; }
        else if (p->error) r = p->error;
        else r = p->addr && p->sent == p->cfg.count && !p->pending ? 0 : LAP_IO_EAGAIN;
    }
    av_give(&lock);
    return r;
}
void lap_ping_stop(int h) {
    if (!ls_lap_api_inside() || h <= 0) return;
    unsigned ctx = ls_lap_context_index();
    av_close(&ping_cancel[ctx][(unsigned)h % PINGS], &ping_pub[ctx][(unsigned)h % PINGS], h);
    ls_lap_av_wake();
}
int lap_dns_resolve(const char *name, uint32_t timeout_ms) {
    int e = ls_lap_av_access(0, true);
    if (e) return e;
    if (!timeout_ms) timeout_ms = 5000;
    char n[NAME];
    if (timeout_ms < 100 || timeout_ms > 10000 || !copy_name(n, name)) return LAP_IO_EINVAL;
    if (!av_take(&lock)) return LAP_IO_EBUSY;
    unsigned ctx = ls_lap_context_index();
    int h = LAP_IO_EBUSY;
    for (unsigned i = 0; i < LOOKUPS; i++) {
        lookup_t *l = &lookups[ctx][i];
        if (l->id) continue;
        memset(l, 0, sizeof(*l));
        serial = serial % 0xfffffff + 1;
        l->id = h = (int)(serial * LOOKUPS + i);
        l->epoch = atomic_load(&ls_lap_av_epochs[ctx]);
        l->result = LAP_IO_EAGAIN;
        memcpy(l->dns.name, n, sizeof(n));
        resolve_begin(&l->dns, ls_lap_av_now(), timeout_ms);
        atomic_store(&dns_pub[ctx][i], h);
        break;
    }
    av_give(&lock);
    ls_lap_av_wake();
    return h;
}
int lap_dns_result(int h, char ip[16]) {
    int e = ls_lap_av_access(0, true);
    if (e) return e;
    if (!ls_lap_api_readable(ip, 16)) return LAP_IO_EINVAL;
    if (!av_take(&lock)) return LAP_IO_EAGAIN;
    unsigned ctx = ls_lap_context_index(), i = (unsigned)h % LOOKUPS;
    lookup_t *l = h > 0 ? &lookups[ctx][i] : NULL;
    int r = LAP_IO_ECLOSED;
    if (l && l->id == h && l->epoch == atomic_load(&ls_lap_av_epochs[ctx]) && atomic_load(&dns_cancel[ctx][i]) != h) {
        r = l->result;
        if (r == 0) fmt_ip(ip, l->addr);
        if (r != LAP_IO_EAGAIN) {
            if (!atomic_load(&l->dns.pending)) {            /* free now: the next lookup may use it */
                atomic_store(&dns_pub[ctx][i], 0);
                l->id = 0;
            } else
                av_close(&dns_cancel[ctx][i], &dns_pub[ctx][i], h);
        }
    }
    av_give(&lock);
    return r;
}
int lap_arp_probe(const char *ip) {
    int e = ls_lap_av_access(0, true);
    if (e) return e;
    char text[16];
    uint32_t a, mask, self;
    if (!ls_lap_av_string(text, sizeof(text), ip) || !parse_ip(text, &a)) return LAP_IO_EINVAL;
    if (!(self = own_ip(&mask))) return LAP_IO_ENETDOWN;
    if ((a & mask) != (self & mask)) return LAP_IO_EINVAL;
    if (!av_take(&lock)) return LAP_IO_EAGAIN;
    unsigned ctx = ls_lap_context_index();
    int r = LAP_IO_EAGAIN;
    if (probes[ctx].head - probes[ctx].tail < PROBES) {
        probes[ctx].ip[probes[ctx].head++ % PROBES] = a;
        r = 0;
    }
    av_give(&lock);
    ls_lap_av_wake();
    return r;
}
int lap_arp_table(lap_arp_entry_t *out, int max) {
    int e = ls_lap_av_access(0, true);
    if (e) return e;
    if (max < 0 || (max && !ls_lap_api_readable(out, (size_t)max * sizeof(*out)))) return LAP_IO_EINVAL;
    if (max > CACHE) max = CACHE;
    if (!av_take(&lock)) return LAP_IO_EAGAIN;
    uint32_t now = ls_lap_av_now(), last = 0;
    touched_at = now;
    int n = 0;
    for (; n < max; n++) {               /* lowest address above the last one copied */
        int at = -1;
        for (int i = 0; i < CACHE; i++)
            if (cache[i].ip > last && (at < 0 || cache[i].ip < cache[at].ip)) at = i;
        if (at < 0) break;
        last = cache[at].ip;
        lap_arp_entry_t *o = &out[n];
        memset(o, 0, sizeof(*o));
        o->size = sizeof(*o); o->version = 1;
        fmt_ip(o->ip, last);
        memcpy(o->mac, cache[at].mac, 6);
        o->iface = LAP_NETIF_WIFI;
        o->age_ms = now - cache[at].seen;
    }
    av_give(&lock);
    ls_lap_av_wake();
    return n;
}

ls_lap_usage_t ls_lap_netdiag_usage(unsigned s) {
    ls_lap_usage_t v = {sizeof(pings[s]) + sizeof(lookups[s]) + sizeof(probes[s]), 0};
    for (int i = 0; i < PINGS; i++) v.handles += atomic_load(&ping_pub[s][i]) > 0;
    for (int i = 0; i < LOOKUPS; i++) v.handles += atomic_load(&dns_pub[s][i]) > 0;
    return v;
}
