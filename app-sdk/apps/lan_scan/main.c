/* LAN SCAN: who is on this network.
   The subnet comes from lap_net_info. An ARP sweep finds every host that is
   switched on, a ping sweep adds round trips (and hosts behind a router that
   answers for them), mDNS and SSDP add names. A chosen host gets a rate
   limited TCP connect scan of 30 common ports. The list saves as CSV to the
   card (apps_data/lan_scan/). API 2.8.

   The handheld shows two rows per host; a wide screen (90 columns or more)
   shows one row per host with every column. The list follows the finger;
   the board's keys sit along the bottom. */
#include <lakeshark_app.h>
#include "lakeshark_ui.h"
#include "lap_scroll.h"
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

LAP_ICON_PX("....................", "....................", "........cccc........", "......cc....cc......",
            ".....c..bbbb..c.....", "....c..b....b..c....", "....c.b..ff..b.c....", "....c.b..ff..b.c....",
            "....c..b....b..c....", ".....c..bbbb..c.....", "......cc....cc......", "........cccc..e.....",
            ".............eee....", "..............eee...", "...............eee..", "................ee..",
            "....................", "....................");

#define MAXH 256
#define SWEEP_PINGS 4
#define NPORTS 30
#define SCAN_PAR 3          /* connects in flight; the SDK allows six */
#define SCAN_GAP_MS 60      /* between new connects */

enum { F_ARP = 1, F_PING = 2, F_MDNS = 4, F_SSDP = 8, F_SELF = 16, F_GW = 32 };
enum { P_NONE, P_OPEN, P_CLOSED, P_FILTERED, P_TRYING };
typedef struct {
    uint32_t ip, rtt_us;
    uint32_t found_ms;          /* when it joined the list, for the branch's flash */
    uint8_t mac[6], flags, scanned;
    char name[40], ssdp[48];
    uint8_t port[NPORTS];
} host_t;

static const uint16_t ports[NPORTS] = {21, 22, 23, 25, 53, 80, 110, 135, 139, 143, 443, 445, 548, 554, 631,
                                       993, 995, 1883, 3306, 3389, 5000, 5432, 5900, 6379, 8000, 8008, 8080,
                                       8443, 9100, 32400};
static const char *const port_names[NPORTS] = {
    "ftp", "ssh", "telnet", "smtp", "dns", "http", "pop3", "msrpc", "netbios", "imap", "https", "smb",
    "afp", "rtsp", "ipp", "imaps", "pop3s", "mqtt", "mysql", "rdp", "upnp", "postgres", "vnc", "redis",
    "http-alt", "cast", "http-alt", "https-alt", "printer", "plex"};

/* A small OUI subset: common home, lab and maker gear. */
static const struct { uint8_t p[3]; const char *name; } ouis[] = {
    {{0xB8, 0x27, 0xEB}, "Raspberry Pi"}, {{0xDC, 0xA6, 0x32}, "Raspberry Pi"}, {{0xE4, 0x5F, 0x01}, "Raspberry Pi"},
    {{0x24, 0x0A, 0xC4}, "Espressif"},    {{0x30, 0xAE, 0xA4}, "Espressif"},    {{0xA4, 0xCF, 0x12}, "Espressif"},
    {{0x84, 0xF3, 0xEB}, "Espressif"},    {{0x60, 0x01, 0x94}, "Espressif"},    {{0x50, 0xC7, 0xBF}, "TP-Link"},
    {{0xF8, 0x1A, 0x67}, "TP-Link"},      {{0xB4, 0xFB, 0xE4}, "Ubiquiti"},     {{0x24, 0xA4, 0x3C}, "Ubiquiti"},
    {{0xFC, 0xEC, 0xDA}, "Ubiquiti"},     {{0x00, 0x17, 0x88}, "Philips Hue"},  {{0x00, 0x11, 0x32}, "Synology"},
    {{0x00, 0x0E, 0x58}, "Sonos"},        {{0x18, 0xB4, 0x30}, "Nest"},         {{0xF4, 0xF5, 0xD8}, "Google"},
    {{0x3C, 0x5A, 0xB4}, "Google"},       {{0x00, 0x1B, 0x63}, "Apple"},        {{0x28, 0xCF, 0xE9}, "Apple"},
    {{0x00, 0x1E, 0xC2}, "Apple"},        {{0x00, 0x0C, 0x29}, "VMware"},       {{0x00, 0x50, 0x56}, "VMware"},
    {{0x08, 0x00, 0x27}, "VirtualBox"},   {{0x52, 0x54, 0x00}, "QEMU/KVM"},     {{0x00, 0x15, 0x5D}, "Hyper-V"},
    {{0x00, 0x1C, 0x42}, "Parallels"},    {{0x00, 0x04, 0x4B}, "NVIDIA"},       {{0x00, 0x80, 0x77}, "Brother"},
    {{0x3C, 0xD9, 0x2B}, "HP"},           {{0x00, 0x00, 0x48}, "Epson"},
};
static const char *const mdns_types[] = {"_http", "_workstation", "_ssh", "_smb", "_googlecast", "_ipp"};
#define NMDNS (int)(sizeof(mdns_types) / sizeof(mdns_types[0]))

static lap_caps_t caps;
static lap_net_info_t net;
static host_t *hosts;
static int nhosts, sel, view, dialog;
static int scroll, smax, hscroll, hsmax, clip0, clip1, kr;    /* rows; hscroll is the host page's */
static bool clipping, dragging;
static int top, py, ph;         /* under the header art: the bezel's first row and height */
static uint32_t now;            /* lap_millis() for this frame: every animation reads it */
static lap_scroll_t drag, hdrag;
static uint32_t self_ip, mask, base, span, gw_ip;
static char status[80];
static uint32_t status_at;
static int W, H;
static bool wide, waiting;
static uint32_t opened_at;

/* the sweep */
static bool scanning;
static uint32_t arp_next, ping_next, arp_done_at, scan_started, table_at;
static uint32_t *order;
static int order_n, pinged;
static struct { int h; uint32_t ip; } sweep[SWEEP_PINGS];
static int mdns_i, mdns_h, mdns_found;
static int ssdp_h, ssdp_sent, ssdp_replies;
static uint32_t ssdp_at;
/* the port scan */
static host_t *target;
static int port_next, port_open, port_done;
static struct { int h, port; uint32_t at; } conn[SCAN_PAR];
static uint32_t conn_at;

