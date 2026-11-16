/* NET DIAG: ping, traceroute, DNS lookup, Wake-on-LAN and the interface's
   addresses, one tab each. API 2.8 (lap_ping_*, lap_dns_*, lap_net_info,
   lap_arp_table) plus a UDP broadcast for the magic packet.

   Tabs along the top (the < name > keys on the handheld, every tab in a
   row on a wide screen), the board's keys along the bottom; tap the target
   line to type a new one. Long tabs follow the finger. Every tab fills
   whatever width it is given. */
#include <lakeshark_app.h>
#include "lakeshark_ui.h"
#include "lap_scroll.h"
#include "lap_art.h"
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

LAP_ICON_PX("....................", "....................", "..ffffffffffffffff..", "..f..............f..",
            "..f.a............f..", "..f.aa...........f..", "..f.a.a......a...f..", "..f.a..a....a.a..f..",
            "..f.a...a..a...a.f..", "..f.a....aa.....af..", "..f.a............f..", "..f.aaaaaaaaaaaa.f..",
            "..f..............f..", "..ffffffffffffffff..", "........ffff........", "......ffffffff......",
            "....................", "....................");

enum { T_PING, T_TRACE, T_DNS, T_WOL, T_INFO, TABS };
static const char *const tab_names[TABS] = {"PING", "TRACE", "DNS", "WOL", "INFO"};
static int tab, W, H, dialog, dialog_for;
static int scroll, smax, clip0, clip1, kr, tbr;    /* tbr: the tab strip's rows */
static bool desk, clipping, dragging;
static int top;                     /* the row the tabs start on: under the header art when there is one */
static uint32_t now;                /* lap_millis() for this frame: every animation reads it */
static lap_wifi_status_t ws;
static lap_scroll_t drag;
static char status[80];
static uint32_t status_at;
static lap_net_info_t net;

/* ping */
#define HIST 256
static char ping_host[64];
static int ping_h, ping_sent, ping_recv;
static uint32_t ping_min, ping_max;
static uint64_t ping_sum;
static int32_t hist[HIST];          /* rtt us, -1 timeout */
static float histf[HIST * 2];           /* the same in ms for the braille graph */
static uint32_t ping_ev_ms;         /* the last reply or timeout, and its rtt, for the path animation */
static int32_t ping_ev_rtt;
static bool ping_ev;
static int hist_n;
static char ping_log[12][64];
static int log_n;
/* traceroute */
#define MAXHOP 30
#define PROBES 3
static char trace_host[64];
static struct { char from[16]; int32_t rtt[PROBES]; uint8_t n, type, code, active; } hops[MAXHOP];
static struct { int h, hop; } tr[4];
static int trace_next, trace_last;  /* next hop to probe; 0 or the destination's hop */
static bool tracing;
/* dns */
static char dns_name[64];
static struct { char name[40], ip[16]; int h, result; uint32_t at, ms; } looks[10];
static int look_n;
static uint32_t dns_ev_ms;          /* the last answer, for the reply running back */
/* wake-on-lan */
static char wol_mac[24], wol_to[24];
static int wol_h, wol_sent;
static char wol_log[6][64];
static int wol_n;
static uint32_t wol_ev_ms;          /* the last magic packet, for its trip across */
/* info */
static lap_arp_entry_t arp[32];
static int arp_n;
static uint32_t arp_at;

/* ---- helpers ------------------------------------------------------------------ */
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
static const char *err_text(int e) {
    switch (e) {
    case LAP_IO_ENOTFOUND: return "name not found";
    case LAP_IO_ENETDOWN: return "network down";
    case LAP_IO_ETIMEOUT: return "timed out";
    case LAP_IO_EACCES: return "no network permission";
    case LAP_IO_EBUSY: return "busy";
    case LAP_IO_EINVAL: return "invalid";
    default: return "error";
    }
}
static void ms_text(char *out, size_t cap, int32_t us) {
    if (us < 0) snprintf(out, cap, "*");
    else snprintf(out, cap, us < 10000 ? "%.2f" : us < 100000 ? "%.1f" : "%.0f", us / 1000.0);
}
/* Content sits inside the tab's bezel: two columns in from each side. */
#define X0 2
static uint8_t hue(uint8_t h) { return LAP_ATTR(h, LAP_BLACK); }
static bool vis(int y) { return y >= 0 && y < H && (!clipping || (y >= clip0 && y < clip1)); }
static void cell(int x, int y, int16_t ch, uint8_t a) { if (vis(y) && x >= 0 && x < W) lap_put_cell(x, y, ch, a); }
static void field(int y, const char *name, const char *value) {
    const uint8_t v = LAP_ATTR(LAP_WHITE | LAP_BRIGHT, LAP_BLUE);
    put(X0, y, 7, LAP_UI_DIM, "%s", name);
    if (vis(y)) lap_fill(X0 + 7, y, W - X0 * 2 - 7, 1, ' ', v);
    put(X0 + 8, y, W - X0 * 2 - 11, v, "%s", value[0] ? value : "(tap to set)");
    put(W - X0 - 2, y, 1, LAP_ATTR(LAP_YELLOW | LAP_BRIGHT, LAP_BLUE), ">");
}
/* A word in a three-row box, .----. | LS | '----'; returns its width. */
static int node(int x, int y, const char *word, uint8_t frame, uint8_t text) {
    char rule[40];
    int n = (int)strlen(word);
    if (n > 34) n = 34;
    memset(rule, '-', (size_t)n + 2);
    rule[n + 2] = 0;
    put(x, y, n + 4, frame, ".%s.", rule);
    put(x, y + 1, 1, frame, "|");
    put(x + 2, y + 1, n, text, "%.*s", n, word);
    put(x + n + 3, y + 1, 1, frame, "|");
    put(x, y + 2, n + 4, frame, "'%s'", rule);
    return n + 4;
}
/* A wire from a to b on row y: dashes with a bead every third cell. */
static void wire(int a, int b, int y, uint8_t attr) {
    for (int x = a; x <= b; x++) cell(x, y, (x - a) % 3 == 1 ? '.' : '-', attr);
}
static void centre(int a, int b, int y, uint8_t attr, const char *t) {
    int n = (int)strlen(t), w = b - a + 1;
    if (n > w) n = w;
    put(a + (w - n) / 2, y, n, attr, "%s", t);
}
/* x for a thing at t of 0..span along a..b */
static int along(int a, int b, int t, int span) { return span > 0 ? a + (b - a) * (t < 0 ? 0 : t > span ? span : t) / span : a; }

