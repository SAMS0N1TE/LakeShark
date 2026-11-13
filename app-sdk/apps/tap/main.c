/* TAP: a packet analyser for every radio the board can listen on.
   Wi-Fi 802.11 and Bluetooth advertising through the API 2.7 capture calls,
   LoRa through lap_packet_*, and the board's own decodes as text records.
   Receive only: nothing is sent, joined, decrypted or injected.

   Wide screens (a desk window, a remote screen) get three panes: packets,
   the protocol tree, the bytes. The handheld shows one pane at a time. */
#include "tap.h"

#define MAXPK 4096u
#define ARENA (1536u * 1024u)
#define CHUNK (16u * 1024u)
#define HIST 24

LAP_ICON_PX("....................", "....................", "..........e.........", ".........eee........",
            "........ee.ee.......", ".......ee...ee......", "......ee..b..ee.....", ".....ee..bbb..ee....",
            "....ee..bb.bb..ee...", "...ee..bb.a.bb..ee..", "..ee..bb.aaa.bb..ee.", "......b.aa.aa.b.....",
            "........a.f.a.......", "..........f.........", "..........f.........", ".........fff........",
            "....................", "....................");

/* ---- the store ------------------------------------------------------------ */
static tap_pkt_t *pk;
static uint8_t *arena, *chunk;
static uint32_t *view;                  /* numbers of the packets the filter shows */
static tap_line_t *lines;
static uint32_t first_no, next_no, aw;
static int v0, vn, sel, top, tsel, ttop, htop;
static tap_stats_t *stats;
static tap_filter_t fprog;
static char ftext[96], fedit[96], status[96];
static bool editing, paused, colours, autoscroll, deltas;
static uint64_t t0, wall0;
static uint32_t skipped;

/* ---- the sources ------------------------------------------------------------ */
typedef struct { bool on; int h, err; uint32_t n, lost; char why[72]; uint16_t hist[HIST]; uint64_t epoch; bool timed; } src_t;
static src_t src[R_COUNT];
static int wifi_hop, wifi_types, lora_preset, lora_radio, min_rssi[3];
static bool drop_link;
static int confirm_h, text_h;
static uint32_t hist_sec;
static const struct { const char *name; uint32_t khz, bw; uint8_t sf, cr, sync; } presets[] = {
    {"MESHCORE US 910.525", 910525, 62500, 7, 5, 0x12}, {"MESHTASTIC US LONGFAST", 906875, 250000, 11, 5, 0x2B},
    {"MESHTASTIC EU LONGFAST", 869525, 250000, 11, 5, 0x2B}, {"MESHCORE EU 869.618", 869618, 62500, 8, 8, 0x12}};

/* ---- screens ----------------------------------------------------------------- */
enum { V_MAIN, V_STATS, V_HELP };
static int W, H, screen, focus, hdetail, stab, sscroll;
static bool desk;
static int list_y, list_h, tree_y, tree_h, hex_y, hex_h;

enum { M_NONE, M_ROOT, M_FILE, M_CAPTURE, M_VIEW, M_ANALYZE, M_STATS, M_CTX };
enum { A_SUB_FILE, A_SUB_CAPTURE, A_SUB_VIEW, A_SUB_ANALYZE, A_SUB_STATS, A_SAVE_ALL, A_SAVE_SHOWN, A_CLEAR, A_QUIT,
       A_PAUSE, A_WIFI, A_WIFI_CH, A_WIFI_TYPES, A_WIFI_RSSI, A_WIFI_DROP, A_BLE, A_BLE_RSSI, A_LORA, A_LORA_PRESET,
       A_LORA_RSSI, A_DEC, A_COLOURS, A_AUTOSCROLL, A_TIME, A_FILTER_EDIT, A_FILTER_CLEAR, A_FOLLOW_DEV, A_FOLLOW_CONV,
       A_FILTER_PROTO, A_FILTER_NOT, A_QF_WIFI, A_QF_BLE, A_QF_LORA, A_QF_EAPOL, A_QF_NEAR, A_ST_HIER, A_ST_ENDP,
       A_ST_CONV, A_ST_CHAN, A_ST_RSSI, A_HELP, A_DETAIL };
static const uint8_t m_root[] = {A_SUB_CAPTURE, A_SUB_VIEW, A_SUB_ANALYZE, A_SUB_STATS, A_SUB_FILE, A_HELP};
static const uint8_t m_file[] = {A_SAVE_ALL, A_SAVE_SHOWN, A_CLEAR, A_QUIT};
static const uint8_t m_capture[] = {A_PAUSE, A_WIFI, A_WIFI_CH, A_WIFI_TYPES, A_WIFI_RSSI, A_WIFI_DROP, A_BLE, A_BLE_RSSI,
                                    A_LORA, A_LORA_PRESET, A_LORA_RSSI, A_DEC};
static const uint8_t m_view[] = {A_COLOURS, A_AUTOSCROLL, A_TIME};
static const uint8_t m_analyze[] = {A_FILTER_EDIT, A_FILTER_CLEAR, A_FOLLOW_DEV, A_FOLLOW_CONV, A_FILTER_PROTO, A_FILTER_NOT,
                                    A_QF_WIFI, A_QF_BLE, A_QF_LORA, A_QF_EAPOL, A_QF_NEAR};
static const uint8_t m_stats[] = {A_ST_HIER, A_ST_ENDP, A_ST_CONV, A_ST_CHAN, A_ST_RSSI};
static const uint8_t m_ctx[] = {A_DETAIL, A_FOLLOW_DEV, A_FOLLOW_CONV, A_FILTER_PROTO, A_FILTER_NOT, A_FILTER_CLEAR};
static int menu, msel, mx, my, mparent;

/* ---- saving ------------------------------------------------------------------- */
static struct { int stage, job; uint32_t no, end, written, bytes; bool shown; int vi; char path[64]; } save;

static void put(int x, int y, int w, uint8_t attr, const char *fmt, ...)
{
    char t[200];
    va_list ap;
    if (w <= 0 || y < 0 || y >= H || x >= W) return;
    va_start(ap, fmt);
    vsnprintf(t, sizeof(t), fmt, ap);
    va_end(ap);
    if (x + w > W) w = W - x;
    if ((size_t)w < sizeof(t)) t[w] = 0;
    lap_put_str(x, y, t, attr);
}
static tap_pkt_t *at_no(uint32_t no) { return &pk[no % MAXPK]; }
static uint32_t count(void) { return next_no - first_no; }
static int sel_no(void) { return vn && sel < vn ? (int)view[v0 + sel] : -1; }

static void evict(void)
{
    if (vn && view[v0] == first_no) {
        v0++; vn--;
        if (sel > 0) sel--;
        if (top > 0) top--;
    }
    first_no++;
}
static void view_push(uint32_t no)
{
    if ((unsigned)(v0 + vn) >= 2 * MAXPK) { memmove(view, view + v0, (size_t)vn * sizeof(*view)); v0 = 0; }
    view[v0 + vn++] = no;
    if (autoscroll && sel == vn - 2) sel = vn - 1;
}
/* One record into the store. data may be NULL when len is 0. */
static void add(tap_pkt_t *m, const uint8_t *data)
{
    src_t *s = &src[m->radio];
    if (paused) { skipped++; return; }
    if (m->radio < 3 && min_rssi[m->radio] && m->rssi < min_rssi[m->radio]) return;   /* capture filter */
    if (m->len > ARENA / 4) return;
    if (!s->timed) {                    /* radio clocks to the wall clock, once per source */
        s->timed = true;                /* wall time when the app opened + time open: steady on every board */
        s->epoch = wall0 + (uint64_t)lap_millis() * 1000u - m->ts_us;
    }
    m->ts_us += s->epoch;
    if (!t0) t0 = m->ts_us;
    if (aw + m->len > ARENA) aw = 0;
    while (count() && (count() >= MAXPK || (at_no(first_no)->len && at_no(first_no)->off < aw + m->len &&
                                           at_no(first_no)->off + at_no(first_no)->len > aw)))
        evict();
    m->no = next_no; m->off = aw;
    if (m->len) memcpy(arena + aw, data, m->len);
    aw += m->len;
    *at_no(next_no++) = *m;
    s->n++;
    s->hist[HIST - 1]++;
    tap_dis_t d = {0};
    tap_dissect(m, arena + m->off, &d);
    tap_stats_add(stats, m, &d);
    if (tap_filter_match(&fprog, m, arena + m->off, &d)) view_push(m->no);
}
static void refilter(void)
{
    const int keep = sel_no();
    v0 = vn = 0; sel = top = 0;
    for (uint32_t no = first_no; no != next_no; no++) {
        tap_pkt_t *p = at_no(no);
        tap_dis_t d = {0};
        tap_dissect(p, arena + p->off, &d);
        if (tap_filter_match(&fprog, p, arena + p->off, &d)) {
            if (keep >= 0 && no <= (uint32_t)keep) sel = vn;
            view[vn++] = no;
        }
    }
    if (keep < 0 && vn) sel = vn - 1;
    tsel = ttop = htop = 0;
}
static bool set_filter(const char *t)
{
    tap_filter_t f;
    if (!tap_filter_compile(t, &f)) { snprintf(status, sizeof(status), "filter: %s", f.err); return false; }
    fprog = f;
    snprintf(ftext, sizeof(ftext), "%s", t);
    refilter();
    if (ftext[0]) snprintf(status, sizeof(status), "filter applied: %d of %u shown", vn, (unsigned)count());
    else snprintf(status, sizeof(status), "filter cleared");
    return true;
}