/* ---- helpers ---------------------------------------------------------------- */
static void put(int x, int y, int w, uint8_t attr, const char *fmt, ...) {
    if (w <= 0 || y < 0 || y >= H) return;
    if (clipping && (y < clip0 || y >= clip1)) return;
    char b[200];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(b, sizeof(b), fmt, ap);
    va_end(ap);
    if ((int)strlen(b) > w) b[w] = 0;
    lap_put_str(x, y, b, attr);
}
static void say(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(status, sizeof(status), fmt, ap);
    va_end(ap);
    status_at = lap_millis();
}
static bool parse_ip(const char *s, uint32_t *out) {
    uint32_t a = 0;
    for (int i = 0; i < 4; i++) {
        char *end;
        long v = strtol(s, &end, 10);
        if (end == s || v < 0 || v > 255 || (i < 3 && *end != '.')) return false;
        a = a << 8 | (uint32_t)v;
        s = end + (i < 3);
    }
    *out = a;
    return true;
}
static void ip_text(char out[16], uint32_t a) {
    snprintf(out, 16, "%u.%u.%u.%u", (unsigned)(a >> 24), (unsigned)(a >> 16 & 255), (unsigned)(a >> 8 & 255),
             (unsigned)(a & 255));
}
static bool mac_known(const host_t *h) { return h->mac[0] | h->mac[1] | h->mac[2] | h->mac[3] | h->mac[4] | h->mac[5]; }
static const char *vendor(const host_t *h) {
    if (h->flags & F_SELF) return "this board";
    if (!mac_known(h)) return "";
    for (unsigned i = 0; i < sizeof(ouis) / sizeof(ouis[0]); i++)
        if (!memcmp(h->mac, ouis[i].p, 3)) return ouis[i].name;
    return h->mac[0] & 2 ? "private MAC" : "unknown";
}
static void mac_text(char out[18], const uint8_t *m) {
    snprintf(out, 18, "%02x:%02x:%02x:%02x:%02x:%02x", m[0], m[1], m[2], m[3], m[4], m[5]);
}
static const char *label(const host_t *h) { return h->name[0] ? h->name : h->ssdp; }

/* ---- hosts ------------------------------------------------------------------- */
static host_t *host(uint32_t ip, bool add) {
    int lo = 0, hi = nhosts;
    while (lo < hi) {
        int mid = (lo + hi) / 2;
        if (hosts[mid].ip < ip) lo = mid + 1; else hi = mid;
    }
    if (lo < nhosts && hosts[lo].ip == ip) return &hosts[lo];
    if (!add || nhosts >= MAXH || (ip & mask) != (self_ip & mask)) return NULL;
    uint32_t keep = sel < nhosts ? hosts[sel].ip : 0, tip = target ? target->ip : 0;
    memmove(&hosts[lo + 1], &hosts[lo], (size_t)(nhosts - lo) * sizeof(host_t));
    memset(&hosts[lo], 0, sizeof(host_t));
    hosts[lo].ip = ip;
    hosts[lo].found_ms = lap_millis();
    nhosts++;
    if (keep && hosts[sel].ip != keep && sel + 1 < nhosts && hosts[sel + 1].ip == keep) sel++;
    if (tip) target = host(tip, false);
    return host(ip, false);
}
static void merge_arp(void) {
    lap_arp_entry_t t[32];
    int n = lap_arp_table(t, 32);
    for (int i = 0; i < n; i++) {
        uint32_t ip;
        if (!parse_ip(t[i].ip, &ip)) continue;
        host_t *h = host(ip, true);
        if (!h) continue;
        h->flags |= F_ARP;
        memcpy(h->mac, t[i].mac, 6);
    }
}

