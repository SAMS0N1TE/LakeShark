/* THROUGHPUT: an iperf2-compatible client. Run `iperf -s` (TCP, port 5001)
   or `iperf -s -u` (UDP) on a computer, enter its address here and START.
   TCP streams for the chosen time and graphs Mbit/s; UDP sends paced
   datagrams in iperf2's format and shows the server's report (loss,
   jitter). iperf3 speaks a different protocol and will not answer.

   What is measured is the app SDK's socket path (lap_tcp_*, lap_udp_*):
   bytes the board's network worker accepted, as iperf2's client reports
   bytes written. API 2.8 for lap_net_info; the sockets are API 1.3 / 2.4.
   The board's keys (API 2.1) along the bottom. */
#include <lakeshark_app.h>
#include "lakeshark_ui.h"
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

LAP_ICON_PX("....................", "....................", "..f.................", "..f..............a..",
            "..f.............aa..", "..f......a.....a.a..", "..f.....aaa...a..a..", "..f....a...a.a...a..",
            "..f...a.....a....a..", "..f..a...........a..", "..f.a............a..", "..faaaaaaaaaaaaaaa..",
            "..fffffffffffffffff.", "....................", "....................", "....................",
            "....................", "....................");

#define SAMPLE_MS 500
#define MAXS 240            /* two minutes of half-second samples */
#define DGRAM 1470

enum { S_IDLE, S_CONNECT, S_RUN, S_FINISH, S_DONE, S_FAIL };
static int state, W, H, dialog, dialog_for, kr;
static char server[64] = "", status[80];
static uint32_t status_at;
static int port = 5001, mode_udp, duration_i = 1, rate_i;
static const int durations[] = {5, 10, 30, 60};
static const int rates[] = {1, 2, 5, 10, 20};          /* UDP Mbit/s */
static int sock = -1, dns_h;
static char server_ip[16];
static uint32_t t0, t_end, stop_at, last_sample, fin_at;
static uint64_t bytes, sample_bytes, total_sent;
static int32_t samples[MAXS];           /* kbit/s */
static int nsamples;
static int32_t peak;
static uint8_t buf[4096];
static int32_t seq;
static uint32_t dgrams, fins;
static struct { bool have; int64_t bytes; int32_t lost, total, outorder; double jitter_ms, secs; } report;

/* ---- helpers ------------------------------------------------------------------ */
static void put(int x, int y, int w, uint8_t attr, const char *fmt, ...) {
    if (w <= 0 || y < 0 || y >= H) return;
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
static void be32(uint8_t *p, uint32_t v) { p[0] = v >> 24; p[1] = v >> 16; p[2] = v >> 8; p[3] = v; }
static uint32_t rd32(const uint8_t *p) { return (uint32_t)p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3]; }
static double mbit(uint64_t b, uint32_t ms) { return ms ? b * 8.0 / ms / 1000.0 : 0; }

/* Settings live in apps_data/throughput/settings.txt: server, port, mode. */
static void load(void) {
    int fd = lap_file_open("settings.txt", "r");
    if (fd < 0) return;
    char t[128] = {0};
    int n = lap_file_read(fd, t, sizeof(t) - 1);
    lap_file_close(fd);
    if (n > 0) {                                /* "server port udp duration rate" */
        char *p = strchr(t, ' ');
        if (!p) return;
        *p++ = 0;
        snprintf(server, sizeof(server), "%s", strcmp(t, "-") ? t : "");
        port = (int)strtol(p, &p, 10);
        mode_udp = (int)strtol(p, &p, 10);
        duration_i = (int)strtol(p, &p, 10);
        rate_i = (int)strtol(p, &p, 10);
    }
    if (port < 1 || port > 65535) port = 5001;
    mode_udp = !!mode_udp;
    if (duration_i < 0 || duration_i > 3) duration_i = 1;
    if (rate_i < 0 || rate_i > 4) rate_i = 0;
}
static void store(void) {
    int fd = lap_file_open("settings.txt", "w");
    if (fd < 0) return;
    char t[128];
    int n = snprintf(t, sizeof(t), "%s %d %d %d %d\n", server[0] ? server : "-", port, mode_udp, duration_i, rate_i);
    lap_file_write(fd, t, (size_t)n);
    lap_file_close(fd);
}