/* ---- capture plumbing ---------------------------------------------------------- */
static void note(int r, int h)
{
    src_t *s = &src[r];
    s->h = h > 0 ? h : 0; s->err = h > 0 ? 0 : h;
    s->why[0] = 0;
    if (h <= 0 && (r == R_WIFI || r == R_BLE)) {
        lap_capture_status_t st;
        if (!lap_capture_status(r == R_WIFI ? LAP_CAPTURE_WIFI : LAP_CAPTURE_BLE, &st)) snprintf(s->why, sizeof(s->why), "%s", st.why);
    }
    if (h <= 0 && !s->why[0]) snprintf(s->why, sizeof(s->why), h == LAP_IO_EACCES || h == LAP_RX_EACCES ? "radio permission was not granted (%d)" :
                                       h == LAP_IO_EBUSY ? "the radio is in use (%d)" : "not available on this board (%d)", h);
}
static void stop(int r)
{
    src_t *s = &src[r];
    if (s->h > 0) {
        if (r == R_WIFI) lap_capture_stop(LAP_CAPTURE_WIFI);
        else if (r == R_BLE) lap_capture_stop(LAP_CAPTURE_BLE);
        else if (r == R_LORA) lap_packet_close(s->h);
        else lap_event_subscribe(0);
    }
    s->h = 0;
}
static void start(int r)
{
    src_t *s = &src[r];
    stop(r);
    s->err = 0; s->why[0] = 0;
    if (!s->on) return;
    if (r == R_WIFI) {
        lap_wifi_capture_config_t c = {.size = sizeof(c), .version = 1, .flags = drop_link ? LAP_CAPTURE_DROP_LINK : 0,
            .types = (uint8_t)(wifi_types == 1 ? LAP_WIFI_MGMT : wifi_types == 2 ? LAP_WIFI_MGMT | LAP_WIFI_DATA : 0), .dwell_ms = 300};
        if (wifi_hop == 1) { c.channel_count = 3; c.channels[0] = 1; c.channels[1] = 6; c.channels[2] = 11; }
        if (wifi_hop == 2) for (c.channel_count = 0; c.channel_count < 13; c.channel_count++) c.channels[c.channel_count] = (uint8_t)(c.channel_count + 1);
        note(r, lap_wifi_capture_start(&c));
    } else if (r == R_BLE) {
        lap_ble_capture_config_t c = {.size = sizeof(c), .version = 1};
        note(r, lap_ble_capture_start(&c));
    } else if (r == R_LORA) {
        lap_caps_t caps;
        lora_radio = -1;
        if (!lap_caps(&caps, sizeof(caps)))
            for (int i = 0; i < caps.radio_count && lora_radio < 0; i++) {
                lap_radio_modes_t m;
                if (!lap_radio_modes(i, &m) && (m.modes & LAP_MODE_LORA)) lora_radio = i;
            }
        if (lora_radio < 0) { s->err = -1; snprintf(s->why, sizeof(s->why), "no LoRa radio on this board"); return; }
        lap_packet_config_t c = {.size = sizeof(c), .version = 1, .frequency_hz = (uint64_t)presets[lora_preset].khz * 1000u,
            .bandwidth_hz = presets[lora_preset].bw, .sync_word = presets[lora_preset].sync, .preamble = 16, .modulation = LAP_MOD_LORA,
            .sf = presets[lora_preset].sf, .coding_rate = presets[lora_preset].cr, .crc = 1};
        note(r, lap_packet_open(lora_radio, &c));
    } else {
        const int e = lap_event_subscribe(0xFFFF);
        s->h = e < 0 ? 0 : 1; s->err = e < 0 ? e : 0;
        if (e < 0) snprintf(s->why, sizeof(s->why), "radio permission was not granted");
    }
}
static void poll(void)
{
    static uint8_t buf[LAP_CAPTURE_SNAP_MAX];
    const uint32_t sec = lap_millis() / 1000;
    for (; hist_sec < sec; hist_sec++)
        for (int r = 0; r < R_COUNT; r++) { memmove(src[r].hist, src[r].hist + 1, sizeof(src[r].hist) - sizeof(src[r].hist[0])); src[r].hist[HIST - 1] = 0; }
    for (int r = R_WIFI; r <= R_BLE; r++) {
        if (src[r].h <= 0) continue;
        lap_capture_meta_t m;
        for (int i = 0, n; i < (r == R_WIFI ? 64 : 32) && (n = lap_capture_read(src[r].h, buf, sizeof(buf), &m)) >= 0; i++) {
            tap_pkt_t p = {.ts_us = m.timestamp_us, .len = (uint16_t)n, .orig = m.original_length, .rate = m.rate_500kbps,
                .rssi = m.rssi, .radio = (uint8_t)r, .ch = m.channel, .kind = m.kind, .addr_type = m.addr_type};
            memcpy(p.addr, m.addr, 6);
            src[r].lost = m.dropped;
            add(&p, buf);
        }
        if (!(lap_frame() % 25)) {
            lap_capture_status_t st;
            if (!lap_capture_status(src[r].h, &st)) { src[r].lost = st.dropped; if (st.reason) snprintf(src[r].why, sizeof(src[r].why), "%s", st.why); }
        }
    }
    if (src[R_LORA].h > 0) {
        lap_packet_meta_t m;
        for (int i = 0, n; i < 8 && (n = lap_packet_read(src[R_LORA].h, buf, 255, &m)) > 0; i++) {
            tap_pkt_t p = {.ts_us = m.timestamp_us, .len = (uint16_t)n, .orig = (uint16_t)n, .radio = R_LORA,
                .rssi = (int8_t)(m.rssi_tenths_dbm == INT16_MIN ? 0 : m.rssi_tenths_dbm / 10),
                .snr10 = (int16_t)(m.snr_tenths_db == INT16_MIN ? 0 : m.snr_tenths_db), .freq_khz = (uint32_t)(m.frequency_hz / 1000),
                .sf = presets[lora_preset].sf, .sync = presets[lora_preset].sync, .bw = (uint8_t)(presets[lora_preset].bw / 125000)};
            if (!p.freq_khz) p.freq_khz = presets[lora_preset].khz;
            src[R_LORA].lost = m.dropped;
            add(&p, buf);
        }
    } else if (src[R_LORA].on && !(lap_frame() % 100)) start(R_LORA);       /* the radio may arrive later */
    if (src[R_DEC].h > 0) {
        lap_event_t e;
        for (int i = 0; i < 8 && lap_event_next(&e) == 1; i++) {
            unsigned kind = e.kind16 ? e.kind16 : e.kind, bit = 0;
            while (bit < 12 && !(kind & (1u << bit))) bit++;
            memset(buf, 0, 24);
            snprintf((char *)buf, 24, "%s", e.from);
            const size_t tl = strnlen(e.text, sizeof(e.text));
            memcpy(buf + 24, e.text, tl);
            tap_pkt_t p = {.ts_us = (uint64_t)lap_millis() * 1000u, .len = (uint16_t)(24 + tl), .orig = (uint16_t)(24 + tl),
                .radio = R_DEC, .kind = (uint8_t)bit, .freq_khz = e.address};
            add(&p, buf);
        }
    }
    if (confirm_h > 0) {
        int v = 0;
        char none[4];
        const int r = lap_dialog_poll(confirm_h, none, sizeof(none), &v);
        if (r) { confirm_h = 0; drop_link = r == 1 && v == 1; if (drop_link) start(R_WIFI); }
    }
    if (text_h > 0) {
        char t[96];
        const int r = lap_dialog_poll(text_h, t, sizeof(t), NULL);
        if (r) { text_h = 0; if (r == 1) set_filter(t); }
    }
}