/* ---- ping --------------------------------------------------------------------- */
static void ping_stop(void) { if (ping_h > 0) lap_ping_stop(ping_h); ping_h = 0; }
static void ping_start(void) {
    ping_stop();
    ping_sent = ping_recv = hist_n = log_n = 0;
    ping_min = ping_max = 0; ping_sum = 0;
    ping_ev = false;
    lap_ping_config_t c = {.size = sizeof(c), .version = 1, .count = 1000, .interval_ms = 1000, .timeout_ms = 2000};
    ping_h = lap_ping_start(ping_host, &c);
    if (ping_h < 0) { say("Ping: %s", err_text(ping_h)); ping_h = 0; }
}
static void ping_poll(void) {
    lap_ping_reply_t r;
    int e = LAP_IO_EAGAIN;
    while (ping_h > 0 && (e = lap_ping_next(ping_h, &r)) == 1) {
        char line[64], ms[12];
        ping_sent++;
        int32_t v = -1;
        if (r.type == LAP_PING_REPLY) {
            ping_recv++;
            v = (int32_t)r.rtt_us;
            ping_sum += r.rtt_us;
            if (!ping_min || r.rtt_us < ping_min) ping_min = r.rtt_us;
            if (r.rtt_us > ping_max) ping_max = r.rtt_us;
            ms_text(ms, sizeof(ms), v);
            snprintf(line, sizeof(line), "seq=%-3u %-15s ttl=%-3u %s ms", r.seq, r.from, r.ttl, ms);
        } else if (r.type == LAP_PING_TIMEOUT) snprintf(line, sizeof(line), "seq=%-3u timeout", r.seq);
        else snprintf(line, sizeof(line), "seq=%-3u %-15s %s", r.seq, r.from,
                      r.type == LAP_PING_UNREACHABLE ? "unreachable" : "ttl exceeded");
        ping_ev = true; ping_ev_ms = lap_millis(); ping_ev_rtt = v;
        if (hist_n == HIST) { memmove(hist, hist + 1, sizeof(hist) - sizeof(hist[0])); hist_n--; }
        hist[hist_n++] = v;
        if (log_n == 12) { memmove(ping_log, ping_log + 1, sizeof(ping_log) - sizeof(ping_log[0])); log_n--; }
        snprintf(ping_log[log_n++], sizeof(ping_log[0]), "%s", line);
    }
    if (ping_h > 0 && e != LAP_IO_EAGAIN && e != 1) {
        if (e) say("Ping: %s", err_text(e));
        ping_stop();
    }
}
/* The path: the board, the wire, the host. A packet runs out and its echo
   runs back each time a reply lands; each leg takes 200 ms plus the round
   trip in ms (at most 450), so a far host is visibly slower than the LAN. */
static int leg_ms(int32_t rtt_us) { int ms = rtt_us < 0 ? 450 : 200 + rtt_us / 1000; return ms > 450 ? 450 : ms; }
static void ping_path(int y) {
    const bool live = ping_h > 0;
    char hb[24];
    snprintf(hb, sizeof(hb), "%.16s", ping_host[0] ? ping_host : "?");
    const int lw = node(X0, y, "LS", hue(LAP_UI_FRAME), hue(LAP_UI_VALUE));
    const int hw = (int)strlen(hb) + 4, hx = W - X0 - hw;
    node(hx, y, hb, hue(LAP_UI_FRAME), live ? LAP_UI_TEXT : LAP_UI_DIM);
    const int a = X0 + lw, b = hx - 1;
    wire(a, b, y + 1, live ? hue(LAP_UI_FRAME) : LAP_UI_FAINT);
    char lab[40];
    uint8_t la = LAP_UI_DIM;
    if (ping_ev) {
        const int leg = leg_ms(ping_ev_rtt), t = (int)(now - ping_ev_ms);
        if (t < leg) {
            const int x = along(a, b - 1, t, leg);
            cell(x, y + 1, 'o', hue(LAP_UI_LIVE));
            cell(x + 1, y + 1, '>', hue(LAP_UI_LIVE));
        } else if (t < 2 * leg && ping_ev_rtt >= 0) {
            const int x = along(b, a + 1, t - leg, leg);
            cell(x - 1, y + 1, '<', hue(LAP_UI_VALUE));
            cell(x, y + 1, 'o', hue(LAP_UI_VALUE));
        }
        if (ping_ev_rtt < 0 && t >= leg) cell(b, y + 1, 'x', hue(LAP_UI_ALARM));
        char ms[12];
        ms_text(ms, sizeof(ms), ping_ev_rtt);
        if (ping_ev_rtt < 0) { snprintf(lab, sizeof(lab), "no echo"); la = hue(LAP_UI_ALARM); }
        else { snprintf(lab, sizeof(lab), "echo %s ms", ms); la = hue(LAP_UI_VALUE); }
    } else snprintf(lab, sizeof(lab), live ? "waiting for an echo" : "START: one ping a second");
    centre(a, b, y + 2, la, lab);
    centre(a, b, y, LAP_UI_FAINT, "ICMP echo");
}
static void draw_ping(int y) {
    field(y, "Host", ping_host);
    y += 2;
    ping_path(y);
    y += 4;
    char a[16], b[16], c[16];
    ms_text(a, sizeof(a), ping_min ? (int32_t)ping_min : -1);
    ms_text(b, sizeof(b), ping_recv ? (int32_t)(ping_sum / ping_recv) : -1);
    ms_text(c, sizeof(c), ping_max ? (int32_t)ping_max : -1);
    int loss = ping_sent ? (ping_sent - ping_recv) * 100 / ping_sent : 0;
    put(X0, y++, W - X0 * 2, LAP_UI_TEXT, "sent %d  received %d  loss %d%%", ping_sent, ping_recv, loss);
    /* min / avg / max as chips, lit once there is a round trip */
    if (vis(y)) {
        char t[3][24];
        snprintf(t[0], sizeof(t[0]), "MIN %s", a);
        snprintf(t[1], sizeof(t[1]), "AVG %s", b);
        snprintf(t[2], sizeof(t[2]), "MAX %s", c);
        int x = X0;
        for (int i = 0; i < 3; i++) x = lap_ui_chip(x, y, t[i], ping_recv > 0, i == 2 ? LAP_UI_WARN : LAP_UI_LIVE);
        put(x, y, W - X0 - x, LAP_UI_DIM, "ms");
    }
    y++;
    /* loss on the VFD: green through amber to red */
    put(X0, y, 5, LAP_UI_DIM, "LOSS");
    if (vis(y)) lap_ui_bargraph(X0 + 5, y, W - X0 * 2 - 10, 1, ping_sent ? loss / 100.0f : 0, -1);
    put(W - X0 - 4, y, 4, loss ? hue(LAP_UI_ALARM) : hue(LAP_UI_LIVE), "%3d%%", loss);
    y += 2;
    /* the round trips in a framed braille graph; timeouts are x on its floor */
    /* a wide screen puts the log beside the graph and gives the graph the height */
    const int avail = clip1 - y;
    int gh = desk ? avail - 2 : avail - 2 - log_n;
    if (!desk && gh < avail / 2 - 2) gh = avail / 2 - 2;
    if (gh > 14) gh = 14;
    if (gh < 3) gh = 3;
    const int bw = desk ? (W - X0 * 2) * 3 / 5 : W - X0 * 2, gx = X0 + 7, gw = bw - 8;
    int32_t topv = 1000;
    const int shown = hist_n < gw ? hist_n : gw;      /* one ping a cell: each drawn as both dot columns */
    for (int i = hist_n - shown; i < hist_n; i++) if (hist[i] > topv) topv = hist[i];
    topv = (topv + 999) / 1000 * 1000;
    char tl[12], box[24];
    ms_text(tl, sizeof(tl), topv);
    snprintf(box, sizeof(box), "RTT ms  %d", hist_n);
    if (vis(y) && vis(y + gh + 1)) lap_box(X0, y, bw, gh + 2, box, hue(LAP_UI_FRAME));
    put(X0 + 1, y + 1, 6, LAP_UI_DIM, "%5s", tl);
    put(X0 + 1, y + gh, 6, LAP_UI_DIM, "%5s", "0");
    for (int r = 0; r < gh; r++) cell(gx - 1, y + 1 + r, '|', LAP_UI_FAINT);
    if (shown > 0 && vis(y + 1) && vis(y + gh)) {
        for (int i = 0; i < shown; i++) {
            const int32_t v = hist[hist_n - shown + i];
            histf[2 * i] = histf[2 * i + 1] = v < 0 ? 0 : v / 1000.0f;
        }
        lap_ui_history(gx, y + 1, gw, gh, histf, shown * 2, 0, topv / 1000.0f, LAP_UI_LIVE);
        for (int i = 0; i < shown; i++)
            if (hist[hist_n - shown + i] < 0) cell(gx + gw - shown + i, y + gh + 1, 'x', hue(LAP_UI_ALARM));
    } else if (!shown) {
        /* nothing yet: a flat line with a blip walking it */
        for (int x = gx; x < gx + gw; x++) cell(x, y + gh, '_', LAP_UI_FAINT);
        cell(gx + (int)(now / 120 % (unsigned)(gw > 0 ? gw : 1)), y + gh, '^', hue(ping_h > 0 ? LAP_UI_LIVE : LAP_UI_FRAME));
    }
    int lx = X0, lw = W - X0 * 2;
    if (desk) { lx = X0 + bw + 2; lw = W - X0 - lx; }
    else y += gh + 2;
    for (int i = log_n - (clip1 - y) > 0 ? log_n - (clip1 - y) : 0; i < log_n && y < clip1; i++)
        put(lx, y++, lw, strstr(ping_log[i], "timeout") ? hue(LAP_UI_ALARM) : LAP_UI_DIM, "%s", ping_log[i]);
}