/* ---- the test ------------------------------------------------------------------ */
static void close_sock(void) {
    if (sock >= 0) { if (mode_udp) lap_udp_close(sock); else lap_tcp_close(sock); }
    sock = -1;
}
static void finish(int st, const char *why) {
    close_sock();
    state = st;
    stop_at = lap_millis() < t_end ? lap_millis() : t_end;
    uint32_t ms = t0 ? stop_at - t0 : 0;
    if (st == S_DONE) say("%s %.1f s  %.2f MB  %.2f Mbit/s", mode_udp ? "UDP" : "TCP", ms / 1000.0, bytes / 1e6, mbit(bytes, ms));
    else if (why) say("%s", why);
}
static void start(void) {
    if (!server[0]) { say("Set the server first"); return; }
    close_sock();
    nsamples = 0; peak = 0; bytes = sample_bytes = total_sent = 0;
    t0 = t_end = stop_at = 0;
    seq = 0; dgrams = fins = 0;
    memset(&report, 0, sizeof(report));
    /* iperf2 pattern; for TCP the first 24 bytes are a client header with
       no flags, which any iperf2 server reads as a plain test. */
    for (unsigned i = 0; i < sizeof(buf); i++) buf[i] = (uint8_t)('0' + i % 10);
    if (mode_udp) memset(buf + 12, 0, 52);       /* datagram header, then a flagless client header */
    if (!mode_udp) {
        sock = lap_tcp_connect(server, port, 5000);
        if (sock < 0) { finish(S_FAIL, "Cannot connect"); return; }
        state = S_CONNECT;
    } else {
        lap_socket_address_t any = {.size = sizeof(any), .version = 1};
        sock = lap_udp_open(&any);
        if (sock < 0) { finish(S_FAIL, "No UDP socket"); return; }
        /* lap_udp wants a numeric address: look the name up first (API 2.8). */
        bool numeric = strlen(server) < sizeof(server_ip);
        for (const char *c = server; *c; c++) numeric = numeric && ((*c >= '0' && *c <= '9') || *c == '.');
        if (numeric) {
            snprintf(server_ip, sizeof(server_ip), "%s", server);
            state = S_RUN;
            t0 = last_sample = lap_millis();
            t_end = t0 + durations[duration_i] * 1000u;
        } else {
            dns_h = lap_dns_resolve(server, 5000);
            if (dns_h < 0) { finish(S_FAIL, "Cannot look up the name"); return; }
            state = S_CONNECT;
        }
    }
    store();
    say("%s to %s:%d", mode_udp ? "UDP" : "TCP", server, port);
}
static void sample(uint32_t now) {
    while (now - last_sample >= SAMPLE_MS && nsamples < MAXS) {
        int32_t kbit = (int32_t)(sample_bytes * 8 / SAMPLE_MS);
        samples[nsamples++] = kbit;
        if (kbit > peak) peak = kbit;
        sample_bytes = 0;
        last_sample += SAMPLE_MS;
    }
}
static void tcp_step(uint32_t now) {
    int s = lap_tcp_state(sock);
    if (state == S_CONNECT) {
        if (s == LAP_TCP_PENDING) return;
        if (s != LAP_TCP_CONNECTED) { finish(S_FAIL, s == LAP_NET_ETIMEOUT ? "Connect timed out" : s == LAP_NET_EDNS ? "Name not found" : "Refused: is iperf -s running?"); return; }
        state = S_RUN;
        t0 = last_sample = now;
        t_end = t0 + durations[duration_i] * 1000u;
    }
    if (s < 0) { finish(bytes ? S_DONE : S_FAIL, "Connection closed by server"); return; }
    /* Keep the worker's 4 KB queue full for a few ms of this frame. */
    static const uint8_t header[24];
    for (int i = 0; i < 64 && now < t_end && lap_millis() - now < 4; i++) {
        int n = total_sent < 24 ? lap_tcp_send(sock, header + total_sent, 24 - (size_t)total_sent)
                                : lap_tcp_send(sock, buf, sizeof(buf));
        if (n <= 0) break;
        total_sent += (uint64_t)n;
        bytes += (uint64_t)n; sample_bytes += (uint64_t)n;
    }
    sample(now);
    if (now >= t_end) finish(S_DONE, NULL);
}
static void udp_datagram(int32_t id, uint32_t now) {
    be32(buf, (uint32_t)id);
    be32(buf + 4, now / 1000);
    be32(buf + 8, now % 1000 * 1000);
}
static void udp_step(uint32_t now) {
    if (state == S_CONNECT) {
        int r = lap_dns_result(dns_h, server_ip);
        if (r == LAP_IO_EAGAIN) return;
        dns_h = 0;
        if (r) { finish(S_FAIL, r == LAP_IO_ENOTFOUND ? "Name not found" : "Lookup failed"); return; }
        state = S_RUN;
        t0 = last_sample = now;
        t_end = t0 + durations[duration_i] * 1000u;
    }
    lap_socket_address_t to = {.size = sizeof(to), .version = 1, .port = (uint16_t)port};
    memcpy(to.ip, server_ip, sizeof(to.ip));
    if (state == S_RUN) {
        /* Pace: the datagrams due by now at the chosen rate. */
        uint64_t due = (uint64_t)rates[rate_i] * 1000000u / 8 * (now - t0) / 1000 / DGRAM + 1;
        while (dgrams < due && now < t_end) {
            udp_datagram(seq, now);
            int n = lap_udp_send(sock, &to, buf, DGRAM);
            if (n == LAP_IO_EAGAIN) break;
            if (n < 0) { finish(S_FAIL, "UDP send failed"); return; }
            seq++; dgrams++;
            bytes += DGRAM; sample_bytes += DGRAM;
        }
        sample(now);
        if (now >= t_end) { state = S_FINISH; fin_at = 0; }
    }
    if (state == S_FINISH) {
        /* iperf2: a negative sequence ends the test; the server answers with
           its report. Ten tries, 250 ms apart. */
        if (fins < 10 && now - fin_at >= 250) {
            udp_datagram(-seq, now);
            if (lap_udp_send(sock, &to, buf, DGRAM) > 0) { fins++; fin_at = now; }
        }
        uint8_t r[DGRAM];
        lap_socket_address_t from = {.size = sizeof(from), .version = 1};
        int n = lap_udp_recv(sock, &from, r, sizeof(r));
        if (n >= 12 + 40) {
            const uint8_t *h = r + 12;
            report.have = (rd32(h) & 0x80000000u) != 0;
            report.bytes = (int64_t)rd32(h + 4) << 32 | rd32(h + 8);
            report.secs = (int32_t)rd32(h + 12) + (int32_t)rd32(h + 16) / 1e6;
            report.lost = (int32_t)rd32(h + 20);
            report.outorder = (int32_t)rd32(h + 24);
            report.total = (int32_t)rd32(h + 28);
            report.jitter_ms = ((int32_t)rd32(h + 32) + (int32_t)rd32(h + 36) / 1e6) * 1000.0;
            finish(S_DONE, NULL);
        } else if (fins >= 10 && now - fin_at > 1000) {
            finish(S_DONE, NULL);
            say("No server report (is iperf -s -u running?)");
        }
    }
}