/* ---- pcapng to the card ------------------------------------------------------------ */
static void save_begin(bool shown)
{
    lap_time_t t;
    if (save.stage) return;
    lap_time(&t);
    memset(&save, 0, sizeof(save));
    snprintf(save.path, sizeof(save.path), "/sdcard/captures/tap-%04d%02d%02d-%02d%02d%02d.pcapng", t.year, t.month, t.day, t.hour, t.minute, t.second);
    save.shown = shown; save.no = first_no; save.end = next_no;
    save.job = lap_sd_mkdir("/sdcard/captures");
    save.stage = 1;
    snprintf(status, sizeof(status), "saving %s", save.path);
}
static void save_step(void)
{
    if (!save.stage) return;
    if (save.job >= 0) {
        const int r = lap_sd_poll(save.job);
        if (r == LAP_SD_PENDING) return;
        if (r < 0 && save.stage > 1) { snprintf(status, sizeof(status), "save failed (%d): is there a card?", r); save.stage = 0; return; }
    }
    size_t n = 0;
    if (save.stage == 1) { n = tap_pcapng_header(chunk, CHUNK); save.stage = 2; }
    else {
        while (n + TAP_PCAPNG_MAX < CHUNK) {
            uint32_t no;
            if (save.shown) { if (save.vi >= vn) break; no = view[v0 + save.vi++]; }
            else { if (save.no == save.end) break; no = save.no++; }
            if (no - first_no >= count()) continue;         /* dropped from the ring meanwhile */
            const tap_pkt_t *p = at_no(no);
            const size_t k = tap_pcapng_packet(p, arena + p->off, chunk + n, CHUNK - n, 0);
            if (k) save.written++;
            n += k;
        }
        if (!n) {
            snprintf(status, sizeof(status), "saved %u packets, %u bytes: %s", (unsigned)save.written, (unsigned)save.bytes, save.path + 8);
            lap_log("saved %u packets %u bytes %s", (unsigned)save.written, (unsigned)save.bytes, save.path);
            save.stage = 0;
            return;
        }
    }
    save.job = lap_sd_write(save.path, save.bytes ? LAP_SD_APPEND : 0, chunk, (uint32_t)n);
    save.bytes += (uint32_t)n;
    if (save.job < 0) { snprintf(status, sizeof(status), "save failed (%d)", save.job); save.stage = 0; }
}

