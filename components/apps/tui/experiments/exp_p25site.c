/* P25 SITE FINDER: whether a P25 Phase 1 site is on the air here, and which
   one, from the NAC and DUID in its frames, on the LoRa chip's FSK engine.

   The engine runs at the symbol rate, 4800 bps, and slices each C4FM symbol
   by its sign. The frame sync is all outer symbols, so its 24 sign bits are
   the session's sync word; the 33 bits after it are the NID's first bits and
   one status symbol, and the nearest NID to them is the answer
   (p25_sitefind.h has the code and why it is enough). Which way the chip
   counts a positive deviation is not known, so both sync words are tried
   until one decodes, and the channel keeps that one. Voice is the P25 app's,
   which samples four bits a symbol on the LR2021.

   The channels come from the P25 profile on the card - its control= lines,
   with the NAC its comments say each should carry - or one frequency, 150 to
   1100 MHz. A simulcast site sending LSM rather than C4FM is not expected to
   come through a sign slicer cleanly.

   `exp p25site start MHz [auto|N|I [BW_Hz [dev_Hz [detect_bits]]]]` is a
   one-run bench setup for that frequency. N or I pins the polarity, and a
   pinned polarity is never switched, not on a quiet channel either. The
   filter is 12500 Hz, the deviation 1800 and the preamble detector off
   unless given (0, 8, 16, 24 or 32 bits). */
#include "../ls_experiments.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_attr.h"
#include "esp_err.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "ls_lora.h"
#include "p25_sitefind.h"

#define MHZ_LO 150.0
#define MHZ_HI 1100.0
#define BW_DEFAULT    12500u       /* the channel */
#define BW_MAX        1000000u
#define DEV_DEFAULT   1800u        /* the outer symbols' deviation */
#define DEV_MIN       600u
#define DEV_MAX       200000u
#define SYMBOL_RATE   4800u
#define PROFILE_PATH  "/sdcard/p25_profile.txt"
#define PROFILE_MAX   16384
#define CH_MAX        32
#define SLICE_MS      1500       /* a control channel sends a NID every 75 ms */
#define NOW_MS        100
#define LOCK_QUIET    3          /* visits without a NID before both ways again */
#define SINGLE_QUIET_US (30LL * 1000000)
#define ROWS          10
#define NACS          4

extern const ls_experiment_t exp_p25site;

/* ---------------------------------------------------------- settings -- */

enum { SRC_PROFILE, SRC_ONE };
static volatile int s_set_src = SRC_PROFILE;
static volatile uint32_t s_set_hz = 859487500u;
static volatile int s_set_dwell = 4;
/* A frequency given on the console is for that one run, and so are its
   overrides: the next start takes them or, if it has none, the defaults. */
static volatile uint32_t s_once_hz;
static int s_once_pol = -1;
static uint32_t s_once_bw = BW_DEFAULT, s_once_dev = DEV_DEFAULT;
static uint8_t s_once_detect;
/* What the running session uses. s_force is 0 or 1 for a pinned polarity. */
static int s_force = -1;
static uint32_t s_rx_bw = BW_DEFAULT, s_rx_dev = DEV_DEFAULT;
static uint8_t s_rx_detect;

/* ------------------------------------------------------------- state -- */

typedef struct {
    p25sf_chan_t ch;
    uint32_t syncs, nids, bad;
    uint32_t dist[P25SF_MAX_DIST + 1];
    uint32_t duid[7];
    uint16_t nac[NACS];            /* NACs heard here, most frequent first */
    uint32_t nac_n[NACS];
    int8_t   lock;                 /* 0 normal, 1 inverted, -1 neither yet */
    uint8_t  next_pol;
    uint8_t  quiet;
    float    rssi_sum, peak;
    uint32_t rssi_n;
    bool     measured;
    int64_t  last_us;
} chan_t;

typedef enum { ST_HOP, ST_LISTEN, ST_STOPPED } st_t;

static EXT_RAM_BSS_ATTR chan_t s_ch[CH_MAX];
static EXT_RAM_BSS_ATTR p25sf_chan_t s_parsed[CH_MAX];
static EXT_RAM_BSS_ATTR uint8_t s_buf[8];
static int s_n, s_cur, s_pol, s_pass, s_dwell_s;
static bool s_single, s_session;
static st_t s_st;
static int64_t s_dwell_end, s_slice_end, s_now_t, s_view_t, s_retry_at;
static uint32_t s_visit_nids, s_hop_errors;