/* ---- drawing -------------------------------------------------------------------- */
static uint32_t now;            /* lap_millis() for this frame: every animation reads it */
static float needle;            /* where the gauge's needle is, 0..1 of full scale; it eases to the reading */
static uint32_t needle_at;
static float histf[MAXS];
static uint8_t hue(uint8_t h) { return LAP_ATTR(h, LAP_BLACK); }
static void cell(int x, int y, int16_t ch, uint8_t a) { if (x >= 0 && x < W && y >= 0 && y < H) lap_put_cell(x, y, ch, a); }
/* Full scale in kbit/s: 1, 2 or 5 x 10^n above the peak (and the UDP rate asked for). */
static int32_t full_scale(void) {
    int32_t top = peak > 1000 ? peak : 1000;
    if (mode_udp && rates[rate_i] * 1100 > top) top = rates[rate_i] * 1100;      /* room over the asked rate */
    for (int32_t b = 1000;; b *= 10) {
        if (top <= b) return b;
        if (top <= 2 * b) return 2 * b;
        if (top <= 5 * b) return 5 * b;
    }
}
static void mbit_text(char *out, size_t cap, float v) { snprintf(out, cap, v >= 100 ? "%.0f" : v >= 10 ? "%.1f" : "%.2f", v); }

/* The speedometer: a half dial of rx by ry cells on the hub cx, cy, lit
   green to amber to red up to the needle, a peak mark held on the arc, the
   scale at the quarters. Cells are taller than wide, so ry is about rx / 1.7. */