/* ---- menus: one tree, a dropdown on wide screens, a box on the handheld ---------------- */
static int menu_items(int m, const uint8_t **out)
{
    switch (m) {
    case M_ROOT: *out = m_root; return sizeof(m_root);
    case M_FILE: *out = m_file; return sizeof(m_file);
    case M_CAPTURE: *out = m_capture; return sizeof(m_capture);
    case M_VIEW: *out = m_view; return sizeof(m_view);
    case M_ANALYZE: *out = m_analyze; return sizeof(m_analyze);
    case M_STATS: *out = m_stats; return sizeof(m_stats);
    default: *out = m_ctx; return sizeof(m_ctx);
    }
}
static const char *onoff(int r) { return !src[r].on ? "OFF" : src[r].h > 0 ? "ON" : "ON (unavailable)"; }
static void rssi_label(char *b, size_t n, const char *what, int v) { if (v) snprintf(b, n, "%s MIN RSSI   < %d dBm >", what, v); else snprintf(b, n, "%s MIN RSSI   < any >", what); }
static void label(int a, char *b, size_t n)
{
    static const char *const hop[3] = {"STAY ON LINK CHANNEL", "HOP 1 / 6 / 11", "HOP 1-13"};
    static const char *const types[3] = {"ALL", "MANAGEMENT", "MGMT + DATA"};
    switch (a) {
    case A_SUB_FILE: snprintf(b, n, "FILE              >"); break;
    case A_SUB_CAPTURE: snprintf(b, n, "CAPTURE           >"); break;
    case A_SUB_VIEW: snprintf(b, n, "VIEW              >"); break;
    case A_SUB_ANALYZE: snprintf(b, n, "ANALYZE           >"); break;
    case A_SUB_STATS: snprintf(b, n, "STATISTICS        >"); break;
    case A_SAVE_ALL: snprintf(b, n, "SAVE ALL AS PCAPNG"); break;
    case A_SAVE_SHOWN: snprintf(b, n, "SAVE SHOWN AS PCAPNG"); break;
    case A_CLEAR: snprintf(b, n, "CLEAR PACKETS"); break;
    case A_QUIT: snprintf(b, n, "CLOSE TAP"); break;
    case A_PAUSE: snprintf(b, n, "%s", paused ? "RESUME CAPTURE" : "PAUSE CAPTURE"); break;
    case A_WIFI: snprintf(b, n, "WI-FI         %s", onoff(R_WIFI)); break;
    case A_WIFI_CH: snprintf(b, n, "WI-FI CHANNELS  < %s >", hop[wifi_hop]); break;
    case A_WIFI_TYPES: snprintf(b, n, "WI-FI FRAMES    < %s >", types[wifi_types]); break;
    case A_WIFI_RSSI: rssi_label(b, n, "WI-FI", min_rssi[R_WIFI]); break;
    case A_WIFI_DROP: snprintf(b, n, "ALLOW LINK DROP  %s", drop_link ? "YES" : "NO"); break;
    case A_BLE: snprintf(b, n, "BLUETOOTH     %s", onoff(R_BLE)); break;
    case A_BLE_RSSI: rssi_label(b, n, "BLE", min_rssi[R_BLE]); break;
    case A_LORA: snprintf(b, n, "LORA          %s", onoff(R_LORA)); break;
    case A_LORA_PRESET: snprintf(b, n, "LORA  < %s >", presets[lora_preset].name); break;
    case A_LORA_RSSI: rssi_label(b, n, "LORA", min_rssi[R_LORA]); break;
    case A_DEC: snprintf(b, n, "BOARD DECODES %s", onoff(R_DEC)); break;
    case A_COLOURS: snprintf(b, n, "COLOUR RULES     %s", colours ? "ON" : "OFF"); break;
    case A_AUTOSCROLL: snprintf(b, n, "FOLLOW NEWEST    %s", autoscroll ? "ON" : "OFF"); break;
    case A_TIME: snprintf(b, n, "TIME  < %s >", deltas ? "SINCE PREVIOUS" : "SINCE FIRST"); break;
    case A_FILTER_EDIT: snprintf(b, n, "EDIT DISPLAY FILTER..."); break;
    case A_FILTER_CLEAR: snprintf(b, n, "CLEAR FILTER"); break;
    case A_FOLLOW_DEV: snprintf(b, n, "FOLLOW THIS DEVICE"); break;
    case A_FOLLOW_CONV: snprintf(b, n, "FOLLOW THIS CONVERSATION"); break;
    case A_FILTER_PROTO: snprintf(b, n, "ONLY THIS PROTOCOL"); break;
    case A_FILTER_NOT: snprintf(b, n, "HIDE THIS PROTOCOL"); break;
    case A_QF_WIFI: snprintf(b, n, "QUICK: wifi and not beacon"); break;
    case A_QF_BLE: snprintf(b, n, "QUICK: ble"); break;
    case A_QF_LORA: snprintf(b, n, "QUICK: lora"); break;
    case A_QF_EAPOL: snprintf(b, n, "QUICK: eapol or deauth"); break;
    case A_QF_NEAR: snprintf(b, n, "QUICK: rssi > -60"); break;
    case A_ST_HIER: snprintf(b, n, "PROTOCOL HIERARCHY"); break;
    case A_ST_ENDP: snprintf(b, n, "ENDPOINTS"); break;
    case A_ST_CONV: snprintf(b, n, "CONVERSATIONS"); break;
    case A_ST_CHAN: snprintf(b, n, "CHANNELS"); break;
    case A_ST_RSSI: snprintf(b, n, "SIGNAL LEVELS"); break;
    case A_HELP: snprintf(b, n, "KEYS AND FILTER HELP"); break;
    default: snprintf(b, n, "PACKET DETAIL"); break;
    }
}
static void open_menu(int m, int x, int y) { mparent = menu == M_ROOT ? M_ROOT : M_NONE; menu = m; msel = 0; mx = x; my = y; }
static const char *leaf_word(int leaf)
{
    static const char *const w[L_COUNT] = {"beacon", "probe", "mgmt", "ctrl", "data", "eapol", "ble", "ibeacon", "eddystone",
                                           "scanrsp", "meshcore", "meshtastic", "lora", "decode"};
    return w[leaf];
}
static void follow(int how)
{
    const int no = sel_no();
    char f[96];
    if (no < 0) { snprintf(status, sizeof(status), "select a packet first"); return; }
    tap_pkt_t *p = at_no((uint32_t)no);
    tap_dis_t d = {0};
    tap_dissect(p, arena + p->off, &d);
    if (how == A_FOLLOW_DEV) snprintf(f, sizeof(f), "addr == \"%s\"", d.src[0] ? d.src : d.dst);
    else if (how == A_FOLLOW_CONV) {
        if (!d.src[0] || !d.dst[0] || !strcmp(d.dst, "Broadcast")) snprintf(f, sizeof(f), "src == \"%s\"", d.src);
        else snprintf(f, sizeof(f), "addr == \"%s\" and addr == \"%s\"", d.src, d.dst);
    } else snprintf(f, sizeof(f), "%s%s", how == A_FILTER_NOT ? "not " : "", leaf_word(d.leaf));
    set_filter(f);
}
static void step_rssi(int r, int dir)
{
    static const int steps[6] = {0, -90, -80, -70, -60, -50};
    int i = 0;
    while (i < 5 && steps[i] != min_rssi[r]) i++;
    min_rssi[r] = steps[(i + (dir < 0 ? 5 : 1)) % 6];
}
static void edit_filter(void)
{
    if (desk) { editing = true; snprintf(fedit, sizeof(fedit), "%s", ftext); }
    else if (!text_h) text_h = lap_text_input("Display filter", ftext, 90, LAP_TEXT_LOWER);
}
static void act(int a, int dir)
{
    bool close = true;
    switch (a) {
    case A_SUB_FILE: case A_SUB_CAPTURE: case A_SUB_VIEW: case A_SUB_ANALYZE: case A_SUB_STATS:
        if (dir < 0) return;
        open_menu(M_FILE + (a - A_SUB_FILE), mx, my);
        mparent = M_ROOT;
        return;
    case A_SAVE_ALL: save_begin(false); break;
    case A_SAVE_SHOWN: save_begin(true); break;
    case A_CLEAR:
        first_no = next_no; aw = 0; v0 = vn = sel = top = 0; t0 = 0; skipped = 0;
        memset(stats, 0, sizeof(*stats));
        snprintf(status, sizeof(status), "cleared");
        break;
    case A_QUIT: lap_exit(); break;
    case A_PAUSE: paused = !paused; snprintf(status, sizeof(status), "%s", paused ? "paused: new frames are not kept" : "capturing"); break;
    case A_WIFI: case A_BLE: case A_LORA: case A_DEC: {
        const int r = a == A_WIFI ? R_WIFI : a == A_BLE ? R_BLE : a == A_LORA ? R_LORA : R_DEC;
        src[r].on = !src[r].on; start(r); close = false;
        if (src[r].on && src[r].h <= 0) snprintf(status, sizeof(status), "%s: %s", tap_radio_name[r], src[r].why);
        break;
    }
    case A_WIFI_CH:
        wifi_hop = (wifi_hop + (dir < 0 ? 2 : 1)) % 3; start(R_WIFI); close = false;
        if (src[R_WIFI].on && src[R_WIFI].h <= 0) snprintf(status, sizeof(status), "Wi-Fi: %s", src[R_WIFI].why);
        break;
    case A_WIFI_TYPES: wifi_types = (wifi_types + (dir < 0 ? 2 : 1)) % 3; start(R_WIFI); close = false; break;
    case A_WIFI_DROP:
        if (drop_link) { drop_link = false; start(R_WIFI); close = false; }
        else if (!confirm_h) confirm_h = lap_confirm("Hopping channels drops the board's Wi-Fi link (LINK, remote screens, network apps). Allow it?");
        break;
    case A_WIFI_RSSI: step_rssi(R_WIFI, dir); close = false; break;
    case A_BLE_RSSI: step_rssi(R_BLE, dir); close = false; break;
    case A_LORA_RSSI: step_rssi(R_LORA, dir); close = false; break;
    case A_LORA_PRESET: lora_preset = (lora_preset + (dir < 0 ? 3 : 1)) % 4; start(R_LORA); close = false; break;
    case A_COLOURS: colours = !colours; close = false; break;
    case A_AUTOSCROLL: autoscroll = !autoscroll; if (autoscroll && vn) sel = vn - 1; close = false; break;
    case A_TIME: deltas = !deltas; close = false; break;
    case A_FILTER_EDIT: edit_filter(); break;
    case A_FILTER_CLEAR: set_filter(""); break;
    case A_FOLLOW_DEV: case A_FOLLOW_CONV: case A_FILTER_PROTO: case A_FILTER_NOT: follow(a); break;
    case A_QF_WIFI: set_filter("wifi and not beacon"); break;
    case A_QF_BLE: set_filter("ble"); break;
    case A_QF_LORA: set_filter("lora"); break;
    case A_QF_EAPOL: set_filter("eapol or deauth"); break;
    case A_QF_NEAR: set_filter("rssi > -60"); break;
    case A_ST_HIER: case A_ST_ENDP: case A_ST_CONV: case A_ST_CHAN: case A_ST_RSSI: screen = V_STATS; stab = a - A_ST_HIER; sscroll = 0; break;
    case A_HELP: screen = V_HELP; break;
    default: if (desk) focus = 1; else hdetail = 1; break;
    }
    if (close) menu = M_NONE;
}