/* ------------------------------------------------------------- view -- */

typedef struct {
    uint32_t hz;
    int16_t  want;
    char     label[P25SF_LABEL];
    uint32_t syncs, nids, bad;
    uint32_t dist[P25SF_MAX_DIST + 1];
    uint32_t duid[7];
    uint16_t nac[NACS];
    uint32_t nac_n[NACS];
    int8_t   lock;
    float    avg, peak;
    bool     measured;
    int64_t  last_us;
} row_t;

typedef struct {
    bool     started;
    bool     single;
    int      n, cur, pass, dwell_s, pol;
    uint8_t  st;
    bool     have_now;
    float    now;
    row_t    rows[ROWS];
    int      n_rows;
    row_t    focus;
    uint32_t hop_errors;
    int64_t  at_us;
} view_t;

static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static EXT_RAM_BSS_ATTR view_t s_view;
static EXT_RAM_BSS_ATTR view_t s_next;

static void row_of(row_t *r, const chan_t *c)
{
    memset(r, 0, sizeof(*r));
    r->hz = c->ch.hz;
    r->want = c->ch.nac;
    snprintf(r->label, sizeof(r->label), "%s", c->ch.label);
    r->syncs = c->syncs;
    r->nids = c->nids;
    r->bad = c->bad;
    memcpy(r->dist, c->dist, sizeof(r->dist));
    memcpy(r->duid, c->duid, sizeof(r->duid));
    memcpy(r->nac, c->nac, sizeof(r->nac));
    memcpy(r->nac_n, c->nac_n, sizeof(r->nac_n));
    r->lock = c->lock;
    r->avg = c->rssi_n ? c->rssi_sum / (float)c->rssi_n : 0.0f;
    r->peak = c->peak;
    r->measured = c->measured;
    r->last_us = c->last_us;
}

static void publish(int64_t now)
{
    view_t *v = &s_next;
    memset(v, 0, sizeof(*v));
    v->started = true;
    v->single = s_single;
    v->n = s_n;
    v->cur = s_cur;
    v->pass = s_pass;
    v->dwell_s = s_dwell_s;
    v->pol = s_pol;
    v->st = (uint8_t)s_st;
    v->hop_errors = s_hop_errors;
    v->at_us = now;
    /* Channels with NIDs first, most NIDs first; then the profile's order. */
    bool used[CH_MAX] = { 0 };
    for (int r = 0; r < ROWS; r++) {
        int best = -1;
        for (int i = 0; i < s_n; i++) {
            if (used[i]) continue;
            if (best < 0 || s_ch[i].nids > s_ch[best].nids) best = i;
        }
        if (best < 0) break;
        used[best] = true;
        row_of(&v->rows[v->n_rows++], &s_ch[best]);
    }
    if (s_cur < s_n) row_of(&v->focus, &s_ch[s_cur]);
    portENTER_CRITICAL(&s_lock);
    v->have_now = s_view.have_now;
    v->now = s_view.now;
    s_view = *v;
    portEXIT_CRITICAL(&s_lock);
    s_view_t = now;
}

static void set_now(float dbm)
{
    portENTER_CRITICAL(&s_lock);
    s_view.now = dbm;
    s_view.have_now = true;
    portEXIT_CRITICAL(&s_lock);
}

/* ------------------------------------------------------------- radio -- */

static esp_err_t open_session(uint32_t hz, int pol)
{
    if (s_session) { ls_lora_fsk_end(); s_session = false; }
    const uint32_t sync = pol ? P25SF_SYNC_INVERTED : P25SF_SYNC_NORMAL;
    /* 4800 symbols a second, sliced at zero. By default 1800 Hz is the outer
       symbols' deviation, 12.5 kHz the channel and the preamble detector off;
       a console run may override the three. */
    const ls_fsk_cfg_t cfg = {
        .freq_hz = hz,
        .bitrate = SYMBOL_RATE,
        .deviation_hz = s_rx_dev,
        .bandwidth_hz = ls_lora_fsk_bw_snap(s_rx_bw),
        .sync_word = sync << (32 - P25SF_SYNC_BITS),
        .sync_bits = P25SF_SYNC_BITS,
        .payload_bytes = P25SF_PAYLOAD_BYTES,
        .preamble_detect_bits = s_rx_detect,
    };
    const esp_err_t err = ls_lora_fsk_begin(&cfg);
    s_session = err == ESP_OK;
    s_pol = pol;
    return err;
}