static void gauge(int cx, int cy, int rx, int ry, float frac, float pk, float full) {
    const float pi = 3.14159265f;
    const int steps = rx * 4;
    for (int t = 0; t <= steps; t++) {
        const float p = (float)t / (float)steps, a = pi * (1.0f - p);
        const int x = cx + (int)roundf((float)rx * cosf(a)), y = cy - (int)roundf((float)ry * sinf(a));
        const float d = a * 180.0f / pi;
        const int16_t ch = d > 157.5f || d < 22.5f ? '|' : d > 112.5f ? '/' : d > 67.5f ? '-' : '\\';
        cell(x, y, ch, hue(LAP_UI_FRAME));
    }
    /* the band inside the rim: dots lit green, amber, red up to the needle */
    const int brx = rx - 3, bry = ry - 2, bsteps = brx * 3;
    for (int t = 0, px = -1, py = -1; t <= bsteps && brx > 1 && bry > 0; t++) {
        const float p = (float)t / (float)bsteps, a = pi * (1.0f - p);
        const int x = cx + (int)roundf((float)brx * cosf(a)), y = cy - (int)roundf((float)bry * sinf(a));
        if (x == px && y == py) continue;
        px = x; py = y;
        const uint8_t zone = p < 0.6f ? LAP_UI_LIVE : p < 0.85f ? LAP_UI_WARN : LAP_UI_ALARM;
        const bool lit = frac > 0 && p <= frac + 0.001f;
        cell(x, y, lit ? ':' : '.', lit ? hue(zone) : LAP_UI_FAINT);
    }
    /* ticks on the rim at the quarters, and the peak held as a star on the band */
    for (int q = 0; q <= 4; q++) {
        const float a = pi * (1.0f - q / 4.0f);
        cell(cx + (int)roundf((float)rx * cosf(a)), cy - (int)roundf((float)ry * sinf(a)), '+', LAP_UI_TEXT);
    }
    if (pk > 0) {
        const float a = pi * (1.0f - (pk > 1 ? 1 : pk));
        cell(cx + (int)roundf((float)brx * cosf(a)), cy - (int)roundf((float)bry * sinf(a)), '*', hue(LAP_UI_VALUE));
    }
    for (int q = 0; q <= 4; q++) {
        const float a = pi * (1.0f - q / 4.0f);
        char t[12];
        mbit_text(t, sizeof(t), full * (float)q / 4.0f);
        if (q == 0) snprintf(t, sizeof(t), "0");
        const int n = (int)strlen(t);
        int x = cx + (int)roundf((float)(rx + 2) * cosf(a)), y = cy - (int)roundf((float)(ry + 1) * sinf(a));
        x -= q == 2 ? n / 2 : q < 2 ? n - (q == 0 ? 0 : 1) : (q == 4 ? -1 : 0);
        put(x, y, n, LAP_UI_DIM, "%s", t);
    }
    /* the needle, from the hub out to most of the radius */
    const float f = frac < 0 ? 0 : frac > 1 ? 1 : frac, a = pi * (1.0f - f);
    const float d = a * 180.0f / pi;
    const int16_t ch = d < 22.5f || d > 157.5f ? '-' : d < 67.5f ? '/' : d < 112.5f ? '|' : '\\';
    int lx = cx, ly = cy;
    for (int k = 1; k <= rx * 2; k++) {
        const float u = 0.8f * (float)k / (float)(rx * 2);
        const int x = cx + (int)roundf(u * (float)rx * cosf(a)), y = cy - (int)roundf(u * (float)ry * sinf(a));
        if (x == lx && y == ly) continue;
        cell(x, y, ch, hue(LAP_UI_VALUE));
        lx = x; ly = y;
    }
    for (int x = cx - rx; x <= cx + rx; x++) cell(x, cy, x == cx ? 'O' : '=', x == cx ? hue(LAP_UI_VALUE) : hue(LAP_UI_FRAME));
}
/* The header: the board and the server with the stream running between
   them, beside the test's facts. */