/* ---- sweep ------------------------------------------------------------------- */
static void stop_scan(void) {
    for (int i = 0; i < SWEEP_PINGS; i++) if (sweep[i].h > 0) lap_ping_stop(sweep[i].h), sweep[i].h = 0;
    if (mdns_h > 0) lap_mdns_stop(mdns_h);
    if (ssdp_h > 0) lap_udp_close(ssdp_h);
    mdns_h = ssdp_h = 0;
    scanning = false;
}
static void start_scan(void) {
    stop_scan();
    lap_net_info(LAP_NETIF_ACTIVE, &net);
    uint32_t m;
    if (!net.link_up || !parse_ip(net.ip, &self_ip) || !parse_ip(net.mask, &m)) {
        say("No network: join Wi-Fi first");
        return;
    }
    mask = m | 0xFFFFFF00u;                 /* a /24 at most: 254 addresses */
    base = self_ip & mask;
    span = ~mask + 1;
    gw_ip = 0;
    parse_ip(net.gateway, &gw_ip);
    if (nhosts && (hosts[0].ip & mask) != base) nhosts = 0;
    host_t *me = host(self_ip, true);
    if (me) { me->flags |= F_SELF; memcpy(me->mac, net.mac, 6); }
    host_t *gw = gw_ip ? host(gw_ip, true) : NULL;
    if (gw) gw->flags |= F_GW;
    if (nhosts <= 2) { sel = scroll = 0; lap_scroll_stop(&drag); }
    arp_next = 1; ping_next = 0; order_n = pinged = 0; arp_done_at = 0;
    mdns_i = mdns_found = 0; mdns_h = 0;
    ssdp_sent = ssdp_replies = 0; ssdp_at = 0;
    scan_started = lap_millis();
    scanning = true;
    lap_socket_address_t any = {.size = sizeof(any), .version = 1};
    ssdp_h = lap_udp_open(&any);
    say("Scanning %u addresses", (unsigned)(span - 2));
}
static void build_order(void) {
    order_n = 0;
    for (int i = 0; i < nhosts; i++)            /* hosts ARP found go first */
        if (!(hosts[i].flags & F_SELF)) order[order_n++] = hosts[i].ip;
    for (uint32_t a = 1; a + 1 < span; a++) {
        uint32_t ip = base | a;
        if (ip != self_ip && !host(ip, false)) order[order_n++] = ip;
    }
}
static void name_from_mdns(const lap_mdns_service_t *s) {
    uint32_t ip;
    if (!parse_ip(s->ip, &ip)) return;
    host_t *h = host(ip, true);
    if (!h) return;
    h->flags |= F_MDNS;
    if (h->name[0]) return;
    snprintf(h->name, sizeof(h->name), "%.39s", s->hostname[0] ? s->hostname : s->instance);
    char *dot = strstr(h->name, ".local");
    if (dot) *dot = 0;
    mdns_found++;
}
static void ssdp_reply(const char *from, char *text) {
    uint32_t ip;
    if (!parse_ip(from, &ip)) return;
    host_t *h = host(ip, true);
    if (!h) return;
    h->flags |= F_SSDP;
    ssdp_replies++;
    for (char *line = text, *end; *line; line = end) {
        end = strchr(line, '\n');
        if (end) *end++ = 0; else end = line + strlen(line);
        char *cr = strchr(line, '\r');
        if (cr) *cr = 0;
        if (!strncmp(line, "SERVER:", 7) || !strncmp(line, "Server:", 7) || !strncmp(line, "server:", 7)) {
            line += 7;
            while (*line == ' ') line++;
            snprintf(h->ssdp, sizeof(h->ssdp), "%.47s", line);
        }
    }
}
static void scan_step(void) {
    uint32_t now = lap_millis();
    /* ARP: a few requests a frame, so answers arrive no faster than the
       board copies the stack's ten-pair ARP table. */
    for (int n = 0; n < 4 && arp_next + 1 < span; n++) {
        char ip[16];
        ip_text(ip, base | arp_next);
        int r = lap_arp_probe(ip);
        if (r == LAP_IO_EAGAIN) break;
        arp_next++;
    }
    if (now - table_at > 250) { table_at = now; merge_arp(); }
    if (arp_next + 1 >= span && !arp_done_at) arp_done_at = now;
    /* Ping: after ARP has had a second to answer, hosts it found first. */
    if (arp_done_at && now - arp_done_at > 1000 && !order_n && !pinged) build_order();
    for (int i = 0; i < SWEEP_PINGS; i++) {
        lap_ping_reply_t r;
        if (sweep[i].h > 0) {
            int e = lap_ping_next(sweep[i].h, &r);
            if (e == 1 && r.type == LAP_PING_REPLY) {
                host_t *h = host(sweep[i].ip, true);
                if (h) { h->flags |= F_PING; h->rtt_us = r.rtt_us; }
            }
            if (e == LAP_IO_EAGAIN || e == 1) continue;
            lap_ping_stop(sweep[i].h);
            sweep[i].h = 0;
            pinged++;
        }
        if (order_n && (int)ping_next < order_n) {
            char ip[16];
            ip_text(ip, order[ping_next]);
            lap_ping_config_t c = {.size = sizeof(c), .version = 1, .count = 1, .timeout_ms = 300};
            int h = lap_ping_start(ip, &c);
            if (h > 0) { sweep[i].h = h; sweep[i].ip = order[ping_next++]; }
        }
    }
    /* mDNS, one service type at a time (each a two second snapshot). */
    if (caps.hardware & LAP_HW_MDNS) {
        if (!mdns_h && mdns_i < NMDNS) {
            mdns_h = lap_mdns_browse(mdns_types[mdns_i], "_tcp");
            if (mdns_h < 0) { mdns_h = 0; mdns_i = NMDNS; }
        } else if (mdns_h > 0) {
            lap_mdns_service_t s = {.size = sizeof(s), .version = 1};
            int e;
            while ((e = lap_mdns_next(mdns_h, &s)) == 1) name_from_mdns(&s);
            if (e != LAP_IO_EAGAIN) { lap_mdns_stop(mdns_h); mdns_h = 0; mdns_i++; }
        }
    }
    /* SSDP: M-SEARCH twice, a second apart; listen four seconds. */
    if (ssdp_h > 0) {
        if (ssdp_sent < 2 && now - ssdp_at >= 1000) {
            static const char msearch[] = "M-SEARCH * HTTP/1.1\r\nHOST: 239.255.255.250:1900\r\n"
                                          "MAN: \"ssdp:discover\"\r\nMX: 2\r\nST: ssdp:all\r\n\r\n";
            lap_socket_address_t to = {.size = sizeof(to), .version = 1, .port = 1900};
            strcpy(to.ip, "239.255.255.250");
            if (lap_udp_send(ssdp_h, &to, msearch, sizeof(msearch) - 1) > 0) { ssdp_sent++; ssdp_at = now; }
        }
        char text[1473];
        lap_socket_address_t from = {.size = sizeof(from), .version = 1};
        int n;
        while ((n = lap_udp_recv(ssdp_h, &from, text, sizeof(text) - 1)) > 0) { text[n] = 0; ssdp_reply(from.ip, text); }
        if (ssdp_sent == 2 && now - ssdp_at > 4000) { lap_udp_close(ssdp_h); ssdp_h = -1; }
    }
    bool pings_left = !order_n || (int)ping_next < order_n;
    for (int i = 0; i < SWEEP_PINGS; i++) pings_left = pings_left || sweep[i].h > 0;
    if (!pings_left && !mdns_h && (mdns_i >= NMDNS || !(caps.hardware & LAP_HW_MDNS)) && ssdp_h <= 0) {
        merge_arp();
        int alive = 0;
        for (int i = 0; i < nhosts; i++) alive += !!(hosts[i].flags & (F_ARP | F_PING | F_SELF));
        stop_scan();
        say("%d hosts in %u s", alive, (unsigned)((lap_millis() - scan_started) / 1000));
    }
}