static int pol_of(const chan_t *c)
{
    if (s_force >= 0) return s_force;
    return c->lock >= 0 ? c->lock : c->next_pol;
}

static void begin_listen(int64_t now)
{
    s_st = ST_LISTEN;
    s_dwell_end = now + (int64_t)s_dwell_s * 1000000;
    s_slice_end = now + SLICE_MS * 1000LL;
    s_now_t = now;
    s_visit_nids = 0;
}

static void next_channel(int64_t now)
{
    chan_t *c = &s_ch[s_cur];
    if (s_st == ST_LISTEN && c->lock >= 0 && !s_visit_nids && ++c->quiet >= LOCK_QUIET) {
        c->lock = -1;
        c->quiet = 0;
    }
    if (++s_cur >= s_n) { s_cur = 0; s_pass++; }
    s_st = ST_HOP;
    publish(now);
}

static void count_nac(chan_t *c, uint16_t nac)
{
    int i = 0;
    while (i < NACS && c->nac_n[i] && c->nac[i] != nac) i++;
    if (i == NACS) i = NACS - 1;          /* the rarest gives way */
    if (c->nac[i] != nac) { c->nac[i] = nac; c->nac_n[i] = 0; }
    c->nac_n[i]++;
    /* Keep most frequent first. */
    while (i > 0 && c->nac_n[i] > c->nac_n[i - 1]) {
        const uint16_t tn = c->nac[i]; c->nac[i] = c->nac[i - 1]; c->nac[i - 1] = tn;
        const uint32_t tc = c->nac_n[i]; c->nac_n[i] = c->nac_n[i - 1]; c->nac_n[i - 1] = tc;
        i--;
    }
}

static void on_packet(int64_t now, const uint8_t *data, float rssi)
{
    chan_t *c = &s_ch[s_cur];
    c->syncs++;
    p25sf_nid_t nid;
    if (!p25sf_nid_decode(p25sf_payload_signs(data, s_pol != 0), &nid)) { c->bad++; return; }
    c->nids++;
    c->dist[nid.dist]++;
    const int slot = p25sf_duid_slot(nid.duid);
    if (slot >= 0) c->duid[slot]++;
    count_nac(c, nid.nac);
    c->lock = (int8_t)s_pol;
    c->next_pol = (uint8_t)s_pol;
    c->quiet = 0;
    c->last_us = now;
    if (rssi > -199.0f) {
        if (!c->measured || rssi > c->peak) c->peak = rssi;
        c->measured = true;
    }
    s_visit_nids++;
}

/* -------------------------------------------------------- experiment -- */

/* The profile's channels into s_parsed. The SD card is a FAT volume, not
   flash, so the worker can read it. */
static int load_profile(char *why, size_t n)
{
    FILE *f = fopen(PROFILE_PATH, "rb");
    if (!f) { snprintf(why, n, "No profile at %s", PROFILE_PATH); return 0; }
    char *text = heap_caps_malloc(PROFILE_MAX, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!text) { fclose(f); snprintf(why, n, "No PSRAM to read the profile"); return 0; }
    const size_t len = fread(text, 1, PROFILE_MAX, f);
    fclose(f);
    const int got = p25sf_profile_parse(text, len, (uint32_t)(MHZ_LO * 1e6), (uint32_t)(MHZ_HI * 1e6),
                                        s_parsed, CH_MAX);
    heap_caps_free(text);
    if (!got) snprintf(why, n, "No control= line in %.0f-%.0f MHz in the profile", MHZ_LO, MHZ_HI);
    return got;
}