static void header(bool running) {
    const uint8_t f = hue(LAP_UI_FRAME);
    put(1, 1, 4, f, ".--.");
    put(1, 2, 4, f, "|LS|");
    put(1, 3, 4, f, "'--'");
    put(13, 1, 4, f, ".--.");
    put(13, 2, 4, f, "|PC|");
    put(13, 3, 4, f, "'--'");
    put(1, 4, 16, LAP_UI_FAINT, "board   iperf");
    for (int i = 0; i < 8; i++) {
        int16_t ch = '-';
        uint8_t a = LAP_UI_FAINT;
        if (running) { ch = (i + 8 - (int)(now / 90 % 8)) % 4 == 0 ? '>' : '='; a = ch == '>' ? hue(LAP_UI_LIVE) : f; }
        else if (state == S_DONE) { ch = '='; a = f; }
        cell(5 + i, 2, ch, a);
    }
    if (state == S_FAIL) cell(9, 2, 'x', hue(LAP_UI_ALARM));
    /* facts: the two lines a tap changes, then the state */
    const int x = 19, w = W - x - 1;
    const uint8_t v = LAP_ATTR(LAP_WHITE | LAP_BRIGHT, LAP_BLUE);
    put(x, 1, w, LAP_UI_DIM, "iperf2 server, port %d", port);
    lap_fill(x, 2, w, 1, ' ', v);
    if (server[0]) put(x + 1, 2, w - 2, v, "%s:%d", server, port);
    else put(x + 1, 2, w - 2, v, "(tap to set)");
    char opts[48];
    if (mode_udp) snprintf(opts, sizeof(opts), "UDP  %d s  %d Mbit/s", durations[duration_i], rates[rate_i]);
    else snprintf(opts, sizeof(opts), "TCP  %d s", durations[duration_i]);
    put(x, 3, w, LAP_UI_TEXT, "%s", opts);
    int cx = lap_ui_chip(x, 4, mode_udp ? "UDP" : "TCP", true, LAP_UI_FRAME);
    static const char *const words[] = {"IDLE", "CONNECT", "RUN", "FINISH", "DONE", "FAILED"};
    lap_ui_chip(cx, 4, words[state], state != S_IDLE, state == S_FAIL ? LAP_UI_ALARM : running ? LAP_UI_LIVE : LAP_UI_VALUE);
}
/* Mbit/s in braille, newest at the right, in a frame with its scale. */
static void history(int x, int y, int w, int h, float full) {
    lap_box(x, y, w, h, "Mbit/s  every 0.5 s", hue(LAP_UI_FRAME));
    const int gh = h - 2, gx = x + 7, gw = w - 8;
    if (gh < 1 || gw < 2) return;
    char t[12];
    mbit_text(t, sizeof(t), full);
    put(x + 1, y + 1, 5, LAP_UI_DIM, "%5s", t);
    put(x + 1, y + gh, 5, LAP_UI_DIM, "%5s", "0");
    for (int r = 0; r < gh; r++) cell(gx - 1, y + 1 + r, '|', LAP_UI_FAINT);
    if (nsamples) {
        for (int i = 0; i < nsamples; i++) histf[i] = samples[i] / 1000.0f;
        lap_ui_history(gx, y + 1, gw, gh, histf, nsamples, 0, full, LAP_UI_LIVE);
    } else {
        for (int c = gx; c < gx + gw; c++) cell(c, y + gh, '_', LAP_UI_FAINT);
        cell(gx + (int)(now / 120 % (unsigned)gw), y + gh, '^', hue(LAP_UI_FRAME));
    }
}
static void draw(void) {
    W = lap_cols(); H = lap_rows();
    now = lap_millis();
    if (dialog > 0) {
        char t[64];
        int v = 0, r = lap_dialog_poll(dialog, t, sizeof(t), &v);
        if (r) {
            lap_dialog_cancel(dialog);
            dialog = 0;
            if (r == 1 && dialog_for == 0) {
                char *colon = strchr(t, ':');
                if (colon) { *colon = 0; int p = atoi(colon + 1); if (p > 0 && p < 65536) port = p; }
                snprintf(server, sizeof(server), "%s", t);
                store();
            }
        }
    }
    if (sock >= 0 && (state == S_CONNECT || state == S_RUN || state == S_FINISH)) {
        if (mode_udp) udp_step(now); else tcp_step(now);
    }
    bool running = state == S_CONNECT || state == S_RUN || state == S_FINISH;
    kr = lap_ui_keys_rows(5);
    /* Five keys wrap to two rows on the handheld; give those rows a finger's
       height (the area has room: the graph stops growing long before). */
    if (W < 60 && kr >= 7) kr += 3;
    const int bottom = H - kr;                  /* the first row of the keys */
    lap_clear(LAP_ATTR(LAP_WHITE, LAP_BLACK));
    lap_fill(0, 0, W, 1, ' ', LAP_ATTR(LAP_BLACK, LAP_CYAN));
    put(1, 0, W - 2, LAP_ATTR(LAP_BLACK, LAP_CYAN), "THROUGHPUT  iperf2 client");
    static const char *const names[] = {" IDLE ", " CONNECTING ", " RUNNING ", " FINISHING ", " DONE ", " FAILED "};
    uint8_t sa = running ? LAP_ATTR(LAP_BLACK, LAP_GREEN) : state == S_FAIL ? LAP_ATTR(LAP_WHITE | LAP_BRIGHT, LAP_RED) : LAP_ATTR(LAP_BLACK, LAP_WHITE);
    put(W - (int)strlen(names[state]) - 1, 0, 14, sa, "%s", names[state]);
    header(running);

    uint32_t end = running ? (now < t_end ? now : t_end) : stop_at;
    uint32_t ms = t0 && end > t0 ? end - t0 : 0;
    const float cur = nsamples && (running || state == S_DONE) ? samples[nsamples - 1] / 1000.0f : 0;
    const int32_t fs = full_scale();
    const float full = fs / 1000.0f;
    /* the needle eases toward the reading: a quarter of the way every ~60 ms */
    const float target = running ? cur / full : state == S_DONE ? (float)(mbit(bytes, ms) / full) : 0;
    const uint32_t dt = now - needle_at;
    needle_at = now;
    const float k = dt > 400 ? 1.0f : (float)dt / 240.0f;
    needle += (target - needle) * (k > 1 ? 1 : k);
    if (needle < 0) needle = 0;

    const bool wide = W >= 90;
    const int top = 6, avail = bottom - top;
    const int lw = wide ? 44 : W, gy = top;
    int ry = wide ? avail - 7 : avail > 40 ? 10 : 7;
    if (ry > 10) ry = 10;
    if (ry < 3) ry = 3;
    int rx = (int)((float)ry * 1.7f + 0.5f);
    if (rx > lw / 2 - 8) rx = lw / 2 - 8;      /* room for the scale labels inside the bezel */
    const int gh = ry + 6;                      /* frame, labels, the arc, the hub, the readout */
    lap_ui_bezel(0, gy, lw, gh, running ? "SPEED  LIVE" : state == S_DONE ? "SPEED  AVERAGE" : "SPEED");
    const int cx = lw / 2, cy = gy + 2 + ry;
    gauge(cx, cy, rx, ry, needle, peak / (float)fs, full);
    char t[16], line[40];
    mbit_text(t, sizeof(t), running ? cur : (float)mbit(bytes, ms));
    snprintf(line, sizeof(line), "%s Mbit/s", t);
    put(cx - (int)strlen(line) / 2, cy + 1, (int)strlen(line), running || state == S_DONE ? hue(LAP_UI_VALUE) : LAP_UI_DIM, "%s", line);
    if (state == S_IDLE) put(cx - 9, cy - ry / 2, 18, LAP_UI_DIM, now / 700 % 2 ? "  START to run   " : "  iperf2 client  ");

    /* the readings, the time used, the history, the server's report */
    const int x0 = wide ? lw + 1 : 1, w0 = W - x0 - 1;
    int y = wide ? gy : gy + gh;
    put(x0, y++, w0, LAP_UI_TEXT, "now %.2f   avg %.2f   peak %.2f Mbit/s", (double)cur, mbit(bytes, ms), peak / 1000.0);
    put(x0, y++, w0, LAP_UI_DIM, "%.1f s   %.2f MB%s", ms / 1000.0, bytes / 1e6, mode_udp ? "" : " accepted");
    const float tf = durations[duration_i] > 0 ? (float)ms / (durations[duration_i] * 1000.0f) : 0;
    put(x0, y, 5, LAP_UI_DIM, "TIME");
    lap_ui_meter(x0 + 5, y, w0 - 13, tf > 1 ? 1 : tf, running ? LAP_UI_LIVE : LAP_UI_FRAME);
    put(x0 + w0 - 7, y++, 7, hue(LAP_UI_VALUE), "%3d s", durations[duration_i]);
    if (mode_udp && dgrams) put(x0, y, w0, LAP_UI_DIM, "%u datagrams of %d bytes", (unsigned)dgrams, DGRAM);
    y++;
    const int rep = report.have ? (report.outorder ? 4 : 3) : state == S_IDLE ? 2 : 0;
    int hh = bottom - y - rep - 1;
    if (hh > 20) hh = 20;
    if (hh < 4) hh = 4;
    history(x0, y, w0, hh, full);
    y += hh;
    if (report.have) {
        put(x0, y++, w0, hue(LAP_UI_WARN), "SERVER REPORT");
        double pct = report.total ? report.lost * 100.0 / report.total : 0;
        put(x0, y++, w0, LAP_UI_TEXT, "%.2f MB in %.1f s  %.2f Mbit/s", report.bytes / 1e6,
            report.secs, report.secs > 0 ? report.bytes * 8 / report.secs / 1e6 : 0);
        put(x0, y++, w0, report.lost ? hue(LAP_UI_WARN) : LAP_UI_TEXT, "lost %d/%d (%.1f%%)  jitter %.3f ms", report.lost,
            report.total, pct, report.jitter_ms);
        if (report.outorder) put(x0, y++, w0, LAP_UI_DIM, "%d out of order", report.outorder);
    } else if (!running && state == S_IDLE) {
        put(x0, y++, w0, LAP_UI_DIM, "On a computer: iperf -s   (UDP: iperf -s -u)");
        put(x0, y++, w0, LAP_UI_DIM, "iperf2 only; iperf3 will not answer.");
    }
    /* a note for eight seconds over the title, left of the state when it fits; a tap there clears it */
    if (status[0] && now - status_at < 8000) {
        const uint8_t a = LAP_ATTR(LAP_BLACK, LAP_YELLOW);
        const int room = (int)strlen(status) + 3 <= W - (int)strlen(names[state]) - 1 ? W - (int)strlen(names[state]) - 1 : W;
        lap_fill(0, 0, room, 1, ' ', a);
        put(1, 0, room - 2, a, "%s", status);
    }
    char dur[8], rate[12];
    snprintf(dur, sizeof(dur), "%d s", durations[duration_i]);
    snprintf(rate, sizeof(rate), mode_udp ? "%d Mb/s" : "-", rates[rate_i]);
    const lap_btn_t keys[5] = {{running ? "STOP" : "START", NULL, 's', running, false, false},
                               {"SERVER", NULL, 'e', false, running, false},
                               {"MODE", mode_udp ? "UDP" : "TCP", 'u', false, running, false},
                               {"TIME", dur, 't', false, running, false},
                               {"RATE", rate, 'r', false, running || !mode_udp, false}};
    lap_ui_keys(0, bottom, W, kr, keys, 5, -1);
}