/* ---- traceroute ----------------------------------------------------------------- */
static void trace_stop(void) {
    for (int i = 0; i < 4; i++) if (tr[i].h > 0) lap_ping_stop(tr[i].h), tr[i].h = 0;
    tracing = false;
}
static void trace_start(void) {
    trace_stop();
    memset(hops, 0, sizeof(hops));
    trace_next = 1; trace_last = 0;
    tracing = true;
}
static void trace_poll(void) {
    if (!tracing) return;
    for (int i = 0; i < 4; i++) {
        if (tr[i].h > 0) {
            lap_ping_reply_t r;
            int e;
            while ((e = lap_ping_next(tr[i].h, &r)) == 1) {
                int k = tr[i].hop - 1;
                if (hops[k].n < PROBES) hops[k].rtt[hops[k].n++] = r.type == LAP_PING_TIMEOUT ? -1 : (int32_t)r.rtt_us;
                if (r.type != LAP_PING_TIMEOUT) {
                    snprintf(hops[k].from, sizeof(hops[k].from), "%s", r.from);
                    hops[k].type = r.type; hops[k].code = r.code;
                    if (r.type == LAP_PING_REPLY || r.type == LAP_PING_UNREACHABLE)
                        if (!trace_last || tr[i].hop < trace_last) trace_last = tr[i].hop;
                }
            }
            if (e == LAP_IO_EAGAIN) continue;
            if (e < 0) { say("Trace: %s", err_text(e)); trace_stop(); return; }
            hops[tr[i].hop - 1].active = 0;
            lap_ping_stop(tr[i].h);
            tr[i].h = 0;
        }
        if (trace_next <= MAXHOP && (!trace_last || trace_next < trace_last)) {
            lap_ping_config_t c = {.size = sizeof(c), .version = 1, .count = PROBES, .interval_ms = 50,
                                   .timeout_ms = 2000, .ttl = (uint8_t)trace_next};
            int h = lap_ping_start(trace_host, &c);
            if (h > 0) { tr[i].h = h; tr[i].hop = trace_next; hops[trace_next - 1].active = 1; trace_next++; }
        }
    }
    bool busy = false;
    for (int i = 0; i < 4; i++) {
        if (tr[i].h > 0 && trace_last && tr[i].hop > trace_last) { lap_ping_stop(tr[i].h); tr[i].h = 0; }
        busy = busy || tr[i].h > 0;
    }
    if (!busy && (trace_last || trace_next > MAXHOP)) {
        tracing = false;
        say(trace_last ? "Reached in %d hops" : "No answer within %d hops", trace_last ? trace_last : MAXHOP);
    }
}
/* The hops as a chain hanging from the board: a node per hop that lights as
   it answers, and a spark running down the links to the frontier. */
