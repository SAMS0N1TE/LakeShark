/* TAP: a packet analyser for every radio the board can listen on.
   Wi-Fi 802.11 and Bluetooth advertising through the API 2.7 capture calls,
   LoRa through lap_packet_*, and the board's own decodes as text records.
   Receive only: nothing is sent, joined, decrypted or injected.

   Wide screens (a desk window, a remote screen) get three panes: packets,
   the protocol tree, the bytes. The handheld shows one pane at a time.

   The store is bounded: MAXPK records and ARENA bytes in PSRAM (malloc), the
   oldest dropped first and counted. Every frame costs a fixed amount of
   work when it arrives; drawing touches only the rows on screen.

   CAPTURE TO CARD streams every kept frame to rotating pcapng files as it
   arrives, so a long capture outlives the store. FILE > OPEN reads a
   capture back from the card and browses it with the same panes; the
   radios are stopped while a file is open. The card is only ever touched
   through the SD jobs: nothing here waits for it. */
#include "tap.h"
#include "lakeshark_ui.h"
#include "lap_scroll.h"
#include "lap_art.h"

#define MAXPK 4096u
#define ARENA (1536u * 1024u)
#define CHUNK (16u * 1024u)
#define SBUF (192u * 1024u)             /* frames waiting for the card */
#define WMAX (60u * 1024u)              /* one write job */
#define FBUF (60u * 1024u)              /* a capture file being read */
#define FLUSH (8u * 1024u)
#define NHIST 8                         /* recent display filters */
#define DIR "/sdcard/captures"
#define HIST 24
#define FINE 48                         /* quarter-second buckets: the header's activity strips */
#define NLINES 80                       /* the detail tree */

LAP_ICON_PX("....................", "....................", "..........e.........", ".........eee........",
            "........ee.ee.......", ".......ee...ee......", "......ee..b..ee.....", ".....ee..bbb..ee....",
            "....ee..bb.bb..ee...", "...ee..bb.a.bb..ee..", "..ee..bb.aaa.bb..ee.", "......b.aa.aa.b.....",
            "........a.f.a.......", "..........f.........", "..........f.........", ".........fff........",
            "....................", "....................");

/* ---- the store ------------------------------------------------------------ */
static tap_pkt_t *pk;
static uint8_t *arena, *chunk, *marks, *sbuf, *fbuf;
static uint32_t *view;                  /* numbers of the packets the filter shows */
static tap_line_t *lines;
static uint32_t first_no, next_no, aw;
static int v0, vn, sel, top, tsel, ttop, htop;
static tap_stats_t *stats;
static tap_filter_t fprog;
static char ftext[96], fedit[96], status[96], fbad[96], ferr[64], fhist[NHIST][96], findtext[64];
static tap_filter_t findprog;
static int editing;                     /* the desk's typing row: 1 the filter, 2 find */
static bool paused, colours, autoscroll, find_filter;
static int tmode, hcol, hpos, ssort;    /* time display; the handheld's third column; history place; table order */
static uint64_t t0, wall0;
static int64_t tz_us;                   /* local time less UTC */
static uint32_t skipped, evicted, status_ms, live_bytes, nmarks;
static bool dirty;                      /* settings to put away */
static int set_job = -1;
static bool status_bad;
static uint32_t *arrive;                /* lap_millis when each record came in: the list slides it in */

/* ---- the sources ------------------------------------------------------------ */
typedef struct { bool on; int h, err, reason; uint32_t n, lost; char why[72]; uint16_t hist[HIST]; uint64_t epoch; bool timed; } src_t;
static src_t src[R_COUNT];
static int wifi_hop, wifi_types, lora_preset, lora_radio, min_rssi[3];
static bool drop_link;
static int confirm_h, text_h, find_h;
static uint32_t hist_sec, fine_q, heard_ms[R_COUNT];
static uint16_t fine[R_COUNT][FINE];
static const struct { const char *name; uint32_t khz, bw; uint8_t sf, cr, sync; } presets[] = {
    {"MESHCORE US 910.525", 910525, 62500, 7, 5, 0x12}, {"MESHTASTIC US LONGFAST", 906875, 250000, 11, 5, 0x2B},
    {"MESHTASTIC EU LONGFAST", 869525, 250000, 11, 5, 0x2B}, {"MESHCORE EU 869.618", 869618, 62500, 8, 8, 0x12}};

/* ---- screens ----------------------------------------------------------------- */
enum { V_MAIN, V_STATS, V_HELP, V_SRC };
enum { ST_HIER, ST_ENDP, ST_CONV, ST_IO, ST_EXPERT, ST_CHAN, ST_RSSI, ST_COUNT };
static int W, H, screen, focus, hdetail, stab, sscroll, smax;
static bool desk;
static int list_y, list_h, tree_y, tree_h, hex_y, hex_h;
/* the finger-sized layout: key bar rows (kr), the stats tab strip's (tr),
   the source panel (hdr_y, hdr_h), the body a screen scrolls in */
static int kr = 1, tr = 1, hdr_y = 1, hdr_h, filt_y = 2, body0, body1;
/* Each list follows the finger. A pick changed by a key or a tap brings
   itself into view (show_sel, tshow); a drag never pulls it back. */
static lap_scroll_t ldrag, tdrag, hdrag, sdrag;
static int lsmax, tsmax, hsmax;
static bool dragging, show_sel = true, tshow = true;

enum { M_NONE, M_ROOT, M_FILE, M_EDIT, M_CAPTURE, M_VIEW, M_ANALYZE, M_STATS, M_CTX };
enum { A_SUB_FILE, A_SUB_EDIT, A_SUB_CAPTURE, A_SUB_VIEW, A_SUB_ANALYZE, A_SUB_STATS, A_SAVE_ALL, A_SAVE_SHOWN, A_SAVE_MARKED,
       A_OPEN, A_CLOSE_FILE, A_PART_NEXT, A_PART_PREV, A_CLEAR, A_QUIT,
       A_FIND, A_FIND_NEXT, A_FIND_PREV, A_GOTO, A_MARK, A_MARK_SHOWN, A_UNMARK_ALL, A_NEXT_MARK, A_PREV_MARK,
       A_PAUSE, A_WIFI, A_WIFI_CH, A_WIFI_TYPES, A_WIFI_RSSI, A_WIFI_DROP, A_BLE, A_BLE_RSSI, A_LORA, A_LORA_PRESET,
       A_LORA_RSSI, A_DEC, A_CARD, A_CARD_SIZE, A_CARD_KEEP,
       A_COLOURS, A_AUTOSCROLL, A_TIME, A_HCOL, A_FILTER_EDIT, A_FILTER_CLEAR, A_RECENT, A_FOLLOW_DEV, A_FOLLOW_CONV,
       A_FILTER_PROTO, A_FILTER_NOT, A_QF_WIFI, A_QF_BLE, A_QF_LORA, A_QF_EAPOL, A_QF_NEAR, A_ST_HIER, A_ST_ENDP,
       A_ST_CONV, A_ST_IO, A_ST_EXPERT, A_ST_CHAN, A_ST_RSSI, A_HELP, A_SOURCES, A_DETAIL };
static const uint8_t m_root[] = {A_SUB_CAPTURE, A_SUB_VIEW, A_SUB_ANALYZE, A_SUB_STATS, A_SUB_FILE, A_SUB_EDIT, A_SAVE_ALL, A_SOURCES, A_HELP};
static const uint8_t m_edit[] = {A_FIND, A_FIND_NEXT, A_FIND_PREV, A_GOTO, A_MARK, A_MARK_SHOWN, A_UNMARK_ALL, A_NEXT_MARK, A_PREV_MARK};
static const uint8_t m_capture[] = {A_PAUSE, A_WIFI, A_WIFI_CH, A_WIFI_TYPES, A_WIFI_RSSI, A_WIFI_DROP, A_BLE, A_BLE_RSSI,
                                    A_LORA, A_LORA_PRESET, A_LORA_RSSI, A_DEC, A_CARD, A_CARD_SIZE, A_CARD_KEEP, A_SOURCES};
static const uint8_t m_view[] = {A_COLOURS, A_AUTOSCROLL, A_TIME, A_HCOL};
static const uint8_t m_analyze[] = {A_FILTER_EDIT, A_FILTER_CLEAR, A_RECENT, A_FOLLOW_DEV, A_FOLLOW_CONV, A_FILTER_PROTO, A_FILTER_NOT,
                                    A_QF_WIFI, A_QF_BLE, A_QF_LORA, A_QF_EAPOL, A_QF_NEAR};
static const uint8_t m_stats[] = {A_ST_HIER, A_ST_ENDP, A_ST_CONV, A_ST_IO, A_ST_EXPERT, A_ST_CHAN, A_ST_RSSI};
static const uint8_t m_ctx[] = {A_DETAIL, A_MARK, A_FOLLOW_DEV, A_FOLLOW_CONV, A_FILTER_PROTO, A_FILTER_NOT, A_FILTER_CLEAR};
static int menu, msel, mx, my, mparent;
/* A menu opened by a finger is drawn finger-sized (mrh rows an item);
   one opened from the keyboard or a mouse stays compact. */
static bool by_touch, mbig;
static int mrh = 1;

/* ---- saving ------------------------------------------------------------------- */
enum { SAVE_ALL, SAVE_SHOWN, SAVE_MARKED };
static struct { int stage, job, mode; uint32_t no, end, written, bytes; int vi; char path[64]; } save;

/* ---- capture to card: a ring of pcapng files, written as frames arrive ---------- */
static const uint32_t card_size[] = {256u << 10, 1u << 20, 4u << 20, 16u << 20, 64u << 20};
static const char *const card_size_name[] = {"256 KB", "1 MB", "4 MB", "16 MB", "64 MB"};
static const uint8_t card_keep[] = {2, 4, 8, 16, 0};
static struct {
    bool on, started, header_due;
    int stage, job[2], rm_job, size_i, keep_i, err;     /* stage: 0 off, 1 making the folder, 2 writing */
    uint32_t fill, flying[2], flying_pk[2], pend_pk, put_bytes, file_no, files, removed, packets, dropped, last_ms;
    uint64_t bytes;
    char base[32];
} card;

/* ---- a capture file open from the card -------------------------------------------- */
static struct {
    int stage, job, n;                  /* stage: 0 none, 1 listing, 2 choosing, 3 reading, 4 read */
    lap_sd_entry_t *ent;
    char name[64];
    uint32_t size, off, fill, pos, w0, index, shown_end, count[R_COUNT], want;
    bool full;                          /* the store holds a part of the file */
    tap_reader_t rd;
} fl;
static bool filemode;

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
/* A line for the status bar; bad ones show in red. */
static void say(bool bad, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(status, sizeof(status), fmt, ap);
    va_end(ap);
    status_bad = bad;
    status_ms = lap_millis();
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
    live_bytes -= at_no(first_no)->len;
    if (marks[first_no % MAXPK]) { marks[first_no % MAXPK] = 0; nmarks--; }
    first_no++;
    evicted++;
}
static bool is_marked(uint32_t no) { return marks[no % MAXPK] != 0; }
/* Does the display filter show this packet? */
static bool shown(const tap_filter_t *f, const tap_pkt_t *p, const tap_dis_t *d)
{
    const tap_fctx_t c = {.t0 = t0, .marked = is_marked(p->no)};
    return tap_filter_match(f, p, arena + p->off, d, &c);
}
static void view_push(uint32_t no)
{
    if ((unsigned)(v0 + vn) >= 2 * MAXPK) { memmove(view, view + v0, (size_t)vn * sizeof(*view)); v0 = 0; }
    const bool glued = top >= vn - (list_h - 1);     /* the view was showing the newest */
    view[v0 + vn++] = no;
    if (autoscroll && sel == vn - 2) { sel = vn - 1; if (glued) show_sel = true; }
}
static void card_put(const tap_pkt_t *p, const uint8_t *data);
/* One record into the store: counted, dissected once for the statistics and
   the filter. Live, the oldest make room. A file keeps what fits from its
   first wanted frame on; the rest is counted and not kept. */
static void store(tap_pkt_t *m, const uint8_t *data)
{
    tap_dis_t d = {0};
    if (!t0) t0 = m->ts_us;
    if (filemode) {
        m->no = fl.index++;
        fl.count[m->radio]++;
        if (m->no < fl.w0 || fl.full || count() >= MAXPK || aw + m->len + 1u > ARENA) {
            if (m->no >= fl.w0) fl.full = true;
            tap_dissect(m, data, &d);
            tap_stats_add(stats, m, &d);
            return;
        }
        fl.shown_end = m->no + 1;
    } else {
        if (aw + m->len + 1u > ARENA) aw = 0;
        /* Oldest first: the ring is full, or the oldest record's bytes are where
           this one goes (a zero-length record counts as one byte, so it never
           shields the records behind it). */
        while (count() && (count() >= MAXPK || (at_no(first_no)->off < aw + m->len + (m->len ? 0u : 1u) &&
                                               at_no(first_no)->off + (at_no(first_no)->len ? at_no(first_no)->len : 1u) > aw)))
            evict();
        m->no = next_no;
    }
    m->off = aw;
    if (m->len) memcpy(arena + aw, data, m->len);
    aw += m->len ? m->len : 1u;         /* an empty record holds a byte, so the next one does not age it out */
    live_bytes += m->len;
    arrive[next_no % MAXPK] = filemode ? lap_millis() - 1000u : lap_millis();
    marks[next_no % MAXPK] = 0;
    *at_no(next_no++) = *m;
    tap_dissect(m, arena + m->off, &d);
    tap_stats_add(stats, m, &d);
    if (shown(&fprog, m, &d)) view_push(m->no);
}
/* One frame from a radio. data may be NULL when len is 0. */
static void add(tap_pkt_t *m, const uint8_t *data)
{
    src_t *s = &src[m->radio];
    if (filemode) return;
    if (paused) { skipped++; return; }
    if (m->radio < 3 && min_rssi[m->radio] && m->rssi < min_rssi[m->radio]) return;   /* capture filter */
    if (!s->timed) {                    /* radio clocks to the wall clock, once per source */
        s->timed = true;                /* wall time when the app opened + time open: steady on every board */
        s->epoch = wall0 + (uint64_t)lap_millis() * 1000u - m->ts_us;
    }
    m->ts_us += s->epoch;
    s->n++;
    s->hist[HIST - 1]++;
    fine[m->radio][FINE - 1]++;
    heard_ms[m->radio] = lap_millis();
    card_put(m, data);
    store(m, data);
}
static void refilter(void)
{
    const int keep = sel_no();
    v0 = vn = 0; sel = top = 0;
    for (uint32_t no = first_no; no != next_no; no++) {
        tap_pkt_t *p = at_no(no);
        tap_dis_t d = {0};
        tap_dissect(p, arena + p->off, &d);
        if (shown(&fprog, p, &d)) {
            if (keep >= 0 && no <= (uint32_t)keep) sel = vn;
            view[vn++] = no;
        }
    }
    if (keep < 0 && vn) sel = vn - 1;
    tsel = ttop = htop = 0;
    show_sel = tshow = true;
    lap_scroll_stop(&ldrag);
}
/* The newest first, each once. */
static void remember(const char *t)
{
    int at = 0;
    while (at < NHIST - 1 && fhist[at][0] && strcmp(fhist[at], t)) at++;
    for (; at > 0; at--) memcpy(fhist[at], fhist[at - 1], sizeof(fhist[0]));
    snprintf(fhist[0], sizeof(fhist[0]), "%s", t);
    dirty = true;
}
/* A filter that does not compile is kept apart: the row turns red and says
   why, and the filter that was showing stays as it was. */
static bool set_filter(const char *t)
{
    tap_filter_t f;
    if (!tap_filter_compile(t, &f)) {
        snprintf(fbad, sizeof(fbad), "%s", t);
        snprintf(ferr, sizeof(ferr), "%s", f.err);
        say(true, "filter: %s", f.err);
        return false;
    }
    fbad[0] = ferr[0] = 0;
    fprog = f;
    snprintf(ftext, sizeof(ftext), "%s", t);
    if (ftext[0]) remember(ftext);
    hpos = -1;
    refilter();
    if (ftext[0]) say(false, "filter applied: %d of %u shown", vn, (unsigned)count());
    else say(false, "filter cleared");
    return true;
}