/* ---- drawing ----------------------------------------------------------------------------- */
static uint8_t row_colour(const tap_dis_t *d) { return colours ? d->colour : LAP_WHITE; }
static void spark(int x, int y, int r, uint8_t attr)
{
    unsigned max = 1;
    for (int i = 0; i < HIST; i++) if (src[r].hist[i] > max) max = src[r].hist[i];
    for (int c = 0; c < HIST / 2; c++) {
        static const uint8_t left[5] = {0, 0x40, 0x44, 0x46, 0x47}, right[5] = {0, 0x80, 0xA0, 0xB0, 0xB8};
        const unsigned a = src[r].hist[c * 2], b = src[r].hist[c * 2 + 1];
        const unsigned ha = a ? 1 + a * 3 / max : 0, hb = b ? 1 + b * 3 / max : 0;
        lap_put_cell(x + c, y, LAP_BRAILLE(left[ha > 4 ? 4 : ha] | right[hb > 4 ? 4 : hb]), attr);
    }
}
static void sources(int y)
{
    static const char *const nm[R_COUNT] = {"WIFI", "BLE", "LORA", "DEC"};
    const int each = W / R_COUNT;
    for (int r = 0; r < R_COUNT; r++) {
        const src_t *s = &src[r];
        const uint8_t c = !s->on ? (LAP_BLACK | LAP_BRIGHT) : s->h > 0 ? (LAP_GREEN | LAP_BRIGHT) : LAP_YELLOW;
        const int x = r * each;
        put(x, y, each, LAP_ATTR(c, LAP_BLACK), "%s %s%u", nm[r], !s->on ? "off " : s->h > 0 ? "" : "n/a ", (unsigned)s->n);
        if (each >= 24 && s->h > 0) spark(x + each - HIST / 2 - 2, y, r, LAP_ATTR(LAP_CYAN, LAP_BLACK));
    }
}
static void filter_bar(int y)
{
    const bool bad = editing && fedit[0] && !tap_filter_compile(fedit, &(tap_filter_t){0});
    const uint8_t a = editing ? LAP_ATTR(LAP_WHITE | LAP_BRIGHT, bad ? LAP_RED : LAP_BLUE) : ftext[0] ? LAP_ATTR(LAP_BLACK, LAP_GREEN) : LAP_ATTR(LAP_WHITE, LAP_BLACK);
    lap_fill(0, y, W, 1, ' ', a);
    put(0, y, W - 16, a, "Filter: %s%s", editing ? fedit : ftext[0] ? ftext : desk ? "(none)  press / to type one" : "(none)  tap here", editing ? "_" : "");
    put(W - 16, y, 16, LAP_ATTR(LAP_YELLOW, LAP_BLACK), "%6d of %-5u", vn, (unsigned)count());
}
static void fmt_time(char *b, size_t n, const tap_pkt_t *p, int vi)
{
    uint64_t base = t0;
    if (deltas && vi > 0) base = at_no(view[v0 + vi - 1])->ts_us;
    const uint64_t us = p->ts_us > base ? p->ts_us - base : 0;
    snprintf(b, n, "%u.%03u", (unsigned)(us / 1000000u), (unsigned)(us / 1000u % 1000u));
}
static void list(int y, int h, bool active)
{
    const uint8_t head = LAP_ATTR(LAP_BLACK, active ? LAP_CYAN : LAP_WHITE);
    lap_fill(0, y, W, 1, ' ', head);
    if (desk) put(0, y, W, head, "%6s %10s  %-18s %-18s %-11s %5s %5s  %s", "No.", "Time", "Source", "Destination", "Protocol", "Len", "dBm", "Info");
    else put(0, y, W, head, "%5s %-9s %-9s %4s %s", "No.", "Proto", "Source", "dBm", "Info");
    h--; y++;
    if (sel < top) top = sel;
    if (sel >= top + h) top = sel - h + 1;
    if (top < 0) top = 0;
    for (int i = 0; i < h; i++) {
        const int vi = top + i;
        if (vi >= vn) break;
        tap_pkt_t *p = at_no(view[v0 + vi]);
        tap_dis_t d = {0};
        char tm[16];
        tap_dissect(p, arena + p->off, &d);
        fmt_time(tm, sizeof(tm), p, vi);
        const uint8_t a = vi == sel ? LAP_ATTR(LAP_BLACK, active ? LAP_YELLOW : LAP_WHITE) : LAP_ATTR(row_colour(&d), LAP_BLACK);
        if (vi == sel) lap_fill(0, y + i, W, 1, ' ', a);
        if (desk) put(0, y + i, W, a, "%6u %10s  %-18.18s %-18.18s %-11.11s %5u %5d  %s", (unsigned)p->no + 1, tm, d.src, d.dst, d.proto, p->orig, p->rssi, d.info);
        else {
            const size_t sl = strlen(d.src);
            put(0, y + i, W, a, "%5u %-9.9s %-9.9s %4d %s", (unsigned)p->no + 1, d.proto, sl > 9 && d.src[2] == ':' ? d.src + sl - 8 : d.src, p->rssi, d.info);
        }
    }
    if (!vn) put(2, y + h / 2, W - 4, LAP_ATTR(LAP_BLACK | LAP_BRIGHT, LAP_BLACK), "%s", count() ? "no packet matches the filter" : paused ? "paused" : "listening: nothing heard yet");
}
/* The tree and the bytes of the selected packet. */
static tap_pkt_t *detail(tap_dis_t *d)
{
    const int no = sel_no();
    if (no < 0) return NULL;
    tap_pkt_t *p = at_no((uint32_t)no);
    memset(d, 0, sizeof(*d));
    d->lines = lines; d->cap = 72;
    tap_dissect(p, arena + p->off, d);
    if (tsel >= d->nlines) tsel = d->nlines ? d->nlines - 1 : 0;
    return p;
}
static void tree(int y, int h, bool active, const tap_dis_t *d)
{
    if (tsel < ttop) ttop = tsel;
    if (tsel >= ttop + h) ttop = tsel - h + 1;
    for (int i = 0; i < h && ttop + i < d->nlines; i++) {
        const tap_line_t *l = &d->lines[ttop + i];
        const bool on = ttop + i == tsel;
        const uint8_t fg = l->depth == 0 ? (LAP_CYAN | LAP_BRIGHT) : l->depth == 1 ? LAP_WHITE : LAP_GREEN;
        const uint8_t a = on ? LAP_ATTR(LAP_BLACK, active ? LAP_YELLOW : LAP_WHITE) : LAP_ATTR(fg, LAP_BLACK);
        if (on) lap_fill(0, y + i, W, 1, ' ', a);
        put(l->depth * 2, y + i, W - l->depth * 2, a, "%s %s", l->depth == 0 ? "v" : l->depth == 1 ? "+" : "-", l->text);
    }
}
static void hexpane(int y, int h, bool active, const tap_pkt_t *p, const tap_dis_t *d)
{
    const int per = W >= 76 ? 16 : W >= 44 ? 8 : 4;
    const uint8_t *b = arena + p->off;
    const int hl0 = d->nlines ? d->lines[tsel].off : 0, hl1 = d->nlines ? hl0 + d->lines[tsel].len : 0;
    const int rows = (p->len + per - 1) / per;
    if (hl1 > hl0 && !active) { if (hl0 / per < htop) htop = hl0 / per; if (hl0 / per >= htop + h) htop = hl0 / per - h + 1; }
    if (htop > rows - h) htop = rows - h;
    if (htop < 0) htop = 0;
    for (int r = 0; r < h && htop + r < rows; r++) {
        const int base = (htop + r) * per;
        put(0, y + r, 5, LAP_ATTR(LAP_BLACK | LAP_BRIGHT, LAP_BLACK), "%04x", base);
        for (int i = 0; i < per && base + i < p->len; i++) {
            const int o = base + i;
            const bool hl = o >= hl0 && o < hl1;
            const uint8_t a = hl ? LAP_ATTR(LAP_BLACK, LAP_CYAN) : LAP_ATTR(b[o] ? LAP_WHITE : (LAP_BLACK | LAP_BRIGHT), LAP_BLACK);
            put(6 + i * 3 + (i >= per / 2 ? 1 : 0), y + r, 2, a, "%02x", b[o]);
            lap_put_cell(8 + per * 3 + i, y + r, b[o] >= 32 && b[o] < 127 ? b[o] : '.', hl ? LAP_ATTR(LAP_BLACK, LAP_CYAN) : LAP_ATTR(LAP_GREEN, LAP_BLACK));
        }
    }
    if (!p->len) put(2, y, W - 2, LAP_ATTR(LAP_BLACK | LAP_BRIGHT, LAP_BLACK), "no bytes in this record");
}
static void rule(int y, uint8_t fg, const char *fmt, ...)
{
    char t[80];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(t, sizeof(t), fmt, ap);
    va_end(ap);
    lap_hline(0, y, W, '-', LAP_ATTR(fg, LAP_BLACK));
    put(2, y, W - 4, LAP_ATTR(fg, LAP_BLACK), "[ %s ]", t);
}
static void bar(int x, int y, int w, uint32_t v, uint32_t max, uint8_t fg)
{
    const int n = max ? (int)((uint64_t)v * (unsigned)w / max) : 0;
    lap_hline(x, y, w, '.', LAP_ATTR(LAP_BLACK | LAP_BRIGHT, LAP_BLACK));
    lap_hline(x, y, n ? n : v ? 1 : 0, '=', LAP_ATTR(fg, LAP_BLACK));
}
static void stats_screen(void)
{
    static const char *const tabs[5] = {"HIERARCHY", "ENDPOINTS", "CONVERSATIONS", "CHANNELS", "SIGNAL"};
    static const uint8_t rc[R_COUNT] = {LAP_CYAN, LAP_BLUE | LAP_BRIGHT, LAP_GREEN | LAP_BRIGHT, LAP_MAGENTA};
    const tap_stats_t *s = stats;
    int x = 1, y = 2;
    for (int i = 0; i < 5; i++) {
        put(x, 0, W - x, i == stab ? LAP_ATTR(LAP_BLACK, LAP_YELLOW) : LAP_ATTR(LAP_WHITE, LAP_BLACK), " %s ", desk || i == stab ? tabs[i] : "");
        x += (desk || i == stab ? (int)strlen(tabs[i]) : 0) + 3;
    }
    lap_hline(0, 1, W, '-', LAP_ATTR(LAP_CYAN, LAP_BLACK));
    const int bw = W > 70 ? 30 : W - 36 > 6 ? W - 36 : 6;
    if (stab == 0) {
        put(0, y++, W, LAP_ATTR(LAP_YELLOW, LAP_BLACK), "%-22s %7s %5s %8s", "Protocol", "Packets", "%", "Bytes");
        for (int r = 0; r < R_COUNT; r++) {
            uint32_t pkn = 0, byn = 0;
            for (int l = 0; l < L_COUNT; l++) if (tap_leaf_radio[l] == r) { pkn += s->pk[l]; byn += s->by[l]; }
            put(0, y, W, LAP_ATTR(rc[r], LAP_BLACK), "%-22s %7u %4u%% %8u", tap_radio_name[r], (unsigned)pkn, s->total ? (unsigned)(pkn * 100u / s->total) : 0, (unsigned)byn);
            bar(47, y++, bw, pkn, s->total, rc[r]);
            for (int l = 0; l < L_COUNT; l++)
                if (tap_leaf_radio[l] == r && s->pk[l] && y < H - 1) {
                    put(0, y, W, LAP_ATTR(LAP_WHITE, LAP_BLACK), "  %-20s %7u %4u%% %8u", tap_leaf_name[l], (unsigned)s->pk[l], (unsigned)(s->pk[l] * 100u / s->total), (unsigned)s->by[l]);
                    bar(47, y++, bw, s->pk[l], s->total, LAP_WHITE);
                }
        }
        put(0, y + 1, W, LAP_ATTR(LAP_BLACK | LAP_BRIGHT, LAP_BLACK), "%u packets, %u bytes since the last clear; %u not kept while paused", (unsigned)s->total, (unsigned)s->bytes, (unsigned)skipped);
    } else if (stab == 1) {
        put(0, y++, W, LAP_ATTR(LAP_YELLOW, LAP_BLACK), "%-18s %-5s %6s %6s %8s %4s %s", "Address", "Radio", "Tx", "Rx", "Bytes", "dBm", "Name");
        for (int i = sscroll; i < s->eps && y < H - 1; i++, y++) {
            const tap_ep_t *e = &s->ep[i];
            put(0, y, W, LAP_ATTR(rc[e->radio], LAP_BLACK), "%-18.18s %-5.5s %6u %6u %8u %4d %s", e->addr, tap_radio_name[e->radio], (unsigned)e->tx, (unsigned)e->rx, (unsigned)e->bytes, e->rssi, e->label);
        }
        if (s->ep_over) put(0, H - 2, W, LAP_ATTR(LAP_RED, LAP_BLACK), "table full: %u later packets came from addresses not listed", (unsigned)s->ep_over);
    } else if (stab == 2) {
        put(0, y++, W, LAP_ATTR(LAP_YELLOW, LAP_BLACK), "%-18s     %-18s %7s %8s", "Address A", "Address B", "Packets", "Bytes");
        for (int i = sscroll; i < s->cvs && y < H - 1; i++, y++) {
            const tap_cv_t *c = &s->cv[i];
            put(0, y, W, LAP_ATTR(rc[s->ep[c->a].radio], LAP_BLACK), "%-18.18s <-> %-18.18s %7u %8u", s->ep[c->a].addr, s->ep[c->b].addr, (unsigned)c->pk, (unsigned)c->bytes);
        }
        if (!s->cvs) put(0, y, W, LAP_ATTR(LAP_BLACK | LAP_BRIGHT, LAP_BLACK), "no two-way traffic seen: broadcasts are listed under ENDPOINTS");
    } else if (stab == 3) {
        uint32_t max = 1;
        for (int c = 0; c < 15; c++) if (s->wifi_ch[c] > max) max = s->wifi_ch[c];
        for (int c = 0; c < 4; c++) if (s->ble_ch[c] > max) max = s->ble_ch[c];
        put(0, y++, W, LAP_ATTR(LAP_YELLOW, LAP_BLACK), "Frames per channel");
        for (int c = 1; c <= 15 && y < H - 1; c++) {
            const uint32_t v = s->wifi_ch[c % 15];
            if (!v && c != 1 && c != 6 && c != 11) continue;
            if (c == 15) put(0, y, 12, LAP_ATTR(LAP_CYAN, LAP_BLACK), "Wi-Fi 5 GHz"); else put(0, y, 12, LAP_ATTR(LAP_CYAN, LAP_BLACK), "Wi-Fi ch %2d", c);
            bar(13, y, W - 24, v, max, LAP_CYAN);
            put(W - 9, y++, 9, LAP_ATTR(LAP_WHITE, LAP_BLACK), "%8u", (unsigned)v);
        }
        for (int c = 0; c < 4 && y < H - 1; c++) {
            if (c == 3) put(0, y, 12, LAP_ATTR(LAP_BLUE | LAP_BRIGHT, LAP_BLACK), "BLE ch ?"); else put(0, y, 12, LAP_ATTR(LAP_BLUE | LAP_BRIGHT, LAP_BLACK), "BLE ch %d", 37 + c);
            bar(13, y, W - 24, s->ble_ch[c], max, LAP_BLUE | LAP_BRIGHT);
            put(W - 9, y++, 9, LAP_ATTR(LAP_WHITE, LAP_BLACK), "%8u", (unsigned)s->ble_ch[c]);
        }
    } else {
        uint32_t max = 1;
        for (int r = 0; r < 3; r++) for (int b = 0; b < 9; b++) if (s->rssi[r][b] > max) max = s->rssi[r][b];
        const int cw = (W - 12) / 3;
        put(0, y, W, LAP_ATTR(LAP_YELLOW, LAP_BLACK), "dBm");
        for (int r = 0; r < 3; r++) put(10 + r * cw, y, cw, LAP_ATTR(rc[r], LAP_BLACK), "%s", tap_radio_name[r]);
        y++;
        for (int b = 8; b >= 0 && y < H - 1; b--, y++) {
            if (b == 0) put(0, y, 10, LAP_ATTR(LAP_WHITE, LAP_BLACK), "< -100"); else put(0, y, 10, LAP_ATTR(LAP_WHITE, LAP_BLACK), "%s%4d", b == 8 ? ">=" : "  ", -110 + b * 10);
            for (int r = 0; r < 3; r++) {
                bar(10 + r * cw, y, cw - 8, s->rssi[r][b], max, rc[r]);
                put(10 + r * cw + cw - 7, y, 6, LAP_ATTR(LAP_WHITE, LAP_BLACK), "%5u", (unsigned)s->rssi[r][b]);
            }
        }
    }
    put(0, H - 1, W, LAP_ATTR(LAP_BLACK, LAP_WHITE), " < > page   UP DOWN scroll   BACK packets ");
}
static void help_screen(void)
{
    static const char *const t[] = {
        "TAP listens. It never transmits, joins, injects or decrypts.", "",
        "SPACE or P pause/resume    /  type a filter     X  clear filter", "F  follow device           V  follow conversation",
        "S  statistics              W  save pcapng       C  clear packets", "O  options (layered)       M  packet menu       TAB next pane",
        "ENTER detail               A  follow newest     BACK up / close", "",
        "Display filters:", "  wifi ble lora decode beacon probe mgmt ctrl data eapol deauth auth",
        "  ibeacon eddystone scanrsp rid meshcore meshtastic protected", "  addr == aa:bb:cc   src == ..   dst == ..   ssid == name   name contains tag",
        "  rssi > -70   len >= 100   ch == 6   no > 500   contains \"text\"", "  and or not ( )        example: wifi and not beacon and rssi > -60", "",
        "Saved files go to /sdcard/captures as pcapng: 802.11 with radiotap,", "Bluetooth LE LL with PHDR and LoRaTap. Board decodes are text and are", "not written. Encrypted payloads are saved exactly as received.",
        "Bluetooth: the controller reports no channel or CRC; files use", "advertising channel 37 and a zero CRC marked 'not checked'."};
    for (int i = 0; i < (int)(sizeof(t) / sizeof(t[0])) && i < H - 1; i++)
        put(1, i, W - 1, LAP_ATTR(i == 0 ? (LAP_YELLOW | LAP_BRIGHT) : t[i][0] == ' ' ? LAP_GREEN : LAP_WHITE, LAP_BLACK), "%s", t[i]);
    put(0, H - 1, W, LAP_ATTR(LAP_BLACK, LAP_WHITE), " BACK packets ");
}
static void menu_draw(void)
{
    const uint8_t *it;
    const int n = menu_items(menu, &it);
    char b[64];
    int w = 12;
    for (int i = 0; i < n; i++) { label(it[i], b, sizeof(b)); if ((int)strlen(b) + 4 > w) w = (int)strlen(b) + 4; }
    if (w > W) w = W;
    int x = desk ? mx : (W - w) / 2, y = desk ? my : (H - n - 2) / 2;
    if (x + w > W) x = W - w;
    if (y + n + 2 > H) y = H - n - 2;
    if (y < 0) y = 0;
    mx = x; my = y;
    lap_fill(x, y, w, n + 2, ' ', LAP_ATTR(LAP_WHITE, LAP_BLUE));
    lap_box(x, y, w, n + 2, menu == M_ROOT ? "OPTIONS" : menu == M_CTX ? "PACKET" : menu == M_FILE ? "FILE" : menu == M_CAPTURE ? "CAPTURE" :
            menu == M_VIEW ? "VIEW" : menu == M_ANALYZE ? "ANALYZE" : "STATISTICS", LAP_ATTR(LAP_CYAN | LAP_BRIGHT, LAP_BLUE));
    for (int i = 0; i < n && y + 1 + i < H; i++) {
        label(it[i], b, sizeof(b));
        const uint8_t a = i == msel ? LAP_ATTR(LAP_BLACK, LAP_YELLOW) : LAP_ATTR(LAP_WHITE | LAP_BRIGHT, LAP_BLUE);
        if (i == msel) lap_fill(x + 1, y + 1 + i, w - 2, 1, ' ', a);
        put(x + 2, y + 1 + i, w - 3, a, "%s", b);
    }
}
static const char *const bar_titles[6] = {"File", "Capture", "View", "Analyze", "Statistics", "Help"};
static void draw(void)
{
    if (!pk) return;
    W = lap_cols(); H = lap_rows();
    desk = W >= 90 && H >= 24;
    poll();
    save_step();
    lap_clear(LAP_ATTR(LAP_WHITE, LAP_BLACK));
    if (screen == V_STATS) stats_screen();
    else if (screen == V_HELP) help_screen();
    else {
        tap_dis_t d;
        tap_pkt_t *p = detail(&d);
        const uint8_t live = paused ? LAP_ATTR(LAP_BLACK, LAP_YELLOW) : LAP_ATTR(LAP_BLACK, LAP_GREEN);
        if (desk) {
            int x = 1;
            lap_fill(0, 0, W, 1, ' ', LAP_ATTR(LAP_BLACK, LAP_WHITE));
            for (int i = 0; i < 6; i++) { put(x, 0, W - x, LAP_ATTR(LAP_BLACK, LAP_WHITE), "%s", bar_titles[i]); x += (int)strlen(bar_titles[i]) + 2; }
            put(W - 22, 0, 12, LAP_ATTR(LAP_BLUE, LAP_WHITE), "TAP analyser");
            put(W - 9, 0, 9, live, "%s", paused ? " PAUSED " : " LIVE   ");
            sources(1);
            filter_bar(2);
            const int body = H - 6;
            list_y = 3; list_h = body * 45 / 100; tree_h = body * 30 / 100; hex_h = body - list_h - tree_h;
            tree_y = list_y + list_h + 1; hex_y = tree_y + tree_h + 1;
            list(list_y, list_h, focus == 0);
            if (p) rule(tree_y - 1, focus == 1 ? LAP_YELLOW : LAP_CYAN, "Packet %u: %s, %u bytes on air, %u kept", (unsigned)p->no + 1, d.proto, p->orig, p->len);
            else rule(tree_y - 1, LAP_CYAN, "Packet detail");
            rule(hex_y - 1, focus == 2 ? LAP_YELLOW : LAP_CYAN, "Bytes");
            if (p) { tree(tree_y, tree_h, focus == 1, &d); hexpane(hex_y, hex_h, focus == 2, p, &d); }
        } else {
            lap_fill(0, 0, W, 1, ' ', LAP_ATTR(LAP_BLACK, LAP_CYAN));
            put(1, 0, W - 10, LAP_ATTR(LAP_BLACK, LAP_CYAN), "TAP  %u pkts", (unsigned)count());
            put(W - 8, 0, 8, live, "%s", paused ? " PAUSED " : "  LIVE  ");
            sources(1);
            filter_bar(2);
            list_y = 3; list_h = H - 4;
            if (!hdetail) list(list_y, list_h, true);
            else if (p) {
                tree_h = (H - 6) * 6 / 10; tree_y = 4; hex_y = tree_y + tree_h + 1; hex_h = H - 1 - hex_y;
                rule(3, LAP_YELLOW, "Packet %u: %s", (unsigned)p->no + 1, d.proto);
                tree(tree_y, tree_h, focus != 2, &d);
                rule(hex_y - 1, focus == 2 ? LAP_YELLOW : LAP_CYAN, "Bytes");
                hexpane(hex_y, hex_h, focus == 2, p, &d);
            } else hdetail = 0;
        }
        const uint8_t sb = LAP_ATTR(LAP_BLACK, LAP_WHITE);
        lap_fill(0, H - 1, W, 1, ' ', sb);
        if (status[0] && desk) put(1, H - 1, W - 2, sb, "%s", status);
        else if (desk) put(1, H - 1, W - 2, sb, "SPACE pause  / filter  F follow  S stats  W save  O options  right-click a packet  ?  help");
        else if (status[0] && (lap_millis() / 3000) % 2) put(0, H - 1, W, sb, "%s", status);
        else if (hdetail) put(0, H - 1, W, sb, "[BACK] [PREV] [NEXT] [BYTES] [MENU]");
        else put(0, H - 1, W, sb, "[%s] [FILTER] [STATS] [SAVE] [MENU]", paused ? "RUN" : "PAUSE");
    }
    if (menu) menu_draw();
    /* a remote mouse: right button opens the packet menu where it is */
    lap_input_event_t ev;
    int cw = 8, chh = 16;
    lap_cell_px(&cw, &chh);
    while (lap_input_next(&ev) == 1)
        if (ev.kind == LAP_INPUT_POINTER_DOWN && ev.button == 2 && screen == V_MAIN && cw > 0 && chh > 0) {
            const int col = ev.x / cw, row = ev.y / chh;
            if (row > list_y && row < list_y + list_h && top + row - list_y - 1 < vn && (desk || !hdetail)) sel = top + row - list_y - 1;
            menu = M_NONE;
            open_menu(M_CTX, col, row);
        }
}