static int draw_trace(int y) {
    field(y, "Host", trace_host);
    y += 2;
    int shown = trace_last ? trace_last : trace_next - 1;
    if (shown > MAXHOP) shown = MAXHOP;
    put(X0, y, 4, hue(LAP_UI_VALUE), "[LS]");
    put(X0 + 5, y, W - X0 * 2 - 5, LAP_UI_DIM, "this board to %s", trace_host);
    y++;
    if (!shown) {
        /* idle: the chain as it will look, dim, with a ? waiting at the end */
        static const char chain[] = " |  o  |  o  |  o  |  ?";
        const int lit = (int)(now / 400 % 5);
        for (int i = 0; chain[i]; i++) cell(X0 + i, y, chain[i], chain[i] == '?' && lit == 4 ? hue(LAP_UI_WARN) : LAP_UI_FAINT);
        y += 2;
        put(X0, y++, W - X0 * 2, LAP_UI_TEXT, "Tap START: one ping per hop, 3 probes each.");
        put(X0, y++, W - X0 * 2, LAP_UI_DIM, "Each hop lights as it answers.");
        return y;
    }
    put(X0 + 3, y, W - X0 * 2 - 3, hue(LAP_UI_WARN), "HOP ADDRESS           RTT ms");
    y++;
    /* the spark: down the links to the newest hop, a link every 90 ms while
       tracing, at a stroll once the path is known */
    const int spark = (int)(now / (tracing ? 90u : 200u) % (unsigned)(shown + 2));
    for (int k = 0; k < shown; k++) {
        cell(X0 + 1, y, spark == k ? '*' : '|', spark == k ? hue(LAP_UI_LIVE) : hue(LAP_UI_FRAME));
        y++;
        char line[96], ms[PROBES][12];
        for (int p = 0; p < PROBES; p++) {
            if (p < hops[k].n) ms_text(ms[p], sizeof(ms[p]), hops[k].rtt[p]);
            else snprintf(ms[p], sizeof(ms[p]), hops[k].active ? "." : "");
        }
        const bool dest = k + 1 == trace_last, unreach = hops[k].type == LAP_PING_UNREACHABLE;
        int16_t g;
        uint8_t ga;
        if (unreach) { g = '!'; ga = hue(LAP_UI_ALARM); }
        else if (dest) { g = '@'; ga = hue(LAP_UI_VALUE); }
        else if (hops[k].from[0]) { g = 'O'; ga = hue(LAP_UI_LIVE); }
        else if (hops[k].active) { g = now / 250 % 2 ? 'o' : '.'; ga = hue(LAP_UI_WARN); }
        else { g = '*'; ga = LAP_UI_FAINT; }
        cell(X0 + 1, y, g, ga);
        snprintf(line, sizeof(line), "%2d  %-15s %6s %6s %6s%s", k + 1, hops[k].from[0] ? hops[k].from : "*", ms[0], ms[1],
                 ms[2], unreach ? " !U" : "");
        put(X0 + 3, y, W - X0 * 2 - 3, dest ? hue(LAP_UI_LIVE) : hops[k].from[0] ? LAP_UI_TEXT : LAP_UI_DIM, "%s", line);
        y++;
    }
    if (trace_last) put(X0, y++, W - X0 * 2, LAP_UI_DIM, "Reached %s in %d hops.", trace_host, trace_last);
    /* the same path laid flat: LS -O-O-*-@ */
    y++;
    int x = X0;
    put(x, y, 2, hue(LAP_UI_VALUE), "LS");
    x += 2;
    for (int k = 0; k < shown && x + 3 < W - X0; k++) {
        const bool dest = k + 1 == trace_last;
        cell(x++, y, '-', spark == k ? hue(LAP_UI_LIVE) : hue(LAP_UI_FRAME));
        cell(x++, y, dest ? '@' : hops[k].from[0] ? 'O' : hops[k].active ? 'o' : '*',
             dest ? hue(LAP_UI_VALUE) : hops[k].from[0] ? hue(LAP_UI_LIVE) : LAP_UI_FAINT);
    }
    if (!trace_last && tracing) cell(x, y, now / 250 % 2 ? '?' : ' ', hue(LAP_UI_WARN));
    y++;
    return y;
}

/* ---- dns ------------------------------------------------------------------------ */
static void dns_start(void) {
    if (look_n == 10) { memmove(looks, looks + 1, sizeof(looks) - sizeof(looks[0])); look_n--; }
    int h = lap_dns_resolve(dns_name, 5000);
    snprintf(looks[look_n].name, sizeof(looks[0].name), "%.39s", dns_name);
    looks[look_n].ip[0] = 0;
    looks[look_n].h = h > 0 ? h : 0;
    looks[look_n].result = h > 0 ? LAP_IO_EAGAIN : h;
    looks[look_n].at = lap_millis();
    looks[look_n++].ms = 0;
}
static void dns_poll(void) {
    for (int i = 0; i < look_n; i++)
        if (looks[i].h > 0) {
            int r = lap_dns_result(looks[i].h, looks[i].ip);
            if (r == LAP_IO_EAGAIN) continue;
            looks[i].result = r;
            looks[i].h = 0;
            looks[i].ms = lap_millis() - looks[i].at;
            dns_ev_ms = lap_millis();
        }
}
/* The resolver: a ? runs to the DNS server while a lookup waits (the server
   spins), and the answer runs back when it lands. */
static int draw_dns(int y) {
    field(y, "Name", dns_name);
    y += 2;
    bool waiting = false;
    for (int i = 0; i < look_n; i++) waiting = waiting || looks[i].result == LAP_IO_EAGAIN;
    char srv[24];
    static const char spin[] = "|/-\\";
    snprintf(srv, sizeof(srv), "DNS %c", waiting ? spin[now / 120 % 4] : ' ');
    const int lw = node(X0, y, "LS", hue(LAP_UI_FRAME), hue(LAP_UI_VALUE));
    const int sx = W - X0 - 9;
    node(sx, y, srv, hue(LAP_UI_FRAME), waiting ? hue(LAP_UI_WARN) : LAP_UI_TEXT);
    const int a = X0 + lw, b = sx - 1;
    wire(a, b, y + 1, waiting ? hue(LAP_UI_FRAME) : LAP_UI_FAINT);
    char lab[64], via[40];
    snprintf(via, sizeof(via), "via %s", net.dns[0] ? net.dns : "-");
    centre(a, b, y, LAP_UI_FAINT, via);
    const int t = (int)(now - dns_ev_ms);
    if (waiting) {
        cell(along(a, b, (int)(now % 600), 600), y + 1, '?', hue(LAP_UI_WARN));
        snprintf(lab, sizeof(lab), "? %s", looks[look_n - 1].name);
        centre(a, b, y + 2, hue(LAP_UI_WARN), lab);
    } else if (look_n) {
        const int i = look_n - 1;
        if (t < 700) cell(along(b, a, t, 700), y + 1, looks[i].result ? 'x' : 'A', hue(looks[i].result ? LAP_UI_ALARM : LAP_UI_LIVE));
        if (looks[i].result) snprintf(lab, sizeof(lab), "%s: %s", looks[i].name, err_text(looks[i].result));
        else snprintf(lab, sizeof(lab), "A %s", looks[i].ip);
        centre(a, b, y + 2, hue(looks[i].result ? LAP_UI_ALARM : LAP_UI_VALUE), lab);
    } else centre(a, b, y + 2, LAP_UI_DIM, "LOOKUP asks for the address");
    y += 4;
    bool narrow = W < 60;
    if (narrow) {
        put(X0, y++, W - X0 * 2, hue(LAP_UI_WARN), "NAME");
        put(X0 + 2, y++, W - X0 * 2 - 2, hue(LAP_UI_WARN), "ADDRESS          TIME");
    } else
        put(X0, y++, W - X0 * 2, hue(LAP_UI_WARN), "NAME                 ADDRESS          TIME");
    for (int i = look_n - 1; i >= 0; i--) {
        char res[32];
        if (looks[i].result == LAP_IO_EAGAIN) snprintf(res, sizeof(res), "...");
        else if (looks[i].result) snprintf(res, sizeof(res), "%s", err_text(looks[i].result));
        else snprintf(res, sizeof(res), "%s", looks[i].ip);
        uint8_t a2 = looks[i].result && looks[i].result != LAP_IO_EAGAIN ? hue(LAP_UI_ALARM) : LAP_UI_DIM;
        if (narrow) {
            put(X0, y++, W - X0 * 2, LAP_UI_TEXT, "%s", looks[i].name);
            put(X0 + 2, y++, W - X0 * 2 - 2, a2, "%-16s %u ms", res, (unsigned)looks[i].ms);
        } else
            put(X0, y++, W - X0 * 2, a2, "%-20.20s %-16s %u ms", looks[i].name, res, (unsigned)looks[i].ms);
    }
    return y;
}