/* ---- capture plumbing ---------------------------------------------------------- */
static void note(int r, int h)
{
    src_t *s = &src[r];
    s->h = h > 0 ? h : 0; s->err = h > 0 ? 0 : h;
    s->why[0] = 0; s->reason = 0;
    if (h <= 0 && (r == R_WIFI || r == R_BLE)) {
        lap_capture_status_t st;
        if (!lap_capture_status(r == R_WIFI ? LAP_CAPTURE_WIFI : LAP_CAPTURE_BLE, &st)) {
            snprintf(s->why, sizeof(s->why), "%s", st.why);
            s->reason = st.reason;
        }
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
    s->err = 0; s->why[0] = 0; s->reason = 0;
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
static void find_set(const char *t);
static void find_run(int dir);
static void poll(void)
{
    static uint8_t buf[LAP_CAPTURE_SNAP_MAX];
    const uint32_t sec = lap_millis() / 1000, q = lap_millis() / 250;
    if (q - fine_q > FINE) fine_q = q - FINE;
    for (; fine_q < q; fine_q++)
        for (int r = 0; r < R_COUNT; r++) { memmove(fine[r], fine[r] + 1, sizeof(fine[r]) - sizeof(fine[r][0])); fine[r][FINE - 1] = 0; }
    for (; hist_sec < sec; hist_sec++)
        for (int r = 0; r < R_COUNT; r++) { memmove(src[r].hist, src[r].hist + 1, sizeof(src[r].hist) - sizeof(src[r].hist[0])); src[r].hist[HIST - 1] = 0; }
    const uint32_t began = lap_millis();
    for (int r = R_WIFI; r <= R_BLE; r++) {
        if (src[r].h <= 0) continue;
        lap_capture_meta_t m;
        /* Up to a whole firmware ring per draw: at 25 draws a second that
           keeps up with thousands of frames a second, and a burst beyond it
           is dropped by the firmware and counted, never queued here. */
        for (int i = 0, n; i < (r == R_WIFI ? 256 : 128) && lap_millis() - began < 12u &&
                           (n = lap_capture_read(src[r].h, buf, sizeof(buf), &m)) >= 0; i++) {
            tap_pkt_t p = {.ts_us = m.timestamp_us, .len = (uint16_t)n, .orig = m.original_length, .rate = m.rate_500kbps,
                .rssi = m.rssi, .radio = (uint8_t)r, .ch = m.channel, .kind = m.kind, .addr_type = m.addr_type};
            memcpy(p.addr, m.addr, 6);
            src[r].lost = m.dropped;
            add(&p, buf);
        }
        if (!(lap_frame() % 25)) {
            lap_capture_status_t st;
            if (!lap_capture_status(src[r].h, &st)) {
                src[r].lost = st.dropped; src[r].reason = st.reason;
                if (st.reason) snprintf(src[r].why, sizeof(src[r].why), "%s", st.why);
            }
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
    } else if (src[R_LORA].on && !filemode && !(lap_frame() % 100)) start(R_LORA);       /* the radio may arrive later */
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
    if (find_h > 0) {
        char t[64];
        const int r = lap_dialog_poll(find_h, t, sizeof(t), NULL);
        if (r) { find_h = 0; if (r == 1 && t[0]) { find_set(t); find_run(1); } }
    }
}

/* ---- capture to card ------------------------------------------------------------------- */
static uint32_t rd32(const uint8_t *p) { return (uint32_t)p[3] << 24 | (uint32_t)p[2] << 16 | (uint32_t)p[1] << 8 | p[0]; }
static void card_path(char *b, size_t n, uint32_t idx) { snprintf(b, n, DIR "/%s_%05u.pcapng", card.base, (unsigned)idx); }
static void card_start(void)
{
    lap_time_t t;
    const int si = card.size_i, ki = card.keep_i;
    if (card.stage) { say(true, "the card is still finishing the last capture"); return; }
    lap_time(&t);
    memset(&card, 0, sizeof(card));
    card.size_i = si; card.keep_i = ki;
    snprintf(card.base, sizeof(card.base), "tap-%04d%02d%02d-%02d%02d%02d", t.year, t.month, t.day, t.hour, t.minute, t.second);
    card.on = card.header_due = true;
    card.file_no = 1; card.rm_job = card.job[1] = -1;
    card.job[0] = lap_sd_mkdir(DIR);
    card.stage = 1;
    card.last_ms = lap_millis();
    say(false, "capture to card: %s_00001.pcapng", card.base);
}
static void card_stop(void)
{
    if (!card.on) return;
    card.on = false;                    /* what waits is still written */
    say(false, "capture to card off: %u packets in %u file%s", (unsigned)(card.packets + card.flying_pk[0] + card.flying_pk[1] + card.pend_pk), (unsigned)(card.files ? card.files : 1),
        card.files > 1 ? "s" : "");
}
/* A frame for the card as it arrives. The stream is cut into files here:
   a file that has reached its size is followed by the next one's header. A
   frame that finds the buffer full is counted as dropped: the card was
   slower than the air. */
static void card_put(const tap_pkt_t *p, const uint8_t *data)
{
    if (!card.on || p->radio == R_DEC) return;
    if (card.put_bytes >= card_size[card.size_i]) { card.header_due = true; card.put_bytes = 0; }
    const uint32_t was = card.fill;
    if (card.header_due) {
        if (card.fill + 512u > SBUF) { card.dropped++; return; }
        card.fill += (uint32_t)tap_pcapng_header(sbuf + card.fill, SBUF - card.fill);
    }
    const size_t k = tap_pcapng_packet(p, data, sbuf + card.fill, SBUF - card.fill, 0);
    if (!k) { card.fill = was; card.dropped++; return; }
    card.header_due = false;
    card.fill += (uint32_t)k;
    card.put_bytes += card.fill - was;
    card.pend_pk++;
}
static void card_fail(int e)
{
    card.dropped += card.flying_pk[0] + card.flying_pk[1] + card.pend_pk;
    memset(card.flying, 0, sizeof(card.flying)); memset(card.flying_pk, 0, sizeof(card.flying_pk));
    card.pend_pk = card.fill = 0;
    card.err = e; card.on = false; card.stage = 0; card.job[0] = card.job[1] = -1;
    say(true, "capture to card stopped (%d): is there a card with room?", e);
}
/* Once a draw: what the writes did, then the next ones. Two may be on their
   way, done in the order they were given. */
static void card_step(void)
{
    char path[96];
    if (!card.stage) return;
    if (card.rm_job >= 0 && lap_sd_poll(card.rm_job) != LAP_SD_PENDING) card.rm_job = -1;
    while (card.job[0] >= 0) {
        const int r = lap_sd_poll(card.job[0]);
        if (r == LAP_SD_PENDING) break;
        if (card.stage == 1) card.stage = 2;        /* the folder may have been there already */
        else if (r < 0) { card_fail(r); return; }
        else { card.bytes += card.flying[0]; card.packets += card.flying_pk[0]; }
        card.job[0] = card.job[1]; card.flying[0] = card.flying[1]; card.flying_pk[0] = card.flying_pk[1];
        card.job[1] = -1; card.flying[1] = card.flying_pk[1] = 0;
    }
    if (card.stage == 1) return;
    /* the ring: the oldest file goes when there are more than are kept */
    const unsigned keep = card_keep[card.keep_i];
    if (keep && card.rm_job < 0 && card.file_no - card.removed > keep) {
        card_path(path, sizeof(path), ++card.removed);
        card.rm_job = lap_sd_remove(path);
    }
    if (!card.fill) { if (!card.on && card.job[0] < 0) card.stage = 0; return; }
    if (card.on && card.fill < FLUSH && lap_millis() - card.last_ms < 1000u) return;
    /* A section header at the front of what waits starts the next file. One
       job takes whole blocks, up to WMAX, and never runs into the next file. */
    for (int slot = card.job[0] < 0 ? 0 : 1; slot < 2 && card.job[slot] < 0 && card.fill; slot++) {
        const bool fresh = rd32(sbuf) == 0x0A0D0D0Au;
        uint32_t n = 0, pk = 0;
        while (n + 12u <= card.fill) {
            const uint32_t len = rd32(sbuf + n + 4);
            if (len < 12u || n + len > card.fill || n + len > WMAX || (n && rd32(sbuf + n) == 0x0A0D0D0Au)) break;
            if (rd32(sbuf + n) == 6u) pk++;
            n += len;
        }
        if (!n) { card_fail(LAP_SD_EINVAL); return; }
        card_path(path, sizeof(path), card.file_no + (fresh && card.started ? 1u : 0u));
        const int j = lap_sd_write(path, fresh ? 0 : LAP_SD_APPEND, sbuf, n);
        if (j < 0) { if (lap_millis() - card.last_ms > 5000u) card_fail(j); return; }    /* the queue is full: again next draw */
        if (fresh && card.started) card.file_no++;
        if (fresh) card.files++;
        card.started = true;
        card.job[slot] = j;
        card.flying[slot] = n; card.flying_pk[slot] = pk;
        memmove(sbuf, sbuf + n, card.fill - n);
        card.fill -= n;
        card.pend_pk -= pk;
        card.last_ms = lap_millis();
        if (card.fill < FLUSH) break;
    }
}

/* ---- a capture file from the card ----------------------------------------------------------- */
static void clear_store(uint32_t from)
{
    lap_scroll_stop(&ldrag);
    first_no = next_no = from; aw = 0; v0 = vn = sel = top = 0; t0 = 0; skipped = evicted = live_bytes = nmarks = 0;
    tsel = ttop = htop = 0;
    memset(stats, 0, sizeof(*stats));
    memset(marks, 0, MAXPK);
}
static void file_more(void)
{
    char path[112];
    snprintf(path, sizeof(path), DIR "/%s", fl.name);
    fl.job = lap_sd_read(path, fl.off + fl.fill, fbuf + fl.fill, FBUF - fl.fill);
    if (fl.job < 0) { say(true, "%s: the card is busy (%d)", fl.name, fl.job); fl.stage = 4; }
}
/* Read fl.name from its start, keeping the frames from w0 on. The radios
   stop: a file is not live, and its frames must not mix with the air's. */
static void file_load(uint32_t w0)
{
    if (fl.stage == 3) { say(true, "still reading %s", fl.name); return; }
    if (!filemode) {
        for (int r = 0; r < R_COUNT; r++) stop(r);
        card_stop();
    }
    filemode = true; paused = false;
    clear_store(w0);
    fl.w0 = fl.shown_end = w0;
    fl.index = fl.off = fl.fill = fl.pos = 0;
    fl.full = false;
    memset(fl.count, 0, sizeof(fl.count));
    memset(&fl.rd, 0, sizeof(fl.rd));
    fl.stage = 3;
    hdetail = 0; focus = 0; screen = V_MAIN; menu = M_NONE;
    file_more();
}
static void goto_no(uint32_t n);
static void file_done(void)
{
    char more[48] = "";
    fl.stage = 4;
    if (fl.rd.other) snprintf(more, sizeof(more), ", %u of kinds TAP does not read", (unsigned)fl.rd.other);
    if (fl.full || fl.w0) say(false, "%s: %u frames, %u to %u here%s", fl.name, (unsigned)fl.index, (unsigned)fl.w0 + 1, (unsigned)fl.shown_end, more);
    else if (!fl.rd.err[0]) say(fl.index == 0, "%s: %u frames%s", fl.name, (unsigned)fl.index, more);
    sel = 0; show_sel = true;
    if (fl.want) { const uint32_t n = fl.want; fl.want = 0; goto_no(n); }
}
static void file_close(void)
{
    if (!filemode) return;
    if (fl.stage == 3) { say(true, "still reading %s", fl.name); return; }
    filemode = false;
    fl.stage = 0;
    clear_store(0);
    hdetail = 0; focus = 0;
    for (int r = 0; r < R_COUNT; r++) start(r);
    say(false, "live: the radios are listening again");
}
static int newest_first(const void *a, const void *b) { return strcmp(((const lap_sd_entry_t *)b)->name, ((const lap_sd_entry_t *)a)->name); }
static void size_text(char *b, size_t n, uint32_t bytes)
{
    if (bytes < 10240u) snprintf(b, n, "%u B", (unsigned)bytes);
    else if (bytes < 10u << 20) snprintf(b, n, "%u KB", (unsigned)(bytes >> 10));
    else snprintf(b, n, "%u MB", (unsigned)(bytes >> 20));
}
static void file_picked(int index)
{
    if (index < 0 || index >= fl.n || fl.stage == 3) return;
    snprintf(fl.name, sizeof(fl.name), "%s", fl.ent[index].name);
    fl.size = fl.ent[index].size;
    fl.want = 0;
    file_load(0);
}
/* FILE > OPEN: list the folder, then the board's picker over what is there. */
static void file_browse(void)
{
    if (fl.stage == 1 || fl.stage == 3) { say(true, "the card is busy"); return; }
    fl.job = lap_sd_list(DIR, fl.ent, 128);
    if (fl.job < 0) { say(true, "the card is busy (%d)", fl.job); return; }
    fl.stage = fl.stage == 4 ? 5 : 1;       /* 5: listing with a file still open */
}
static void file_step(void)
{
    if (fl.stage == 1 || fl.stage == 5) {
        const int r = lap_sd_poll(fl.job);
        if (r == LAP_SD_PENDING) return;
        fl.stage = fl.stage == 5 ? 4 : 0;
        if (r < 0) { say(true, "no captures on the card yet (%d)", r); return; }
        int n = 0;
        for (int i = 0; i < r; i++) {
            const char *dot = strrchr(fl.ent[i].name, '.');
            if (!fl.ent[i].dir && dot && (!strcmp(dot, ".pcapng") || !strcmp(dot, ".pcap") || !strcmp(dot, ".cap"))) fl.ent[n++] = fl.ent[i];
        }
        if (!n) { say(true, "no capture files in " DIR); return; }
        qsort(fl.ent, (size_t)n, sizeof(fl.ent[0]), newest_first);
        fl.n = n > 48 ? 48 : n;
        if (!lap_picker_open("OPEN CAPTURE", file_picked)) { say(true, "the list could not open"); return; }
        for (int i = 0; i < fl.n; i++) {
            char d[16];
            size_text(d, sizeof(d), fl.ent[i].size);
            lap_picker_add(fl.ent[i].name, d);
        }
        if (n > 48) say(false, "the newest 48 of %d captures", n);
        return;
    }
    if (fl.stage != 3) return;
    const int r = lap_sd_poll(fl.job);
    if (r == LAP_SD_PENDING) return;
    if (r < 0) { say(true, "%s: reading failed (%d)", fl.name, r); snprintf(fl.rd.err, sizeof(fl.rd.err), "reading failed (%d)", r); file_done(); return; }
    const bool eof = (uint32_t)r < FBUF - fl.fill;
    fl.fill += (uint32_t)r;
    int rc = fl.fill ? 0 : -1;
    size_t used = 0;
    while (fl.pos < fl.fill) {
        tap_pkt_t m;
        const uint8_t *data = NULL;
        rc = tap_read_block(&fl.rd, fbuf + fl.pos, fl.fill - fl.pos, &m, &data, &used);
        if (rc < 0) break;
        if (rc == 1) store(&m, data);
        fl.pos += (uint32_t)used;
    }
    if (rc == -2) { say(true, "%s: %s", fl.name, fl.rd.err); file_done(); return; }
    if (eof) {
        if (rc == -1) {
            snprintf(fl.rd.err, sizeof(fl.rd.err), "%s", fl.index ? "the file ends inside a block" : "not a capture file (too short)");
            say(true, "%s: %s", fl.name, fl.rd.err);
        }
        file_done();
        return;
    }
    if (rc == -1 && used > FBUF) { fl.off += fl.pos + (uint32_t)used; fl.fill = fl.pos = 0; }     /* a block too big to hold: stepped over */
    else { memmove(fbuf, fbuf + fl.pos, fl.fill - fl.pos); fl.off += fl.pos; fl.fill -= fl.pos; fl.pos = 0; }
    file_more();
}

/* ---- go to, find, mark ------------------------------------------------------------------------ */
static void pick(int vi)
{
    sel = vi;
    tsel = ttop = htop = 0;
    show_sel = tshow = true;
    lap_scroll_stop(&ldrag);
}
static void goto_no(uint32_t n)
{
    const uint32_t no = n - 1;
    if (!n) return;
    if (no - first_no < count()) {
        int lo = 0, hi = vn - 1, at = -1;
        while (lo <= hi && at < 0) {
            const int mid = (lo + hi) / 2;
            if (view[v0 + mid] == no) at = mid; else if (view[v0 + mid] < no) lo = mid + 1; else hi = mid - 1;
        }
        if (at < 0) { say(true, "packet %u is hidden by the filter", (unsigned)n); return; }
        pick(at);
        say(false, "packet %u", (unsigned)n);
    } else if (filemode && fl.stage == 4 && no < fl.index) { fl.want = n; file_load(no > 200u ? no - 200u : 0); }
    else if (count()) say(true, "no packet %u: %u to %u are here", (unsigned)n, (unsigned)first_no + 1, (unsigned)next_no);
    else say(true, "no packets yet");
}
static void goto_done(double v) { if (v >= 1 && v < 4e9) goto_no((uint32_t)v); }
static void goto_open(void)
{
    if (!lap_numpad_open("GO TO PACKET", "number", sel_no() >= 0 ? sel_no() + 1 : 1, goto_done)) say(true, "the keypad could not open");
}
/* What to look for: a display filter when it is one, else plain text (in
   quotes to force text). */
static void find_set(const char *t)
{
    find_filter = t[0] != '"' && tap_filter_compile(t, &findprog) && findprog.count;
    if (find_filter) { snprintf(findtext, sizeof(findtext), "%s", t); return; }
    size_t n = 0;
    for (const char *c = t; *c && n + 1 < sizeof(findtext); c++) if (*c != '"') findtext[n++] = *c >= 'A' && *c <= 'Z' ? (char)(*c + 32) : *c;
    findtext[n] = 0;
}
static void find_open(void);
static void find_run(int dir)
{
    if (!findtext[0]) { find_open(); return; }
    for (int k = 1; k <= vn; k++) {
        const int vi = ((sel + dir * k) % vn + vn) % vn;
        tap_pkt_t *q = at_no(view[v0 + vi]);
        tap_dis_t d = {0};
        tap_dissect(q, arena + q->off, &d);
        if (find_filter ? shown(&findprog, q, &d) : tap_text_match(findtext, q, arena + q->off, &d)) {
            const bool wrapped = dir > 0 ? vi <= sel : vi >= sel;
            pick(vi);
            say(false, "found %s%s: packet %u%s", find_filter ? "" : "text ", findtext, (unsigned)q->no + 1, wrapped ? " (wrapped)" : "");
            return;
        }
    }
    say(true, "not found: %s%s", find_filter ? "filter " : "text ", findtext);
}
static void mark_toggle(void)
{
    const int no = sel_no();
    if (no < 0) { say(true, "select a packet first"); return; }
    uint8_t *m = &marks[(uint32_t)no % MAXPK];
    *m = !*m;
    nmarks = *m ? nmarks + 1 : nmarks - 1;
    say(false, "packet %d %s: %u marked", no + 1, *m ? "marked" : "unmarked", (unsigned)nmarks);
}
static void mark_shown(void)
{
    for (int vi = 0; vi < vn; vi++) { uint8_t *m = &marks[view[v0 + vi] % MAXPK]; if (!*m) { *m = 1; nmarks++; } }
    say(false, "%u marked", (unsigned)nmarks);
}
static void mark_step(int dir)
{
    for (int k = 1; k <= vn; k++) {
        const int vi = ((sel + dir * k) % vn + vn) % vn;
        if (is_marked(view[v0 + vi])) { pick(vi); return; }
    }
    say(true, "no marked packet is shown");
}
#if defined(LAP_API_23)
/* What the owner chose, and the recent filters, kept between visits. */
static void settings_load(void)
{
    uint8_t b[256];
    const int n = lap_settings_get("state", b, sizeof(b));
    if (n < 8 || b[0] != 1) return;
    tmode = b[1] % 3; colours = b[2] != 0; hcol = b[3] % 4; card.size_i = b[4] % 5; card.keep_i = b[5] % 5; autoscroll = b[6] != 0; ssort = b[7] % 6;
    for (int i = 0, at = 8; i < NHIST && at < n; i++) {
        const size_t l = strnlen((const char *)b + at, (size_t)(n - at));
        if (!l || at + (int)l >= n) break;
        snprintf(fhist[i], sizeof(fhist[i]), "%s", (const char *)b + at);
        at += (int)l + 1;
    }
}
static void settings_step(void)
{
    if (set_job >= 0) { if (lap_sd_poll(set_job) == LAP_SD_PENDING) return; set_job = -1; }
    if (!dirty) return;
    uint8_t b[256] = {1, (uint8_t)tmode, colours, (uint8_t)hcol, (uint8_t)card.size_i, (uint8_t)card.keep_i, autoscroll, (uint8_t)ssort};
    size_t at = 8;
    for (int i = 0; i < NHIST && fhist[i][0]; i++) {
        const size_t l = strlen(fhist[i]) + 1;
        if (at + l > sizeof(b)) break;
        memcpy(b + at, fhist[i], l);
        at += l;
    }
    set_job = lap_settings_set("state", b, at);
    dirty = false;
}
#else
static void settings_load(void) {}
static void settings_step(void) { dirty = false; }
#endif

/* ---- pcapng to the card ------------------------------------------------------------ */
static void save_begin(int mode)
{
    static const char *const tail[3] = {"", "-shown", "-marked"};
    lap_time_t t;
    if (save.stage) return;
    if (mode == SAVE_MARKED && !nmarks) { say(true, "no packet is marked: K marks the selected one"); return; }
    lap_time(&t);
    memset(&save, 0, sizeof(save));
    snprintf(save.path, sizeof(save.path), DIR "/tap-%04d%02d%02d-%02d%02d%02d%s.pcapng", t.year, t.month, t.day, t.hour, t.minute, t.second, tail[mode]);
    save.mode = mode; save.no = first_no; save.end = next_no;
    save.job = lap_sd_mkdir(DIR);
    save.stage = 1;
    say(false, "saving %s", save.path);
}
static void save_step(void)
{
    if (!save.stage) return;
    if (save.job >= 0) {
        const int r = lap_sd_poll(save.job);
        if (r == LAP_SD_PENDING) return;
        if (r < 0 && save.stage > 1) { say(true, "save failed (%d): is there a card?", r); save.stage = 0; return; }
    }
    size_t n = 0;
    if (save.stage == 1) { n = tap_pcapng_header(chunk, CHUNK); save.stage = 2; }
    else {
        while (n + TAP_PCAPNG_MAX < CHUNK) {
            uint32_t no;
            if (save.mode == SAVE_SHOWN) { if (save.vi >= vn) break; no = view[v0 + save.vi++]; }
            else { if (save.no == save.end) break; no = save.no++; }
            if (no - first_no >= count()) continue;         /* dropped from the ring meanwhile */
            if (save.mode == SAVE_MARKED && !is_marked(no)) continue;
            const tap_pkt_t *p = at_no(no);
            const size_t k = tap_pcapng_packet(p, arena + p->off, chunk + n, CHUNK - n, 0);
            if (k) save.written++;
            n += k;
        }
        if (!n) {
            say(false, "saved %u packets, %u bytes: %s", (unsigned)save.written, (unsigned)save.bytes, save.path + 8);
            lap_log("saved %u packets %u bytes %s", (unsigned)save.written, (unsigned)save.bytes, save.path);
            save.stage = 0;
            return;
        }
    }
    save.job = lap_sd_write(save.path, save.bytes ? LAP_SD_APPEND : 0, chunk, (uint32_t)n);
    save.bytes += (uint32_t)n;
    if (save.job < 0) { say(true, "save failed (%d)", save.job); save.stage = 0; }
}

/* ---- menus: one tree, a dropdown on wide screens, a box on the handheld ---------------- */
static int menu_items(int m, const uint8_t **out)
{
    static uint8_t file[12];
    int n = 0;
    switch (m) {
    case M_ROOT: *out = m_root; return sizeof(m_root);
    case M_FILE:                        /* what FILE offers depends on what is open */
        file[n++] = A_OPEN;
        if (filemode) {
            file[n++] = A_CLOSE_FILE;
            if (fl.full || fl.w0) { file[n++] = A_PART_NEXT; file[n++] = A_PART_PREV; }
        }
        file[n++] = A_SAVE_ALL; file[n++] = A_SAVE_SHOWN; file[n++] = A_SAVE_MARKED;
        if (!filemode) file[n++] = A_CLEAR;
        file[n++] = A_QUIT;
        *out = file;
        return n;
    case M_EDIT: *out = m_edit; return sizeof(m_edit);
    case M_CAPTURE: *out = m_capture; return sizeof(m_capture);
    case M_VIEW: *out = m_view; return sizeof(m_view);
    case M_ANALYZE: *out = m_analyze; return sizeof(m_analyze);
    case M_STATS: *out = m_stats; return sizeof(m_stats);
    default: *out = m_ctx; return sizeof(m_ctx);
    }
}
static const char *onoff(int r) { return !src[r].on ? "OFF" : src[r].h > 0 ? "ON" : "ON (unavailable)"; }
static void rssi_label(char *b, size_t n, const char *what, int v) { if (v) snprintf(b, n, "%s MIN RSSI   < %d dBm >", what, v); else snprintf(b, n, "%s MIN RSSI   < any >", what); }
static const char *const tmode_name[3] = {"SINCE FIRST", "SINCE PREVIOUS", "TIME OF DAY"};
static const char *const hcol_name[4] = {"SOURCE", "TIME", "DESTINATION", "LENGTH"};
static void label(int a, char *b, size_t n)
{
    static const char *const hop[3] = {"STAY ON LINK CHANNEL", "HOP 1 / 6 / 11", "HOP 1-13"};
    static const char *const types[3] = {"ALL", "MANAGEMENT", "MGMT + DATA"};
    switch (a) {
    case A_SUB_FILE: snprintf(b, n, "FILE              >"); break;
    case A_SUB_EDIT: snprintf(b, n, "EDIT AND GO       >"); break;
    case A_OPEN: snprintf(b, n, "OPEN A CAPTURE..."); break;
    case A_CLOSE_FILE: snprintf(b, n, "CLOSE FILE, GO LIVE"); break;
    case A_PART_NEXT: snprintf(b, n, "NEXT PART OF THE FILE"); break;
    case A_PART_PREV: snprintf(b, n, "PREVIOUS PART"); break;
    case A_SAVE_MARKED: snprintf(b, n, "SAVE MARKED (%u)", (unsigned)nmarks); break;
    case A_FIND: snprintf(b, n, "FIND..."); break;
    case A_FIND_NEXT: snprintf(b, n, "FIND NEXT"); break;
    case A_FIND_PREV: snprintf(b, n, "FIND PREVIOUS"); break;
    case A_GOTO: snprintf(b, n, "GO TO PACKET..."); break;
    case A_MARK: snprintf(b, n, "%s", sel_no() >= 0 && is_marked((uint32_t)sel_no()) ? "UNMARK PACKET" : "MARK PACKET"); break;
    case A_MARK_SHOWN: snprintf(b, n, "MARK ALL DISPLAYED"); break;
    case A_UNMARK_ALL: snprintf(b, n, "UNMARK ALL (%u)", (unsigned)nmarks); break;
    case A_NEXT_MARK: snprintf(b, n, "NEXT MARK"); break;
    case A_PREV_MARK: snprintf(b, n, "PREVIOUS MARK"); break;
    case A_CARD: snprintf(b, n, "CAPTURE TO CARD  %s", card.on ? "ON" : "OFF"); break;
    case A_CARD_SIZE: snprintf(b, n, "CARD FILE SIZE  < %s >", card_size_name[card.size_i]); break;
    case A_CARD_KEEP:
        if (card_keep[card.keep_i]) snprintf(b, n, "CARD FILES KEPT < %u >", card_keep[card.keep_i]); else snprintf(b, n, "CARD FILES KEPT < ALL >");
        break;
    case A_HCOL: snprintf(b, n, "LIST COLUMN  < %s >", hcol_name[hcol]); break;
    case A_RECENT: snprintf(b, n, "RECENT FILTERS..."); break;
    case A_ST_IO: snprintf(b, n, "I/O GRAPH"); break;
    case A_ST_EXPERT: snprintf(b, n, "EXPERT INFORMATION"); break;
    case A_SUB_CAPTURE: snprintf(b, n, "CAPTURE           >"); break;
    case A_SUB_VIEW: snprintf(b, n, "VIEW              >"); break;
    case A_SUB_ANALYZE: snprintf(b, n, "ANALYZE           >"); break;
    case A_SUB_STATS: snprintf(b, n, "STATISTICS        >"); break;
    case A_SAVE_ALL: snprintf(b, n, "SAVE ALL AS PCAPNG"); break;
    case A_SAVE_SHOWN: snprintf(b, n, "SAVE DISPLAYED (%d)", vn); break;
    case A_CLEAR: snprintf(b, n, "CLEAR PACKETS"); break;
    case A_QUIT: snprintf(b, n, "CLOSE TAP"); break;
    case A_PAUSE: snprintf(b, n, "%s", filemode ? "GO LIVE (CLOSE FILE)" : paused ? "RESUME CAPTURE" : "PAUSE CAPTURE"); break;
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
    case A_TIME: snprintf(b, n, "TIME  < %s >", tmode_name[tmode]); break;
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
    case A_SOURCES: snprintf(b, n, "SOURCES AND DROPS"); break;
    default: snprintf(b, n, "PACKET DETAIL"); break;
    }
}
static void open_menu(int m, int x, int y) { mparent = menu == M_ROOT ? M_ROOT : M_NONE; menu = m; msel = 0; mx = x; my = y; mbig = by_touch; }
static void open_stats(int tab) { screen = V_STATS; stab = tab; sscroll = 0; lap_scroll_stop(&sdrag); }
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
    if (no < 0) { say(true, "select a packet first"); return; }
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
    const char *seed = fbad[0] ? fbad : ftext;      /* a refused filter comes back to be mended */
    if (desk) { editing = 1; hpos = -1; snprintf(fedit, sizeof(fedit), "%s", seed); }
    else if (!text_h && !find_h) text_h = lap_text_input("Display filter", seed, 90, LAP_TEXT_LOWER);
}
static void find_open(void)
{
    if (desk) { editing = 2; snprintf(fedit, sizeof(fedit), "%s", findtext); }
    else if (!text_h && !find_h) find_h = lap_text_input("Find: a filter, or \"text\"", findtext, 60, LAP_TEXT_LOWER);
}
static void recent_picked(int index) { if (index >= 0 && index < NHIST && fhist[index][0]) set_filter(fhist[index]); }
static void recent_open(void)
{
    if (!fhist[0][0]) { say(true, "no filters used yet"); return; }
    if (!lap_picker_open("RECENT FILTERS", recent_picked)) return;
    for (int i = 0; i < NHIST && fhist[i][0]; i++) lap_picker_add(fhist[i], NULL);
}
static void act(int a, int dir)
{
    bool close = true;
    if (filemode && a > A_PAUSE && a <= A_CARD) { say(true, "a file is open: LIVE returns to the radios"); return; }
    switch (a) {
    case A_SUB_FILE: case A_SUB_EDIT: case A_SUB_CAPTURE: case A_SUB_VIEW: case A_SUB_ANALYZE: case A_SUB_STATS:
        if (dir < 0) return;
        open_menu(M_FILE + (a - A_SUB_FILE), mx, my);
        mparent = M_ROOT;
        return;
    case A_SAVE_ALL: save_begin(SAVE_ALL); break;
    case A_SAVE_SHOWN: save_begin(SAVE_SHOWN); break;
    case A_SAVE_MARKED: save_begin(SAVE_MARKED); break;
    case A_OPEN: file_browse(); break;
    case A_CLOSE_FILE: file_close(); break;
    case A_PART_NEXT: if (fl.full) file_load(fl.shown_end); else say(true, "the end of the file is here"); break;
    case A_PART_PREV: if (fl.w0) file_load(fl.w0 > MAXPK ? fl.w0 - MAXPK : 0); else say(true, "the start of the file is here"); break;
    case A_CLEAR:
        if (filemode) { say(true, "a file is open: LIVE closes it"); break; }
        clear_store(next_no);
        say(false, "cleared");
        break;
    case A_QUIT: lap_exit(); break;
    case A_FIND: find_open(); break;
    case A_FIND_NEXT: find_run(1); break;
    case A_FIND_PREV: find_run(-1); break;
    case A_GOTO: goto_open(); break;
    case A_MARK: mark_toggle(); break;
    case A_MARK_SHOWN: mark_shown(); break;
    case A_UNMARK_ALL: memset(marks, 0, MAXPK); nmarks = 0; say(false, "no packet is marked"); break;
    case A_NEXT_MARK: mark_step(1); break;
    case A_PREV_MARK: mark_step(-1); break;
    case A_PAUSE:
        if (filemode) { file_close(); break; }
        paused = !paused; say(false, "%s", paused ? "paused: new frames are not kept" : "capturing");
        break;
    case A_CARD: if (card.on) card_stop(); else card_start(); close = false; break;
    case A_CARD_SIZE: card.size_i = (card.size_i + (dir < 0 ? 4 : 1)) % 5; dirty = true; close = false; break;
    case A_CARD_KEEP: card.keep_i = (card.keep_i + (dir < 0 ? 4 : 1)) % 5; dirty = true; close = false; break;
    case A_HCOL: hcol = (hcol + (dir < 0 ? 3 : 1)) % 4; dirty = true; close = false; break;
    case A_RECENT: recent_open(); break;
    case A_WIFI: case A_BLE: case A_LORA: case A_DEC: {
        const int r = a == A_WIFI ? R_WIFI : a == A_BLE ? R_BLE : a == A_LORA ? R_LORA : R_DEC;
        src[r].on = !src[r].on; start(r); close = false;
        if (src[r].on && src[r].h <= 0) say(true, "%s: %s", tap_radio_name[r], src[r].why);
        break;
    }
    case A_WIFI_CH:
        wifi_hop = (wifi_hop + (dir < 0 ? 2 : 1)) % 3; start(R_WIFI); close = false;
        if (src[R_WIFI].on && src[R_WIFI].h <= 0) say(true, "Wi-Fi: %s", src[R_WIFI].why);
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
    case A_COLOURS: colours = !colours; dirty = true; close = false; break;
    case A_AUTOSCROLL: autoscroll = !autoscroll; if (autoscroll && vn) { sel = vn - 1; show_sel = true; } dirty = true; close = false; break;
    case A_TIME: tmode = (tmode + (dir < 0 ? 2 : 1)) % 3; dirty = true; close = false; break;
    case A_FILTER_EDIT: edit_filter(); break;
    case A_FILTER_CLEAR: set_filter(""); break;
    case A_FOLLOW_DEV: case A_FOLLOW_CONV: case A_FILTER_PROTO: case A_FILTER_NOT: follow(a); break;
    case A_QF_WIFI: set_filter("wifi and not beacon"); break;
    case A_QF_BLE: set_filter("ble"); break;
    case A_QF_LORA: set_filter("lora"); break;
    case A_QF_EAPOL: set_filter("eapol or deauth"); break;
    case A_QF_NEAR: set_filter("rssi > -60"); break;
    case A_ST_HIER: case A_ST_ENDP: case A_ST_CONV: case A_ST_IO: case A_ST_EXPERT: case A_ST_CHAN: case A_ST_RSSI: open_stats(a - A_ST_HIER); break;
    case A_HELP: screen = V_HELP; sscroll = 0; lap_scroll_stop(&sdrag); break;
    case A_SOURCES: screen = V_SRC; sscroll = 0; lap_scroll_stop(&sdrag); break;
    default: if (desk) focus = 1; else hdetail = 1; tshow = true; break;
    }
    if (close) menu = M_NONE;
}

/* ---- drawing ----------------------------------------------------------------------------- */
static uint8_t row_colour(const tap_dis_t *d) { return colours ? d->colour : LAP_WHITE; }
static const char *const short_name[R_COUNT] = {"WIFI", "BLE", "LORA", "DEC"};
static const uint8_t src_hue[R_COUNT] = {LAP_CYAN | LAP_BRIGHT, LAP_BLUE | LAP_BRIGHT, LAP_GREEN | LAP_BRIGHT, LAP_MAGENTA | LAP_BRIGHT};
/* 12345, 12k, 3M: counts that keep their column. */
static const char *qty(char *b, size_t n, uint32_t v)
{
    if (v < 10000u) snprintf(b, n, "%u", (unsigned)v);
    else if (v < 10000000u) snprintf(b, n, "%uk", (unsigned)((v + 500u) / 1000u));
    else snprintf(b, n, "%uM", (unsigned)((v + 500000u) / 1000000u));
    return b;
}
/* Packets a second over the last second, from the quarter-second buckets. */
static unsigned pps(int r) { return (unsigned)(fine[r][FINE - 1] + fine[r][FINE - 2] + fine[r][FINE - 3] + fine[r][FINE - 4]); }
/* A source's braille activity strip, cells wide: the quarter-second counts,
   scaled to the busiest moment in view (at least 4). */
static void activity(int x, int y, int cells, int r)
{
    if (cells < 2) return;
    float v[FINE];
    float hi = 4;
    for (int i = 0; i < FINE; i++) { v[i] = (float)fine[r][i]; if (v[i] > hi) hi = v[i]; }
    const int n = cells * 2 < FINE ? cells * 2 : FINE;
    lap_ui_history(x, y, cells, 1, v + FINE - n, n, 0, hi, src[r].on && src[r].h > 0 ? src_hue[r] : LAP_BLACK | LAP_BRIGHT);
}
/* The source lamp: lit while frames are coming, a steady dot when it is
   listening and quiet, a warning when it could not start. */
static void lamp(int x, int y, int r)
{
    const src_t *s = &src[r];
    const uint32_t now = lap_millis();
    if (!s->on) lap_put_cell(x, y, '.', LAP_UI_FAINT);
    else if (s->h <= 0) lap_put_cell(x, y, '!', LAP_ATTR(LAP_UI_WARN, LAP_BLACK));
    else if (!paused && now - heard_ms[r] < 300u && heard_ms[r]) lap_put_cell(x, y, '*', LAP_ATTR(LAP_UI_LIVE, LAP_BLACK));
    else lap_put_cell(x, y, 'o', LAP_ATTR(paused ? LAP_UI_WARN : LAP_GREEN, LAP_BLACK));
}
/* One source's line: lamp, "WIFI 30", the radio's drops, its activity and rate. */
static void source_line(int x, int y, int w, int r)
{
    const src_t *s = &src[r];
    char t[24], a[12], b[12];
    const int n = snprintf(t, sizeof(t), "%s %s%s", short_name[r], !s->on ? "off " : s->h > 0 ? "" : "n/a ", qty(a, sizeof(a), s->n));
    const uint8_t c = !s->on ? LAP_UI_FAINT : s->h > 0 ? LAP_UI_TEXT : LAP_ATTR(LAP_UI_WARN, LAP_BLACK);
    lamp(x, y, r);
    put(x + 2, y, w - 2, c, "%s", t);
    int at = x + 2 + n;
    if (s->lost) { put(at + 1, y, w - n - 3, LAP_ATTR(LAP_UI_ALARM, LAP_BLACK), "-%s", qty(b, sizeof(b), s->lost)); at += 2 + (int)strlen(b); }
    const int rate_x = x + w - 6, sx = x + 18 > at + 1 ? x + 18 : at + 1;
    if (s->on && s->h > 0 && rate_x - sx >= 4) {
        activity(sx, y, rate_x - sx - 1, r);
        put(rate_x, y, 6, LAP_UI_DIM, "%4u/s", pps(r));
    }
}
/* The antenna on the panel: waves ripple out while frames come in, still
   when paused, a single ring when the air is quiet. rows: 2 to 4. */
static void antenna(int x, int y, int rows)
{
    const uint32_t now = lap_millis();
    bool busy = false;
    for (int r = 0; r < R_COUNT; r++) busy |= heard_ms[r] && now - heard_ms[r] < 1500u;
    const int k = paused ? 0 : busy ? (int)(now / 160u % 4u) : (int)(now / 600u % 2u);
    for (int i = 1; i <= 3; i++) {
        const uint8_t a = LAP_ATTR(i <= k ? (busy ? LAP_UI_LIVE : LAP_GREEN) : LAP_BLACK | LAP_BRIGHT, LAP_BLACK);
        lap_put_cell(x + 4 - i, y, i <= k ? '(' : ' ', a);
        lap_put_cell(x + 4 + i, y, i <= k ? ')' : ' ', a);
    }
    lap_put_cell(x + 4, y, 'o', LAP_ATTR(paused ? LAP_UI_WARN : busy ? LAP_UI_LIVE : LAP_UI_VALUE, LAP_BLACK));
    const uint8_t mast = LAP_ATTR(LAP_UI_FRAME, LAP_BLACK);
    if (rows >= 4) { put(x + 4, y + 1, 1, mast, "|"); put(x + 3, y + 2, 3, mast, "/|\\"); put(x + 1, y + 3, 7, LAP_UI_DIM, "[ TAP ]"); }
    else if (rows == 3) { put(x + 3, y + 1, 3, mast, "/|\\"); put(x + 1, y + 2, 7, LAP_UI_DIM, "[ TAP ]"); }
    else put(x + 1, y + 1, 7, mast, "_/|\\_");
}
/* The source panel: the antenna beside each source's line (two columns of
   two on a wide screen). A tap on it opens SOURCES AND DROPS. */
static void file_panel(int y, int h);
static void source_panel(int y, int h)
{
    if (filemode) { file_panel(y, h); return; }
    lap_ui_bezel(0, y, W, h, paused ? " SOURCES  PAUSED " : card.on ? " SOURCES  TO CARD " : " SOURCES ");
    antenna(1, y + 1, h - 2);
    const int x0 = 11, w = W - 1 - x0;
    if (h - 2 >= R_COUNT) for (int r = 0; r < R_COUNT; r++) source_line(x0, y + 1 + r, w, r);
    else for (int r = 0; r < R_COUNT; r++) source_line(x0 + (r / 2) * (w / 2), y + 1 + r % 2, w / 2 - 1, r);
}
/* A capture file's name without its ending, for a title. */
static const char *file_title(void)
{
    static char t[40];
    snprintf(t, sizeof(t), "%.39s", fl.name);
    char *dot = strrchr(t, '.');
    if (dot) *dot = 0;
    return t;
}
/* A file is open: the card in place of the antenna, what the file is in
   place of the sources. Nothing here is live, and it says so. */
static void file_panel(int y, int h)
{
    static const char *const art[4] = {"  _____ ", " |FILE \\", " |      |", " |______|"};
    const bool reading = fl.stage == 3;
    const int rows = h - 2, x0 = 11, w = W - 1 - x0;
    char sz[16], a[12], b[12], c[12], t[96];
    lap_ui_bezel(0, y, W, h, reading ? " FILE  READING " : " FILE  NOT LIVE ");
    for (int i = 0; i < rows && i < 4; i++) put(1, y + 1 + i, 9, LAP_ATTR(i == 1 && rows >= 4 ? LAP_UI_VALUE : LAP_UI_FRAME, LAP_BLACK), "%s", art[rows >= 4 ? i : i ? 3 : 1]);
    if (rows >= 4) {                    /* the head runs along the card while it is read */
        const int k = reading ? (int)(lap_millis() / 120u % 4u) : -1;
        for (int i = 0; i < 4; i++) lap_put_cell(4 + i, y + 3, i == k ? '>' : '.', LAP_ATTR(i == k ? LAP_UI_LIVE : LAP_BLACK | LAP_BRIGHT, LAP_BLACK));
    }
    size_text(sz, sizeof(sz), fl.size);
    snprintf(t, sizeof(t), "WIFI %s  BLE %s  LORA %s", qty(a, sizeof(a), fl.count[R_WIFI]), qty(b, sizeof(b), fl.count[R_BLE]), qty(c, sizeof(c), fl.count[R_LORA]));
    put(x0, y + 1, w, LAP_UI_TEXT, "%s", fl.name);
    if (rows >= 4) {
        if (reading && fl.size) {
            put(x0, y + 2, 9, LAP_UI_DIM, "reading");
            lap_ui_meter(x0 + 9, y + 2, w - 10, (float)(fl.off + fl.pos) / (float)fl.size, LAP_MAGENTA);
        } else put(x0, y + 2, w, LAP_UI_DIM, "%u frames, %s", (unsigned)fl.index, sz);
        put(x0, y + 3, w, LAP_ATTR(LAP_CYAN | LAP_BRIGHT, LAP_BLACK), "%s", t);
        if (fl.full || fl.w0) put(x0, y + 4, w, LAP_ATTR(LAP_UI_WARN, LAP_BLACK), "part %u-%u of %u: FILE > NEXT PART", (unsigned)fl.w0 + 1, (unsigned)fl.shown_end, (unsigned)fl.index);
        else put(x0, y + 4, w, LAP_UI_DIM, "a file, not the air: LIVE goes back");
    } else {
        put(x0 + (int)strlen(fl.name) + 2, y + 1, w, LAP_UI_DIM, "%u frames, %s", (unsigned)fl.index, sz);
        if (fl.full || fl.w0) put(x0, y + 2, w, LAP_ATTR(LAP_UI_WARN, LAP_BLACK), "%s   part %u-%u of %u: File > Next part", t, (unsigned)fl.w0 + 1, (unsigned)fl.shown_end, (unsigned)fl.index);
        else put(x0, y + 2, w, LAP_ATTR(LAP_CYAN | LAP_BRIGHT, LAP_BLACK), "%s   a file, not the air: LIVE goes back to the radios", t);
    }
}
/* The title row's capture lamp: blinks while capturing, steady amber paused,
   FILE when a file is open; REC beside it while frames go to the card. */
static void capture_lamp(int x, int y)
{
    const bool on = paused || lap_millis() % 1000u < 600u;
    if (filemode) { lap_ui_chip(x, y, "FILE", fl.stage != 3 || on, LAP_MAGENTA); return; }
    if (card.on) lap_ui_chip(x - 6, y, "REC", lap_millis() % 1000u < 700u, LAP_RED);
    lap_ui_chip(x, y, paused ? "PAUSED" : "LIVE", on, paused ? LAP_YELLOW : LAP_GREEN);
}
/* What the user can do about a source that is not running, or a limit of
   one that is. NULL when there is nothing to say. */
static const char *hint(int r)
{
    const src_t *s = &src[r];
    if (s->on && s->h <= 0 && s->reason == LAP_CAPTURE_BUSY) return "Another app holds this capture. Close it, then switch the source off and on.";
    if (r == R_WIFI && s->on && s->h <= 0) {
        if (s->reason == LAP_CAPTURE_NO_TRANSPORT)
            return "Wi-Fi capture needs the C6 forwarder firmware on the coprocessor. Install it once from the console:";
        if (s->reason == LAP_CAPTURE_LINK_IN_USE)
            return "The board is on a Wi-Fi network and hopping would drop it. Allow that under CAPTURE > ALLOW LINK DROP, or stay on the link channel.";
    }
    if (r == R_BLE && s->on) return "Bluetooth: advertising channels 37, 38 and 39 only. Connections and data channels are not heard.";
    return NULL;
}
/* A console command that goes with the hint, on a line of its own. */
static const char *hint_cmd(int r)
{
    return r == R_WIFI && src[r].on && src[r].h <= 0 && src[r].reason == LAP_CAPTURE_NO_TRANSPORT ? "c6 flash /sdcard/lakeshark/c6/ls_c6_net.bin" : NULL;
}
/* Words of t across w columns from x at row *y, drawn between clip0 and
   clip1 (rows outside are counted, not drawn); *y moves past them. */
static int clip0, clip1;
static void wrap(int x, int *y, int w, uint8_t attr, const char *t)
{
    if (w < 8) w = 8;
    while (*t) {
        while (*t == ' ') t++;
        if (!*t) break;
        int n = (int)strlen(t);
        if (n > w) { n = w; while (n > 0 && t[n] != ' ') n--; if (!n) n = w; }
        if (*y >= clip0 && *y < clip1) put(x, *y, w, attr, "%.*s", n, t);
        (*y)++;
        t += n;
    }
}
static void filter_bar(int y)
{
    tap_filter_t probe;
    probe.err[0] = 0;
    if (editing == 2) {
        const uint8_t a = LAP_ATTR(LAP_WHITE | LAP_BRIGHT, LAP_BLUE);
        lap_fill(0, y, W, 1, ' ', a);
        put(0, y, W - 34, a, "Find: %s_", fedit);
        put(W - 34, y, 34, a, "a filter or \"text\"; N next, B back");
        return;
    }
    const bool bad = editing == 1 && fedit[0] && !tap_filter_compile(fedit, &probe);
    const bool refused = !editing && fbad[0];
    const uint8_t a = editing ? LAP_ATTR(LAP_WHITE | LAP_BRIGHT, bad ? LAP_RED : LAP_BLUE) : refused ? LAP_ATTR(LAP_WHITE | LAP_BRIGHT, LAP_RED)
                              : ftext[0] ? LAP_ATTR(LAP_BLACK, LAP_GREEN) : LAP_ATTR(LAP_WHITE, LAP_BLACK);
    lap_fill(0, y, W, 1, ' ', a);
    /* a refused filter: its text, then why, turn about */
    if (refused && lap_millis() / 1500u % 2u) put(0, y, W - 16, a, "! %s", ferr);
    else put(0, y, W - 16, a, "Filter: %s%s", editing ? fedit : refused ? fbad : ftext[0] ? ftext : desk ? "(none)  press / to type one" : "(none)  tap here", editing ? "_" : "");
    if (bad && W >= 90) put(W - 40, y, 40, a, " ! %s", probe.err);
    else put(W - 16, y, 16, LAP_ATTR(LAP_UI_VALUE, LAP_BLACK), "%6d of %-5u", vn, (unsigned)count());
}
static void fmt_time(char *b, size_t n, const tap_pkt_t *p, int vi, bool wide)
{
    if (tmode == 2) {                   /* the time of day it was heard */
        const uint64_t loc = (uint64_t)((int64_t)p->ts_us + tz_us);
        const unsigned s = (unsigned)(loc / 1000000u % 86400u);
        if (wide) snprintf(b, n, "%02u:%02u:%02u.%03u", s / 3600u, s / 60u % 60u, s % 60u, (unsigned)(loc / 1000u % 1000u));
        else snprintf(b, n, "%02u:%02u:%02u", s / 3600u, s / 60u % 60u, s % 60u);
        return;
    }
    const uint64_t base = tmode == 0 ? t0 : vi > 0 ? at_no(view[v0 + vi - 1])->ts_us : p->ts_us;
    const uint64_t us = p->ts_us > base ? p->ts_us - base : 0;
    snprintf(b, n, "%u.%03u", (unsigned)(us / 1000000u), (unsigned)(us / 1000u % 1000u));
}
/* Days since 1970 to a date. */
static void civil(int64_t z, int *y, unsigned *m, unsigned *d)
{
    z += 719468;
    const int64_t era = (z >= 0 ? z : z - 146096) / 146097;
    const unsigned doe = (unsigned)(z - era * 146097), yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100), mp = (5 * doy + 2) / 153;
    *d = doy - (153 * mp + 2) / 5 + 1;
    *m = mp < 10 ? mp + 3 : mp - 9;
    *y = (int)(yoe + era * 400) + (*m <= 2);
}
/* Nothing to list yet: a dish listening, its sweep turning. */
static int empty_art(int y, int bottom)
{
    static const char *const dish[5] = {"    .-\"\"-.    ", "   /  ..  \\   ", "  |  (  )  |  ", "   \\  ''  /   ", "    '-..-'    "};
    if (y + 7 > bottom) return y;
    const int x = (W - 14) / 2;
    for (int i = 0; i < 5; i++) put(x, y + i, 14, LAP_ATTR(LAP_UI_FRAME, LAP_BLACK), "%s", dish[i]);
    /* the sweep: one bright dot walking round the ring */
    static const int8_t ring[8][2] = {{7, 0}, {11, 1}, {12, 2}, {11, 3}, {7, 4}, {3, 3}, {2, 2}, {3, 1}};
    const int k = paused ? 0 : (int)(lap_millis() / 125u % 8u);
    lap_put_cell(x + ring[k][0], y + ring[k][1], '*', LAP_ATTR(paused ? LAP_UI_WARN : LAP_UI_LIVE, LAP_BLACK));
    put(x + 6, y + 5, 3, LAP_ATTR(LAP_UI_FRAME, LAP_BLACK), "/|\\");
    return y + 6;
}
static void list(int y, int h, bool active)
{
    const uint8_t head = LAP_ATTR(LAP_BLACK, active ? LAP_CYAN : LAP_WHITE);
    lap_fill(0, y, W, 1, ' ', head);
    static const char *const col[4] = {"Source", "Time", "Dest", "Len"};
    if (desk) put(0, y, W, head, "%6s %12s  %-18s %-18s %-11s %5s %5s  %s", "No.", tmode == 2 ? "Time of day" : tmode ? "Delta" : "Time", "Source", "Destination", "Protocol", "Len", "dBm", "Info");
    else put(0, y, W, head, "%5s %-9s %-9s %4s %s", "No.", "Proto", col[hcol], "dBm", "Info");      /* a tap on this row changes the third column */
    h--; y++;
    lsmax = vn > h ? vn - h : 0;
    if (show_sel && !dragging) {
        if (sel < top) top = sel;
        if (sel >= top + h) top = sel - h + 1;
        show_sel = false;
    }
    if (top > lsmax) top = lsmax;
    if (top < 0) top = 0;
    const uint32_t now = lap_millis();
    for (int i = 0; i < h; i++) {
        const int vi = top + i;
        if (vi >= vn) break;
        tap_pkt_t *p = at_no(view[v0 + vi]);
        tap_dis_t d = {0};
        char tm[16];
        tap_dissect(p, arena + p->off, &d);
        fmt_time(tm, sizeof(tm), p, vi, desk);
        const bool marked = is_marked(p->no);
        /* a packet slides in from the left in the live colour, settling in a quarter second */
        const uint32_t age = now - arrive[p->no % MAXPK];
        const int slide = vi != sel && age < 240u ? (int)(240u - age) / 40 : 0;
        /* a marked packet keeps its bar whatever the colouring rules say, as in Wireshark */
        const uint8_t a = vi == sel ? LAP_ATTR(LAP_BLACK, active ? LAP_YELLOW : LAP_WHITE) : marked ? LAP_ATTR(LAP_WHITE | LAP_BRIGHT, LAP_MAGENTA)
                          : slide ? LAP_ATTR(LAP_UI_LIVE, LAP_BLACK) : LAP_ATTR(row_colour(&d), colours ? tap_rule[d.rule].bg : LAP_BLACK);
        if (vi == sel || marked) lap_fill(0, y + i, W, 1, ' ', a);
        if (slide) put(0, y + i, slide, LAP_ATTR(LAP_GREEN, LAP_BLACK), "%.*s", slide, ">>>>>>");
        if (desk) put(slide, y + i, W - slide, a, "%6u %12s  %-18.18s %-18.18s %-11.11s %5u %5d  %s", (unsigned)p->no + 1, tm, d.src, d.dst, d.proto, p->orig, p->rssi, d.info);
        else {
            char c3[20];
            const char *who = hcol == 2 ? d.dst : d.src;
            const size_t sl = strlen(who);
            if (hcol == 1) snprintf(c3, sizeof(c3), "%s", tm);
            else if (hcol == 3) snprintf(c3, sizeof(c3), "%u", p->orig);
            else snprintf(c3, sizeof(c3), "%s", sl > 9 && who[2] == ':' ? who + sl - 8 : who);
            put(slide, y + i, W - slide, a, "%5u %-9.9s %-9.9s %4d %s", (unsigned)p->no + 1, d.proto, c3, p->rssi, d.info);
        }
        if (marked && !slide && p->no + 1 < (desk ? 100000u : 10000u)) lap_put_cell(0, y + i, '*', a);
    }
    if (vn > h) {           /* where the window is in the list */
        const int th = h > 1 ? (int)((int64_t)top * (h - 1) / (lsmax > 0 ? lsmax : 1)) : 0;
        lap_put_cell(W - 1, y + th, LAP_BLOCK, LAP_ATTR(LAP_UI_FRAME, LAP_BLACK));
    }
    int at = y + (vn - top < h ? vn - top : h);
    if (!vn) {
        at = empty_art(at + 1, y + h);
        put(2, ++at, W - 4, LAP_ATTR(LAP_WHITE, LAP_BLACK), "%s", count() ? "no packet matches the filter" : filemode ? (fl.stage == 3 ? "reading the file" : "no frames TAP reads in this file") :
            paused ? "paused" : "listening: nothing heard yet");
    }
    /* Room under the rows: why a source is missing, and what to do. */
    clip0 = y; clip1 = y + h;
    for (int r = 0; r < R_COUNT && at + 3 < y + h && !filemode; r++) {
        const char *tip = hint(r);
        if (!src[r].on || (count() ? !tip || (src[r].h > 0 && r != R_WIFI) : src[r].h > 0 && !tip) || (count() && r == R_BLE)) continue;
        at++;
        if (src[r].h <= 0) {
            char t[96];
            snprintf(t, sizeof(t), "%s: %s", tap_radio_name[r], src[r].why);
            wrap(2, &at, W - 4, LAP_ATTR(LAP_UI_WARN, LAP_BLACK), t);
        }
        if (tip) wrap(2, &at, W - 4, LAP_ATTR(LAP_CYAN, LAP_BLACK), tip);
        if (hint_cmd(r)) wrap(4, &at, W - 4, LAP_UI_TEXT, hint_cmd(r));
    }
}
/* The tree and the bytes of the selected packet. */
static tap_pkt_t *detail(tap_dis_t *d)
{
    const int no = sel_no();
    if (no < 0) return NULL;
    tap_pkt_t *p = at_no((uint32_t)no);
    /* The frame itself first, as Wireshark's tree starts: number, lengths,
       when, and the colouring rule that matched. */
    enum { HEAD = 5 };
    memset(d, 0, sizeof(*d));
    d->lines = lines + HEAD; d->cap = NLINES - HEAD;
    tap_dissect(p, arena + p->off, d);
    d->lines = lines; d->nlines += HEAD;
    for (int i = 0; i < HEAD; i++) { lines[i].depth = i ? 1 : 0; lines[i].off = lines[i].len = 0; }
    if (p->radio == R_DEC) snprintf(lines[0].text, sizeof(lines[0].text), "Record %u: a decode by the board, %u bytes of text", (unsigned)p->no + 1, p->len);
    else snprintf(lines[0].text, sizeof(lines[0].text), "Frame %u: %u bytes on air, %u kept", (unsigned)p->no + 1, p->orig, p->len);
    snprintf(lines[4].text, sizeof(lines[4].text), "In a capture file: frame.len %u", p->orig + (unsigned)tap_file_overhead(p));
    const uint64_t loc = (uint64_t)((int64_t)p->ts_us + tz_us);
    const unsigned s = (unsigned)(loc / 1000000u % 86400u);
    int yy; unsigned mm, dd;
    civil((int64_t)(loc / 1000000u / 86400u), &yy, &mm, &dd);
    snprintf(lines[1].text, sizeof(lines[1].text), "Arrival: %04d-%02u-%02u %02u:%02u:%02u.%06u", yy, mm, dd, s / 3600u, s / 60u % 60u, s % 60u, (unsigned)(loc % 1000000u));
    const uint64_t prev = sel > 0 ? at_no(view[v0 + sel - 1])->ts_us : p->ts_us;
    const uint64_t rel = p->ts_us > t0 ? p->ts_us - t0 : 0, dl = p->ts_us > prev ? p->ts_us - prev : 0;
    snprintf(lines[2].text, sizeof(lines[2].text), "Since first %u.%06u s, previous %u.%06u s", (unsigned)(rel / 1000000u), (unsigned)(rel % 1000000u),
             (unsigned)(dl / 1000000u), (unsigned)(dl % 1000000u));
    snprintf(lines[3].text, sizeof(lines[3].text), "Colouring rule: %s (%s)%s", tap_rule[d->rule].name, tap_rule[d->rule].filter, is_marked(p->no) ? "; marked" : "");
    if (tsel >= d->nlines) tsel = d->nlines ? d->nlines - 1 : 0;
    return p;
}
/* The tree; the selected line wraps onto the rows under it when it is
   wider than the pane, so nothing it says is cut off. */
static void tree(int y, int h, bool active, const tap_dis_t *d)
{
    int extra = 0;
    if (tsel < d->nlines) {
        const tap_line_t *l = &d->lines[tsel];
        const int wd = W - l->depth * 2 - 2, n = (int)strlen(l->text);
        if (wd > 8 && n > wd) extra = (n - 1) / wd;
        if (extra > h - 1) extra = h - 1;
    }
    tsmax = d->nlines > h ? d->nlines - h : 0;
    if (tshow && !dragging) {
        if (tsel < ttop) ttop = tsel;
        if (tsel >= ttop + h - extra) ttop = tsel - (h - extra) + 1;
        tshow = false;
    }
    if (ttop > tsmax + extra) ttop = tsmax + extra;
    if (ttop < 0) ttop = 0;
    for (int i = 0, row = 0; row < h && ttop + i < d->nlines; i++, row++) {
        const tap_line_t *l = &d->lines[ttop + i];
        const bool on = ttop + i == tsel;
        const uint8_t fg = l->depth == 0 ? (LAP_CYAN | LAP_BRIGHT) : l->depth == 1 ? LAP_WHITE : LAP_GREEN;
        const uint8_t a = on ? LAP_ATTR(LAP_BLACK, active ? LAP_YELLOW : LAP_WHITE) : LAP_ATTR(fg, LAP_BLACK);
        const int x = l->depth * 2;
        if (on) lap_fill(0, y + row, W, 1 + extra, ' ', a);
        put(x, y + row, W - x, a, "%s %s", l->depth == 0 ? "v" : l->depth == 1 ? "+" : "-", l->text);
        if (on && extra) {
            const int wd = W - x - 2;
            for (int k = 1; k <= extra && row + 1 < h; k++) put(x + 2, y + ++row, wd, a, "%s", l->text + (size_t)k * (size_t)wd);
        } else if ((int)strlen(l->text) > W - x - 2) lap_put_cell(W - 1, y + row, '>', LAP_UI_FAINT);
    }
}
static void hexpane(int y, int h, bool active, const tap_pkt_t *p, const tap_dis_t *d, bool follow_field)
{
    const int per = W >= 76 ? 16 : W >= 44 ? 8 : 4;
    const uint8_t *b = arena + p->off;
    const int hl0 = d->nlines ? d->lines[tsel].off : 0, hl1 = d->nlines ? hl0 + d->lines[tsel].len : 0;
    const int rows = (p->len + per - 1) / per;
    hsmax = rows > h ? rows - h : 0;
    if (follow_field && hl1 > hl0 && !active && !dragging) { if (hl0 / per < htop) htop = hl0 / per; if (hl0 / per >= htop + h) htop = hl0 / per - h + 1; }
    if (htop > hsmax) htop = hsmax;
    if (htop < 0) htop = 0;
    for (int r = 0; r < h && htop + r < rows; r++) {
        const int base = (htop + r) * per;
        put(0, y + r, 5, LAP_UI_FAINT, "%04x", base);
        for (int i = 0; i < per && base + i < p->len; i++) {
            const int o = base + i;
            const bool hl = o >= hl0 && o < hl1;
            const uint8_t a = hl ? LAP_ATTR(LAP_BLACK, LAP_CYAN) : LAP_ATTR(b[o] ? LAP_WHITE : (LAP_BLACK | LAP_BRIGHT), LAP_BLACK);
            put(6 + i * 3 + (i >= per / 2 ? 1 : 0), y + r, 2, a, "%02x", b[o]);
            lap_put_cell(8 + per * 3 + i, y + r, b[o] >= 32 && b[o] < 127 ? b[o] : '.', hl ? LAP_ATTR(LAP_BLACK, LAP_CYAN) : LAP_ATTR(LAP_GREEN, LAP_BLACK));
        }
    }
    if (!p->len) put(2, y, W - 2, LAP_UI_FAINT, "no bytes in this record");
}
static void rule(int y, uint8_t fg, const char *fmt, ...)
{
    char t[80];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(t, sizeof(t), fmt, ap);
    va_end(ap);
    lap_hline(0, y, W, '-', LAP_ATTR(fg, LAP_BLACK));
    put(1, y, 1, LAP_ATTR(fg, LAP_BLACK), "+");
    put(2, y, W - 4, LAP_ATTR(LAP_BLACK, fg), " %s ", t);
}
/* A share of the total as the kit's ASCII meter; a few cells of dots when there is none. */
static void bar(int x, int y, int w, uint32_t v, uint32_t max, uint8_t fg)
{
    if (w < 3) return;
    if (y < clip0 || y >= clip1) return;
    lap_ui_meter(x, y, w, max ? (float)v / (float)max : 0, fg);
}
/* Scrolling rows from `first` down to the key bar; sets how far it goes. */
static void scroll_limit(int rows, int first)
{
    smax = rows - (body1 - first);
    if (smax < 0) smax = 0;
    if (sscroll > smax) sscroll = smax;
}
static const char *const stat_tabs[ST_COUNT] = {"HIERARCHY", "ENDPOINTS", "CONVERSATIONS", "I/O GRAPH", "EXPERT", "CHANNELS", "SIGNAL"};
static const char *const sort_name[6] = {"PACKETS", "BYTES", "SIGNAL", "LAST SEEN", "FIRST SEEN", "ADDRESS"};
static uint8_t order[TAP_EPS];          /* a table's rows in the chosen order */
static char order_tail[12], order_tail2[12];
static int xrow[X_COUNT];               /* where each expert row was drawn: a tap filters to it */
#define DESC(u, v) (((u) < (v)) - ((u) > (v)))
static int by_endpoint(const void *a, const void *b)
{
    const tap_ep_t *x = &stats->ep[*(const uint8_t *)a], *y = &stats->ep[*(const uint8_t *)b];
    int c = ssort == 0 ? DESC(x->tx + x->rx, y->tx + y->rx) : ssort == 1 ? DESC(x->bytes, y->bytes) : ssort == 2 ? DESC(x->tx ? x->rssi : -128, y->tx ? y->rssi : -128) :
            ssort == 3 ? DESC(x->last_ms, y->last_ms) : ssort == 4 ? -DESC(x->first_ms, y->first_ms) : strcmp(x->addr, y->addr);
    return c ? c : (int)*(const uint8_t *)a - (int)*(const uint8_t *)b;
}
static int by_conversation(const void *a, const void *b)
{
    const tap_cv_t *x = &stats->cv[*(const uint8_t *)a], *y = &stats->cv[*(const uint8_t *)b];
    int c = ssort == 0 ? DESC(x->pk, y->pk) : ssort == 1 ? DESC(x->bytes, y->bytes) : ssort == 2 ? DESC(x->rssi, y->rssi) : ssort == 3 ? DESC(x->last_ms, y->last_ms) :
            ssort == 4 ? -DESC(x->first_ms, y->first_ms) : strcmp(stats->ep[x->a].addr, stats->ep[y->a].addr);
    return c ? c : (int)*(const uint8_t *)a - (int)*(const uint8_t *)b;
}
/* Seconds since the first packet: 12.3, then 1234. */
static const char *secs(char *b, size_t n, uint32_t ms)
{
    if (ms < 1000000u) snprintf(b, n, "%u.%u", (unsigned)(ms / 1000u), (unsigned)(ms / 100u % 10u));
    else snprintf(b, n, "%u", (unsigned)(ms / 1000u));
    return b;
}
/* Nothing counted yet: a bar chart waiting for its bars. */
static void stats_empty(int y)
{
    static const char *const art[5] = {"  |            ", "  |   _        ", "  |  | |   _   ", "  |  | |  | |  ", "  +--+-+--+-+--"};
    const int x = (W - 15) / 2;
    if (y + 7 >= body1) return;
    for (int i = 0; i < 5; i++) put(x, y + i, 15, LAP_ATTR(LAP_UI_FRAME, LAP_BLACK), "%s", art[i]);
    const int k = (int)(lap_millis() / 300u % 3u);       /* a bar growing where the next will be */
    for (int i = 0; i <= k; i++) put(x + 11, y + 3 - i, 3, LAP_ATTR(LAP_UI_LIVE, LAP_BLACK), "%s", i == k ? " _ " : "| |");
    put(2, y + 6, W - 4, LAP_UI_DIM, "Nothing counted yet: the tables fill as packets come.");
}
/* Nothing to report: a calm scope. */
static void expert_empty(int y)
{
    static const char *const art[3] = {" .-------. ", " |       | ", " '-------' "};
    const int x = (W - 11) / 2, k = (int)(lap_millis() / 150u % 7u);
    if (y + 5 >= body1) return;
    for (int i = 0; i < 3; i++) put(x, y + i, 11, LAP_ATTR(LAP_UI_FRAME, LAP_BLACK), "%s", art[i]);
    put(x + 2, y + 1, 7, LAP_ATTR(LAP_GREEN, LAP_BLACK), "-------");
    lap_put_cell(x + 2 + k, y + 1, '*', LAP_ATTR(LAP_UI_LIVE, LAP_BLACK));
    put(2, y + 4, W - 4, LAP_UI_DIM, "Nothing to report: no errors, warnings or notes.");
}
static void stats_screen(void)
{
    static const uint8_t rc[R_COUNT] = {LAP_CYAN, LAP_BLUE | LAP_BRIGHT, LAP_GREEN | LAP_BRIGHT, LAP_MAGENTA};
    const tap_stats_t *s = stats;
    const bool narrow = W < 70;
    int y = body0;
    clip0 = body0; clip1 = body1;
    smax = 0;
    for (int k = 0; k < X_COUNT; k++) xrow[k] = -1;
    if (!s->total && stab != ST_HIER && stab != ST_EXPERT) { stats_empty(y + 1); return; }
    if (stab == ST_HIER) {
        /* narrow: name 14, packets 6, % 4, bytes 7, then the meter */
        const int bx = narrow ? 36 : 47, bw = narrow ? W - bx : W > 70 ? 30 : 6;
        y -= sscroll;
        if (y >= clip0) {
            if (narrow) put(0, y, W, LAP_ATTR(LAP_UI_VALUE, LAP_BLACK), "%-14s %6s %4s %7s", "Protocol", "Pkts", "%", "Bytes");
            else put(0, y, W, LAP_ATTR(LAP_UI_VALUE, LAP_BLACK), "%-22s %7s %5s %8s", "Protocol", "Packets", "%", "Bytes");
        }
        y++;
        for (int r = 0; r < R_COUNT; r++) {
            uint32_t pkn = 0, byn = 0;
            for (int l = 0; l < L_COUNT; l++) if (tap_leaf_radio[l] == r) { pkn += s->pk[l]; byn += s->by[l]; }
            const unsigned pc = s->total ? (unsigned)(pkn * 100u / s->total) : 0;
            if (y >= clip0 && y < clip1) {
                if (narrow) put(0, y, W, LAP_ATTR(rc[r], LAP_BLACK), "%-14.14s %6u %3u%% %7u", tap_radio_name[r], (unsigned)pkn, pc, (unsigned)byn);
                else put(0, y, W, LAP_ATTR(rc[r], LAP_BLACK), "%-22s %7u %4u%% %8u", tap_radio_name[r], (unsigned)pkn, pc, (unsigned)byn);
            }
            bar(bx, y++, bw, pkn, s->total, rc[r]);
            for (int l = 0; l < L_COUNT; l++)
                if (tap_leaf_radio[l] == r && s->pk[l]) {
                    const unsigned lp = (unsigned)(s->pk[l] * 100u / s->total);
                    if (y >= clip0 && y < clip1) {
                        if (narrow) put(0, y, W, LAP_ATTR(LAP_WHITE, LAP_BLACK), "  %-12.12s %6u %3u%% %7u", tap_leaf_name[l], (unsigned)s->pk[l], lp, (unsigned)s->by[l]);
                        else put(0, y, W, LAP_ATTR(LAP_WHITE, LAP_BLACK), "  %-20s %7u %4u%% %8u", tap_leaf_name[l], (unsigned)s->pk[l], lp, (unsigned)s->by[l]);
                    }
                    bar(bx, y++, bw, s->pk[l], s->total, LAP_WHITE);
                }
        }
        char t[200];
        if (filemode) snprintf(t, sizeof(t), "%u packets, %u bytes in the whole file", (unsigned)s->total, (unsigned)s->bytes);
        else snprintf(t, sizeof(t), "%u packets, %u bytes since the last clear; %u aged out of the store; %u not kept while paused",
                      (unsigned)s->total, (unsigned)s->bytes, (unsigned)evicted, (unsigned)skipped);
        y++;
        wrap(0, &y, W, LAP_UI_FAINT, t);
        if (!s->total) stats_empty(y + 1);
        scroll_limit(y + sscroll - body0, body0);
    } else if (stab == ST_ENDP) {
        char a[12], b[12], c[12], e2[12];
        for (int i = 0; i < s->eps; i++) order[i] = (uint8_t)i;
        qsort(order, (size_t)s->eps, 1, by_endpoint);
        if (narrow) put(0, y++, W, LAP_ATTR(LAP_UI_VALUE, LAP_BLACK), "%-17s %-4s %s", "Address", "Via", "Name");
        else put(0, y++, W, LAP_ATTR(LAP_UI_VALUE, LAP_BLACK), "%-18s %-5s %6s %6s %8s %4s %8s %8s %s", "Address", "Radio", "Tx", "Rx", "Bytes", "dBm", "First s", "Last s", "Name");
        const int per = narrow ? 2 : 1, rows = s->eps * per + (s->ep_over ? 1 : 0);
        scroll_limit(rows, y);
        for (int line = sscroll; line < s->eps * per && y < body1; line++, y++) {
            const tap_ep_t *e = &s->ep[order[line / per]];
            if (!narrow) put(0, y, W, LAP_ATTR(rc[e->radio], LAP_BLACK), "%-18.18s %-5.5s %6u %6u %8u %4d %8s %8s %s", e->addr, tap_radio_name[e->radio], (unsigned)e->tx,
                             (unsigned)e->rx, (unsigned)e->bytes, e->tx ? e->rssi : 0, secs(a, sizeof(a), e->first_ms), secs(b, sizeof(b), e->last_ms), e->label);
            else if (!(line & 1)) put(0, y, W, LAP_ATTR(rc[e->radio], LAP_BLACK), "%-17.17s %-4s %s", e->addr, short_name[e->radio], e->label);
            else if (e->tx) put(0, y, W, LAP_UI_DIM, "   %s tx %s rx %s B %d dBm %s-%s s", qty(a, sizeof(a), e->tx), qty(b, sizeof(b), e->rx), qty(c, sizeof(c), e->bytes), e->rssi,
                                secs(e2, sizeof(e2), e->first_ms), secs(order_tail, sizeof(order_tail), e->last_ms));
            else put(0, y, W, LAP_UI_DIM, "   0 tx %s rx %s B, heard of only, %s-%s s", qty(b, sizeof(b), e->rx), qty(c, sizeof(c), e->bytes),
                     secs(e2, sizeof(e2), e->first_ms), secs(order_tail, sizeof(order_tail), e->last_ms));
        }
        if (s->ep_over && y < body1) put(0, y, W, LAP_ATTR(LAP_UI_ALARM, LAP_BLACK), "table full: %u more from addresses not listed", (unsigned)s->ep_over);
    } else if (stab == ST_CONV) {
        char a[12], b[12], c[12], e2[12];
        for (int i = 0; i < s->cvs; i++) order[i] = (uint8_t)i;
        qsort(order, (size_t)s->cvs, 1, by_conversation);
        if (narrow) put(0, y++, W, LAP_ATTR(LAP_UI_VALUE, LAP_BLACK), "%-17s    %-17s", "Address A", "Address B");
        else put(0, y++, W, LAP_ATTR(LAP_UI_VALUE, LAP_BLACK), "%-18s     %-18s %7s %8s %6s %6s %4s %8s %8s", "Address A", "Address B", "Packets", "Bytes", "A>B", "B>A", "dBm", "First s", "Last s");
        const int per = narrow ? 2 : 1;
        scroll_limit(s->cvs * per + (s->cv_over ? 1 : 0), y);
        for (int line = sscroll; line < s->cvs * per && y < body1; line++, y++) {
            const tap_cv_t *v = &s->cv[order[line / per]];
            const uint8_t hue = LAP_ATTR(rc[s->ep[v->a].radio], LAP_BLACK);
            if (!narrow) put(0, y, W, hue, "%-18.18s <-> %-18.18s %7u %8u %6u %6u %4d %8s %8s", s->ep[v->a].addr, s->ep[v->b].addr, (unsigned)v->pk, (unsigned)v->bytes,
                             (unsigned)v->ab, (unsigned)v->ba, v->rssi, secs(a, sizeof(a), v->first_ms), secs(b, sizeof(b), v->last_ms));
            else if (!(line & 1)) put(0, y, W, hue, "%-17.17s <> %-17.17s", s->ep[v->a].addr, s->ep[v->b].addr);
            else put(0, y, W, LAP_UI_DIM, "   %s pk %s B  A>B %s  B>A %s  %d dBm  %s-%s s", qty(a, sizeof(a), v->pk), qty(b, sizeof(b), v->bytes), qty(c, sizeof(c), v->ab),
                     qty(e2, sizeof(e2), v->ba), v->rssi, secs(order_tail, sizeof(order_tail), v->first_ms), secs(order_tail2, sizeof(order_tail2), v->last_ms));
        }
        if (s->cv_over && y < body1) put(0, y, W, LAP_ATTR(LAP_UI_ALARM, LAP_BLACK), "table full: %u more conversations not listed", (unsigned)s->cv_over);
        if (!s->cvs) wrap(0, &y, W, LAP_UI_FAINT, "No two-way traffic seen: broadcasts are listed under ENDPOINTS.");
    } else if (stab == ST_IO) {
        /* packets a second per source, in braille: two buckets a cell, the newest at the right */
        static float v[512];
        const int dots = W * 2 < 512 ? W * 2 : 512, have = s->io_n < TAP_IO ? s->io_n : TAP_IO;
        const int gh = narrow ? 5 : 3, n = have < dots ? have : dots, from = have - n;
        const int wide = n && dots / n > 1 ? dots / n : 1;      /* a short capture is drawn across the screen */
        const uint32_t step = s->io_step_ms ? s->io_step_ms : 1000u;
        char a[12], b[12];
        y -= sscroll;
        if (y >= clip0) put(0, y, W, LAP_ATTR(LAP_UI_VALUE, LAP_BLACK), "Packets a second, one dot %u s%s", (unsigned)(step / 1000u), filemode ? ", the whole file" : "");
        y++;
        for (int r = 0; r < R_COUNT; r++) {
            uint32_t peak = 0, tot = 0;
            for (int i = 0; i < have; i++) tot += s->io[r][i];
            for (int i = 0; i < n; i++) {
                for (int k = 0; k < wide; k++) v[i * wide + k] = (float)s->io[r][from + i] * 1000.0f / (float)step;
                if (s->io[r][from + i] > peak) peak = s->io[r][from + i];
            }
            if (!tot && r == R_DEC) continue;
            const float top = peak ? (float)peak * 1000.0f / (float)step : 1.0f;
            if (y >= clip0 && y < clip1) {
                put(0, y, W, LAP_ATTR(rc[r], LAP_BLACK), "%-5s %s packets", short_name[r], qty(a, sizeof(a), tot));
                put(W - 16, y, 16, LAP_UI_DIM, "peak %7u/s", (unsigned)(top + 0.5f));
            }
            y++;
            if (y >= clip0 && y + gh <= clip1) {
                lap_hline(0, y + gh - 1, W, '_', LAP_UI_FAINT);
                if (n) lap_ui_history(0, y, W, gh, v, n * wide, 0, top, rc[r]);
            }
            y += gh;
        }
        if (y >= clip0 && y < clip1) {      /* the time under the graphs */
            const int x0 = W - (n * wide + 1) / 2 > 0 ? W - (n * wide + 1) / 2 : 0;
            lap_hline(x0, y, W - x0, '-', LAP_ATTR(LAP_UI_FRAME, LAP_BLACK));
            put(x0, y, 10, LAP_UI_DIM, "%s s ", secs(a, sizeof(a), (uint32_t)from * step));
            snprintf(b, sizeof(b), " %s s", secs(a, sizeof(a), (uint32_t)have * step));
            put(W - (int)strlen(b), y, (int)strlen(b), LAP_UI_DIM, "%s", b);
        }
        y += 2;
        wrap(0, &y, W, LAP_UI_FAINT, "A long capture folds the graph in half and doubles the seconds a dot stands for.");
        scroll_limit(y + sscroll - body0, body0);
    } else if (stab == ST_EXPERT) {
        static const char *const sev_name[4] = {"ERROR", "WARN", "NOTE", "CHAT"}, *const sev_long[4] = {"Errors", "Warnings", "Notes", "Chats"};
        static const uint8_t sev_hue[4] = {LAP_RED | LAP_BRIGHT, LAP_YELLOW | LAP_BRIGHT, LAP_CYAN | LAP_BRIGHT, LAP_BLUE | LAP_BRIGHT};
        /* what the capture path lost is expert information too, live only */
        uint32_t lost = 0, sev[4] = {0, 0, 0, 0};
        char t[160];
        for (int r = 0; r < R_COUNT; r++) lost += src[r].lost;
        for (int k = 0; k < X_COUNT; k++) if (s->x[k].n) sev[tap_expert_kind[k].sev]++;
        if (!filemode) { sev[SEV_WARN] += (lost ? 1u : 0u) + (card.dropped ? 1u : 0u); sev[SEV_NOTE] += evicted ? 1u : 0u; }
        y -= sscroll;
        if (y >= clip0) for (int i = 0, x = 0; i < 4; i++) {
            const int w = snprintf(t, sizeof(t), "%s %u", sev_long[i], (unsigned)sev[i]);
            put(x, y, W - x, sev[i] ? LAP_ATTR(sev_hue[i], LAP_BLACK) : LAP_UI_FAINT, "%s", t);
            x += w + 2;
        }
        y += 2;
        const int first = y;
        for (int k = 0; k < X_COUNT; k++) {
            const tap_expert_t *x = &s->x[k];
            const int sv = tap_expert_kind[k].sev;
            if (!x->n) continue;
            if (y >= clip0 && y + 1 < clip1) {
                char last[16];
                const int ln = snprintf(last, sizeof(last), "#%u", (unsigned)x->last_no + 1);
                xrow[k] = y;
                put(0, y, 6, LAP_ATTR(LAP_BLACK, sev_hue[sv] & 7), "%-5s", sev_name[sv]);
                put(7, y, W - 21, LAP_ATTR(sev_hue[sv], LAP_BLACK), "%s", tap_expert_kind[k].what);
                put(W - 14, y, 6, LAP_UI_TEXT, "%6u", (unsigned)x->n);
                put(W - ln, y, ln, LAP_UI_DIM, "%s", last);
                put(7, y + 1, W - 7, LAP_UI_DIM, "%s", x->note);
            }
            y += 2;
        }
        if (!filemode) {
            if (lost) { snprintf(t, sizeof(t), "WARN   The radios dropped %u frames: more came than the screen could read.", (unsigned)lost); wrap(0, &y, W, LAP_ATTR(LAP_UI_WARN, LAP_BLACK), t); }
            if (card.dropped) { snprintf(t, sizeof(t), "WARN   %u frames did not reach the card: it was slower than the air.", (unsigned)card.dropped); wrap(0, &y, W, LAP_ATTR(LAP_UI_WARN, LAP_BLACK), t); }
            if (evicted) { snprintf(t, sizeof(t), "NOTE   %u packets aged out of the store%s.", (unsigned)evicted, card.on ? "; the card has them" : "; CAPTURE TO CARD keeps them"); wrap(0, &y, W, LAP_ATTR(LAP_CYAN, LAP_BLACK), t); }
        }
        if (y == first) expert_empty(y);
        else { y++; wrap(0, &y, W, LAP_UI_FAINT, "Count, then the last packet it was seen in. Tap a row to show its packets in the list."); }
        scroll_limit(y + sscroll - body0, body0);
    } else if (stab == ST_CHAN) {
        /* frames per channel as VFD bars, the busiest full */
        uint32_t max = 1;
        for (int c = 0; c < 15; c++) if (s->wifi_ch[c] > max) max = s->wifi_ch[c];
        for (int c = 0; c < 4; c++) if (s->ble_ch[c] > max) max = s->ble_ch[c];
        y -= sscroll;
        if (y >= clip0) put(0, y, W, LAP_ATTR(LAP_UI_VALUE, LAP_BLACK), "Frames per channel");
        y++;
        for (int c = 1; c <= 15; c++) {
            const uint32_t v = s->wifi_ch[c % 15];
            if (!v && c != 1 && c != 6 && c != 11) continue;
            if (y >= clip0 && y < clip1) {
                if (c == 15) put(0, y, 12, LAP_ATTR(LAP_CYAN, LAP_BLACK), "Wi-Fi 5 GHz"); else put(0, y, 12, LAP_ATTR(LAP_CYAN, LAP_BLACK), "Wi-Fi ch %2d", c);
                lap_ui_bargraph(13, y, W - 24, 1, (float)v / (float)max, -1);
                put(W - 9, y, 9, LAP_UI_TEXT, "%8u", (unsigned)v);
            }
            y++;
        }
        for (int c = 0; c < 4; c++, y++) {
            if (y < clip0 || y >= clip1) continue;
            if (c == 3) put(0, y, 12, LAP_ATTR(LAP_BLUE | LAP_BRIGHT, LAP_BLACK), "BLE ch ?"); else put(0, y, 12, LAP_ATTR(LAP_BLUE | LAP_BRIGHT, LAP_BLACK), "BLE ch %d", 37 + c);
            lap_ui_bargraph(13, y, W - 24, 1, (float)s->ble_ch[c] / (float)max, -1);
            put(W - 9, y, 9, LAP_UI_TEXT, "%8u", (unsigned)s->ble_ch[c]);
        }
        y++;
        wrap(0, &y, W, LAP_UI_FAINT, "Bluetooth: the controller does not say which advertising channel it heard, so live frames count under '?'.");
        scroll_limit(y + sscroll - body0, body0);
    } else {
        uint32_t max = 1;
        for (int r = 0; r < 3; r++) for (int b = 0; b < 9; b++) if (s->rssi[r][b] > max) max = s->rssi[r][b];
        const int cw = (W - 10) / 3;
        y -= sscroll;
        if (y >= clip0) {
            put(0, y, W, LAP_ATTR(LAP_UI_VALUE, LAP_BLACK), "dBm");
            for (int r = 0; r < 3; r++) put(10 + r * cw, y, cw, LAP_ATTR(rc[r], LAP_BLACK), "%s", narrow ? short_name[r] : tap_radio_name[r]);
        }
        y++;
        for (int b = 8; b >= 0; b--, y++) {
            if (y < clip0 || y >= clip1) continue;
            if (b == 0) put(0, y, 10, LAP_UI_DIM, "< -100"); else put(0, y, 10, LAP_UI_DIM, "%s%4d", b == 8 ? ">=" : "  ", -110 + b * 10);
            for (int r = 0; r < 3; r++) {
                bar(10 + r * cw, y, cw - 7, s->rssi[r][b], max, rc[r]);
                put(10 + r * cw + cw - 6, y, 6, LAP_UI_TEXT, "%5u", (unsigned)s->rssi[r][b]);
            }
        }
        scroll_limit(y + sscroll - body0, body0);
    }
}
/* A magnifier over a packet, at the top of the help. */
static int help_art(int y)
{
    static const char *const art[4] = {"   .--.             ", "  / )) \\  [01 a4 7f]", "  \\ (( /--[88 8e 03]", "   '--'   [  ...  ]"};
    const int x = (W - 20) / 2;
    for (int i = 0; i < 4; i++, y++)
        if (y >= clip0 && y < clip1) put(x, y, 20, LAP_ATTR(i == 0 || i == 3 ? LAP_UI_FRAME : LAP_UI_VALUE, LAP_BLACK), "%s", art[i]);
    return y + 1;
}
static void help_screen(void)
{
    /* Paragraphs; a leading space marks filter syntax. */
    static const char *const t[] = {
        "TAP listens. It never transmits, joins, injects or decrypts.", "",
        "Touch: tap a packet, tap it again for its tree and bytes. Drag a list up and down with a finger. Tap the source panel for sources and drops, the filter row to type a filter, the list's heading to change its third column.", "",
        "Keys: SPACE or P pause, / filter, X clear filter, R recent filters, E find, N next find, B find back, G go to packet, K mark, T time display, F follow device, V follow conversation, S statistics, W save pcapng, L open a capture (or back to live), C clear, I sources and drops, O options, M packet menu, TAB next pane, ENTER detail, A follow newest, BACK up a level.", "",
        "Display filters take Wireshark's names:",
        " frame.number frame.len frame.time_relative frame.marked",
        " wlan wlan.addr wlan.sa wlan.da wlan.ta wlan.ra wlan.bssid wlan.ssid",
        " wlan.fc.type wlan.fc.type_subtype wlan.fc.protected wlan.fc.retry",
        " wlan_radio.channel radiotap.dbm_antsignal eapol",
        " btle btle.advertising_address",
        " btcommon.eir_ad.entry.device_name btcommon.eir_ad.entry.company_id",
        " == != > < >= <= contains matches in {..}   && || !   and or not ( )",
        " wlan.fc.type_subtype == 0x08 && wlan.ssid contains \"Lake\"",
        "A bare field is true when the frame has it; == and contains keep case, matches does not.", "",
        "And TAP's short forms, which ignore case and match addresses by their start:",
        " wifi ble lora decode beacon probe mgmt ctrl data eapol deauth auth ibeacon eddystone scanrsp rid meshcore meshtastic protected bad retry cut marked",
        " addr == aa:bb:cc   src == ..   dst == ..",
        " ssid == name   name contains tag",
        " rssi > -70   len >= 100   ch == 6   no > 500",
        " contains \"text\"",
        " wifi and not beacon and rssi > -60", "",
        "A filter that is wrong turns the row red and says why; the list keeps the last good one. FIND takes a filter, or text in quotes.", "",
        "CAPTURE > CAPTURE TO CARD writes every frame to /sdcard/captures as it arrives, in files of the size chosen, keeping the newest few: a long capture is on the card even after the store has aged it out. W saves what is in the store: all, the displayed or the marked packets.", "",
        "FILE > OPEN A CAPTURE reads a pcapng or pcap file from /sdcard/captures. The radios stop while it is open and the title says FILE; LIVE goes back. A file larger than the store is shown a part at a time; its statistics cover all of it.", "",
        "Files are pcapng for Wireshark: 802.11 with radiotap, Bluetooth LE LL with PHDR and LoRaTap. Board decodes are text and are not written. Encrypted payloads are saved exactly as received. To a PC: lsterm's FILES panel (Ctrl-F, captures, G), or the console's sd get, or the card itself.", "",
        "Wi-Fi capture on the T-Display needs the C6 forwarder firmware, installed once from the console:", " c6 flash /sdcard/lakeshark/c6/ls_c6_net.bin", "",
        "Bluetooth: advertising channels only. The controller reports no channel or CRC; files use advertising channel 37 and a zero CRC marked 'not checked'.", "",
        "The store keeps the newest 4096 packets or 1.5 MB, whichever fills first; older ones age out and are counted under SOURCES AND DROPS.", "",
        "Colouring rules, the first that matches (VIEW > COLOUR RULES):"};
    int y = body0 - sscroll;
    clip0 = body0; clip1 = body1;
    y = help_art(y);
    for (int i = 0; i < (int)(sizeof(t) / sizeof(t[0])); i++) {
        if (!t[i][0]) { y++; continue; }
        const uint8_t a = i == 0 ? LAP_ATTR(LAP_UI_VALUE, LAP_BLACK) : t[i][0] == ' ' ? LAP_ATTR(LAP_GREEN, LAP_BLACK) : LAP_UI_DIM;
        wrap(t[i][0] == ' ' ? 3 : 1, &y, W - (t[i][0] == ' ' ? 4 : 2), a, t[i]);
    }
    for (int i = 0; i < CR_COUNT; i++, y++)
        if (y >= clip0 && y < clip1) {
            put(3, y, 20, LAP_ATTR(tap_rule[i].fg, tap_rule[i].bg), "%s", tap_rule[i].name);
            put(24, y, W - 24, LAP_UI_FAINT, "%s", tap_rule[i].filter);
        }
    put(3, y < clip1 && y >= clip0 ? y : -1, W - 4, LAP_ATTR(LAP_WHITE | LAP_BRIGHT, LAP_MAGENTA), "A marked packet");
    y += 2;
    scroll_limit(y + sscroll - body0, body0);
}
/* Each source: running or why not, what came and what was lost. */
static void sources_screen(void)
{
    static const char *const big[R_COUNT] = {"WI-FI 802.11", "BLUETOOTH LE", "LORA", "BOARD DECODES"};
    int y = body0 - sscroll;
    char t[200], sz[16], lim[16];
    clip0 = body0; clip1 = body1;
    if (filemode) {
        /* a file: what it is, how much of it is here, and that none of it is live */
        size_text(sz, sizeof(sz), fl.size);
        wrap(0, &y, W, LAP_ATTR(LAP_MAGENTA | LAP_BRIGHT, LAP_BLACK), "FILE, NOT LIVE");
        wrap(2, &y, W - 2, LAP_UI_TEXT, fl.name);
        snprintf(t, sizeof(t), "%u frames read, %s%s", (unsigned)fl.index, sz, fl.stage == 3 ? ", still reading" : "");
        wrap(2, &y, W - 2, LAP_UI_DIM, t);
        if (fl.rd.other) { snprintf(t, sizeof(t), "%u frames are of kinds TAP does not read (other link types, Bluetooth requests) and are not shown", (unsigned)fl.rd.other); wrap(2, &y, W - 2, LAP_ATTR(LAP_UI_WARN, LAP_BLACK), t); }
        if (fl.rd.err[0]) wrap(2, &y, W - 2, LAP_ATTR(LAP_UI_ALARM, LAP_BLACK), fl.rd.err);
        if (fl.full || fl.w0) {
            snprintf(t, sizeof(t), "Frames %u to %u are in the store; FILE > NEXT PART and PREVIOUS PART move through the file, GO TO PACKET jumps. Statistics cover the whole file.", (unsigned)fl.w0 + 1, (unsigned)fl.shown_end);
            wrap(2, &y, W - 2, LAP_ATTR(LAP_UI_WARN, LAP_BLACK), t);
        }
        wrap(2, &y, W - 2, LAP_ATTR(LAP_CYAN, LAP_BLACK), "The radios are stopped while a file is open. LIVE closes it and listens again.");
        y++;
    }
    for (int r = 0; r < R_COUNT && !filemode; r++) {
        const src_t *s = &src[r];
        const uint8_t c = !s->on ? (LAP_BLACK | LAP_BRIGHT) : s->h > 0 ? (LAP_GREEN | LAP_BRIGHT) : LAP_YELLOW;
        if (y >= clip0 && y < clip1) {
            lamp(0, y, r);
            put(2, y, W - 2, LAP_ATTR(c, LAP_BLACK), "%-14s %s", big[r], !s->on ? "OFF" : s->h > 0 ? "RUNNING" : "NOT AVAILABLE");
            if (s->on && s->h > 0 && W - 34 >= 6) { activity(30, y, W - 37, r); put(W - 6, y, 6, LAP_UI_DIM, "%4u/s", pps(r)); }
        }
        y++;
        snprintf(t, sizeof(t), "%u frames heard, %u dropped by the radio", (unsigned)s->n, (unsigned)s->lost);
        wrap(2, &y, W - 2, s->lost ? LAP_ATTR(LAP_UI_ALARM, LAP_BLACK) : LAP_UI_DIM, t);
        if (s->on && s->h <= 0 && s->why[0]) wrap(2, &y, W - 2, LAP_ATTR(LAP_UI_WARN, LAP_BLACK), s->why);
        const char *tip = hint(r);
        if (tip) wrap(2, &y, W - 2, LAP_ATTR(LAP_CYAN, LAP_BLACK), tip);
        if (hint_cmd(r)) wrap(4, &y, W - 4, LAP_UI_TEXT, hint_cmd(r));
        y++;
    }
    if (!filemode) {
        /* the card: what went to it, what did not, which file is being written */
        const unsigned keep = card_keep[card.keep_i];
        size_text(lim, sizeof(lim), card_size[card.size_i]);
        if (y >= clip0 && y < clip1) {
            put(0, y, W, LAP_UI_TEXT, "CAPTURE TO CARD");
            put(17, y, W - 17, LAP_ATTR(card.on ? LAP_UI_LIVE : card.err ? LAP_UI_ALARM : LAP_BLACK | LAP_BRIGHT, LAP_BLACK), "%s", card.on ? "ON" : card.err ? "STOPPED" : "OFF");
        }
        y++;
        if (card.base[0]) {
            snprintf(t, sizeof(t), "%s_%05u.pcapng", card.base, (unsigned)card.file_no);
            wrap(2, &y, W - 2, LAP_ATTR(LAP_WHITE, LAP_BLACK), t);
            if (y >= clip0 && y < clip1) { put(2, y, 8, LAP_UI_DIM, "file"); lap_ui_meter(10, y, W - 12, (float)card.put_bytes / (float)card_size[card.size_i], LAP_RED); }
            y++;
            size_text(sz, sizeof(sz), (uint32_t)(card.bytes > 0xFFFFFFFFu ? 0xFFFFFFFFu : card.bytes));
            snprintf(t, sizeof(t), "%u packets, %s written in %u file%s; %u waiting", (unsigned)card.packets, sz, (unsigned)card.files, card.files == 1 ? "" : "s",
                     (unsigned)(card.pend_pk + card.flying_pk[0] + card.flying_pk[1]));
            wrap(2, &y, W - 2, LAP_ATTR(LAP_WHITE, LAP_BLACK), t);
            snprintf(t, sizeof(t), "%u dropped: the card was slower than the air", (unsigned)card.dropped);
            wrap(2, &y, W - 2, card.dropped ? LAP_ATTR(LAP_UI_ALARM, LAP_BLACK) : LAP_UI_DIM, t);
            if (card.err) { snprintf(t, sizeof(t), "stopped on a card error (%d): is there a card with room?", card.err); wrap(2, &y, W - 2, LAP_ATTR(LAP_UI_ALARM, LAP_BLACK), t); }
        }
        if (keep) snprintf(t, sizeof(t), "Files of %s in " DIR "; the newest %u are kept, %u removed so far.", lim, keep, (unsigned)card.removed);
        else snprintf(t, sizeof(t), "Files of %s in " DIR "; all are kept.", lim);
        wrap(2, &y, W - 2, LAP_UI_FAINT, t);
        if (!card.on) wrap(2, &y, W - 2, LAP_UI_FAINT, "On, every kept frame goes to the card as it arrives: nothing is lost when the store ages packets out.");
        y++;
    }
    wrap(0, &y, W, LAP_UI_TEXT, "STORE (PSRAM)");
    /* how full the store is: packets and bytes */
    if (y >= clip0 && y < clip1) { put(2, y, 8, LAP_UI_DIM, "packets"); lap_ui_meter(10, y, W - 12, (float)count() / (float)MAXPK, LAP_CYAN); }
    y++;
    if (y >= clip0 && y < clip1) { put(2, y, 8, LAP_UI_DIM, "bytes"); lap_ui_meter(10, y, W - 12, (float)live_bytes / (float)ARENA, LAP_CYAN); }
    y++;
    snprintf(t, sizeof(t), "%u of %u packets; %u KB of %u KB", (unsigned)count(), MAXPK, (unsigned)((live_bytes + 1023u) / 1024u), ARENA / 1024u);
    wrap(2, &y, W - 2, LAP_ATTR(LAP_WHITE, LAP_BLACK), t);
    if (!filemode) {
        snprintf(t, sizeof(t), "%u aged out, oldest first; %u not kept while paused", (unsigned)evicted, (unsigned)skipped);
        wrap(2, &y, W - 2, evicted ? LAP_ATTR(LAP_UI_WARN, LAP_BLACK) : LAP_ATTR(LAP_WHITE, LAP_BLACK), t);
        wrap(2, &y, W - 2, LAP_UI_FAINT, "A full store drops the oldest packet for each new one; a burst faster than the screen reads is dropped by the radio and counted above.");
    }
    scroll_limit(y + sscroll - body0, body0);
}
static void menu_draw(void)
{
    const uint8_t *it;
    const int n = menu_items(menu, &it);
    char b[64];
    int w = 12;
    if (msel >= n) msel = n - 1;
    for (int i = 0; i < n; i++) { label(it[i], b, sizeof(b)); if ((int)strlen(b) + 4 > w) w = (int)strlen(b) + 4; }
    /* Finger-sized: three rows an item (a row of air either side of the
       words), the width of the screen, as tall as fits. */
    mrh = 1;
    if (mbig) {
        mrh = 3;
        while (mrh > 1 && n * mrh + 2 > H) mrh--;
        if (w < W - 4) w = W - 4;
    }
    if (w > W) w = W;
    const int hh = n * mrh + 2;
    int x = desk && !mbig ? mx : (W - w) / 2, y = desk && !mbig ? my : (H - hh) / 2;
    if (x + w > W) x = W - w;
    if (y + hh > H) y = H - hh;
    if (y < 0) y = 0;
    mx = x; my = y;
    lap_fill(x, y, w, hh, ' ', LAP_ATTR(LAP_WHITE, LAP_BLUE));
    lap_box(x, y, w, hh, menu == M_ROOT ? "OPTIONS" : menu == M_CTX ? "PACKET" : menu == M_FILE ? "FILE" : menu == M_EDIT ? "EDIT AND GO" : menu == M_CAPTURE ? "CAPTURE" :
            menu == M_VIEW ? "VIEW" : menu == M_ANALYZE ? "ANALYZE" : "STATISTICS", LAP_ATTR(LAP_CYAN | LAP_BRIGHT, LAP_BLUE));
    for (int i = 0; i < n && y + 1 + i * mrh < H; i++) {
        label(it[i], b, sizeof(b));
        const int ry = y + 1 + i * mrh;
        const uint8_t a = i == msel ? LAP_ATTR(LAP_BLACK, LAP_YELLOW) : LAP_ATTR(LAP_WHITE | LAP_BRIGHT, LAP_BLUE);
        if (i == msel) lap_fill(x + 1, ry, w - 2, mrh, ' ', a);
        put(x + 3, ry + mrh / 2, w - 4, a, "%s", b);
        /* a faint rule between finger-sized items, so each reads as its own key */
        if (mrh == 3 && i + 1 < n && i != msel && i + 1 != msel)
            lap_hline(x + 2, ry + mrh - 1, w - 4, '-', LAP_ATTR(LAP_BLUE | LAP_BRIGHT, LAP_BLUE));
    }
}
static const char *const bar_titles[7] = {"File", "Edit", "Capture", "View", "Analyze", "Statistics", "Help"};
static void move(int by);
/* The previous or next packet, from any pane. */
static void step_packet(int by)
{
    sel += by;
    if (sel >= vn) sel = vn - 1;
    if (sel < 0) sel = 0;
    tsel = ttop = htop = 0;
    show_sel = tshow = true;
    lap_scroll_stop(&tdrag); lap_scroll_stop(&hdrag);
}
static void set_stab(int t) { stab = (t + ST_COUNT) % ST_COUNT; sscroll = 0; lap_scroll_stop(&sdrag); }
static bool sortable(void) { return stab == ST_ENDP || stab == ST_CONV; }
/* A sideways swipe: the next table, or on the handheld's detail the next
   packet. Up and down belong to the lists, which follow the finger. */
static void swiped(void)
{
    const int sw = lap_swipe();
    if (!sw || menu) return;
    if (screen == V_STATS && (sw == LAP_SWIPE_LEFT || sw == LAP_SWIPE_RIGHT)) { set_stab(stab + (sw == LAP_SWIPE_LEFT ? 1 : -1)); return; }
    if (screen == V_MAIN && !desk && hdetail && (sw == LAP_SWIPE_LEFT || sw == LAP_SWIPE_RIGHT)) step_packet(sw == LAP_SWIPE_LEFT ? 1 : -1);
}
/* The key bar for the screen in front; its letters are the keyboard's. */
static int keys_for(lap_btn_t *k)
{
#define KEY(l, v, c, on) ((lap_btn_t){l, v, c, on, false, false})
    if (screen == V_STATS) {
        int n = 0;
        k[n++] = KEY("UP", NULL, 0, false); k[n++] = KEY("DOWN", NULL, 0, false);
        if (sortable()) k[n++] = KEY("SORT", sort_name[ssort], 'z', false);     /* in place of SOURCES: four keys keep one row */
        else k[n++] = KEY("SOURCES", NULL, 'i', false);
        k[n++] = KEY("BACK", NULL, 0, false);
        return n;
    }
    if (screen == V_HELP) { k[0] = KEY("UP", NULL, 0, false); k[1] = KEY("DOWN", NULL, 0, false); k[2] = KEY("BACK", NULL, 0, false); return 3; }
    if (screen == V_SRC) {
        k[0] = KEY("CAPTURE", "OPTIONS", 'o', false); k[1] = KEY("TO CARD", card.on ? "ON" : "OFF", 'd', card.on);
        k[2] = KEY("STATS", NULL, 's', false); k[3] = KEY("BACK", NULL, 0, false);
        return 4;
    }
    if (desk) {
        k[0] = filemode ? KEY("LIVE", "CLOSE FILE", 'l', false) : KEY(paused ? "RUN" : "PAUSE", NULL, 'p', paused); k[1] = KEY("FILTER", ftext[0] ? "ON" : NULL, '/', ftext[0] != 0);
        k[2] = KEY("FOLLOW", NULL, 'f', false); k[3] = KEY("STATS", NULL, 's', false); k[4] = KEY("SAVE", NULL, 'w', save.stage != 0);
        k[5] = KEY("SOURCES", NULL, 'i', false); k[6] = KEY("MENU", NULL, 'o', false);
        return 7;
    }
    if (hdetail) { k[0] = KEY("BACK", NULL, 0, false); k[1] = KEY("PREV", NULL, 0, false); k[2] = KEY("NEXT", NULL, 0, false); k[3] = KEY("MENU", "PACKET", 'm', false); return 4; }
    k[0] = filemode ? KEY("LIVE", NULL, 'l', false) : KEY(paused ? "RUN" : "PAUSE", NULL, 'p', paused);
    k[1] = KEY("FILTER", ftext[0] ? "ON" : NULL, '/', ftext[0] != 0);
    k[2] = KEY("STATS", NULL, 's', false); k[3] = KEY("MENU", NULL, 'o', false);
    return 4;
#undef KEY
}
static void press(int b)
{
    const int page = body1 - body0 > 3 ? body1 - body0 - 2 : 1;
    if (screen == V_STATS) {
        if (b == 0) sscroll = sscroll > page ? sscroll - page : 0;
        else if (b == 1) sscroll = sscroll + page > smax ? smax : sscroll + page;
        else if (b == 2 && sortable()) { ssort = (ssort + 1) % 6; sscroll = 0; dirty = true; }
        else if (b == 2) { screen = V_SRC; sscroll = 0; lap_scroll_stop(&sdrag); }
        else screen = V_MAIN;
    } else if (screen == V_HELP) {
        if (b == 0) sscroll = sscroll > page ? sscroll - page : 0;
        else if (b == 1) sscroll = sscroll + page > smax ? smax : sscroll + page;
        else screen = V_MAIN;
    } else if (screen == V_SRC) {
        if (b == 0) { screen = V_MAIN; open_menu(M_CAPTURE, 1, 1); }
        else if (b == 1) act(A_CARD, 1);
        else if (b == 2) { screen = V_STATS; sscroll = 0; lap_scroll_stop(&sdrag); }
        else screen = V_MAIN;
    } else if (desk) {
        switch (b) {
        case 0: act(A_PAUSE, 1); break;
        case 1: edit_filter(); break;
        case 2: follow(A_FOLLOW_DEV); break;
        case 3: screen = V_STATS; sscroll = 0; lap_scroll_stop(&sdrag); break;
        case 4: save_begin(SAVE_ALL); break;
        case 5: screen = V_SRC; sscroll = 0; lap_scroll_stop(&sdrag); break;
        default: open_menu(M_ROOT, 1, 1); break;
        }
    } else if (hdetail) {
        if (b == 0) { hdetail = 0; focus = 0; show_sel = true; }
        else if (b == 1) step_packet(-1);
        else if (b == 2) step_packet(1);
        else open_menu(M_CTX, 0, 0);
    } else {
        if (b == 0) act(A_PAUSE, 1);
        else if (b == 1) edit_filter();
        else if (b == 2) { screen = V_STATS; sscroll = 0; lap_scroll_stop(&sdrag); }
        else open_menu(M_ROOT, 0, 0);
    }
}
/* The title row: what is in front and the capture lamp. */
static void title(const char *what)
{
    const uint8_t tb = LAP_ATTR(LAP_BLACK, LAP_CYAN);
    const int w = W - (card.on && !filemode ? 16 : 10);
    lap_fill(0, 0, W, 1, ' ', tb);
    if (what) put(1, 0, w, tb, "%s%s", what, filemode ? "  (FILE)" : "");
    else if (filemode && fl.stage == 3) put(1, 0, w, tb, "FILE %.26s  reading %u", file_title(), (unsigned)fl.index);
    else if (filemode && (fl.full || fl.w0)) put(1, 0, w, tb, "FILE %.22s  %u-%u of %u", file_title(), (unsigned)fl.w0 + 1, (unsigned)fl.shown_end, (unsigned)fl.index);
    else if (filemode) put(1, 0, w, tb, "FILE %.26s  %u pkts", file_title(), (unsigned)count());
    else if (evicted) put(1, 0, w, tb, "TAP  %u pkts  %u aged out", (unsigned)count(), (unsigned)evicted);
    else put(1, 0, w, tb, "TAP  %u pkts", (unsigned)count());
    capture_lamp(W - 9, 0);
}
static void draw(void)
{
    if (!pk) return;
    W = lap_cols(); H = lap_rows();
    desk = W >= 90 && H >= 24;
    poll();
    save_step();
    card_step();
    file_step();
    settings_step();
    lap_clear(LAP_ATTR(LAP_WHITE, LAP_BLACK));
    swiped();
    lap_btn_t k[8];
    const int nk = keys_for(k);
    kr = lap_ui_keys_rows(nk);
    tr = desk ? 1 : lap_ui_keys_rows(3);
    /* The lists follow a finger that went down on them, where they were drawn last frame. */
    dragging = false;
    if (!menu) {
        if (screen == V_MAIN) {
            dragging |= lap_scroll_follow(&ldrag, &top, lsmax, list_y + 1, list_y + list_h);
            dragging |= lap_scroll_follow(&tdrag, &ttop, tsmax, tree_y, tree_y + tree_h);
            dragging |= lap_scroll_follow(&hdrag, &htop, hsmax, hex_y, hex_y + hex_h);
        } else dragging = lap_scroll_follow(&sdrag, &sscroll, smax, body0, body1);
    }
    list_h = tree_h = hex_h = 0;
    body1 = H - kr;
    if (screen != V_MAIN) {
        const char *const what = screen == V_STATS ? "TAP  STATISTICS" : screen == V_HELP ? "TAP  HELP" : "TAP  SOURCES AND DROPS";
        if (screen == V_STATS && desk) {
            /* the tables as tabs along the top */
            lap_fill(0, 0, W, 1, ' ', LAP_ATTR(LAP_WHITE, LAP_BLACK));
            for (int i = 0, x = 1; i < ST_COUNT; i++) {
                put(x, 0, W - x, i == stab ? LAP_ATTR(LAP_BLACK, LAP_YELLOW) : LAP_ATTR(LAP_WHITE, LAP_BLACK), " %s ", stat_tabs[i]);
                x += (int)strlen(stat_tabs[i]) + 3;
            }
            capture_lamp(W - 9, 0);
            lap_hline(0, 1, W, '-', LAP_ATTR(LAP_UI_FRAME, LAP_BLACK));
            body0 = 2;
        } else if (screen == V_STATS) {
            title(what);
            char n[16];
            snprintf(n, sizeof(n), "%d/%d", stab + 1, ST_COUNT);
            const lap_btn_t t[3] = {{"<", NULL, 0, false, false, false}, {stat_tabs[stab], n, 0, false, false, false}, {">", NULL, 0, false, false, false}};
            art_strip(1, W, tr, t[1].label, t[1].value, false);     /* not lap_ui_keys: one bar a frame keeps the bottom keys from looking just changed */
            body0 = 1 + tr;
        } else if (screen == V_SRC) {
            /* the same source panel as the list, the details under it */
            title(what);
            hdr_y = 1; hdr_h = desk ? 4 : 6;
            source_panel(hdr_y, hdr_h);
            body0 = hdr_y + hdr_h + 1;
        } else { title(what); body0 = 1; }
        if (screen == V_STATS) stats_screen();
        else if (screen == V_HELP) help_screen();
        else sources_screen();
    } else {
        tap_dis_t d;
        tap_pkt_t *p = detail(&d);
        if (desk) {
            int x = 1;
            lap_fill(0, 0, W, 1, ' ', LAP_ATTR(LAP_BLACK, LAP_WHITE));
            for (int i = 0; i < 7; i++) { put(x, 0, W - x, LAP_ATTR(LAP_BLACK, LAP_WHITE), "%s", bar_titles[i]); x += (int)strlen(bar_titles[i]) + 2; }
            if (filemode) put(x + 1, 0, W - x - 12, LAP_ATTR(LAP_MAGENTA, LAP_WHITE), "FILE %s", fl.name);
            else put(W - (card.on ? 29 : 23), 0, 12, LAP_ATTR(LAP_BLUE, LAP_WHITE), "TAP analyser");
            capture_lamp(W - 9, 0);
            hdr_y = 1; hdr_h = 4;
            source_panel(hdr_y, hdr_h);
            filt_y = hdr_y + hdr_h;
            filter_bar(filt_y);
            body0 = filt_y + 1;
            const int body = body1 - body0 - 2;
            list_y = body0; list_h = body * 45 / 100; tree_h = body * 30 / 100; hex_h = body - list_h - tree_h;
            tree_y = list_y + list_h + 1; hex_y = tree_y + tree_h + 1;
            list(list_y, list_h, focus == 0);
            if (p) rule(tree_y - 1, focus == 1 ? LAP_YELLOW : LAP_CYAN, "Packet %u: %s, %u bytes on air, %u kept%s", (unsigned)p->no + 1, d.proto, p->orig, p->len, is_marked(p->no) ? ", marked" : "");
            else rule(tree_y - 1, LAP_CYAN, "Packet detail");
            rule(hex_y - 1, focus == 2 ? LAP_YELLOW : LAP_CYAN, "Bytes");
            if (p) { const bool moved = tshow; tree(tree_y, tree_h, focus == 1, &d); hexpane(hex_y, hex_h, focus == 2, p, &d, moved); }
            else { tree_h = hex_h = 0; }
        } else {
            title(NULL);
            hdr_y = 1; hdr_h = 6;
            source_panel(hdr_y, hdr_h);
            filt_y = hdr_y + hdr_h;
            filter_bar(filt_y);
            body0 = filt_y + 1;
            if (!hdetail) { list_y = body0; list_h = body1 - body0; list(list_y, list_h, true); }
            else if (p) {
                /* the tree as tall as it needs (and a row to grow into), the bytes below */
                const int avail = body1 - body0 - 2;
                tree_y = body0 + 1;
                tree_h = avail * 6 / 10;
                if (d.nlines + 2 < tree_h) tree_h = d.nlines + 2 > 6 ? d.nlines + 2 : 6;
                hex_y = tree_y + tree_h + 1; hex_h = body1 - hex_y;
                rule(body0, LAP_YELLOW, "Packet %u: %s%s", (unsigned)p->no + 1, d.proto, is_marked(p->no) ? "  * marked" : "");
                const bool moved = tshow;     /* the bytes follow the field only when the pick moved */
                tree(tree_y, tree_h, focus != 2, &d);
                rule(hex_y - 1, focus == 2 ? LAP_YELLOW : LAP_CYAN, "Bytes");
                hexpane(hex_y, hex_h, focus == 2, p, &d, moved);
            } else hdetail = 0;
        }
    }
    /* A note for four seconds over the title; the keys stay where they are. */
    if (status[0] && lap_millis() - status_ms < 4000u) {
        const uint8_t sb = status_bad ? LAP_ATTR(LAP_WHITE | LAP_BRIGHT, LAP_RED) : LAP_ATTR(LAP_BLACK, LAP_WHITE);
        lap_fill(0, 0, W, 1, ' ', sb);
        put(1, 0, W - 2, sb, "%s", status);
    }
    lap_ui_keys(0, H - kr, W, kr, k, nk, -1);
    if (menu) menu_draw();
    /* a remote mouse: right button opens the packet menu where it is */
    lap_input_event_t ev;
    int cw = 8, chh = 16;
    lap_cell_px(&cw, &chh);
    while (lap_input_next(&ev) == 1)
        if (ev.kind == LAP_INPUT_POINTER_DOWN && ev.button == 2 && screen == V_MAIN && cw > 0 && chh > 0) {
            const int col = ev.x / cw, row = ev.y / chh;
            if (row > list_y && row < list_y + list_h && top + row - list_y - 1 < vn && (desk || !hdetail)) { sel = top + row - list_y - 1; tsel = ttop = htop = 0; }
            menu = M_NONE;
            by_touch = false;                   /* a mouse's menu stays compact */
            open_menu(M_CTX, col, row);
        }
}

/* ---- input --------------------------------------------------------------------------------- */
static void move(int by)
{
    if (focus == 1 || (!desk && hdetail && focus != 2)) { tsel += by; if (tsel < 0) tsel = 0; tshow = true; }
    else if (focus == 2) { htop += by; if (htop < 0) htop = 0; }
    else {
        sel += by;
        if (sel >= vn) sel = vn - 1;
        if (sel < 0) sel = 0;
        tsel = ttop = htop = 0;
        show_sel = tshow = true;
    }
}
static bool key(lap_key_t k, char ch)
{
    by_touch = false;
    if (editing) {
        const size_t n = strlen(fedit);
        if (k == LAP_KEY_ENTER && editing == 1) { if (set_filter(fedit)) editing = 0; }
        else if (k == LAP_KEY_ENTER) { editing = 0; if (fedit[0]) { find_set(fedit); find_run(1); } }
        else if (k == LAP_KEY_BACK) { editing = 0; fbad[0] = 0; }
        else if (editing == 1 && (k == LAP_KEY_UP || k == LAP_KEY_DOWN)) {       /* the recent filters, newest first */
            hpos += k == LAP_KEY_UP ? 1 : -1;
            if (hpos >= NHIST || (hpos >= 0 && !fhist[hpos][0])) hpos--;
            if (hpos < -1) hpos = -1;
            snprintf(fedit, sizeof(fedit), "%s", hpos < 0 ? "" : fhist[hpos]);
        }
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
        const int page = body1 - body0 > 3 ? body1 - body0 - 2 : 1;
        if (k == LAP_KEY_BACK) screen = V_MAIN;
        else if (k == LAP_KEY_LEFT && screen == V_STATS) set_stab(stab - 1);
        else if ((k == LAP_KEY_RIGHT || k == LAP_KEY_TAB) && screen == V_STATS) set_stab(stab + 1);
        else if (k == LAP_KEY_DOWN || k == LAP_KEY_WHEEL_DOWN || k == LAP_KEY_PGDN) { sscroll += k == LAP_KEY_PGDN ? page : 1; if (sscroll > smax) sscroll = smax; }
        else if (k == LAP_KEY_UP || k == LAP_KEY_WHEEL_UP || k == LAP_KEY_PGUP) { sscroll -= k == LAP_KEY_PGUP ? page : 1; if (sscroll < 0) sscroll = 0; }
        else if (k == LAP_KEY_CHAR && (ch == 's' || ch == 'S')) { screen = V_STATS; sscroll = 0; lap_scroll_stop(&sdrag); }
        else if (k == LAP_KEY_CHAR && (ch == 'i' || ch == 'I')) { screen = V_SRC; sscroll = 0; lap_scroll_stop(&sdrag); }
        else if (k == LAP_KEY_CHAR && (ch == 'o' || ch == 'O')) { screen = V_MAIN; open_menu(M_CAPTURE, 1, 1); }
        else if (k == LAP_KEY_CHAR && (ch == 'z' || ch == 'Z') && screen == V_STATS && sortable()) { ssort = (ssort + 1) % 6; sscroll = 0; dirty = true; }
        else if (k == LAP_KEY_CHAR && (ch == 'd' || ch == 'D') && screen == V_SRC) act(A_CARD, 1);
        else if (k == LAP_KEY_CHAR && (ch == 'e' || ch == 'E') && screen == V_STATS) set_stab(ST_EXPERT);
        else return false;
        return true;
    }
    status[0] = 0;
    switch (k) {
    case LAP_KEY_UP: case LAP_KEY_WHEEL_UP: move(k == LAP_KEY_UP ? -1 : -3); return true;
    case LAP_KEY_DOWN: case LAP_KEY_WHEEL_DOWN: move(k == LAP_KEY_DOWN ? 1 : 3); return true;
    case LAP_KEY_PGUP: move(-(list_h > 3 ? list_h - 2 : 1)); return true;
    case LAP_KEY_PGDN: move(list_h > 3 ? list_h - 2 : 1); return true;
    case LAP_KEY_HOME: move(-(int)MAXPK); return true;
    case LAP_KEY_END: move((int)MAXPK); return true;
    case LAP_KEY_TAB: focus = (focus + 1) % 3; if (!desk && hdetail && focus == 0) focus = 1; return true;
    case LAP_KEY_ENTER: case LAP_KEY_RIGHT:
        if (desk) focus = focus == 0 ? 1 : focus;
        else if (sel_no() >= 0) { hdetail = 1; focus = 1; tshow = true; }
        return true;
    case LAP_KEY_LEFT: if (desk) focus = 0; else { hdetail = 0; focus = 0; show_sel = true; } return true;
    case LAP_KEY_BACK:
        if (!desk && hdetail) { hdetail = 0; focus = 0; show_sel = true; return true; }
        if (desk && focus) { focus = 0; return true; }
        return false;                   /* HOME */
    case LAP_KEY_MENU: case LAP_KEY_F10: open_menu(M_ROOT, 1, 1); return true;
    case LAP_KEY_CHAR: break;
    default: return false;
    }
    switch (ch >= 'A' && ch <= 'Z' ? ch + 32 : ch) {
    case ' ': case 'p': if (filemode) say(true, "a file is open: L goes back to live"); else act(A_PAUSE, 1); break;
    case '/': edit_filter(); break;
    case 'x': set_filter(""); break;
    case 'f': follow(A_FOLLOW_DEV); break;
    case 'v': follow(A_FOLLOW_CONV); break;
    case 's': screen = V_STATS; sscroll = 0; lap_scroll_stop(&sdrag); break;
    case 'i': screen = V_SRC; sscroll = 0; lap_scroll_stop(&sdrag); break;
    case 'w': save_begin(SAVE_ALL); break;
    case 'k': mark_toggle(); break;
    case 'e': case 6: find_open(); break;
    case 'n': find_run(ch == 'N' ? -1 : 1); break;
    case 'b': find_run(-1); break;
    case 't': act(A_TIME, 1); say(false, "time: %s", tmode_name[tmode]); break;
    case 'l': if (filemode) file_close(); else file_browse(); break;
    case 'r': recent_open(); break;
    case 'c': act(A_CLEAR, 1); break;
    case 'a': act(A_AUTOSCROLL, 1); break;
    case 'o': open_menu(M_ROOT, 1, 1); break;
    case 'm': open_menu(M_CTX, W / 3, list_y + 2); break;
    case '?': case 'h': screen = V_HELP; sscroll = 0; lap_scroll_stop(&sdrag); break;
    case 'g': goto_open(); break;
    default: return false;
    }
    return true;
}
static bool touch(int col, int row)
{
    by_touch = true;
    if (menu) {
        const uint8_t *it;
        const int n = menu_items(menu, &it);
        if (row > my && row <= my + n * mrh && col >= mx) { msel = (row - my - 1) / mrh; if (msel >= n) msel = n - 1; act(it[msel], 1); }
        else menu = M_NONE;
        return true;
    }
    if (editing) { editing = 0; return true; }
    if (row >= H - kr) {                /* the key bar */
        const int b = dragging ? -1 : lap_ui_key_hit(col, row);
        if (b >= 0) { status[0] = 0; press(b); }
        return true;
    }
    if (row == 0 && status[0] && lap_millis() - status_ms < 4000u) { status[0] = 0; return true; }   /* the note over the title: a tap clears it */
    if (screen != V_MAIN) {
        if (screen == V_STATS && desk && row == 0) {
            for (int i = 0, x = 1; i < ST_COUNT; i++) {
                const int w = (int)strlen(stat_tabs[i]) + 2;
                if (col >= x && col < x + w) { set_stab(i); break; }
                x += w + 1;
            }
        } else if (screen == V_STATS && !desk && row >= 1 && row < 1 + tr) {
            const int t = col * 3 / (W > 0 ? W : 1);        /* the strip's three keys: < name > */
            if (t != 1) set_stab(stab + (t < 1 ? -1 : 1));
        } else if (screen == V_SRC && row >= hdr_y && row < hdr_y + hdr_h) { screen = V_MAIN; open_menu(filemode ? M_FILE : M_CAPTURE, 1, 1); }
        else if (screen == V_STATS && stab == ST_EXPERT && !dragging) {
            /* an expert row: the list, filtered to what it counted */
            for (int i = 0; i < X_COUNT; i++)
                if (xrow[i] >= 0 && (row == xrow[i] || row == xrow[i] + 1)) { if (set_filter(tap_expert_kind[i].filter)) screen = V_MAIN; break; }
        }
        return true;
    }
    status[0] = 0;
    if (row == 0 && desk) {
        int x = 1;
        for (int i = 0; i < 7; i++) {
            const int w = (int)strlen(bar_titles[i]);
            if (col >= x - 1 && col <= x + w) { if (i == 6) screen = V_HELP; else open_menu(M_FILE + i, x - 1, 1); return true; }
            x += w + 2;
        }
        if (col >= W - 9 && !filemode) act(A_PAUSE, 1);
        return true;
    }
    if (row == 0) { if (!filemode) act(A_PAUSE, 1); return true; }
    if (row >= hdr_y && row < hdr_y + hdr_h) { screen = V_SRC; sscroll = 0; lap_scroll_stop(&sdrag); return true; }
    if (row == filt_y) { edit_filter(); return true; }
    if (dragging) return true;          /* the finger is moving a list, not picking */
    if (!desk && !hdetail && row == list_y) { hcol = (hcol + 1) % 4; dirty = true; return true; }     /* the heading: the third column */
    if ((desk || !hdetail) && row > list_y && row < list_y + list_h) {
        const int vi = top + row - list_y - 1;
        if (vi < vn) { if (!desk && vi == sel) { hdetail = 1; focus = 1; tshow = true; } else { sel = vi; tsel = ttop = htop = 0; focus = 0; tshow = true; } }
        return true;
    }
    if (row >= tree_y && row < tree_y + tree_h && (desk || hdetail)) { tsel = ttop + row - tree_y; focus = 1; return true; }
    if (row >= hex_y && row < hex_y + hex_h && (desk || hdetail)) { focus = 2; return true; }
    /* Empty ground. Every tap is TAP's own: one handed back would be turned
       into UP, ENTER or DOWN by where it fell, and ENTER opens things. */
    return true;
}

static void enter(void)
{
    pk = calloc(MAXPK, sizeof(*pk));
    arena = malloc(ARENA);
    chunk = malloc(CHUNK);
    view = calloc(2 * MAXPK, sizeof(*view));
    lines = calloc(NLINES, sizeof(*lines));
    marks = calloc(MAXPK, 1);
    sbuf = malloc(SBUF);
    fbuf = malloc(FBUF);
    memset(&fl, 0, sizeof(fl));
    fl.ent = calloc(128, sizeof(*fl.ent));
    memset(&card, 0, sizeof(card));
    card.size_i = 2; card.keep_i = 2; card.job[0] = card.job[1] = card.rm_job = -1;
    filemode = find_filter = dirty = false;
    tmode = hcol = ssort = 0; hpos = -1; nmarks = 0; set_job = -1; find_h = 0;
    fbad[0] = ferr[0] = findtext[0] = 0;
    memset(fhist, 0, sizeof(fhist));
    findprog.count = 0;
    stats = calloc(1, sizeof(*stats));
    arrive = calloc(MAXPK, sizeof(*arrive));
    first_no = next_no = aw = 0;
    v0 = vn = sel = top = tsel = ttop = htop = 0;
    ftext[0] = fedit[0] = status[0] = 0;
    fprog.count = 0;
    editing = 0;
    paused = drop_link = false;
    colours = autoscroll = true;
    t0 = 0; skipped = evicted = live_bytes = status_ms = 0; status_bad = false; hist_sec = 0; smax = 0;
    screen = V_MAIN; focus = hdetail = stab = sscroll = 0; menu = M_NONE;
    wifi_hop = wifi_types = lora_preset = 0;
    confirm_h = text_h = 0;
    memset(min_rssi, 0, sizeof(min_rssi));
    memset(src, 0, sizeof(src));
    memset(&save, 0, sizeof(save));
    memset(fine, 0, sizeof(fine)); memset(heard_ms, 0, sizeof(heard_ms)); fine_q = lap_millis() / 250;
    memset(&ldrag, 0, sizeof(ldrag)); memset(&tdrag, 0, sizeof(tdrag)); memset(&hdrag, 0, sizeof(hdrag)); memset(&sdrag, 0, sizeof(sdrag));
    lsmax = tsmax = hsmax = 0; dragging = false; show_sel = tshow = true;
    kr = tr = 1; hdr_y = 1; hdr_h = 0; filt_y = 2; body0 = 1; body1 = 1;
    list_y = list_h = tree_y = tree_h = hex_y = hex_h = 0;
    if (!pk || !arena || !chunk || !view || !lines || !stats || !arrive || !marks || !sbuf || !fbuf || !fl.ent) { lap_log("no memory"); lap_exit(); return; }
    lap_time_t now;
    lap_time(&now);
    wall0 = (uint64_t)(now.unix > 0 ? now.unix : 0) * 1000000u;
    /* the hours the board's clock is from UTC, for the time of day column */
    int off = now.hour * 3600 + now.minute * 60 + now.second - (int)(now.unix > 0 ? now.unix % 86400 : 0);
    if (off > 14 * 3600) off -= 86400;
    if (off < -12 * 3600) off += 86400;
    tz_us = now.unix > 0 ? (int64_t)((off + (off < 0 ? -30 : 30)) / 60 * 60) * 1000000 : 0;
    settings_load();
    for (int r = 0; r < R_COUNT; r++) { src[r].on = true; start(r); }
    if (src[R_WIFI].h <= 0 && src[R_WIFI].reason == LAP_CAPTURE_NO_TRANSPORT) say(true, "Wi-Fi capture needs the C6 forwarder: tap WIFI");
}
static void leave(void)
{
    for (int r = 0; r < R_COUNT; r++) stop(r);
    /* what waits still goes to the card: up to four jobs can queue behind the one running */
    card.on = false;
    for (int i = 0; i < 2 && card.stage == 2 && card.fill; i++) { card.job[0] = card.job[1] = card.rm_job = -1; card_step(); }
    card.stage = 0; card.on = false;
    filemode = false; fl.stage = 0;
    free(pk); free(arena); free(chunk); free(view); free(lines); free(stats); free(arrive); free(marks); free(sbuf); free(fbuf); free(fl.ent);
    pk = NULL; arena = chunk = marks = sbuf = fbuf = NULL; view = NULL; lines = NULL; stats = NULL; arrive = NULL; fl.ent = NULL;
}

LAP_APP(.id = "tap", .name = "TAP", .sub = "packet sniffer", .version = "1.2.0", .author = "LakeShark",
        .icon = LAP_ICON_WAVE, .color = LAP_CYAN, .flags = LAP_CAP_RADIO_RX,
        .enter = enter, .leave = leave, .draw = draw, .key = key, .touch = touch);