static bool p25_start(char *why, size_t n)
{
    const uint32_t once = s_once_hz;
    s_force = once ? s_once_pol : -1;
    s_rx_bw = once ? s_once_bw : BW_DEFAULT;
    s_rx_dev = once ? s_once_dev : DEV_DEFAULT;
    s_rx_detect = once ? s_once_detect : 0;
    s_once_hz = 0;
    s_once_pol = -1;
    s_once_bw = BW_DEFAULT;
    s_once_dev = DEV_DEFAULT;
    s_once_detect = 0;

    const uint32_t caps = ls_lora_caps();
    if (!(caps & LS_LORA_CAP_FSK)) { snprintf(why, n, "No FSK receiver on this chip"); return false; }
    int got;
    if (once || s_set_src == SRC_ONE) {
        memset(&s_parsed[0], 0, sizeof(s_parsed[0]));
        s_parsed[0].hz = once ? once : s_set_hz;
        s_parsed[0].nac = -1;
        got = 1;
        s_single = true;
    } else {
        got = load_profile(why, n);
        if (!got) return false;
        s_single = false;
    }
    memset(s_ch, 0, sizeof(s_ch));
    for (int i = 0; i < got; i++) {
        s_ch[i].ch = s_parsed[i];
        s_ch[i].lock = -1;
        s_ch[i].last_us = -1;
    }
    s_n = got;
    s_cur = 0;
    s_pass = 0;
    s_dwell_s = s_set_dwell;
    s_hop_errors = 0;
    s_retry_at = 0;
    s_session = false;
    const esp_err_t err = open_session(s_ch[0].ch.hz, pol_of(&s_ch[0]));
    if (err != ESP_OK) {
        snprintf(why, n, "%.4f MHz refused: %s", s_ch[0].ch.hz / 1e6, esp_err_to_name(err));
        return false;
    }
    const int64_t now = esp_timer_get_time();
    begin_listen(now);
    portENTER_CRITICAL(&s_lock);
    memset(&s_view, 0, sizeof(s_view));
    portEXIT_CRITICAL(&s_lock);
    publish(now);
    return true;
}

static void p25_stop(void)
{
    if (s_session) ls_lora_fsk_end();
    s_session = false;
    portENTER_CRITICAL(&s_lock);
    s_view.st = ST_STOPPED;
    s_view.have_now = false;
    portEXIT_CRITICAL(&s_lock);
}

static void poll_listen(int64_t now)
{
    chan_t *c = &s_ch[s_cur];
    float rssi = -200.0f;
    const int got = ls_lora_fsk_poll(s_buf, sizeof(s_buf), &rssi);
    /* Only a read of exactly the configured length is a packet: a short one
       is not parsed with what an earlier packet left in the buffer. */
    if (got == P25SF_PAYLOAD_BYTES) on_packet(now, s_buf, rssi);

    if (now - s_now_t >= NOW_MS * 1000) {
        s_now_t = now;
        float dbm;
        const esp_err_t err = ls_lora_rssi_inst(&dbm);
        if (err == ESP_OK) {
            set_now(dbm);
            c->rssi_sum += dbm;
            c->rssi_n++;
            if (!c->measured || dbm > c->peak) c->peak = dbm;
            c->measured = true;
        } else if (err == ESP_ERR_INVALID_STATE) {
            ls_lora_fsk_receive();
        }
    }

    if (!s_single && now >= s_dwell_end) { next_channel(now); return; }
    if (s_force < 0 && s_single && c->lock >= 0 && c->last_us >= 0 && now - c->last_us > SINGLE_QUIET_US) {
        c->lock = -1;
        s_slice_end = now;
    }
    if (s_force < 0 && c->lock < 0 && now >= s_slice_end) {
        /* Neither way has decoded yet: the other way round. A pinned
           polarity stays as it is. */
        c->next_pol = (uint8_t)!s_pol;
        if (open_session(c->ch.hz, c->next_pol) != ESP_OK) {
            s_hop_errors++;
            s_retry_at = now + 1000000;
            if (s_single) s_st = ST_HOP;
            else next_channel(now);
            return;
        }
        s_slice_end = now + SLICE_MS * 1000LL;
    }
}

static void p25_poll(void)
{
    const int64_t now = esp_timer_get_time();
    switch (s_st) {
    case ST_HOP: {
        if (now < s_retry_at) return;
        chan_t *c = &s_ch[s_cur];
        if (open_session(c->ch.hz, pol_of(c)) != ESP_OK) {
            /* A refused session is tried again in a second, not every poll. */
            s_hop_errors++;
            s_retry_at = now + 1000000;
            next_channel(now);
            return;
        }
        begin_listen(now);
        break;
    }
    case ST_LISTEN: poll_listen(now); break;
    case ST_STOPPED: return;
    }
    if (now - s_view_t >= 250000) publish(now);
}