/* ---- input ---------------------------------------------------------------------- */
static void act(int b) {
    bool running = state == S_CONNECT || state == S_RUN || state == S_FINISH;
    if (b == 0) { if (running) finish(S_DONE, "Stopped"); else start(); return; }
    if (running) return;
    if (b == 1 && dialog <= 0) {
        char now[72];
        snprintf(now, sizeof(now), server[0] ? "%s:%d" : "", server, port);
        dialog = lap_text_input("iperf2 server (host or host:port)", now, 70, LAP_TEXT_LOWER);
        dialog_for = 0;
    } else if (b == 2) { mode_udp = !mode_udp; store(); }
    else if (b == 3) { duration_i = (duration_i + 1) % 4; store(); }
    else if (b == 4 && mode_udp) { rate_i = (rate_i + 1) % 5; store(); }
}
static bool key(lap_key_t k, char ch) {
    if (k == LAP_KEY_ENTER) { act(0); return true; }
    if (k != LAP_KEY_CHAR) return false;
    static const char keys[] = "  eutr";            /* SPACE/S start, E server, U mode, T time, R rate */
    if (ch == 's') ch = ' ';
    const char *p = ch ? strchr(keys + 1, ch) : NULL;
    if (!p) return false;
    act((int)(p - keys) - 1);
    return true;
}
static bool touch(int col, int row) {
    if (row >= H - kr) { const int b = lap_ui_key_hit(col, row); if (b >= 0) act(b); return true; }
    if (row == 0) { if (status[0] && lap_millis() - status_at < 8000) status[0] = 0; return true; }
    if (row == 2) { act(1); return true; }
    if (row == 3) { act(2); return true; }
    /* Every tap on this screen is the app's: one left unused reaches the
       board as UP, ENTER or DOWN by where it fell, and ENTER acts. */
    return true;
}
static void enter(void) {
    state = S_IDLE;
    sock = -1;
    kr = 1;
    dialog = 0;
    status[0] = 0;
    nsamples = 0;
    needle = 0;
    needle_at = lap_millis();
    load();
}
static void leave(void) {
    close_sock();
    if (dialog > 0) lap_dialog_cancel(dialog);
}

LAP_APP(.id = "throughput", .name = "THROUGHPUT", .sub = "iperf2 TCP/UDP", .version = "1.1.0",
        .author = "LakeShark", .icon = LAP_ICON_WAVE, .color = LAP_GREEN, .flags = LAP_CAP_NETWORK,
        .enter = enter, .leave = leave, .draw = draw, .key = key, .touch = touch);