/* ---- input --------------------------------------------------------------------------------- */
static void move(int by)
{
    if (focus == 1 || (!desk && hdetail && focus != 2)) { tsel += by; if (tsel < 0) tsel = 0; }
    else if (focus == 2) { htop += by; if (htop < 0) htop = 0; }
    else {
        sel += by;
        if (sel >= vn) sel = vn - 1;
        if (sel < 0) sel = 0;
        tsel = ttop = htop = 0;
    }
}
static bool key(lap_key_t k, char ch)
{
    if (editing) {
        const size_t n = strlen(fedit);
        if (k == LAP_KEY_ENTER) { if (set_filter(fedit)) editing = false; }
        else if (k == LAP_KEY_BACK) editing = false;
        else if (k == LAP_KEY_BACKSPACE) { if (n) fedit[n - 1] = 0; }
        else if (k == LAP_KEY_CHAR && ch >= 32 && n + 1 < sizeof(fedit)) { fedit[n] = ch; fedit[n + 1] = 0; }
        return true;
    }
    if (menu) {
        const uint8_t *it;
        const int n = menu_items(menu, &it);
        if (k == LAP_KEY_UP || k == LAP_KEY_WHEEL_UP) msel = (msel + n - 1) % n;
        else if (k == LAP_KEY_DOWN || k == LAP_KEY_WHEEL_DOWN) msel = (msel + 1) % n;
        else if (k == LAP_KEY_ENTER || k == LAP_KEY_RIGHT) act(it[msel], 1);
        else if (k == LAP_KEY_LEFT) act(it[msel], -1);
        else if (k == LAP_KEY_BACK) { const int up = mparent; menu = M_NONE; if (up) open_menu(up, mx, my); }
        return true;
    }
    if (screen != V_MAIN) {
        if (k == LAP_KEY_BACK) screen = V_MAIN;
        else if (k == LAP_KEY_LEFT) { stab = (stab + 4) % 5; sscroll = 0; }
        else if (k == LAP_KEY_RIGHT || k == LAP_KEY_TAB) { stab = (stab + 1) % 5; sscroll = 0; }
        else if (k == LAP_KEY_DOWN || k == LAP_KEY_WHEEL_DOWN) sscroll++;
        else if (k == LAP_KEY_UP || k == LAP_KEY_WHEEL_UP) { if (sscroll) sscroll--; }
        else return false;
        return true;
    }
    status[0] = 0;
    switch (k) {
    case LAP_KEY_UP: case LAP_KEY_WHEEL_UP: move(k == LAP_KEY_UP ? -1 : -3); return true;
    case LAP_KEY_DOWN: case LAP_KEY_WHEEL_DOWN: move(k == LAP_KEY_DOWN ? 1 : 3); return true;
    case LAP_KEY_PGUP: move(-(list_h - 2)); return true;
    case LAP_KEY_PGDN: move(list_h - 2); return true;
    case LAP_KEY_HOME: move(-(int)MAXPK); return true;
    case LAP_KEY_END: move((int)MAXPK); return true;
    case LAP_KEY_TAB: focus = (focus + 1) % 3; if (!desk && hdetail && focus == 0) focus = 1; return true;
    case LAP_KEY_ENTER: case LAP_KEY_RIGHT:
        if (desk) focus = focus == 0 ? 1 : focus;
        else if (sel_no() >= 0) { hdetail = 1; focus = 1; }
        return true;
    case LAP_KEY_LEFT: if (desk) focus = 0; else { hdetail = 0; focus = 0; } return true;
    case LAP_KEY_BACK:
        if (!desk && hdetail) { hdetail = 0; focus = 0; return true; }
        if (desk && focus) { focus = 0; return true; }
        return false;                   /* HOME */
    case LAP_KEY_MENU: case LAP_KEY_F10: open_menu(M_ROOT, 1, 1); return true;
    case LAP_KEY_CHAR: break;
    default: return false;
    }
    switch (ch >= 'A' && ch <= 'Z' ? ch + 32 : ch) {
    case ' ': case 'p': act(A_PAUSE, 1); break;
    case '/': edit_filter(); break;
    case 'x': set_filter(""); break;
    case 'f': follow(A_FOLLOW_DEV); break;
    case 'v': follow(A_FOLLOW_CONV); break;
    case 's': screen = V_STATS; break;
    case 'w': save_begin(false); break;
    case 'c': act(A_CLEAR, 1); break;
    case 'a': act(A_AUTOSCROLL, 1); break;
    case 'o': open_menu(M_ROOT, 1, 1); break;
    case 'm': open_menu(M_CTX, W / 3, list_y + 2); break;
    case '?': case 'h': screen = V_HELP; break;
    case 'g': move(ch == 'G' ? (int)MAXPK : -(int)MAXPK); break;
    default: return false;
    }
    return true;
}
static bool touch(int col, int row)
{
    if (menu) {
        const uint8_t *it;
        const int n = menu_items(menu, &it);
        if (row > my && row <= my + n && col >= mx) { msel = row - my - 1; act(it[msel], 1); }
        else menu = M_NONE;
        return true;
    }
    if (editing) { editing = false; return true; }
    if (screen != V_MAIN) {
        if (row == 0) { stab = (stab + 1) % 5; sscroll = 0; } else if (row == H - 1) screen = V_MAIN;
        return true;
    }
    status[0] = 0;
    if (row == 0 && desk) {
        int x = 1;
        for (int i = 0; i < 6; i++) {
            const int w = (int)strlen(bar_titles[i]);
            if (col >= x - 1 && col <= x + w) { if (i == 5) screen = V_HELP; else open_menu(M_FILE + i, x - 1, 1); return true; }
            x += w + 2;
        }
        if (col >= W - 9) act(A_PAUSE, 1);
        return true;
    }
    if (row == 0) { act(A_PAUSE, 1); return true; }
    if (row == 1) {
        if (desk) { const int r = col / (W / R_COUNT); if (r < R_COUNT) act(r == R_WIFI ? A_WIFI : r == R_BLE ? A_BLE : r == R_LORA ? A_LORA : A_DEC, 1); }
        else open_menu(M_CAPTURE, 0, 2);
        return true;
    }
    if (row == 2) { edit_filter(); return true; }
    if (row == H - 1 && !desk) {
        const int b = col * 5 / (W > 0 ? W : 1);
        if (hdetail) { if (b == 0) { hdetail = 0; focus = 0; } else if (b == 1) { focus = 0; move(-1); focus = 1; } else if (b == 2) { focus = 0; move(1); focus = 1; }
                       else if (b == 3) focus = focus == 2 ? 1 : 2; else open_menu(M_CTX, 0, 0); }
        else if (b == 0) act(A_PAUSE, 1); else if (b == 1) edit_filter(); else if (b == 2) screen = V_STATS; else if (b == 3) save_begin(false); else open_menu(M_ROOT, 0, 0);
        return true;
    }
    if ((desk || !hdetail) && row > list_y && row < list_y + list_h) {
        const int vi = top + row - list_y - 1;
        if (vi < vn) { if (!desk && vi == sel) { hdetail = 1; focus = 1; } else { sel = vi; tsel = ttop = htop = 0; focus = 0; } }
        return true;
    }
    if (row >= tree_y && row < tree_y + tree_h && (desk || hdetail)) { tsel = ttop + row - tree_y; focus = 1; return true; }
    if (row >= hex_y && row < hex_y + hex_h && (desk || hdetail)) { focus = 2; return true; }
    return false;
}