/* ------------------------------------------------------------ readout -- */

static void nac_text(char *out, size_t n, int v)
{
    if (v < 0) snprintf(out, n, "-");
    else snprintf(out, n, "%03X", (unsigned)v);
}

static int p25_lines(char (*out)[LS_EXP_LINE], int max)
{
    view_t *v = heap_caps_malloc(sizeof(*v), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!v) {
        if (max > 0) snprintf(out[0], LS_EXP_LINE, "no memory for the readout");
        return max > 0 ? 1 : 0;
    }
    portENTER_CRITICAL(&s_lock);
    *v = s_view;
    portEXIT_CRITICAL(&s_lock);
    int n = 0;

    if (!v->started) {
        if (n < max) {
            if (s_set_src == SRC_ONE) snprintf(out[n++], LS_EXP_LINE, "ONE FREQ %.4f MHz", s_set_hz / 1e6);
            else snprintf(out[n++], LS_EXP_LINE, "PROFILE %s", PROFILE_PATH);
        }
        if (n < max) snprintf(out[n++], LS_EXP_LINE, "C4FM sign bits at 4800 bps, sync %06X/%06X",
                              (unsigned)P25SF_SYNC_NORMAL, (unsigned)P25SF_SYNC_INVERTED);
        if (n < max) snprintf(out[n++], LS_EXP_LINE, "NID taken within %d bits of a codeword", P25SF_MAX_DIST);
        heap_caps_free(v);
        return n;
    }

    if (n < max) {
        if (v->single) snprintf(out[n++], LS_EXP_LINE, "ONE FREQ  %.4f MHz", v->focus.hz / 1e6);
        else snprintf(out[n++], LS_EXP_LINE, "PROFILE %d ch  dwell %d s  pass %d", v->n, v->dwell_s, v->pass);
    }
    if (n < max) {
        char lvl[16] = "";
        if (v->have_now) snprintf(lvl, sizeof(lvl), "%.0f dBm", (double)v->now);
        if (v->st == ST_STOPPED) snprintf(out[n++], LS_EXP_LINE, "stopped");
        else {
            char line[96];
            snprintf(line, sizeof(line), "NOW %8.4f %s  %s%s%s", v->focus.hz / 1e6,
                     v->pol ? "inverted" : "normal", v->focus.label, v->focus.label[0] ? "  " : "", lvl);
            snprintf(out[n++], LS_EXP_LINE, "%.*s", LS_EXP_LINE - 1, line);
        }
    }
    if (n < max) snprintf(out[n++], LS_EXP_LINE, "MHZ      NAC WANT  NIDS  BAD POL RSSI LAST");
    for (int i = 0; i < v->n_rows && n < max; i++) {
        const row_t *r = &v->rows[i];
        char nac[6], want[6], pol[4], rssi[8], age[8];
        nac_text(nac, sizeof(nac), r->nids ? r->nac[0] : -1);
        nac_text(want, sizeof(want), r->want);
        snprintf(pol, sizeof(pol), "%s", r->lock < 0 ? "-" : r->lock ? "I" : "N");
        if (r->measured) snprintf(rssi, sizeof(rssi), "%.0f", (double)r->peak);
        else snprintf(rssi, sizeof(rssi), "-");
        if (r->last_us < 0) snprintf(age, sizeof(age), "-");
        else {
            const int64_t s = (v->at_us - r->last_us) / 1000000;
            if (s < 60) snprintf(age, sizeof(age), "%ds", (int)s);
            else if (s < 3600) snprintf(age, sizeof(age), "%dm", (int)(s / 60));
            else snprintf(age, sizeof(age), "%dh", (int)(s / 3600));
        }
        snprintf(out[n++], LS_EXP_LINE, "%8.4f %3s %4s %5lu %4lu %3s %4s %4s", r->hz / 1e6, nac, want,
                 (unsigned long)r->nids, (unsigned long)r->bad, pol, rssi, age);
    }

    /* The channel in hand, in detail. */
    const row_t *f = &v->focus;
    if (f->syncs && n < max)
        snprintf(out[n++], LS_EXP_LINE, "%.4f syncs %lu  dist 0/1/2 %lu/%lu/%lu", f->hz / 1e6,
                 (unsigned long)f->syncs, (unsigned long)f->dist[0], (unsigned long)f->dist[1],
                 (unsigned long)f->dist[2]);
    if (f->nids && n < max) {
        static const char *const NAMES[7] = { "HDU", "TDU", "LDU1", "TSBK", "LDU2", "PDU", "TDULC" };
        char line[LS_EXP_LINE] = "";
        for (int i = 0; i < 7; i++) {
            if (!f->duid[i]) continue;
            const size_t l = strlen(line);
            snprintf(line + l, sizeof(line) - l, "%s%s %lu", l ? " " : "", NAMES[i], (unsigned long)f->duid[i]);
        }
        snprintf(out[n++], LS_EXP_LINE, "%s", line);
    }
    if (f->nids && f->nac_n[1] && n < max) {
        char line[LS_EXP_LINE] = "NACS";
        for (int i = 0; i < NACS && f->nac_n[i]; i++) {
            const size_t l = strlen(line);
            snprintf(line + l, sizeof(line) - l, " %03X x%lu", (unsigned)f->nac[i], (unsigned long)f->nac_n[i]);
        }
        snprintf(out[n++], LS_EXP_LINE, "%s", line);
    }
    if (f->nids && f->want >= 0 && n < max && f->nac[0] != (uint16_t)f->want)
        snprintf(out[n++], LS_EXP_LINE, "profile expects NAC %03X here", (unsigned)f->want);
    if (v->hop_errors && n < max) snprintf(out[n++], LS_EXP_LINE, "sessions refused %lu", (unsigned long)v->hop_errors);
    heap_caps_free(v);
    return n;
}