/* ---- wake-on-lan ------------------------------------------------------------------ */
static bool parse_mac(const char *s, uint8_t m[6]) {
    int n = 0;
    while (*s && n < 12) {
        char c = *s++;
        int v = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
        if (v < 0) { if (c == ':' || c == '-' || c == '.') continue; return false; }
        m[n / 2] = (uint8_t)(n % 2 ? m[n / 2] | v : v << 4);
        n++;
    }
    return n == 12 && !*s;
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
static void broadcast_default(void) {
    uint32_t a, m;
    if (parse_ip(net.ip, &a) && parse_ip(net.mask, &m)) {
        a |= ~m;
        snprintf(wol_to, sizeof(wol_to), "%u.%u.%u.%u", (unsigned)(a >> 24), (unsigned)(a >> 16 & 255),
                 (unsigned)(a >> 8 & 255), (unsigned)(a & 255));
    }
}
static void wol_send(void) {
    uint8_t mac[6], pkt[102];
    if (!parse_mac(wol_mac, mac)) { say("MAC: six hex pairs, e.g. 00:11:22:33:44:55"); return; }
    if (!wol_to[0]) broadcast_default();
    if (wol_h <= 0) {
        lap_socket_address_t any = {.size = sizeof(any), .version = 1};
        wol_h = lap_udp_open(&any);
        if (wol_h < 0) { say("UDP: %s", err_text(wol_h)); wol_h = 0; return; }
    }
    memset(pkt, 0xFF, 6);
    for (int i = 0; i < 16; i++) memcpy(pkt + 6 + i * 6, mac, 6);
    /* "address" or "address:port"; port 9 (discard) by default */
    lap_socket_address_t to = {.size = sizeof(to), .version = 1, .port = 9};
    char addr[24];
    snprintf(addr, sizeof(addr), "%s", wol_to);
    char *colon = strchr(addr, ':');
    if (colon) { *colon = 0; int p = atoi(colon + 1); if (p > 0 && p < 65536) to.port = (uint16_t)p; }
    snprintf(to.ip, sizeof(to.ip), "%.15s", addr);
    int r = lap_udp_send(wol_h, &to, pkt, sizeof(pkt));
    if (wol_n == 6) { memmove(wol_log, wol_log + 1, sizeof(wol_log) - sizeof(wol_log[0])); wol_n--; }
    if (r == (int)sizeof(pkt)) {
        wol_sent++;
        wol_ev_ms = lap_millis();
        snprintf(wol_log[wol_n++], sizeof(wol_log[0]), "%02x:%02x:%02x:%02x:%02x:%02x -> %s:%u", mac[0], mac[1], mac[2],
                 mac[3], mac[4], mac[5], to.ip, to.port);
        say("Magic packet sent (102 bytes)");
    } else {
        snprintf(wol_log[wol_n++], sizeof(wol_log[0]), "send failed: %s", err_text(r));
    }
}
/* The magic packet crossing to a sleeping machine; it starts at the
   board when SEND goes and lands 1.2 s later. Whether the machine wakes is
   not something the board can see, so it only flashes at the landing. */
static int draw_wol(int y) {
    field(y, "MAC", wol_mac);
    y += 1;
    field(y, "To", wol_to);
    y += 2;
    const int t = wol_sent ? (int)(now - wol_ev_ms) : 1 << 30;
    static const char *const zz[] = {"z  ", "zZ ", "zZz", "   "};
    const bool landed = t >= 1200 && t < 1800;
    const int lw = node(X0, y, "LS", hue(LAP_UI_FRAME), hue(LAP_UI_VALUE));
    const int px = W - X0 - 7;
    node(px, y, landed ? "!!!" : zz[now / 400 % 4], hue(landed ? LAP_UI_WARN : LAP_UI_FRAME), landed ? hue(LAP_UI_WARN) : LAP_UI_DIM);
    const int a = X0 + lw, b = px - 1;
    wire(a, b, y + 1, LAP_UI_FAINT);
    static const char pkt[] = "[FFx6|MACx16]";
    const int pl = (int)sizeof(pkt) - 1;
    if (t < 1200 && b - a > pl) put(along(a, b - pl + 1, t, 1200), y + 1, pl, hue(LAP_UI_LIVE), "%s", pkt);
    char lab[48];
    uint16_t port = 9;
    const char *colon = strchr(wol_to, ':');
    if (colon && atoi(colon + 1) > 0) port = (uint16_t)atoi(colon + 1);
    snprintf(lab, sizeof(lab), "102 bytes UDP to %.*s:%u", colon ? (int)(colon - wol_to) : 15, wol_to[0] ? wol_to : "?", port);
    centre(a, b, y, LAP_UI_FAINT, "magic packet");
    centre(a, b, y + 2, t < 1800 ? hue(LAP_UI_VALUE) : LAP_UI_DIM, lab);
    y += 4;
    put(X0, y++, W - X0 * 2, LAP_UI_DIM, "Magic packet: 6 x FF, then the MAC 16 times,");
    put(X0, y++, W - X0 * 2, LAP_UI_DIM, "as a UDP broadcast to port 9.");
    y++;
    put(X0, y++, W - X0 * 2, hue(LAP_UI_WARN), "SENT (%d)", wol_sent);
    for (int i = wol_n - 1; i >= 0; i--) put(X0, y++, W - X0 * 2, LAP_UI_TEXT, "%s", wol_log[i]);
    return y;
}

/* ---- info ------------------------------------------------------------------------- */
static int draw_info(int y) {
    lap_net_info_t eth;
    int er = lap_net_info(LAP_NETIF_ETH, &eth);
    uint8_t k = hue(LAP_UI_FRAME), v = LAP_UI_TEXT;
    char mac[18];
    snprintf(mac, sizeof(mac), "%02x:%02x:%02x:%02x:%02x:%02x", net.mac[0], net.mac[1], net.mac[2], net.mac[3], net.mac[4], net.mac[5]);
    char wifi[64] = "not fitted";
    if (net.kind == LAP_NETIF_WIFI)
        snprintf(wifi, sizeof(wifi), ws.connected ? "%s  %d dBm  ch %u" : "not joined", ws.ssid, ws.rssi, ws.channel);
    struct { const char *k; const char *v; } rows[] = {
        {"Wi-Fi", wifi}, {"Link", net.link_up ? "up" : "down"}, {"Address", net.ip[0] ? net.ip : "-"},
        {"Mask", net.mask[0] ? net.mask : "-"}, {"Gateway", net.gateway[0] ? net.gateway : "-"},
        {"DNS", net.dns[0] ? net.dns : "-"}, {"MAC", net.kind ? mac : "-"},
        {"Ethernet", er == LAP_IO_ENOTSUP ? "not available" : eth.link_up ? eth.ip : "down"},
    };
    /* a mast beside the table: its rings ripple outward while the link is up */
    const int mx = W - X0 - 9, my = y + 2;
    if (mx > X0 + 36) {
        const int ring = net.link_up ? (int)(now / 300 % 4) : -1;
        const uint8_t on = hue(LAP_UI_LIVE), off = LAP_UI_FAINT;
        cell(mx, my, '(', ring == 2 ? on : off);
        cell(mx + 1, my, '(', ring == 1 ? on : off);
        cell(mx + 2, my, '(', ring == 0 ? on : off);
        cell(mx + 3, my, 'o', net.link_up ? hue(LAP_UI_VALUE) : hue(LAP_UI_ALARM));
        cell(mx + 4, my, ')', ring == 0 ? on : off);
        cell(mx + 5, my, ')', ring == 1 ? on : off);
        cell(mx + 6, my, ')', ring == 2 ? on : off);
        put(mx + 3, my + 1, 1, hue(LAP_UI_FRAME), "|");
        put(mx + 2, my + 2, 3, hue(LAP_UI_FRAME), "/|\\");
        put(mx + 1, my + 3, 5, hue(LAP_UI_FRAME), "/_|_\\");
        put(mx, my + 4, 7, net.link_up ? hue(LAP_UI_LIVE) : hue(LAP_UI_ALARM), net.link_up ? "  LINK " : "  DOWN ");
    }
    for (unsigned i = 0; i < sizeof(rows) / sizeof(rows[0]); i++) {
        put(X0, y, 10, k, "%s", rows[i].k);
        put(X0 + 10, y++, W - X0 * 2 - 10, v, "%s", rows[i].v);
    }
    /* the signal on the board's meter */
    if (net.kind == LAP_NETIF_WIFI && ws.connected) {
        float f = (ws.rssi + 90) / 60.0f;
        f = f < 0 ? 0 : f > 1 ? 1 : f;
        put(X0, y, 10, k, "Signal");
        if (vis(y)) lap_ui_meter(X0 + 10, y, W - X0 * 2 - 18, f, ws.rssi >= -67 ? LAP_UI_LIVE : ws.rssi >= -80 ? LAP_UI_WARN : LAP_UI_ALARM);
        put(W - X0 - 7, y, 7, hue(LAP_UI_VALUE), "%4d dB", ws.rssi);
        y++;
    }
    y++;
    put(X0, y++, W - X0 * 2, hue(LAP_UI_WARN), "ARP TABLE (%d)", arp_n);
    for (int i = 0; i < arp_n; i++) {
        snprintf(mac, sizeof(mac), "%02x:%02x:%02x:%02x:%02x:%02x", arp[i].mac[0], arp[i].mac[1], arp[i].mac[2],
                 arp[i].mac[3], arp[i].mac[4], arp[i].mac[5]);
        put(X0, y++, W - X0 * 2, LAP_UI_DIM, "%-16s %s  %us", arp[i].ip, mac, (unsigned)(arp[i].age_ms / 1000));
    }
    if (!arp_n) put(X0, y++, W - X0 * 2, LAP_UI_FAINT, "(empty: the stack has not talked to anyone yet)");
    return y;
}

/* ---- frame, input ------------------------------------------------------------------- */
static void ask(int what) {
    static const char *const prompts[] = {"Ping host", "Traceroute host", "Name to look up", "MAC address", "Broadcast address"};
    const char *now[] = {ping_host, trace_host, dns_name, wol_mac, wol_to};
    if (dialog > 0) return;
    dialog = lap_text_input(prompts[what], now[what], 63, LAP_TEXT_LOWER);
    dialog_for = what;
}
static void answer(void) {
    char t[64];
    int v = 0, r = lap_dialog_poll(dialog, t, sizeof(t), &v);
    if (!r) return;
    lap_dialog_cancel(dialog);
    dialog = 0;
    if (r != 1) return;
    char *dst[] = {ping_host, trace_host, dns_name, wol_mac, wol_to};
    size_t cap[] = {sizeof(ping_host), sizeof(trace_host), sizeof(dns_name), sizeof(wol_mac), sizeof(wol_to)};
    char *s = t;
    while (*s == ' ') s++;
    for (char *e = s + strlen(s); e > s && e[-1] == ' ';) *--e = 0;
    snprintf(dst[dialog_for], cap[dialog_for], "%s", s);
    if (dialog_for == 0 && ping_h > 0) ping_start();
}
static void primary(void) {
    if (tab == T_PING) { if (ping_h > 0) ping_stop(); else if (ping_host[0]) ping_start(); else ask(0); }
    else if (tab == T_TRACE) { if (tracing) trace_stop(); else if (trace_host[0]) trace_start(); else ask(1); }
    else if (tab == T_DNS) { if (dns_name[0]) dns_start(); else ask(2); }
    else if (tab == T_WOL) wol_send();
    else arp_at = 0;
}
static void second(void) {
    if (tab <= T_DNS) ask(tab);
    else if (tab == T_WOL) ask(3);
}
static void third(void) {
    if (tab == T_PING) { ping_sent = ping_recv = hist_n = log_n = 0; ping_min = ping_max = 0; ping_sum = 0; }
    else if (tab == T_TRACE) { trace_stop(); memset(hops, 0, sizeof(hops)); trace_next = 1; trace_last = 0; }
    else if (tab == T_DNS) look_n = 0;
    else if (tab == T_WOL) ask(4);
}
/* The header: a terminal with its aerial beside the interface's key facts,
   as NOTES has its notepad. The cursor blinks; the aerial's arcs run while
   the link is up. Four rows, from row 1. */
static void header(void) {
    const uint8_t f = hue(LAP_UI_FRAME);
    /* the terminal types the tab's command, a letter every 150 ms, and holds it */
    static const char *const cmd[TABS] = {"ping", "trace", "dig", "wol", "ip a"};
    const int n = (int)strlen(cmd[tab]), typed = (int)(now / 150 % (unsigned)(n + 8));
    put(1, 1, 10, f, ".--------.");
    put(1, 2, 1, f, "|");
    put(2, 2, 2, hue(LAP_UI_LIVE), ">");
    put(4, 2, typed < n ? typed : n, LAP_UI_TEXT, "%s", cmd[tab]);
    if (now / 300 % 2) put(4 + (typed < n ? typed : n), 2, 1, hue(LAP_UI_LIVE), "_");
    put(10, 2, 1, f, "|");
    put(1, 3, 10, f, "'---..---'");
    put(3, 4, 6, f, "[____]");
    /* the aerial: its rings ripple outward while the link is up */
    const int ring = net.link_up ? (int)(now / 250 % 3) : -1;
    const uint8_t on = hue(LAP_UI_LIVE), off = net.link_up ? f : LAP_UI_FAINT;
    put(15, 1, 1, net.link_up ? hue(LAP_UI_VALUE) : hue(LAP_UI_ALARM), net.link_up ? "o" : "x");
    put(13, 2, 1, ring == 1 ? on : off, "(");
    put(14, 2, 1, ring == 0 ? on : off, "(");
    put(15, 2, 1, f, "|");
    put(16, 2, 1, ring == 0 ? on : off, ")");
    put(17, 2, 1, ring == 1 ? on : off, ")");
    put(14, 3, 3, f, "/|\\");
    put(13, 4, 5, f, "/_|_\\");
    const int x = 21, w = W - x - 1;
    if (net.kind == LAP_NETIF_WIFI && ws.connected) put(x, 1, w, LAP_UI_TEXT, "%s", ws.ssid);
    else put(x, 1, w, LAP_UI_TEXT, "%s", net.kind == LAP_NETIF_ETH ? "Ethernet" : net.kind ? "Wi-Fi: not joined" : "no interface");
    put(x, 2, w, hue(LAP_UI_VALUE), "%s", net.ip[0] ? net.ip : "no address");
    if (net.mask[0]) put(x + (int)strlen(net.ip) + 1, 2, w - (int)strlen(net.ip) - 1, LAP_UI_DIM, "/ %s", net.mask);
    put(x, 3, w, LAP_UI_DIM, "gw %s  dns %s", net.gateway[0] ? net.gateway : "-", net.dns[0] ? net.dns : "-");
    char rssi[12], ch[8];
    snprintf(rssi, sizeof(rssi), "%d dBm", ws.rssi);
    snprintf(ch, sizeof(ch), "CH %u", ws.channel);
    int cx = lap_ui_chip(x, 4, net.link_up ? "LINK" : "DOWN", true, net.link_up ? LAP_UI_LIVE : LAP_UI_ALARM);
    if (net.kind == LAP_NETIF_WIFI && ws.connected) {
        cx = lap_ui_chip(cx, 4, rssi, true, ws.rssi >= -67 ? LAP_UI_LIVE : LAP_UI_WARN);
        lap_ui_chip(cx, 4, ch, false, LAP_UI_FRAME);
    }
}
static const char *panel_title(char *t, size_t cap) {
    if (tab == T_PING) snprintf(t, cap, "PING%s", ping_h > 0 ? "  LIVE" : "");
    else if (tab == T_TRACE) {
        if (tracing) snprintf(t, cap, "TRACEROUTE  HOP %d", trace_next - 1);
        else if (trace_last) snprintf(t, cap, "TRACEROUTE  %d HOPS", trace_last);
        else snprintf(t, cap, "TRACEROUTE");
    } else if (tab == T_DNS) snprintf(t, cap, "DNS LOOKUP");
    else if (tab == T_WOL) snprintf(t, cap, "WAKE-ON-LAN");
    else snprintf(t, cap, "INTERFACE");
    return t;
}
static void go_tab(int t) {
    tab = (t + TABS) % TABS;
    scroll = 0;
    lap_scroll_stop(&drag);
}
static void draw(void) {
    W = lap_cols(); H = lap_rows();
    desk = W >= 90;
    if (dialog > 0) answer();
    now = lap_millis();
    lap_net_info(LAP_NETIF_ACTIVE, &net);
    memset(&ws, 0, sizeof(ws));
    lap_wifi_status(&ws);
    if (!ping_host[0] && net.gateway[0]) snprintf(ping_host, sizeof(ping_host), "%s", net.gateway);
    if (!wol_to[0] && net.link_up) broadcast_default();
    ping_poll();
    trace_poll();
    dns_poll();
    if (tab == T_INFO && lap_millis() - arp_at > 1000) { arp_at = lap_millis(); arp_n = lap_arp_table(arp, 32); if (arp_n < 0) arp_n = 0; }
    int sw = lap_swipe();
    if (sw == LAP_SWIPE_LEFT) go_tab(tab + 1);
    if (sw == LAP_SWIPE_RIGHT) go_tab(tab - 1);
    /* the title, the header art on a tall screen, the tabs; the keys at the
       bottom; the tab's bezel between, its content scrolling inside the frame */
    kr = lap_ui_keys_rows(3);
    const bool art = !desk && H >= 40;
    top = art ? 6 : 1;
    tbr = desk ? 1 : lap_ui_keys_rows(3);
    const int py = top + tbr, ph = H - kr - py;
    clip0 = py + 1;
    clip1 = py + ph - 1;
    if (tab == T_PING) { scroll = smax = 0; dragging = false; }     /* ping fits itself to the area */
    else {
        dragging = lap_scroll_follow(&drag, &scroll, smax, clip0, clip1);
        if (scroll > smax) scroll = smax;
    }

    lap_clear(LAP_ATTR(LAP_WHITE, LAP_BLACK));
    lap_fill(0, 0, W, 1, ' ', LAP_ATTR(LAP_BLACK, LAP_CYAN));
    put(1, 0, W - 2, LAP_ATTR(LAP_BLACK, LAP_CYAN), "NET DIAG");
    put(W - 17, 0, 16, net.link_up ? LAP_ATTR(LAP_BLACK, LAP_CYAN) : LAP_ATTR(LAP_WHITE | LAP_BRIGHT, LAP_RED), "%15s", net.link_up ? net.ip : "no network");
    if (art) header();
    if (desk)
        for (int i = 0; i < TABS; i++) {
            int x0 = i * W / TABS, x1 = (i + 1) * W / TABS;
            uint8_t a = i == tab ? LAP_ATTR(LAP_BLACK, LAP_YELLOW) : LAP_ATTR(LAP_WHITE, LAP_BLUE);
            lap_fill(x0, top, x1 - x0, 1, ' ', a);
            put(x0 + (x1 - x0 - (int)strlen(tab_names[i])) / 2, top, x1 - x0, a, "%s", tab_names[i]);
        }
    else {
        char n[16];
        snprintf(n, sizeof(n), "%d/%d", tab + 1, TABS);
        art_strip(top, W, tbr, tab_names[tab], n, false);   /* one board key bar a frame: see lap_art.h */
    }
    char title[40];
    lap_ui_bezel(0, py, W, ph, panel_title(title, sizeof(title)));
    int y = clip0 - scroll, end = clip0;
    clipping = true;
    if (tab == T_PING) draw_ping(y);
    else if (tab == T_TRACE) end = draw_trace(y);
    else if (tab == T_DNS) end = draw_dns(y);
    else if (tab == T_WOL) end = draw_wol(y);
    else end = draw_info(y);
    clipping = false;
    if (tab != T_PING) {
        smax = end + scroll - clip1;
        if (smax < 0) smax = 0;
        /* more above / below, on the frame */
        if (scroll > 0) put(W - 3, py, 1, hue(LAP_UI_VALUE), "^");
        if (scroll < smax) put(W - 3, py + ph - 1, 1, hue(LAP_UI_VALUE), "v");
    }
    /* a note for six seconds over the title, left of the address; a tap there clears it */
    if (status[0] && lap_millis() - status_at < 6000) {
        const uint8_t a = LAP_ATTR(LAP_BLACK, LAP_YELLOW);
        const int room = (int)strlen(status) <= W - 20 ? W - 18 : W;
        lap_fill(0, 0, room, 1, ' ', a);
        put(1, 0, room - 2, a, "%s", status);
    }
    lap_btn_t k[3] = {{"START", NULL, 's', false, false, false}, {"HOST", NULL, 'e', false, false, false},
                      {"CLEAR", NULL, 'c', false, false, false}};
    int n = 3;
    if (tab == T_PING) { if (ping_h > 0) { k[0].label = "STOP"; k[0].on = true; } }
    else if (tab == T_TRACE) { if (tracing) { k[0].label = "STOP"; k[0].on = true; } }
    else if (tab == T_DNS) { k[0].label = "LOOKUP"; k[1].label = "NAME"; }
    else if (tab == T_WOL) { k[0].label = "SEND"; k[1].label = "MAC"; k[2].label = "TO"; }
    else { k[0].label = "REFRESH"; n = 1; }
    lap_ui_keys(0, H - kr, W, kr, k, n, -1);
}
static bool key(lap_key_t k, char ch) {
    const int page = clip1 - clip0 > 1 ? clip1 - clip0 - 1 : 1;
    if (k == LAP_KEY_TAB || k == LAP_KEY_RIGHT) { go_tab(tab + 1); return true; }
    if (k == LAP_KEY_LEFT) { go_tab(tab - 1); return true; }
    if (k == LAP_KEY_ENTER) { primary(); return true; }
    if (k == LAP_KEY_UP) { if (scroll > 0) scroll--; return true; }
    if (k == LAP_KEY_DOWN) { if (scroll < smax) scroll++; return true; }
    if (k == LAP_KEY_PGUP) { scroll = scroll > page ? scroll - page : 0; return true; }
    if (k == LAP_KEY_PGDN) { scroll = scroll + page < smax ? scroll + page : smax; return true; }
    if (k != LAP_KEY_CHAR) return false;
    if (ch >= '1' && ch <= '5') { go_tab(ch - '1'); return true; }
    if (ch == ' ' || ch == 's') { primary(); return true; }
    if (ch == 'e' || ch == 'h') { second(); return true; }
    if (ch == 'c') { third(); return true; }
    return false;
}
static bool touch(int col, int row) {
    if (row >= H - kr) {
        if (dragging) return true;
        int b = lap_ui_key_hit(col, row);
        if (b == 0) primary(); else if (b == 1) second(); else if (b == 2) third();
        return true;
    }
    if (row == 0) { if (status[0] && lap_millis() - status_at < 6000) status[0] = 0; return true; }
    if (row < top) return true;                         /* the header art */
    if (row >= top && row < top + tbr) {
        if (desk) go_tab(col * TABS / (W > 0 ? W : 1));
        else {
            const int t = col * 3 / (W > 0 ? W : 1) - 1;      /* the strip's three keys: < name > */
            if (t) go_tab(tab + t);
        }
        return true;
    }
    if (row < clip0 || row >= clip1 || dragging) return true;
    /* the target lines sit at the top of the tab */
    const int f = row - clip0 + scroll;
    if (f == 0 && tab != T_INFO) { ask(tab == T_WOL ? 3 : tab); return true; }
    if (f == 1 && tab == T_WOL) { ask(4); return true; }
    /* Every tap on this screen is the app's: one left unused reaches the
       board as UP, ENTER or DOWN by where it fell, and ENTER acts. */
    return true;
}
static void enter(void) {
    tab = T_PING;
    dialog = 0;
    scroll = smax = 0;
    memset(&drag, 0, sizeof(drag));
    dragging = clipping = false;
    kr = tbr = top = 1;
    clip0 = 3; clip1 = lap_rows() - 1;
    ping_ev = false;
    dns_ev_ms = wol_ev_ms = 0;
    ping_h = wol_h = 0;
    memset(tr, 0, sizeof(tr));
    tracing = false;
    trace_next = 1; trace_last = 0;
    look_n = wol_n = wol_sent = log_n = hist_n = 0;
    ping_host[0] = wol_mac[0] = wol_to[0] = status[0] = 0;
    snprintf(trace_host, sizeof(trace_host), "example.org");
    snprintf(dns_name, sizeof(dns_name), "example.org");
}
static void leave(void) {
    ping_stop();
    trace_stop();
    if (wol_h > 0) lap_udp_close(wol_h);
    if (dialog > 0) lap_dialog_cancel(dialog);
}

LAP_APP(.id = "net_diag", .name = "NET DIAG", .sub = "ping trace dns", .version = "1.1.0",
        .author = "LakeShark", .icon = LAP_ICON_CHIP, .color = LAP_GREEN, .flags = LAP_CAP_NETWORK,
        .enter = enter, .leave = leave, .draw = draw, .key = key, .touch = touch);