/* ---- port scan ------------------------------------------------------------------ */
static void ports_start(host_t *h) {
    for (int i = 0; i < SCAN_PAR; i++) if (conn[i].h >= 0) lap_tcp_close(conn[i].h), conn[i].h = -1;
    target = h;
    memset(h->port, 0, sizeof(h->port));
    h->scanned = 1;
    port_next = port_open = port_done = 0;
    char ip[16];
    ip_text(ip, h->ip);
    say("Port scan of %s", ip);
}
static void ports_step(void) {
    if (!target || target->scanned != 1) return;
    uint32_t now = lap_millis();
    char ip[16];
    ip_text(ip, target->ip);
    for (int i = 0; i < SCAN_PAR; i++) {
        if (conn[i].h >= 0) {
            int s = lap_tcp_state(conn[i].h);
            if (s == LAP_TCP_PENDING) continue;
            uint8_t *p = &target->port[conn[i].port];
            *p = s == LAP_TCP_CONNECTED ? P_OPEN : s == LAP_NET_ETIMEOUT ? P_FILTERED : P_CLOSED;
            port_open += *p == P_OPEN;
            port_done++;
            lap_tcp_close(conn[i].h);
            conn[i].h = -1;
        }
        if (port_next < NPORTS && now - conn_at >= SCAN_GAP_MS) {
            int h = lap_tcp_connect(ip, ports[port_next], 1500);
            if (h < 0) break;               /* sockets busy: next frame */
            conn[i].h = h; conn[i].port = port_next; conn_at = now;
            target->port[port_next++] = P_TRYING;
        }
    }
    if (port_done == NPORTS) {
        target->scanned = 2;
        say("%s: %d of %d ports open", ip, port_open, NPORTS);
    }
}

/* ---- CSV ---------------------------------------------------------------------- */
static void save(void) {
    lap_time_t t;
    lap_time(&t);
    char name[40];
    if (t.synced) snprintf(name, sizeof(name), "scan_%04d%02d%02d_%02d%02d%02d.csv", t.year, t.month, t.day, t.hour, t.minute, t.second);
    else snprintf(name, sizeof(name), "scan_%lu.csv", (unsigned long)lap_millis());
    int fd = lap_file_open(name, "w");
    if (fd < 0) { say("Cannot write %s", name); return; }
    static const char head[] = "ip,mac,vendor,rtt_ms,name,ssdp_server,found_by,open_ports\n";
    lap_file_write(fd, head, sizeof(head) - 1);
    int rows = 0;
    for (int i = 0; i < nhosts; i++) {
        host_t *h = &hosts[i];
        char ip[16], mac[18] = "", line[320], open[160] = "", by[24] = "";
        ip_text(ip, h->ip);
        if (mac_known(h)) mac_text(mac, h->mac);
        for (int p = 0, n = 0; p < NPORTS; p++)
            if (h->port[p] == P_OPEN) n += snprintf(open + n, sizeof(open) - n, "%s%u", n ? " " : "", ports[p]);
        snprintf(by, sizeof(by), "%s%s%s%s%s", h->flags & F_SELF ? "self " : "", h->flags & F_ARP ? "arp " : "",
                 h->flags & F_PING ? "ping " : "", h->flags & F_MDNS ? "mdns " : "", h->flags & F_SSDP ? "ssdp" : "");
        for (char *c = h->ssdp; *c; c++) if (*c == '"' || *c == ',') *c = ' ';
        for (char *c = h->name; *c; c++) if (*c == '"' || *c == ',') *c = ' ';
        int n = snprintf(line, sizeof(line), "%s,%s,%s,", ip, mac, vendor(h));
        n += h->rtt_us ? snprintf(line + n, sizeof(line) - n, "%.2f", h->rtt_us / 1000.0) : 0;
        n += snprintf(line + n, sizeof(line) - n, ",%s,%s,%s,%s\n", h->name, h->ssdp, by, open);
        lap_file_write(fd, line, (size_t)n);
        rows++;
    }
    lap_file_close(fd);
    say("Saved %d hosts to %s", rows, name);
}

/* ---- drawing -------------------------------------------------------------------- */
static void rtt_text(char out[12], const host_t *h) {
    if (h->flags & F_SELF) snprintf(out, 12, "self");
    else if (h->rtt_us) snprintf(out, 12, h->rtt_us < 10000 ? "%.1f ms" : "%.0f ms", h->rtt_us / 1000.0);
    else snprintf(out, 12, h->flags & F_ARP ? "arp" : "-");
}
/* ---- art -------------------------------------------------------------------------- */
#define X0 2                    /* content sits two columns inside the bezel */
static uint8_t hue(uint8_t h) { return LAP_ATTR(h, LAP_BLACK); }
static bool vis(int y) { return y >= 0 && y < H && (!clipping || (y >= clip0 && y < clip1)); }
static void cell(int x, int y, int16_t ch, uint8_t a) { if (vis(y) && x >= 0 && x < W) lap_put_cell(x, y, ch, a); }
static bool alive(const host_t *h) { return h->flags & (F_ARP | F_PING | F_SELF); }
static int alive_n(void) { int n = 0; for (int i = 0; i < nhosts; i++) n += alive(&hosts[i]); return n; }
static void net_text(char *out, size_t cap) {
    if (net.link_up) {
        char ip[16];
        ip_text(ip, base);
        int bits = 32;
        for (uint32_t m = ~mask; m; m >>= 1) bits--;
        snprintf(out, cap, "%s/%d  me %s", ip, bits, net.ip);
    } else snprintf(out, cap, "no network");
}
/* The header: a switch whose port lights flicker while the sweep runs and
   stay lit, one per host found, when it rests; the network's facts beside. */
static void header(void) {
    const uint8_t f = hue(LAP_UI_FRAME);
    put(1, 1, 13, f, ".-----------.");
    put(1, 2, 1, f, "|");
    put(13, 2, 1, f, "|");
    put(1, 3, 13, f, "'-+-+-+-+-+-'");
    put(1, 4, 13, f, "  | | | | |  ");
    const int found = alive_n();
    for (int i = 0; i < 6; i++) {
        bool on = scanning ? ((now / 90u + (unsigned)i * 7u) * 2654435761u >> 29 & 1u) != 0 : i < found;
        cell(2 + i * 2, 2, on ? 'o' : '.', on ? hue(scanning ? LAP_UI_LIVE : LAP_UI_VALUE) : LAP_UI_FAINT);
    }
    char t[48], ip[16];
    net_text(t, sizeof(t));
    const int x = 17, w = W - x - 1;
    put(x, 1, w, net.link_up ? LAP_UI_TEXT : hue(LAP_UI_ALARM), "%s", t);
    host_t *g = gw_ip ? host(gw_ip, false) : NULL;
    if (g) { ip_text(ip, g->ip); put(x, 2, w, LAP_UI_DIM, "gw %s  %s", ip, vendor(g)); }
    else put(x, 2, w, LAP_UI_DIM, "gw %s", net.gateway[0] ? net.gateway : "-");
    int named = 0, open = 0;
    for (int i = 0; i < nhosts; i++) {
        named += label(&hosts[i])[0] != 0;
        for (int p = 0; p < NPORTS; p++) if (hosts[i].port[p] == P_OPEN) { open++; break; }
    }
    put(x, 3, w, LAP_UI_DIM, "%d up  %d named  %d with open ports", found, named, open);
    snprintf(t, sizeof(t), "%d HOSTS", found);
    int cx = lap_ui_chip(x, 4, scanning ? "SWEEP" : "READY", scanning, LAP_UI_LIVE);
    cx = lap_ui_chip(cx, 4, t, found > 0, LAP_UI_VALUE);
    if (target && target->scanned == 1) lap_ui_chip(cx, 4, "PORTS", true, LAP_UI_WARN);
}
/* The subnet as a strip, each cell a few addresses: . not asked yet, :
   asked, o a host, @ the gateway. A cursor walks it: the sweep's place while
   it runs, a slow patrol after. */