/* ------------------------------------------------------------ console -- */

/* Digits only: no sign, space, hex or exponent, and nothing above `max`. */
static bool parse_uint(const char *text, uint32_t max, uint32_t *out)
{
    if (!*text) return false;
    uint64_t v = 0;
    for (const char *p = text; *p; p++) {
        if (*p < '0' || *p > '9') return false;
        v = v * 10 + (uint64_t)(*p - '0');
        if (v > max) return false;
    }
    *out = (uint32_t)v;
    return true;
}

/* A decimal number of MHz, to the Hz it rounds to, within MHZ_LO..MHZ_HI. */
static bool parse_mhz(const char *text, uint32_t *hz)
{
    int digits = 0, dots = 0;
    for (const char *p = text; *p; p++) {
        if (*p == '.') { if (++dots > 1) return false; }
        else if (*p >= '0' && *p <= '9') digits++;
        else return false;
    }
    if (!digits || strlen(text) > 24) return false;
    const double v = strtod(text, NULL);
    if (!(v >= 0.0 && v <= MHZ_HI + 1.0)) return false;
    const uint32_t r = (uint32_t)(v * 1e6 + 0.5);
    if (r < (uint32_t)(MHZ_LO * 1e6) || r > (uint32_t)(MHZ_HI * 1e6)) return false;
    *hz = r;
    return true;
}

static bool same_text(const char *a, const char *b)
{
    for (; *a && *b; a++, b++)
        if ((*a | 0x20) != (*b | 0x20)) return false;
    return !*a && !*b;
}

/* `exp p25site start 154.785` listens to one frequency for that run; no
   argument takes the profile's control channels, or OPTIONS' one. After the
   frequency come the run's polarity and receive settings, see the top of the
   file. Nothing is kept unless every argument is good. */