static void enter(void)
{
    pk = calloc(MAXPK, sizeof(*pk));
    arena = malloc(ARENA);
    chunk = malloc(CHUNK);
    view = calloc(2 * MAXPK, sizeof(*view));
    lines = calloc(72, sizeof(*lines));
    stats = calloc(1, sizeof(*stats));
    first_no = next_no = aw = 0;
    v0 = vn = sel = top = tsel = ttop = htop = 0;
    ftext[0] = fedit[0] = status[0] = 0;
    fprog.count = 0;
    editing = paused = deltas = drop_link = false;
    colours = autoscroll = true;
    t0 = 0; skipped = 0; hist_sec = 0;
    screen = V_MAIN; focus = hdetail = stab = sscroll = 0; menu = M_NONE;
    wifi_hop = wifi_types = lora_preset = 0;
    confirm_h = text_h = 0;
    memset(min_rssi, 0, sizeof(min_rssi));
    memset(src, 0, sizeof(src));
    memset(&save, 0, sizeof(save));
    if (!pk || !arena || !chunk || !view || !lines || !stats) { lap_log("no memory"); lap_exit(); return; }
    lap_time_t now;
    lap_time(&now);
    wall0 = (uint64_t)(now.unix > 0 ? now.unix : 0) * 1000000u;
    for (int r = 0; r < R_COUNT; r++) { src[r].on = true; start(r); }
}
static void leave(void)
{
    for (int r = 0; r < R_COUNT; r++) stop(r);
    free(pk); free(arena); free(chunk); free(view); free(lines); free(stats);
    pk = NULL; arena = chunk = NULL; view = NULL; lines = NULL; stats = NULL;
}

LAP_APP(.id = "tap", .name = "TAP", .sub = "packet analyser", .version = "1.0.0", .author = "LakeShark",
        .icon = LAP_ICON_WAVE, .color = LAP_CYAN, .flags = LAP_CAP_RADIO_RX,
        .enter = enter, .leave = leave, .draw = draw, .key = key, .touch = touch);