static void subnet_strip(int y) {
    const int x0 = X0 + 6, n = W - X0 - x0;
    put(X0, y, 5, LAP_UI_DIM, "SWEEP");
    const uint32_t total = span > 2 ? span - 2 : 0;
    if (n < 4) return;
    if (!total) { for (int c = 0; c < n; c++) cell(x0 + c, y, '.', LAP_UI_FAINT); return; }
    uint8_t st[256] = {0};
    const int cells = n > 256 ? 256 : n;
    for (int i = 0; i < nhosts; i++) {
        const host_t *h = &hosts[i];
        if (!alive(h) || h->ip <= base) continue;
        const int c = (int)((uint64_t)(h->ip - base - 1) * (uint64_t)cells / total);
        if (c >= 0 && c < cells) st[c] = h->flags & F_GW ? 3 : st[c] > 2 ? st[c] : 2;
    }
    uint32_t at = 0;
    if (scanning) at = arp_next + 1 < span ? arp_next : order_n && ping_next ? order[ping_next - 1] - base : arp_next;
    const int cur = scanning ? (int)((uint64_t)(at ? at - 1 : 0) * (uint64_t)cells / total)
                             : nhosts ? (int)(now / 250 % (unsigned)cells) : -1;
    for (int c = 0; c < cells; c++) {
        const uint32_t lo = 1 + (uint32_t)((uint64_t)c * total / (uint64_t)cells);
        const bool asked = !scanning ? nhosts > 0 : arp_next > lo;
        int16_t ch = st[c] == 3 ? '@' : st[c] == 2 ? 'o' : asked ? ':' : '.';
        uint8_t a = st[c] == 3 ? hue(LAP_UI_VALUE) : st[c] == 2 ? hue(LAP_UI_LIVE) : asked ? hue(LAP_UI_FRAME) : LAP_UI_FAINT;
        if (c == cur) a = scanning ? LAP_ATTR(LAP_BLACK, LAP_YELLOW) : LAP_ATTR(LAP_BLACK, LAP_CYAN);
        cell(x0 + c, y, ch, a);
    }
}
static void progress(int y) {
    char a[64];
    unsigned total = span > 2 ? span - 2 : 0;
    snprintf(a, sizeof(a), "ARP %u/%u  PING %d/%d", (unsigned)(arp_next > total ? total : arp_next - (arp_next > 0)),
             total, pinged, order_n ? order_n : (int)total - 1);
    put(X0, y, W - X0 * 2, hue(LAP_UI_FRAME), "%s", a);
    char b[48];
    if (caps.hardware & LAP_HW_MDNS) snprintf(b, sizeof(b), "mDNS %d/%d  SSDP %d", mdns_i, NMDNS, ssdp_replies);
    else snprintf(b, sizeof(b), "mDNS n/a  SSDP %d", ssdp_replies);
    if (wide) put(X0 + 1 + (int)strlen(a) + 2, y, W, hue(LAP_UI_FRAME), "%s", b);
    else put(X0, y + 1, W - X0 * 2, hue(LAP_UI_FRAME), "%s", b);
    int bar_y = wide ? y + 1 : y + 2, done = (int)(arp_next + pinged), all = (int)total * 2;
    if (all <= 0) return;
    int fill = scanning ? (W - X0 * 2) * (done > all ? all : done) / all : (nhosts ? W - X0 * 2 : 0);
    lap_fill(X0, bar_y, W - X0 * 2, 1, LAP_SHADE_25, LAP_ATTR(LAP_BLUE, LAP_BLACK));
    lap_fill(X0, bar_y, fill, 1, LAP_BLOCK, LAP_ATTR(scanning ? LAP_GREEN : LAP_CYAN, LAP_BLACK));
    /* a bright head on the bar while it moves */
    if (scanning && fill > 0 && fill < W - X0 * 2) cell(X0 + fill, bar_y, now / 150 % 2 ? LAP_SHADE_75 : LAP_SHADE_50, hue(LAP_UI_LIVE));
}
/* The router the hosts hang from; the trunk leaves its foot at X0 + 4. */
static void router(int y) {
    const uint8_t f = hue(net.link_up ? LAP_UI_FRAME : LAP_UI_DIM);
    put(X0, y, 10, f, ".========.");
    put(X0, y + 1, 1, f, "|");
    put(X0 + 1, y + 1, 6, net.link_up ? LAP_UI_TEXT : LAP_UI_DIM, "ROUTER");
    cell(X0 + 8, y + 1, !net.link_up ? 'x' : scanning && now / 120 % 2 ? '*' : 'o',
         !net.link_up ? hue(LAP_UI_ALARM) : hue(LAP_UI_LIVE));
    put(X0 + 9, y + 1, 1, f, "|");
    put(X0, y + 2, 10, f, "'===+===='");
    host_t *g = gw_ip ? host(gw_ip, false) : NULL;
    char ip[16], rtt[12];
    if (g) {
        ip_text(ip, g->ip);
        rtt_text(rtt, g);
        put(X0 + 10, y + 1, 3, f, "-- ");
        put(X0 + 13, y + 1, W - X0 - (X0 + 13), hue(LAP_UI_VALUE), "%s  %s", ip, vendor(g));
    } else put(X0 + 11, y + 1, W - X0 - (X0 + 11), LAP_UI_DIM, net.link_up ? "gateway not found yet" : "no network");
}
static int list_y, per_row;
static bool show_sel;           /* pull the selection into view on the next frame */
static void draw_list(void) {
    char ip[16], mac[18], rtt[12];
    const int fy = py + 1;
    subnet_strip(fy);
    progress(fy + 1);
    const int ry = fy + 1 + (wide ? 2 : 3);
    router(ry);
    const int tx = X0 + 4;      /* the trunk */
    uint8_t hd = hue(LAP_UI_WARN);
    if (wide) put(tx + 4, ry + 3, W - X0 - tx - 4, hd, "%-16s %-18s %-13s %8s  %-28s %s", "ADDRESS", "MAC", "VENDOR", "RTT", "NAME", "OPEN PORTS");
    else put(tx + 4, ry + 3, W - X0 - tx - 4, hd, "%-16s %-9s %s", "ADDRESS", "RTT", "VENDOR / NAME");
    put(tx, ry + 3, 1, hue(LAP_UI_FRAME), "|");
    if (nhosts) put(W - X0 - 7, ry, 7, hue(LAP_UI_VALUE), "%3d/%-3d", sel + 1, nhosts);
    /* list_y, smax and the scroll were settled in draw() */
    if (!nhosts) {
        /* nothing yet: a dangling line and a question at its end */
        cell(tx, list_y, '|', hue(LAP_UI_FRAME));
        cell(tx, list_y + 1, now / 400 % 2 ? '?' : ' ', hue(LAP_UI_WARN));
        put(tx + 3, list_y + 1, W - X0 - tx - 3, LAP_UI_TEXT, net.link_up ? "Tap SCAN to find hosts." : "Join a Wi-Fi network, then SCAN.");
        put(tx + 3, list_y + 2, W - X0 - tx - 3, LAP_UI_DIM, "Hosts branch off here as they answer.");
        return;
    }
    clipping = true;
    /* the trunk runs past rows scrolled away above */
    for (int y = clip0; y < clip1 && y < list_y + nhosts * per_row - scroll; y++) cell(tx, y, '|', hue(LAP_UI_FRAME));
    for (int i = scroll / per_row; i < nhosts; i++) {
        host_t *h = &hosts[i];
        int y = list_y + i * per_row - scroll;
        if (y >= clip1) break;
        bool on = i == sel, last = i == nhosts - 1;
        const bool fresh = now - h->found_ms < 1200u;
        uint8_t a = on ? LAP_ATTR(LAP_BLACK, LAP_CYAN) : alive(h) ? LAP_UI_TEXT : LAP_UI_DIM;
        uint8_t dim = on ? a : hue(LAP_UI_FRAME);
        for (int r = 0; r < per_row; r++)
            if (vis(y + r)) {
                lap_fill(tx + 1, y + r, W - X0 - tx - 1, 1, ' ', LAP_ATTR(LAP_WHITE, LAP_BLACK));
                if (on) lap_fill(tx + 3, y + r, W - X0 - tx - 3, 1, ' ', a);
            }
        /* the branch: +-- to the host; it flashes as the host is found */
        const uint8_t br = fresh ? hue(LAP_UI_LIVE) : alive(h) ? hue(LAP_UI_FRAME) : LAP_UI_FAINT;
        cell(tx, y, last ? '`' : '+', br);
        cell(tx + 1, y, fresh && now / 150 % 2 ? '=' : '-', br);
        cell(tx + 2, y, fresh && now / 150 % 2 ? '>' : '-', br);
        if (per_row > 1) cell(tx, y + 1, last ? ' ' : '|', hue(LAP_UI_FRAME));
        ip_text(ip, h->ip);
        rtt_text(rtt, h);
        mac_known(h) ? mac_text(mac, h->mac) : (void)strcpy(mac, "-");
        char open[64] = "";
        for (int p = 0, n = 0; p < NPORTS && n < (int)sizeof(open) - 8; p++)
            if (h->port[p] == P_OPEN) n += snprintf(open + n, sizeof(open) - n, "%s%u", n ? "," : "", ports[p]);
        const int x = tx + 4, w = W - X0 - x;
        if (wide) {
            put(x, y, w, a, "%-16s %-18s %-13.13s %8s  %-28.28s %s", ip, mac, vendor(h), rtt, label(h), open);
        } else {
            put(x, y, w, a, "%-16s %-9s %s", ip, rtt, vendor(h));
            put(x + 2, y + 1, w - 2, dim, "%s  %s%s%s", mac, label(h), open[0] ? "  open " : "", open);
        }
        if (h->flags & F_GW) put(W - X0 - 2, y, 2, on ? a : hue(LAP_UI_WARN), "GW");
    }
    /* under the last branch, what the strip's marks mean */
    const int ly = list_y + nhosts * per_row - scroll + 1;
    put(tx + 4, ly, W - X0 - tx - 4, LAP_UI_FAINT, "SWEEP  . unasked  : asked  o host  @ gw");
    clipping = false;
}
/* The host page scrolls as one piece inside the bezel: rows from clip0 - hscroll. */
static void draw_host(void) {
    host_t *h = sel < nhosts ? &hosts[sel] : NULL;
    if (!h) { view = 0; return; }
    char ip[16], mac[18], rtt[12];
    ip_text(ip, h->ip);
    rtt_text(rtt, h);
    mac_known(h) ? mac_text(mac, h->mac) : (void)strcpy(mac, "unknown");
    uint8_t k = hue(LAP_UI_FRAME), v = LAP_UI_TEXT;
    clipping = true;
    int y = clip0 - hscroll;
    struct { const char *k, *v; } rows[] = {
        {"Address", ip}, {"MAC", mac}, {"Vendor", vendor(h)[0] ? vendor(h) : "-"}, {"Round trip", rtt},
        {"mDNS name", h->name[0] ? h->name : "-"}, {"SSDP server", h->ssdp[0] ? h->ssdp : "-"},
    };
    for (unsigned i = 0; i < sizeof(rows) / sizeof(rows[0]); i++) {
        put(X0, y, 12, k, "%s", rows[i].k);
        put(X0 + 13, y++, W - X0 * 2 - 13, i ? v : hue(LAP_UI_VALUE), "%s", rows[i].v);
    }
    put(X0, y, 12, k, "Found by");
    put(X0 + 13, y++, W - X0 * 2 - 13, v, "%s%s%s%s%s%s", h->flags & F_SELF ? "self " : "", h->flags & F_GW ? "gateway " : "",
        h->flags & F_ARP ? "ARP " : "", h->flags & F_PING ? "ping " : "", h->flags & F_MDNS ? "mDNS " : "",
        h->flags & F_SSDP ? "SSDP" : "");
    y++;
    if (vis(y)) lap_hline(X0, y, W - X0 * 2, '-', hue(LAP_UI_FRAME));
    if (!h->scanned) put(X0 + 2, y, W - X0 * 2 - 4, hue(LAP_UI_WARN), " TCP PORTS: tap PORTS ");
    else put(X0 + 2, y, W - X0 * 2 - 4, hue(LAP_UI_WARN), " TCP PORTS %d/%d %s ", h == target ? port_done : NPORTS,
             NPORTS, h->scanned == 1 ? "scanning" : "done");
    y += 2;
    /* the thirty ports as a row of lamps: . not tried, ? knocking, O open, x closed, - no answer */
    put(X0, y, 6, LAP_UI_DIM, "PORTS");
    for (int i = 0; i < NPORTS; i++) {
        const uint8_t s2 = h->port[i];
        int16_t ch = s2 == P_OPEN ? 'O' : s2 == P_CLOSED ? 'x' : s2 == P_FILTERED ? '-' : s2 == P_TRYING ? (now / 120 % 2 ? '?' : ' ') : '.';
        uint8_t a = s2 == P_OPEN ? hue(LAP_UI_LIVE) : s2 == P_TRYING ? hue(LAP_UI_WARN) : s2 == P_CLOSED ? LAP_UI_DIM : LAP_UI_FAINT;
        cell(X0 + 7 + i, y, ch, a);
    }
    y += 2;
    /* columns of "port service state" */
    int colw = 24, cols = (W - X0 * 2) / colw < 1 ? 1 : (W - X0 * 2) / colw, rows_n = (NPORTS + cols - 1) / cols;
    for (int i = 0; i < NPORTS; i++) {
        int cx = X0 + (i / rows_n) * colw, cy = y + i % rows_n;
        static const char *const st[] = {"", "open", "closed", "filtered", "..."};
        uint8_t a = h->port[i] == P_OPEN ? hue(LAP_UI_LIVE) : h->port[i] == P_TRYING ? hue(LAP_UI_WARN) : LAP_UI_DIM;
        put(cx, cy, colw - 1, a, "%5u %-9s %s", ports[i], port_names[i], st[h->port[i]]);
    }
    y += rows_n;
    clipping = false;
    hsmax = y + hscroll - clip1;
    if (hsmax < 0) hsmax = 0;
}
static void open_host(void) {
    if (sel >= nhosts) return;
    view = 1;
    hscroll = 0;
    lap_scroll_stop(&hdrag);
}
static void step(int d) {
    sel += d;
    if (sel >= nhosts) sel = nhosts - 1;
    if (sel < 0) sel = 0;
    show_sel = true;
}
static void draw(void) {
    W = lap_cols(); H = lap_rows();
    wide = W >= 90;
    if (!hosts) return;
    if (dialog > 0) {
        char t[4];
        int v = 0, r = lap_dialog_poll(dialog, t, sizeof(t), &v);
        if (r) {
            lap_dialog_cancel(dialog);
            dialog = 0;
            if (r == 1 && v == 1) start_scan();
        }
    }
    if (!scanning) lap_net_info(LAP_NETIF_ACTIVE, &net);
    if (waiting && (net.link_up || lap_millis() - opened_at > 3000)) { waiting = false; start_scan(); }
    if (scanning) scan_step();
    ports_step();
    if (lap_swipe() == LAP_SWIPE_RIGHT && view) view = 0;
    /* the area between the header and the keys; the list scrolls inside it */
    kr = lap_ui_keys_rows(view ? 5 : 4);
    if (view && !wide && kr >= 7) kr += 3;     /* five keys wrap to two rows on the handheld: a finger's height each */
    clip1 = H - kr - 1;
    per_row = wide ? 1 : 2;
    /* the title; the header art on a tall screen; the bezel down to the keys.
       In the bezel the list keeps a strip, the progress, the router and a
       column header above it, and scrolls below them. */
    now = lap_millis();
    const bool art = !wide && H >= 40;
    top = art ? 6 : 1;
    py = top;
    ph = H - kr - py;
    list_y = py + 1 + 1 + (wide ? 2 : 3) + 3 + 1;
    if (view) {
        clip0 = py + 1;
        dragging = lap_scroll_follow(&hdrag, &hscroll, hsmax, clip0, clip1);
        if (hscroll > hsmax) hscroll = hsmax;
    } else {
        clip0 = list_y;
        if (sel >= nhosts) sel = nhosts ? nhosts - 1 : 0;
        const int vis = clip1 - list_y > per_row ? clip1 - list_y : per_row;
        smax = nhosts * per_row + (nhosts ? 2 : 0) - vis;      /* the legend under the list */
        if (smax < 0) smax = 0;
        dragging = lap_scroll_follow(&drag, &scroll, smax, clip0, clip1);
        if (scroll > smax) scroll = smax;
        if (show_sel && !dragging) {
            const int sy = sel * per_row;
            if (sy < scroll) scroll = sy;
            if (sy + per_row > scroll + vis) scroll = sy + per_row - vis;
        }
    }
    if (!dragging) show_sel = false;
    lap_clear(LAP_ATTR(LAP_WHITE, LAP_BLACK));
    lap_fill(0, 0, W, 1, ' ', LAP_ATTR(LAP_BLACK, LAP_CYAN));
    put(1, 0, W - 2, LAP_ATTR(LAP_BLACK, LAP_CYAN), view ? "LAN SCAN  host" : "LAN SCAN  %d hosts", nhosts);
    const char *right = scanning ? " SCANNING " : target && target->scanned == 1 ? " PORTS " : " READY ";
    put(W - (int)strlen(right) - 1, 0, 12, scanning || (target && target->scanned == 1) ? LAP_ATTR(LAP_BLACK, LAP_GREEN) : LAP_ATTR(LAP_BLACK, LAP_WHITE), "%s", right);
    if (art) header();
    char title[48], ip[16];
    if (view && sel < nhosts) { ip_text(ip, hosts[sel].ip); snprintf(title, sizeof(title), "HOST %s", ip); }
    else snprintf(title, sizeof(title), "MAP  %d hosts", alive_n());
    lap_ui_bezel(0, py, W, ph, title);
    if (view) draw_host(); else draw_list();
    const int sc = view ? hscroll : scroll, sm = view ? hsmax : smax;
    if (sc > 0) put(view ? W - 3 : W - 1, view ? py : list_y, 1, hue(LAP_UI_VALUE), "^");     /* on the frame */
    if (sc < sm) put(W - 3, py + ph - 1, 1, hue(LAP_UI_VALUE), "v");
    /* a note for six seconds over the title; a tap there clears it */
    if (status[0] && lap_millis() - status_at < 6000) {
        lap_fill(0, 0, W, 1, ' ', LAP_ATTR(LAP_BLACK, LAP_YELLOW));
        put(1, 0, W - 2, LAP_ATTR(LAP_BLACK, LAP_YELLOW), "%s", status);
    }
    if (view) {
        const lap_btn_t k[5] = {{"BACK", NULL, 0, false, false, false}, {"PREV", NULL, 0, false, sel <= 0, false},
                                {"NEXT", NULL, 0, false, sel + 1 >= nhosts, false}, {"PORTS", NULL, 'p', false, false, false},
                                {"SAVE", NULL, 'w', false, false, false}};
        lap_ui_keys(0, H - kr, W, kr, k, 5, -1);
    } else {
        const bool none = !nhosts;
        const lap_btn_t k[4] = {{scanning ? "STOP" : "SCAN", NULL, 's', scanning, false, false},
                                {"HOST", NULL, 'h', false, none, false}, {"PORTS", NULL, 'p', false, none, false},
                                {"SAVE", NULL, 'w', false, none, false}};
        lap_ui_keys(0, H - kr, W, kr, k, 4, -1);
    }
}