static bool p25_configure(int argc, char **argv, char *why, size_t n)
{
    uint32_t hz;
    s_once_hz = 0;
    s_once_pol = -1;
    s_once_bw = BW_DEFAULT;
    s_once_dev = DEV_DEFAULT;
    s_once_detect = 0;
    if (argc == 1 && !strcmp(argv[0], "profile")) { s_set_src = SRC_PROFILE; return true; }
    if (argc < 1 || argc > 5 || !parse_mhz(argv[0], &hz)) {
        snprintf(why, n, "%.0f-%.0f MHz [auto|N|I [BW_Hz [dev_Hz [detect 0|8|16|24|32]]]] | profile",
                 MHZ_LO, MHZ_HI);
        return false;
    }
    int pol = -1;
    if (argc >= 2) {
        if (same_text(argv[1], "auto")) pol = -1;
        else if (same_text(argv[1], "N")) pol = 0;
        else if (same_text(argv[1], "I")) pol = 1;
        else { snprintf(why, n, "polarity: auto, N or I"); return false; }
    }
    uint32_t bw = BW_DEFAULT, dev = DEV_DEFAULT, detect = 0;
    if (argc >= 3 && (!parse_uint(argv[2], BW_MAX, &bw) || !bw)) {
        snprintf(why, n, "BW_Hz: whole Hz, at most %u", BW_MAX);
        return false;
    }
    if (argc >= 4 && (!parse_uint(argv[3], DEV_MAX, &dev) || dev < DEV_MIN)) {
        snprintf(why, n, "dev_Hz: whole Hz, %u-%u", DEV_MIN, DEV_MAX);
        return false;
    }
    /* The rung the part will use, as its own begin checks it. */
    const uint32_t need = SYMBOL_RATE + 2 * dev, rung = ls_lora_fsk_bw_snap(bw);
    if (bw < need || rung < need) {
        snprintf(why, n, "filter %u Hz (rung %u) must hold %u + 2*%u Hz", (unsigned)bw, (unsigned)rung,
                 SYMBOL_RATE, (unsigned)dev);
        return false;
    }
    if (argc == 5 && (!parse_uint(argv[4], 32, &detect) || detect % 8)) {
        snprintf(why, n, "detect bits: 0 (off), 8, 16, 24 or 32");
        return false;
    }
    s_once_pol = pol;
    s_once_bw = bw;
    s_once_dev = dev;
    s_once_detect = (uint8_t)detect;
    s_once_hz = hz;
    return true;
}

/* ------------------------------------------------------------ OPTIONS -- */

static void restart(void) { if (ls_exp_running() == &exp_p25site) ls_exp_start(&exp_p25site); }

static const char *const SRC_NAMES[] = { "PROFILE", "ONE FREQ" };
static int o_src(const ls_opt_t *o) { (void)o; return s_set_src; }
static void o_set_src(const ls_opt_t *o, int v) { (void)o; s_set_src = v == SRC_ONE ? SRC_ONE : SRC_PROFILE; restart(); }

static double o_freq(const ls_opt_t *o) { (void)o; return s_set_hz / 1e6; }
static void o_set_freq(const ls_opt_t *o, double v)
{
    (void)o;
    s_set_hz = (uint32_t)(v * 1e6 + 0.5);
    s_set_src = SRC_ONE;
    restart();
}
static void o_show_freq(const ls_opt_t *o, char *out, size_t n) { (void)o; snprintf(out, n, "%.4f MHz", s_set_hz / 1e6); }

static double o_dwell(const ls_opt_t *o) { (void)o; return s_set_dwell; }
static void o_set_dwell(const ls_opt_t *o, double v) { (void)o; s_set_dwell = (int)(v + 0.5); restart(); }
static void o_show_dwell(const ls_opt_t *o, char *out, size_t n) { (void)o; snprintf(out, n, "%d s", s_set_dwell); }

static const ls_opt_t OPTS[] = {
    { .label = "CHANNELS", .kind = LS_OPT_CYCLE, .names = SRC_NAMES, .n = 2, .get = o_src, .set = o_set_src },
    { .label = "FREQUENCY", .kind = LS_OPT_NUMBER, .num = o_freq, .set_num = o_set_freq,
      .lo = MHZ_LO, .hi = MHZ_HI, .unit = "MHz, one channel", .show = o_show_freq },
    { .label = "DWELL", .kind = LS_OPT_NUMBER, .num = o_dwell, .set_num = o_set_dwell,
      .lo = 2, .hi = 60, .unit = "seconds per profile channel", .show = o_show_dwell },
};

const ls_experiment_t exp_p25site = {
    .id = "p25site",
    .name = "P25 SITE FINDER",
    .sub = "P25 NAC and frame type, VHF/UHF",
    .maturity = LS_EXP_TRYING,
    .needs = "a strong P25 Phase 1 C4FM signal",
    .start = p25_start,
    .stop = p25_stop,
    .poll = p25_poll,
    .lines = p25_lines,
    .configure = p25_configure,
    .opts = OPTS,
    .n_opts = (int)(sizeof(OPTS) / sizeof(OPTS[0])),
};