/* ---- input ---------------------------------------------------------------------- */
static void act(char c) {
    if (c == 's') {
        if (scanning) { stop_scan(); say("Stopped"); }
        else if (nhosts > 2 && !dialog) dialog = lap_confirm("Scan again? The list is kept and updated.");
        else start_scan();
    } else if (c == 'p' && sel < nhosts) {
        ports_start(&hosts[sel]);
        if (!view) open_host();
    } else if (c == 'w') save();
    else if (c == 'h' && sel < nhosts) open_host();
}
static bool key(lap_key_t k, char ch) {
    const int page = (clip1 - list_y) / (per_row > 0 ? per_row : 1) > 1 ? (clip1 - list_y) / (per_row > 0 ? per_row : 1) : 1;
    if (k == LAP_KEY_UP) { if (sel > 0) step(-1); return true; }
    if (k == LAP_KEY_DOWN) { if (sel + 1 < nhosts) step(1); return true; }
    if (k == LAP_KEY_PGDN) { step(page); return true; }
    if (k == LAP_KEY_PGUP) { step(-page); return true; }
    if (k == LAP_KEY_ENTER) { if (view) view = 0; else open_host(); return true; }
    if (k == LAP_KEY_BACK && view) { view = 0; return true; }
    if (k == LAP_KEY_CHAR && (ch == 's' || ch == 'p' || ch == 'w' || ch == 'h')) { act(ch); return true; }
    return false;
}
static bool touch(int col, int row) {
    if (row >= H - kr) {
        if (dragging) return true;
        int b = lap_ui_key_hit(col, row);
        if (b < 0) return true;
        if (view) {
            if (b == 0) view = 0;
            else if (b == 1 || b == 2) step(b == 1 ? -1 : 1);
            else act(b == 3 ? 'p' : 'w');
        } else act("shpw"[b]);
        return true;
    }
    if (row == 0) {
        if (status[0] && lap_millis() - status_at < 6000) status[0] = 0;
        else if (view) view = 0;
        return true;
    }
    if (row < py) return true;                  /* the header art */
    if (dragging) return true;
    if (!view && row >= list_y && row < clip1) {
        int i = (row - list_y + scroll) / per_row;
        if (i < nhosts) { if (i == sel) open_host(); else sel = i; }
        return true;
    }
    /* Every tap on this screen is the app's: one left unused reaches the
       board as UP, ENTER or DOWN by where it fell, and ENTER acts. */
    return true;
}
static void enter(void) {
    lap_caps(&caps, sizeof(caps));
    hosts = calloc(MAXH, sizeof(host_t));
    order = calloc(MAXH, sizeof(uint32_t));
    if (!hosts || !order) { lap_log("no memory"); lap_exit(); return; }
    nhosts = sel = view = dialog = 0;
    scroll = smax = hscroll = hsmax = 0;
    memset(&drag, 0, sizeof(drag));
    memset(&hdrag, 0, sizeof(hdrag));
    dragging = clipping = show_sel = false;
    kr = 1; per_row = 2; list_y = clip0 = 6;
    top = py = 1; ph = 1;
    target = NULL;
    status[0] = 0;
    for (int i = 0; i < SCAN_PAR; i++) conn[i].h = -1;
    memset(sweep, 0, sizeof(sweep));
    W = lap_cols(); H = lap_rows();
    clip1 = H - 1;
    waiting = true;                         /* the first interface snapshot takes a frame */
    opened_at = lap_millis();
}
static void leave(void) {
    stop_scan();
    for (int i = 0; i < SCAN_PAR; i++) if (conn[i].h >= 0) lap_tcp_close(conn[i].h);
    if (dialog > 0) lap_dialog_cancel(dialog);
    free(hosts); free(order);
    hosts = NULL; order = NULL;
}

LAP_APP(.id = "lan_scan", .name = "LAN SCAN", .sub = "hosts + ports", .version = "1.1.0",
        .author = "LakeShark", .icon = LAP_ICON_TOWER, .color = LAP_CYAN, .flags = LAP_CAP_NETWORK,
        .enter = enter, .leave = leave, .draw = draw, .key = key, .touch = touch);
