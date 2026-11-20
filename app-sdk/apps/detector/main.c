/* DETECTOR: rules over what the board's watchers see, as an SD app.
 *
 * The built-in DETECTOR (firmware 2.21 and before) moved here with SDK 2.2.
 * The board hands over what SWEEP, Remote ID, the drone-link listener,
 * drone watch, REC, CATCH, the pager decoder and P25 see (lap_seen_*); the
 * app reads aircraft (lap_aircraft) and the mesh (lap_mesh_*) itself. The
 * rules are the same file, /sdcard/detector/rules.json, run by the same
 * engine (det_engine.c), and the alerts other boards share over the mesh
 * are the same #LSA/1 line.
 *
 * RUN starts a background service (LAP_CAP_SERVICE) that keeps watching
 * after the screen closes; without it DETECTOR watches only while open.
 * Opening another SD app stops the service, as for any app; RADIOS shows it.
 *
 * Sinks: a banner (lap_notify; an emergency is a lap_alert), a chirp,
 * vibration, speech, the mesh share and a JSONL log of metadata in
 * /sdcard/detector/events. Quiet hours hold back the noisy ones. Every
 * alert also goes on the board's timeline as LAP_EV_DETECT. */
#define LAP_TARGET_API_22     /* API 2.2, as the store has listed it: nothing newer is used */
#include "lakeshark_app.h"
#include "lakeshark_ui.h"
#include "det.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <math.h>

#define A(fg) LAP_ATTR((fg), LAP_BLACK)
#define RULES_PATH "/sdcard/detector/rules.json"
#define LOG_DIR "/sdcard/detector/events"
#define LOG_ROTATE (4u * 1024u * 1024u)
#define LOG_FILES 8
#define AIR_MAX 16
#define MSG_MAX 24
#define NODE_MAX 12
#define LOST_S (30u * 60u)
#define SETTINGS_MAGIC 0x31544544u    /* "DET1" */
#define NSRC 12                       /* DET_RID .. DET_MESH_ALERT */
#define HB 100                        /* activity buckets: 100 x 6 s = ten minutes */
#define HB_MS 6000u

LAP_ICON_PX("....................",
            "......bbbbbbbb......",
            "....bb........bb....",
            "...b...bbbbbb...b...",
            "..b...b......b...b..",
            "..b..b...99...b..b..",
            ".b...b..9999..b...b.",
            ".b...b..9999..b...b.",
            "..b..b...99...b..b..",
            "..b...b......b...b..",
            "...b...bbbbbb...b...",
            "....bb........bb....",
            "......bbbbbbbb......",
            "....................",
            "...3.3.3.3.3.3.3....",
            "....................",
            "..666666666666666...",
            "....................");

enum { VIEW_ALERTS = 0, VIEW_AIR, VIEW_RULES, VIEWS };
static const char *const VIEW_NAME[VIEWS] = {"ALERTS", "AIRCRAFT", "RULES"};

typedef struct {
    uint32_t magic;
    uint16_t sources;
    uint8_t sinks, view;
    det_prefs_t prefs;
    bool running;
    uint8_t log_index;
    uint32_t log_bytes;
} saved_t;

static struct {
    bool ready, running;
    det_t *d;
    char *rules_text;
    int rules_job;
    int view, pick, first;
    uint32_t seen_mask;
    lap_service_t svc;
    lap_seen_t seen;
    lap_time_t t;
    lap_gps_t gps;
    lap_aircraft_t air[AIR_MAX];
    int air_n;
    uint32_t air_at;
    /* mesh */
    lap_mesh_self_t me;
    lap_mesh_msg_t msgs[MSG_MAX];
    lap_mesh_node_t nodes[NODE_MAX];
    uint32_t mesh_seq, last_msg;
    bool mesh_primed;
    struct { char id[17]; bool lost; } known[NODE_MAX * 2];
    int known_n;
    bool peers_seeded;
    uint32_t peers_at;
    det_alert_gate_t gate;
    det_alert_seen_t heard;
    struct { char text[DET_ALERT_SUMMARY]; uint8_t source, flags; uint32_t extra; int64_t at; bool on; } held;
    uint32_t seq, boot;
    uint32_t shared, held_n, refused, heard_n, repeats, newer;
    uint8_t note;
    char line[160];
    /* SD log */
    char log[4096];
    size_t log_n;
    int log_job, dir_job[2];
    uint8_t log_index;
    uint32_t log_bytes;
    bool log_fresh, dirs_made, sd_failed;
    char log_path[64];
    /* words handed to the board: app memory, not the stack */
    char title[32], body[160], words[96];
    lap_decoded_t emit;
    saved_t saved;
    /* screen */
    lap_btn_t keys[5];
    char key_text[2][16];
    lap_chip_t chips[4];
    int kh, list_x, list_w, list_y, list_rows;
    uint32_t test_at;
    /* what each source has done, for its tile: real counts and times only */
    struct { uint32_t seen, hits, seen_ms, hit_ms; bool emerg; } src[NSRC];
    uint16_t rule_hits[DET_RULES];
    /* the last ten minutes in HB_MS buckets: sightings and matches */
    uint16_t hb_seen[HB], hb_hit[HB];
    uint8_t hb_flag[HB];                         /* 1 a match, 2 an emergency */
    uint32_t hb_at, run_bucket;                  /* the bucket RUN was last pressed in */
    float hist[HB];
} s;

static int64_t now_us(void) { return (int64_t)lap_millis() * 1000; }

/* ---- activity: what the tiles and the history strip show ------------------ */

/* The source's slot 0..11 for a single DET_* bit, or -1. */
static int src_index(unsigned bit) {
    if (!bit || (bit & (bit - 1))) return -1;
    int i = 0;
    while (bit > 1) { bit >>= 1; i++; }
    return i < NSRC ? i : -1;
}
/* The bucket now, emptying the ones the clock has passed. */
static int hb_now(void) {
    const uint32_t at = lap_millis() / HB_MS;
    if (at != s.hb_at) {
        uint32_t gone = at - s.hb_at;
        if (gone > HB) gone = HB;
        for (uint32_t k = 1; k <= gone; k++) {
            const unsigned i = (s.hb_at + k) % HB;
            s.hb_seen[i] = s.hb_hit[i] = 0;
            s.hb_flag[i] = 0;
        }
        s.hb_at = at;
    }
    return (int)(at % HB);
}
/* n more sightings; fresh when one was heard just now (the tile's lamp). */
static void note_seen(unsigned bit, unsigned n, bool fresh) {
    const int i = src_index(bit);
    if (i < 0 || !n) return;
    s.src[i].seen += n;
    if (fresh) s.src[i].seen_ms = lap_millis() | 1;
    const int b = hb_now();
    s.hb_seen[b] = (uint16_t)(s.hb_seen[b] + n > 60000 ? 60000 : s.hb_seen[b] + n);
}
static void note_hit(const det_event_t *e) {
    if (e->test) return;
    const bool emerg = det_emergency(e);
    const int i = src_index(e->source);
    if (i >= 0) {
        s.src[i].hits++;
        s.src[i].hit_ms = lap_millis() | 1;
        s.src[i].emerg = emerg;
    }
    for (unsigned r = 0; r < DET_RULES; r++)
        if ((e->rules >> r) & 1) s.rule_hits[r]++;
    const int b = hb_now();
    if (s.hb_hit[b] < 60000) s.hb_hit[b]++;
    s.hb_flag[b] |= emerg ? 2 : 1;
}

/* ---- settings ----------------------------------------------------------- */

static void save(void) {
    s.saved = (saved_t){.magic = SETTINGS_MAGIC, .sources = s.d->sources, .sinks = s.d->sinks, .view = (uint8_t)s.view,
                        .prefs = s.d->prefs, .running = s.running, .log_index = s.log_index, .log_bytes = s.log_bytes};
    const int fd = lap_file_open("settings", "w");
    if (fd < 0) return;
    lap_file_write(fd, &s.saved, sizeof(s.saved));
    lap_file_close(fd);
}
static void load(void) {
    const int fd = lap_file_open("settings", "r");
    if (fd < 0) return;
    const int n = lap_file_read(fd, &s.saved, sizeof(s.saved));
    lap_file_close(fd);
    if (n != (int)sizeof(s.saved) || s.saved.magic != SETTINGS_MAGIC) return;
    s.d->sources = s.saved.sources & DET_SOURCES;
    s.d->sinks = s.saved.sinks & 127;
    det_prefs_set(s.d, &s.saved.prefs);
    s.view = s.saved.view < VIEWS ? s.saved.view : 0;
    s.running = s.saved.running;
    s.log_index = s.saved.log_index % LOG_FILES;
    s.log_bytes = s.saved.log_bytes;
}

/* ---- the rules file ----------------------------------------------------- */

static void reload(void) {
    if (s.rules_job >= 0 || !s.rules_text) return;
    s.rules_job = lap_sd_read(RULES_PATH, 0, s.rules_text, DET_RULES_BYTES + 1);
    if (s.rules_job < 0) snprintf(s.d->error, sizeof(s.d->error), "SD busy: rules not read");
}
static void rules_poll(void) {
    if (s.rules_job < 0) return;
    const int r = lap_sd_poll(s.rules_job);
    if (r == LAP_SD_PENDING) return;
    s.rules_job = -1;
    if (r == LAP_SD_ENOENT) snprintf(s.d->error, sizeof(s.d->error), "No SD rules file: using built-in rules");
    else if (r < 0) snprintf(s.d->error, sizeof(s.d->error), "Rules unreadable (%d): previous rules kept", r);
    else if (det_parse(s.d, s.rules_text, (size_t)r))   /* a file past the limit is refused there */
        memset(s.rule_hits, 0, sizeof(s.rule_hits));    /* new rules, new places in the list */
}

/* ---- the SD log ----------------------------------------------------------- */

static void log_poll(void) {
    for (int i = 0; i < 2; i++)
        if (s.dir_job[i] >= 0 && lap_sd_poll(s.dir_job[i]) != LAP_SD_PENDING) s.dir_job[i] = -1;
    if (s.log_job >= 0) {
        const int r = lap_sd_poll(s.log_job);
        if (r == LAP_SD_PENDING) return;
        s.log_job = -1;
        if (r < 0) { s.sd_failed = true; snprintf(s.d->error, sizeof(s.d->error), "SD log write failed (%d)", r); }
    }
    if (!s.log_n || s.dir_job[0] >= 0 || s.dir_job[1] >= 0) return;
    if (s.log_bytes + s.log_n > LOG_ROTATE) { s.log_index = (uint8_t)((s.log_index + 1) % LOG_FILES); s.log_bytes = 0; s.log_fresh = true; }
    snprintf(s.log_path, sizeof(s.log_path), LOG_DIR "/detector-%u.jsonl", (unsigned)s.log_index);
    /* A new file in the ring starts over the oldest one's bytes. */
    s.log_job = lap_sd_write(s.log_path, s.log_fresh ? 0 : LAP_SD_APPEND, s.log, (uint32_t)s.log_n);
    if (s.log_job >= 0) { s.log_bytes += (uint32_t)s.log_n; s.log_n = 0; s.log_fresh = false; save(); }
}
static bool log_event(const det_event_t *e) {
    if (!s.dirs_made) {
        s.dir_job[0] = lap_sd_mkdir("/sdcard/detector");
        s.dir_job[1] = lap_sd_mkdir(LOG_DIR);
        s.dirs_made = true;
    }
    char *at = s.log + s.log_n;
    const int n = det_log_line(s.d, e, ((uint64_t)s.boot << 32) | s.boot, at, sizeof(s.log) - s.log_n);
    if (n < 0) return false;
    s.log_n += (size_t)n;
    return true;
}

/* ---- the mesh ------------------------------------------------------------- */

static unsigned bit_index(unsigned bits) { unsigned i = 0; while (bits > 1) { bits >>= 1; i++; } return i; }
static unsigned send_alert(const char *summary, uint8_t source, uint8_t flags) {
    det_alert_t m;
    memset(&m, 0, sizeof(m));
    memcpy(m.origin, s.me.id, 8);
    m.seq = (s.boot & 0xFFFF0000u) | (++s.seq & 0xFFFFu);
    m.source = source;
    m.flags = flags;
    snprintf(m.summary, sizeof(m.summary), "%s", summary);
    if (det_alert_encode(&m, s.line, sizeof(s.line)) < 0) return DET_NOTE_FAILED;
    if (!lap_mesh_send(s.d->prefs.mesh_chan, s.line)) return DET_NOTE_FAILED;
    s.shared++;
    return DET_NOTE_SENT;
}
static unsigned share(const det_event_t *e, int64_t now) {
    unsigned note;
    if (e->source == DET_MESH_ALERT) note = DET_NOTE_RELAYED;
    else if (!lap_mesh_self(&s.me) || !s.me.running || !s.me.tx_armed) note = DET_NOTE_TX_OFF;   /* the operator arms TRANSMIT */
    else if (!lap_mesh_channel_private(s.d->prefs.mesh_chan, NULL, 0)) note = DET_NOTE_NO_CHANNEL;
    else {
        char sum[DET_ALERT_SUMMARY];
        det_describe(e, sum, sizeof(sum));
        const uint8_t flags = (e->test ? DET_ALERT_F_TEST : 0) | (det_emergency(e) ? DET_ALERT_F_EMERG : 0);
        const int g = det_alert_gate(&s.gate, det_alert_hash(sum), now);
        if (g == DET_ALERT_REPEAT) note = DET_NOTE_REPEAT;
        else if (g == DET_ALERT_WAIT) {
            /* The newest waits for the minute to end, standing for the rest. */
            s.held.extra += s.held.on;
            snprintf(s.held.text, sizeof(s.held.text), "%s", sum);
            s.held.source = (uint8_t)bit_index(e->test ? 1 : e->source);
            s.held.flags = flags; s.held.at = now; s.held.on = true;
            note = DET_NOTE_HELD; s.held_n++;
        } else note = send_alert(sum, (uint8_t)bit_index(e->test ? 1 : e->source), flags);
    }
    if (note != DET_NOTE_SENT && note != DET_NOTE_HELD && note != DET_NOTE_RELAYED) s.refused++;
    s.note = (uint8_t)note;
    return note;
}
static void held_poll(int64_t now) {
    if (s.held.on && now - s.held.at > DET_ALERT_HOLD_US) { s.held.on = false; s.held.extra = 0; }   /* stale: dropped */
    if (!s.held.on || now < s.gate.next_at) return;
    if (!lap_mesh_self(&s.me) || !s.me.tx_armed || !lap_mesh_channel_private(s.d->prefs.mesh_chan, NULL, 0)) return;
    char sum[DET_ALERT_SUMMARY + 8];
    if (s.held.extra) snprintf(sum, sizeof(sum), "%.48s +%u", s.held.text, (unsigned)s.held.extra);
    else snprintf(sum, sizeof(sum), "%s", s.held.text);
    s.held.on = false; s.held.extra = 0;
    if (det_alert_gate(&s.gate, det_alert_hash(sum), now) == DET_ALERT_GO) s.note = (uint8_t)send_alert(sum, s.held.source, s.held.flags);
}
/* Range (0.1 km) and bearing from our fix to where the sender is. */
static bool locate(double lat, double lon, uint16_t *dist, uint16_t *bearing) {
    if (!lap_gps(&s.gps) || !s.gps.fix) return false;
    const float r = 0.0174532925f;
    const float p1 = (float)s.gps.lat * r, p2 = (float)lat * r, dl = (float)(lon - s.gps.lon) * r;
    const float a = sinf((p2 - p1) / 2) * sinf((p2 - p1) / 2) + cosf(p1) * cosf(p2) * sinf(dl / 2) * sinf(dl / 2);
    const float km = 12742.0f * asinf(sqrtf(a > 1 ? 1 : a));
    float brg = atan2f(sinf(dl) * cosf(p2), cosf(p1) * sinf(p2) - sinf(p1) * cosf(p2) * cosf(dl)) / r;
    if (brg < 0) brg += 360;
    *dist = (uint16_t)(km > 6500 ? 65000 : km * 10 + 0.5f);
    *bearing = (uint16_t)((int)(brg + 0.5f) % 360);
    return true;
}
static void mesh_poll(int64_t now) {
    const bool alerts = det_wants(s.d, DET_MESH_ALERT) || (s.d->sources & DET_MESH_ALERT);
    if (alerts) {
        const uint32_t seq = lap_mesh_seq();
        if (seq != s.mesh_seq || !s.mesh_primed) {
            s.mesh_seq = seq;
            const int n = lap_mesh_messages(s.msgs, MSG_MAX);
            uint32_t newest = s.last_msg;
            lap_mesh_self(&s.me);
            for (int i = 0; i < n; i++) {
                const lap_mesh_msg_t *m = &s.msgs[i];
                if ((int32_t)(m->id - newest) > 0) newest = m->id;
                /* The first look only learns where the log ends. Private slots only. */
                if (!s.mesh_primed || m->mine || m->direct || !m->channel || (int32_t)(m->id - s.last_msg) <= 0) continue;
                const char *colon = strstr(m->text, ": ");
                if (!colon) continue;
                det_alert_t a;
                const int r = det_alert_decode(colon + 2, &a);
                if (r == DET_ALERT_NEWER) s.newer++;
                if (r != DET_ALERT_OK) continue;
                if (s.me.id[0] && !strncmp(s.me.id, a.origin, 8)) continue;   /* our own, relayed back */
                uint32_t o = (uint32_t)strtoul(a.origin, NULL, 16);
                if (det_alert_seen(&s.heard, o, a.seq)) { s.repeats++; continue; }
                s.heard_n++;
                note_seen(DET_MESH_ALERT, 1, true);
                det_signal_t g;
                memset(&g, 0, sizeof(g));
                g.source = DET_MESH_ALERT; g.kind = a.source; g.flag = a.flags; g.a = a.seq;
                g.distance_x10_km = DET_NO_DISTANCE;
                memcpy(g.who, a.origin, 8);
                size_t k = (size_t)(colon - m->text);
                memcpy(g.name, m->text, k > 16 ? 16 : k);
                snprintf(g.text, sizeof(g.text), "%s", a.summary);
                bool known = false;
                if (a.has_pos) known = locate(a.lat_e4 / 1e4, a.lon_e4 / 1e4, &g.distance_x10_km, &g.bearing);
                if (!known) {
                    const int nn = lap_mesh_nodes(s.nodes, NODE_MAX);
                    for (int j = 0; j < nn && !known; j++)
                        if (!strncmp(s.nodes[j].id, a.origin, 8) && s.nodes[j].has_loc)
                            known = locate(s.nodes[j].lat, s.nodes[j].lon, &g.distance_x10_km, &g.bearing);
                }
                det_signal(s.d, &g, now);
            }
            s.last_msg = newest;
            s.mesh_primed = true;
        }
    }
    /* New and lost nodes, every 10 s. The first look seeds the list quietly. */
    if (!det_wants(s.d, DET_MESH_PEER) || lap_millis() - s.peers_at < 10000) return;
    s.peers_at = lap_millis();
    const int n = lap_mesh_nodes(s.nodes, NODE_MAX);
    for (int i = 0; i < n; i++) {
        const lap_mesh_node_t *p = &s.nodes[i];
        if (p->heard_s < 10) note_seen(DET_MESH_PEER, 1, true);   /* heard since the last look */
        int k = 0;
        while (k < s.known_n && strcmp(s.known[k].id, p->id)) k++;
        const bool lost = p->heard_s > LOST_S;
        det_signal_t g;
        memset(&g, 0, sizeof(g));
        g.source = DET_MESH_PEER;
        g.rssi = (int8_t)(p->rssi < -127 ? -127 : p->rssi > 0 ? 0 : p->rssi);
        memcpy(g.who, p->id, 16);
        snprintf(g.name, sizeof(g.name), "%s", p->name);
        if (k == s.known_n) {
            if (s.known_n == (int)(sizeof(s.known) / sizeof(*s.known))) continue;
            snprintf(s.known[k].id, sizeof(s.known[k].id), "%s", p->id);
            s.known[s.known_n++].lost = lost;
            if (s.peers_seeded && !lost) det_signal(s.d, &g, now);
        } else if (lost != s.known[k].lost) {
            s.known[k].lost = lost;
            g.kind = lost;
            det_signal(s.d, &g, now);
        }
    }
    s.peers_seeded = true;
}

/* ---- sinks ---------------------------------------------------------------- */

static const char *compass(unsigned b) { static const char *const p[] = {"N", "NE", "E", "SE", "S", "SW", "W", "NW"}; return p[(b + 22) / 45 % 8]; }
static uint8_t deliver(det_t *d, det_event_t *e, uint8_t sinks) {
    uint8_t ok = 0;
    note_hit(e);
    if (sinks & DET_BANNER) {
        snprintf(s.title, sizeof(s.title), "%s", e->source == DET_MESH_ALERT ? "MESH" : det_label(e));
        char line[96];
        det_describe(e, line, sizeof(line));
        if (e->source == DET_MESH_ALERT) {
            if (e->distance_x10_km != DET_NO_DISTANCE)
                snprintf(s.body, sizeof(s.body), "%.16s: %.80s / %u.%u km %s", e->name[0] ? e->name : e->origin, line,
                         e->distance_x10_km / 10, e->distance_x10_km % 10, compass(e->bearing));
            else snprintf(s.body, sizeof(s.body), "%.16s: %.80s", e->name[0] ? e->name : e->origin, line);
        } else if (e->coalesced) snprintf(s.body, sizeof(s.body), "%.120s +%u grouped", line, (unsigned)e->coalesced);
        else snprintf(s.body, sizeof(s.body), "%s", e->test ? "sink test" : line);
        const bool sent = det_emergency(e) ? lap_alert(s.title, s.body, LAP_ALERT_ALARM) || lap_notify(s.title, s.body)
                                           : lap_notify(s.title, s.body);
        if (sent) ok |= DET_BANNER;
    }
    if (sinks & DET_TONE) { lap_sound(LAP_SOUND_CHIRP); ok |= DET_TONE; }
    if ((sinks & DET_VIBRATE) && lap_vibrate(400, 80)) ok |= DET_VIBRATE;
    if (sinks & DET_TTS) { det_speech(e, s.words, sizeof(s.words)); if (lap_say(s.words)) ok |= DET_TTS; }
    if (sinks & DET_MESH) {
        const unsigned note = share(e, e->at);
        e->mesh_note = (uint8_t)note;
        if (note == DET_NOTE_SENT || note == DET_NOTE_HELD) ok |= DET_MESH;
    }
    if (sinks & DET_SD) {
        e->delivered = ok | DET_SD;            /* the record says what the others did */
        if (log_event(e)) ok |= DET_SD;
    }
    /* An alert, not a test: onto the board's timeline (TIME MACHINE). */
    if (!e->test) {
        memset(&s.emit, 0, sizeof(s.emit));
        s.emit.kind = LAP_EV_DETECT;
        s.emit.address = e->icao;
        snprintf(s.emit.from, sizeof(s.emit.from), "%s", det_source_name(e->source));
        snprintf(s.emit.text, sizeof(s.emit.text), "%s", e->reason);
        lap_emit(&s.emit);
    }
    (void)d;
    return ok;
}

/* ---- watching ------------------------------------------------------------- */

static void watch_mask(void) {
    const uint32_t m = s.running ? det_seen_mask(s.d) : 0;
    if (m != s.seen_mask) { s.seen_mask = m; lap_seen_watch(m); }
}
static void take_seen(int64_t now) {
    for (int budget = 0; budget < 48 && lap_seen_next(&s.seen) == 1; budget++) {
        const lap_seen_t *x = &s.seen;
        const int64_t at = now - (int64_t)x->age_ms * 1000;
        note_seen(x->source, 1, x->age_ms < 2000);
        if (x->source & (LAP_SEEN_REC | LAP_SEEN_CATCH | LAP_SEEN_PAGER | LAP_SEEN_P25)) {
            det_signal_t g;
            memset(&g, 0, sizeof(g));
            g.source = x->source;
            g.rssi = x->rssi;
            g.a = x->a;
            g.b = x->b;
            g.flag = (x->flags & (LAP_SEEN_F_NEW | LAP_SEEN_F_EMERGENCY)) ? 1 : 0;
            g.distance_x10_km = DET_NO_DISTANCE;
            snprintf(g.text, sizeof(g.text), "%s", x->text);
            det_signal(s.d, &g, at);
            continue;
        }
        det_obs_t o = {.entity = x->entity, .source = x->source, .category = x->category, .section = x->section,
                       .rssi = x->rssi, .validated = x->flags & LAP_SEEN_F_VALIDATED, .flood = x->flags & LAP_SEEN_F_FLOOD,
                       .approaching = x->flags & LAP_SEEN_F_APPROACHING, .band = x->band,
                       .rate_guess = (uint16_t)(x->source == LAP_SEEN_LORA ? x->b : 0), .hits_per_min = x->hits_per_min};
        if (x->source == LAP_SEEN_DRONE) det_drone_watch(s.d, &o, at);
        else det_observe(s.d, &o, at);
    }
    s.d->dropped = lap_seen_dropped();
}
static void air_poll(int64_t now) {
    if (lap_millis() - s.air_at < 1000) return;
    s.air_at = lap_millis();
    s.air_n = lap_aircraft(s.air, AIR_MAX);
    if (s.air_n < 0) s.air_n = 0;
    if (!det_wants(s.d, DET_ADSB)) return;
    for (int i = 0; i < s.air_n; i++) {
        const lap_aircraft_t *p = &s.air[i];
        if (p->age_ms > 60000) continue;           /* stale aircraft never alert */
        note_seen(DET_ADSB, 1, p->age_ms < 1500);   /* the engine counts each look; fresh when just heard */
        det_air_t a = {.icao = p->icao, .emergency = p->emergency, .category = (int8_t)det_air_category_bit(p->category),
                       .squawk = p->squawk, .altitude_ft = p->has_alt ? p->alt_ft : INT32_MIN,
                       .distance_x10_km = p->range_km >= 0 ? (uint16_t)(p->range_km >= 6553 ? 65534 : p->range_km * 10 + 0.5f) : DET_NO_DISTANCE};
        memcpy(a.callsign, p->callsign, sizeof(a.callsign));
        det_air(s.d, &a, now);
    }
}
/* Everything a tick does; the screen does it too when there is no service. */
static void pump(void) {
    const int64_t now = now_us();
    lap_time(&s.t);
    s.d->utc = s.t.synced ? s.t.unix : 0;
    rules_poll();
    log_poll();
    if (!s.running) return;
    watch_mask();
    take_seen(now);
    air_poll(now);
    mesh_poll(now);
    held_poll(now);
}
static void tick(void) { pump(); }
static void stopped(void) { s.ready = false; }
static void run(bool on) {
    s.running = on;
    if (on) {
        s.svc = (lap_service_t){tick, stopped, 200};
        s.run_bucket = lap_millis() / HB_MS;
        if (lap_service_start(&s.svc) != LAP_SVC_OK) snprintf(s.d->error, sizeof(s.d->error), "Background not allowed: watching while open");
        s.mesh_primed = false;
        s.peers_seeded = false;
        s.known_n = 0;
    } else {
        if (lap_service_running()) lap_service_stop();
        lap_seen_watch(0);
        s.seen_mask = 0;
    }
    save();
}

/* ---- OPTIONS -------------------------------------------------------------- */

enum { O_SRC = 100, O_SINK = 200, O_QUIET = 300, O_FROM, O_UNTIL, O_UTC, O_QMESH, O_CHAN = 400, O_RX = 410, O_RELOAD = 500, O_CLEAR };
static const char *const CHANS[] = {"1", "2", "3"};
static const lap_opt_t SOURCES[] = {
    {.label = "BLE REMOTE ID", .kind = LAP_OPT_TOGGLE, .id = O_SRC + 0},
    {.label = "SWEEP BLE", .kind = LAP_OPT_TOGGLE, .id = O_SRC + 1},
    {.label = "SWEEP WI-FI", .kind = LAP_OPT_TOGGLE, .id = O_SRC + 2},
    {.label = "LORA DRONE LINK", .kind = LAP_OPT_TOGGLE, .id = O_SRC + 3},
    {.label = "AIRCRAFT (ADS-B)", .kind = LAP_OPT_TOGGLE, .id = O_SRC + 4},
    {.label = "DRONE WATCH", .kind = LAP_OPT_TOGGLE, .id = O_SRC + 5},
    {.label = "REC WATCH", .kind = LAP_OPT_TOGGLE, .id = O_SRC + 6},
    {.label = "REC CATCH", .kind = LAP_OPT_TOGGLE, .id = O_SRC + 7},
    {.label = "PAGER", .kind = LAP_OPT_TOGGLE, .id = O_SRC + 8},
    {.label = "P25", .kind = LAP_OPT_TOGGLE, .id = O_SRC + 9},
    {.label = "MESH NODES", .kind = LAP_OPT_TOGGLE, .id = O_SRC + 10},
    {.label = "MESH ALERTS", .kind = LAP_OPT_TOGGLE, .id = O_SRC + 11},
};
static const lap_opt_t SINKS[] = {
    {.label = "BANNER", .kind = LAP_OPT_TOGGLE, .id = O_SINK + 0},
    {.label = "VIBRATE", .kind = LAP_OPT_TOGGLE, .id = O_SINK + 1},
    {.label = "SD LOG (NO ADDRESSES)", .kind = LAP_OPT_TOGGLE, .id = O_SINK + 2},
    {.label = "SPEECH", .kind = LAP_OPT_TOGGLE, .id = O_SINK + 3},
    {.label = "CHIRP", .kind = LAP_OPT_TOGGLE, .id = O_SINK + 4},
    {.label = "SHARE ON MESH", .kind = LAP_OPT_TOGGLE, .id = O_SINK + 6},
};
static const lap_opt_t QUIET[] = {
    {.label = "QUIET HOURS", .kind = LAP_OPT_TOGGLE, .id = O_QUIET},
    {.label = "QUIET FROM", .kind = LAP_OPT_LEVEL, .id = O_FROM, .lo = 0, .hi = 23, .step = 1, .unit = "local hour"},
    {.label = "QUIET UNTIL", .kind = LAP_OPT_LEVEL, .id = O_UNTIL, .lo = 0, .hi = 23, .step = 1, .unit = "local hour"},
    {.label = "UTC OFFSET", .kind = LAP_OPT_LEVEL, .id = O_UTC, .lo = -12, .hi = 14, .step = 1, .unit = "hours"},
    {.label = "SHARE WHILE QUIET", .kind = LAP_OPT_TOGGLE, .id = O_QMESH},
};
static const lap_opt_t MESH[] = {
    {.label = "CHANNEL SLOT", .kind = LAP_OPT_CYCLE, .id = O_CHAN, .names = CHANS, .n = 3},
    {.label = "RECEIVED: BANNER", .kind = LAP_OPT_TOGGLE, .id = O_RX + 0},
    {.label = "RECEIVED: VIBRATE", .kind = LAP_OPT_TOGGLE, .id = O_RX + 1},
    {.label = "RECEIVED: SPEECH", .kind = LAP_OPT_TOGGLE, .id = O_RX + 3},
    {.label = "RECEIVED: CHIRP", .kind = LAP_OPT_TOGGLE, .id = O_RX + 4},
};
static double opt_get(int id);
static void opt_set(int id, double v);
static const lap_opt_list_t SOURCES_LIST = {"SOURCES", SOURCES, 12, NULL, NULL};
static const lap_opt_list_t SINKS_LIST = {"SINKS", SINKS, 6, NULL, NULL};
static const lap_opt_list_t QUIET_LIST = {"QUIET", QUIET, 5, NULL, NULL};
static const lap_opt_list_t MESH_LIST = {"MESH", MESH, 5, NULL, NULL};
static const lap_opt_t ROOT[] = {
    {.label = "SOURCES", .kind = LAP_OPT_MENU, .sub = &SOURCES_LIST},
    {.label = "SINKS", .kind = LAP_OPT_MENU, .sub = &SINKS_LIST},
    {.label = "QUIET HOURS", .kind = LAP_OPT_MENU, .sub = &QUIET_LIST},
    {.label = "MESH SHARE", .kind = LAP_OPT_MENU, .sub = &MESH_LIST},
    {.label = "RELOAD RULES", .kind = LAP_OPT_ACTION, .id = O_RELOAD},
    {.label = "CLEAR ALERTS", .kind = LAP_OPT_ACTION, .id = O_CLEAR},
};
static const lap_opt_list_t OPTIONS = {"DETECTOR", ROOT, 6, opt_get, opt_set};
static double opt_get(int id) {
    const det_prefs_t *p = &s.d->prefs;
    if (id >= O_SRC && id < O_SRC + 12) return (s.d->sources >> (id - O_SRC)) & 1;
    if (id >= O_SINK && id < O_SINK + 7) return (s.d->sinks >> (id - O_SINK)) & 1;
    if (id >= O_RX && id < O_RX + 5) return (p->mesh_rx_sinks >> (id - O_RX)) & 1;
    switch (id) {
    case O_QUIET: return p->quiet_on;
    case O_FROM: return p->quiet_start;
    case O_UNTIL: return p->quiet_end;
    case O_UTC: return p->utc_offset;
    case O_QMESH: return p->quiet_mesh;
    case O_CHAN: return p->mesh_chan - 1;
    default: return 0;
    }
}
static void opt_set(int id, double v) {
    det_prefs_t p = s.d->prefs;
    const bool on = v != 0;
    if (id >= O_SRC && id < O_SRC + 12) { const uint16_t b = (uint16_t)(1u << (id - O_SRC)); s.d->sources = on ? s.d->sources | b : s.d->sources & ~b; }
    else if (id >= O_SINK && id < O_SINK + 7) { const uint8_t b = (uint8_t)(1u << (id - O_SINK)); s.d->sinks = on ? s.d->sinks | b : s.d->sinks & ~b; }
    else if (id >= O_RX && id < O_RX + 5) { const uint8_t b = (uint8_t)(1u << (id - O_RX)); p.mesh_rx_sinks = on ? p.mesh_rx_sinks | b : p.mesh_rx_sinks & ~b; }
    else switch (id) {
    case O_QUIET: p.quiet_on = on; break;
    case O_FROM: p.quiet_start = (int8_t)v; break;
    case O_UNTIL: p.quiet_end = (int8_t)v; break;
    case O_UTC: p.utc_offset = (int8_t)v; break;
    case O_QMESH: p.quiet_mesh = on; break;
    case O_CHAN: p.mesh_chan = (uint8_t)((int)v + 1); break;
    case O_RELOAD: reload(); return;
    case O_CLEAR:
        s.d->history_n = 0; s.pick = s.first = 0;
        for (int i = 0; i < NSRC; i++) { s.src[i].hits = s.src[i].hit_ms = 0; s.src[i].emerg = false; }
        memset(s.rule_hits, 0, sizeof(s.rule_hits));
        memset(s.hb_hit, 0, sizeof(s.hb_hit));
        memset(s.hb_flag, 0, sizeof(s.hb_flag));
        return;
    default: return;
    }
    det_prefs_set(s.d, &p);
    save();
}

/* ---- screen --------------------------------------------------------------- */

/* Each source's tile: its name, a short tag for the RULES list, the hue of
   its family and a 5x3 picture whose 'o' cells are its lamp. Drones are
   magenta, the BLE/Wi-Fi sweep cyan, aircraft blue, REC green, the pager
   and P25 yellow, the mesh white; red is kept for emergencies. */
typedef struct { const char *name, *tag; uint8_t hue; const char *art[3]; } src_look_t;
static const src_look_t LOOK[NSRC] = {
    {"REMOTE ID", "RID", LAP_MAGENTA, {"x-.-x", " |o| ", "x-'-x"}},
    {"SWEEP BLE", "BLE", LAP_CYAN, {" .-. ", "(|o|)", " '-' "}},
    {"SWEEP WIFI", "WIFI", LAP_CYAN, {".---.", " .-. ", "  o  "}},
    {"DRONE LINK", "LORA", LAP_MAGENTA, {"/\\/\\/", "\\/\\/\\", " -o- "}},
    {"AIRCRAFT", "ADSB", LAP_BLUE, {"  |  ", "--o--", " -'- "}},
    {"DRONE WATCH", "DRNW", LAP_MAGENTA, {".-=-.", "(-o-)", "'-=-'"}},
    {"REC WATCH", "REC", LAP_GREEN, {".---.", "|o-o|", "'---'"}},
    {"REC CATCH", "CTCH", LAP_GREEN, {"  |  ", "  | .", "  '-o"}},
    {"PAGER", "PAGE", LAP_YELLOW, {".---.", "|===|", "'-o-'"}},
    {"P25", "P25", LAP_YELLOW, {" |   ", "[=o=]", "[###]"}},
    {"MESH NODES", "NODE", LAP_WHITE, {"o---o", " \\ / ", "  o  "}},
    {"MESH ALERTS", "ALRT", LAP_WHITE, {"  .  ", " /!\\ ", "/_o_\\"}},
};

static uint8_t event_hue(const det_event_t *e) {
    static const uint8_t cats[] = {LAP_YELLOW, LAP_MAGENTA, LAP_CYAN, LAP_GREEN, LAP_RED};
    if (e->test) return LAP_WHITE;
    if (det_emergency(e)) return LAP_RED | LAP_BRIGHT;
    if ((e->source & (DET_BLE | DET_WIFI)) && e->category < 5) return cats[e->category] | LAP_BRIGHT;   /* SWEEP's own */
    const int i = src_index(e->source);
    return (uint8_t)((i >= 0 ? LOOK[i].hue : LAP_CYAN) | LAP_BRIGHT);
}
/* Brightness is age: a fresh alert in its own bright hue, the last ten
   minutes in the plain hue, anything older faint. */
static uint8_t aged(uint8_t hue, int64_t at) {
    const int64_t a = (now_us() - at) / 1000000;
    if (a < 60) return A(hue | LAP_BRIGHT);
    if (a < 600) return A(hue & 7);
    return LAP_UI_FAINT;
}
static void age_text(int64_t at, char *out, size_t cap) {
    const int64_t a = (now_us() - at) / 1000000;
    if (a < 60) snprintf(out, cap, "%lds", (long)(a < 0 ? 0 : a));
    else if (a < 3600) snprintf(out, cap, "%ldm", (long)(a / 60));
    else snprintf(out, cap, "%ldh", (long)(a / 3600));
}
static void count_text(char *out, size_t cap, uint32_t v) {
    if (v < 10000) snprintf(out, cap, "%lu", (unsigned long)v);
    else if (v < 1000000) snprintf(out, cap, "%luk", (unsigned long)(v / 1000));
    else snprintf(out, cap, "%luM", (unsigned long)(v / 1000000));
}
static void clip(int x, int y, int w, const char *t, uint8_t attr) {
    char b[128];
    if (w <= 0) return;
    snprintf(b, sizeof(b), "%.*s", w < 127 ? w : 127, t);
    lap_put_str(x, y, b, attr);
}
/* "== TITLE ======================= right" across w. */
static void rule(int x, int y, int w, const char *title, const char *right, uint8_t hue) {
    if (w < 8) return;
    lap_hline(x, y, w, '=', LAP_UI_FRAME);
    char t[64];
    snprintf(t, sizeof(t), " %s ", title);
    lap_put_str(x + 2, y, t, A(hue));
    if (right && right[0]) {
        const int left_end = x + 2 + (int)strlen(t);
        snprintf(t, sizeof(t), " %s ", right);
        const int n = (int)strlen(t);
        if (x + w - 1 - n > left_end) lap_put_str(x + w - 1 - n, y, t, LAP_UI_DIM);
    }
}
/* A label in DIM and its value in attr, from x; returns where the next one goes. */
static int stat(int x, int y, int end, const char *label, const char *value, uint8_t attr) {
    const int need = (int)strlen(label) + 1 + (int)strlen(value);
    if (x + need > end) return end;
    lap_put_str(x, y, label, LAP_UI_DIM);
    lap_put_str(x + (int)strlen(label) + 1, y, value, attr);
    return x + need + 2;
}
static int rules_on(void) {
    int n = 0;
    for (unsigned r = 0; r < s.d->n_rules; r++) n += s.d->rules[r].enabled;
    return n;
}
static bool watched(int i) { return ((s.d->sources >> i) & 1) && det_wants(s.d, 1u << i); }

/* One source: picture, name, what it has seen, its last match. The picture
   is in its family's hue (bright while it is being watched, faint when
   OPTIONS has it off); the lamp flashes white for two seconds after a real
   sighting and stays red or amber for a minute after a match. Nothing moves
   on its own. */
static void tile(int x, int y, int w, int i) {
    const src_look_t *l = &LOOK[i];
    const bool allowed = (s.d->sources >> i) & 1, used = watched(i);
    const uint32_t now = lap_millis();
    const bool fresh = s.running && used && s.src[i].seen_ms && now - s.src[i].seen_ms < 2000;
    const bool hit = s.src[i].hit_ms && now - s.src[i].hit_ms < 60000;
    const uint8_t body = !allowed ? LAP_UI_FAINT : used && s.running ? A(l->hue | LAP_BRIGHT) : A(l->hue);
    const uint8_t lamp = hit ? (s.src[i].emerg ? LAP_UI_ALARM : LAP_UI_VALUE) : fresh ? LAP_UI_TEXT : body;
    for (int r = 0; r < 3; r++)
        for (int k = 0; l->art[r][k] && k < 5; k++)
            if (l->art[r][k] != ' ') lap_put_cell(x + k, y + r, l->art[r][k], l->art[r][k] == 'o' ? lamp : body);
    const int tx = x + 6, tw = w - 6;
    clip(tx, y, tw, l->name, used ? LAP_UI_TEXT : allowed ? LAP_UI_DIM : LAP_UI_FAINT);
    char t[24], n[12];
    if (!allowed) clip(tx, y + 1, tw, "off", LAP_UI_FAINT);
    else if (!used) clip(tx, y + 1, tw, "no rule on", LAP_UI_FAINT);
    else if (!s.running) clip(tx, y + 1, tw, "ready", LAP_UI_DIM);
    else if (!s.src[i].seen) clip(tx, y + 1, tw, "listening", LAP_UI_DIM);
    else {
        count_text(n, sizeof(n), s.src[i].seen);
        lap_put_str(tx, y + 1, n, fresh ? LAP_UI_LIVE : A(LAP_GREEN));
        clip(tx + (int)strlen(n) + 1, y + 1, tw - (int)strlen(n) - 1, "seen", LAP_UI_DIM);
    }
    if (s.src[i].hits) {
        count_text(n, sizeof(n), s.src[i].hits);
        age_text((int64_t)s.src[i].hit_ms * 1000, t, sizeof(t));
        lap_put_str(tx, y + 2, n, hit ? lamp : LAP_UI_DIM);
        char rest[24];
        snprintf(rest, sizeof(rest), "hit %s", t);
        clip(tx + (int)strlen(n) + 1, y + 2, tw - (int)strlen(n) - 1, rest, LAP_UI_DIM);
    }
}
/* The twelve sources in `cols` columns; gap puts a blank row between rows of
   tiles. Returns the rows used. */
static int draw_sources(int x, int y, int w, int cols, int gap) {
    int on = 0;
    for (int i = 0; i < NSRC; i++) on += watched(i);
    char right[32];
    snprintf(right, sizeof(right), "%d of %d watched", on, NSRC);
    rule(x, y, w, "SOURCES", right, LAP_UI_TEXT);
    const int tw = (w - 1) / cols, rows = (NSRC + cols - 1) / cols;
    for (int i = 0; i < NSRC; i++) {
        const int c = i % cols, tx = x + 1 + c * tw;
        /* the last column runs to the edge, so its names may be a letter longer */
        tile(tx, y + 1 + (i / cols) * (3 + gap), c == cols - 1 ? x + w - tx : tw - 1, i);
    }
    return 1 + rows * (3 + gap) - gap;
}
/* n values as one braille line in w x h cells, two a cell, newest at the
   right edge, each joined to the one before; values before `from` are left
   out. */
static void braille_line(int x, int y, int w, int h, const float *v, int n, int from, float lo, float hi, uint8_t attr) {
    static const uint8_t BITS[4][2] = {{0x01, 0x08}, {0x02, 0x10}, {0x04, 0x20}, {0x40, 0x80}};
    if (w < 1 || h < 1 || hi <= lo) return;
    const int dots = h * 4, first = w * 2 - n;
    int prev = -1;
    uint8_t cell[16];
    const int rows = h < 16 ? h : 16;
    for (int cx = 0; cx < w; cx++) {
        memset(cell, 0, sizeof(cell));
        bool any = false;
        for (int half = 0; half < 2; half++) {
            const int k = cx * 2 + half - first;
            if (k < from || k >= n) continue;
            int d = (int)((hi - v[k]) / (hi - lo) * (dots - 1) + 0.5f);
            d = d < 0 ? 0 : d >= dots ? dots - 1 : d;
            const int a = prev >= 0 && prev < d ? prev : d, b = prev >= 0 && prev > d ? prev : d;
            for (int q = a; q <= b; q++)
                if (q / 4 < rows) cell[q / 4] |= BITS[q % 4][half];
            prev = d;
            any = true;
        }
        if (!any) continue;
        for (int r = 0; r < rows; r++)
            if (cell[r]) lap_put_cell(x + cx, y + r, LAP_BRAILLE(cell[r]), attr);
    }
}
/* Sightings over the last minutes as a braille line, matches as ticks under
   it on the same time line (red for an emergency), oldest at the left. */
static int draw_activity(int x, int y, int w, int h) {
    const int now = hb_now();
    const int cells = w - 2;
    int n = cells * 2 < HB ? cells * 2 : HB;
    if (n < 4 || h < 5) return 0;
    uint32_t seen = 0, hits = 0;
    float top = 4;
    for (int k = 0; k < n; k++) {
        const int b = ((now - (n - 1 - k)) % HB + HB) % HB;
        s.hist[k] = s.hb_seen[b];
        if (s.hist[k] > top) top = s.hist[k];
        seen += s.hb_seen[b];
        hits += s.hb_hit[b];
    }
    const unsigned mins = (unsigned)(n * HB_MS / 60000u);
    char right[48], a[12], b[12];
    count_text(a, sizeof(a), seen);
    count_text(b, sizeof(b), hits);
    snprintf(right, sizeof(right), "%s seen  %s hit  %u min", a, b, mins);
    rule(x, y, w, "ACTIVITY", right, LAP_UI_TEXT);
    const int gh = h - 3;
    /* Only the buckets since RUN: before it nothing was watched, not nothing seen. */
    int from = 0;
    while (from < n && (int64_t)s.hb_at - (n - 1 - from) < (int64_t)s.run_bucket) from++;
    braille_line(x + 1, y + 1, cells, gh, s.hist, n, from, 0, top * 1.25f, LAP_UI_LIVE);
    const int ty = y + 1 + gh;
    for (int c = 0; c < cells; c++) {
        uint8_t f = 0;
        for (int j = 0; j < 2; j++) {
            const int k = n - cells * 2 + c * 2 + j;    /* right-aligned like the braille */
            if (k < 0) continue;
            f |= s.hb_flag[((now - (n - 1 - k)) % HB + HB) % HB];
        }
        lap_put_cell(x + 1 + c, ty, f ? '|' : '.', f & 2 ? LAP_UI_ALARM : f ? LAP_UI_VALUE : LAP_UI_FAINT);
    }
    char l[12];
    snprintf(l, sizeof(l), "-%um", mins);
    lap_put_str(x + 1, ty + 1, l, LAP_UI_FAINT);
    lap_put_str(x + 1 + cells - 3, ty + 1, "now", LAP_UI_DIM);
    lap_put_str(x + 1 + cells / 2 - 3, ty + 1, "hits ^", LAP_UI_FAINT);
    return h;
}

/* No alerts yet: a listening post, its lamp lit by real sightings only. */
static const char *const POST[] = {
    "  .    '    .  ",
    "'   .-\"\"\"-.   '",
    "   /  .-.  \\   ",
    "  |  ( o )  |  ",
    "   \\  '-'  /   ",
    ".   '-._.-'   .",
    "      |||      ",
    "     /|||\\     ",
    "    / ||| \\    ",
    "   /__|||__\\   ",
};
static void draw_idle(int x, int y, int w, int h, bool any) {
    int fresh = 0, used = 0;
    for (int i = 0; i < NSRC; i++) {
        used += watched(i);
        fresh += s.running && s.src[i].seen_ms && lap_millis() - s.src[i].seen_ms < 2000;
    }
    const int art = (int)(sizeof(POST) / sizeof(*POST));
    int ty = y + 1;
    if (h >= art + 6 && w >= 20) {
        const int ax = x + (w - 15) / 2, ay = y + (h - art - 5) / 2;
        for (int r = 0; r < art; r++)
            for (int k = 0; POST[r][k]; k++) {
                const char c = POST[r][k];
                if (c == ' ') continue;
                const uint8_t at = c == 'o' ? (fresh ? LAP_UI_LIVE : s.running ? A(LAP_GREEN) : LAP_UI_DIM)
                                 : r < 6 && (c == '.' || c == '\'') && (k < 3 || k > 11) ? LAP_UI_FAINT
                                 : r < 6 ? (s.running ? LAP_UI_FRAME : LAP_UI_DIM) : LAP_UI_DIM;
                lap_put_cell(ax + k, ay + r, c, at);
            }
        ty = ay + art + 1;
    }
    char t[96];
    const int cx = x + 1, cw = w - 2;
    if (s.running) {
        snprintf(t, sizeof(t), "WATCHING %d sources, %d rules on", used, rules_on());
        clip(cx + (cw - (int)strlen(t)) / 2, ty, cw, t, LAP_UI_LIVE);
        snprintf(t, sizeof(t), any ? "Listening for the next match." : "Nothing has matched a rule yet.");
    } else {
        snprintf(t, sizeof(t), "STOPPED: RUN watches %d sources, %d rules on", used, rules_on());
        clip(cx + (cw - (int)strlen(t) > 0 ? (cw - (int)strlen(t)) / 2 : 0), ty, cw, t, LAP_UI_WARN);
        snprintf(t, sizeof(t), "TEST tries the sinks. OPTIONS picks them.");
    }
    clip(cx + (cw - (int)strlen(t) > 0 ? (cw - (int)strlen(t)) / 2 : 0), ty + 1, cw, t, LAP_UI_DIM);
}

static const struct { uint8_t bit; const char *name; } SINK_CHIPS[] = {
    {DET_BANNER, "BANNER"}, {DET_TONE, "CHIRP"}, {DET_VIBRATE, "VIBRATE"},
    {DET_TTS, "SPEECH"}, {DET_SD, "SD"}, {DET_MESH, "MESH"},
};
static void draw_alerts(int x, int y, int w, int h) {
    const det_t *d = s.d;
    char t[128], u[96];
    snprintf(u, sizeof(u), d->history_n ? "%u, newest first" : "", d->history_n);
    rule(x, y, w, "ALERTS", u, LAP_UI_TEXT);
    y++; h--;
    s.list_x = x; s.list_w = w; s.list_y = y; s.list_rows = 0;
    if (!d->history_n) { draw_idle(x, y, w, h, false); return; }
    const bool split = h >= 12;
    const int rows = split ? h - 7 : h;
    if (s.pick >= (int)d->history_n) s.pick = (int)d->history_n - 1;
    if (s.pick < s.first) s.first = s.pick;
    if (s.pick >= s.first + rows) s.first = s.pick - rows + 1;
    s.list_rows = rows;
    for (int r = 0; r < rows && s.first + r < (int)d->history_n; r++) {
        const int i = s.first + r;
        const det_event_t *e = &d->history[i];
        char age[8];
        age_text(e->at, age, sizeof(age));
        det_describe(e, t, sizeof(t));
        const uint8_t hue = event_hue(e), ink = aged(hue, e->at);
        const int si = src_index(e->source);
        lap_put_cell(x, y + r, i == s.pick ? '>' : ' ', A(hue));
        clip(x + 1, y + r, 4, age, ink == LAP_UI_FAINT ? LAP_UI_FAINT : LAP_UI_DIM);
        clip(x + 6, y + r, 4, e->test ? "" : si >= 0 ? LOOK[si].tag : "?", ink);
        clip(x + 11, y + r, w - 12, t, i == s.pick ? A(hue) : ink == LAP_UI_FAINT ? LAP_UI_FAINT : LAP_UI_TEXT);
    }
    /* A short list leaves room: the listening post under it. */
    const int shown = (int)d->history_n - s.first < rows ? (int)d->history_n - s.first : rows;
    if (rows - shown >= 15) draw_idle(x, y + shown + 1, w, rows - shown - 1, true);
    if (!split) return;
    const det_event_t *e = &d->history[s.pick];
    const int by = y + rows;
    lap_box(x, by, w, 7, det_label(e), A(event_hue(e)));
    det_describe(e, t, sizeof(t));
    clip(x + 1, by + 1, w - 2, t, LAP_UI_TEXT);
    /* Where it went: delivered green, failed red, held back amber, not asked faint. */
    int cx = x + 1;
    for (unsigned k = 0; k < sizeof(SINK_CHIPS) / sizeof(*SINK_CHIPS) && cx < x + w - 2; k++) {
        const uint8_t b = SINK_CHIPS[k].bit;
        const uint8_t at = e->failed & b ? LAP_UI_ALARM : e->delivered & b ? LAP_UI_LIVE
                         : e->selected & b ? LAP_UI_WARN : LAP_UI_FAINT;
        snprintf(u, sizeof(u), "%s%s", (e->delivered | e->failed) & b ? "+" : e->selected & b ? "~" : "", SINK_CHIPS[k].name);
        clip(cx, by + 2, x + w - 1 - cx, u, at);
        cx += (int)strlen(u) + 1;
    }
    snprintf(u, sizeof(u), "%s%s%s", e->quiet ? "quiet hours held some back  " : "",
             e->mesh_note ? "mesh " : "", e->mesh_note ? det_note_name(e->mesh_note) : "");
    clip(x + 1, by + 3, w - 2, u, LAP_UI_DIM);
    if (e->source == DET_MESH_ALERT && e->distance_x10_km != DET_NO_DISTANCE) {
        snprintf(u, sizeof(u), "from %.16s, %u.%u km %s", e->name[0] ? e->name : e->origin, e->distance_x10_km / 10,
                 e->distance_x10_km % 10, compass(e->bearing));
        clip(x + 1, by + 4, w - 2, u, LAP_UI_VALUE);
    } else if (e->coalesced) {
        snprintf(u, sizeof(u), "+%u more held by cooldowns", (unsigned)e->coalesced);
        clip(x + 1, by + 4, w - 2, u, LAP_UI_VALUE);
    }
    size_t n = (size_t)snprintf(t, sizeof(t), "rule ");
    for (unsigned r = 0; r < d->n_rules && r < 64 && n + 2 < sizeof(t); r++)
        if ((e->rules >> r) & 1) n += (size_t)snprintf(t + n, sizeof(t) - n, "%s%s", n > 5 ? ", " : "", d->rules[r].id);
    if (n > 5) clip(x + 1, by + 5, w - 2, t, LAP_UI_FAINT);
}

/* No aircraft: a plane in outline. */
static const char *const PLANE[] = {
    "       _|_       ",
    "--====( o )====--",
    "  'o'  '-'  'o'  ",
    "        |        ",
    "      -=+=-      ",
};
static void draw_air(int x, int y, int w, int h) {
    char t[64];
    snprintf(t, sizeof(t), s.air_n ? "%d heard" : "", s.air_n);
    rule(x, y, w, "AIRCRAFT", t, A(LAP_BLUE | LAP_BRIGHT));
    y++; h--;
    s.list_x = x; s.list_w = w; s.list_y = y; s.list_rows = 0;
    if (!s.air_n) {
        const int ax = x + (w - 17) / 2, ay = y + 2;
        for (int r = 0; r < 5 && ay + r < y + h; r++)
            for (int k = 0; PLANE[r][k]; k++)
                if (PLANE[r][k] != ' ') lap_put_cell(ax + k, ay + r, PLANE[r][k], PLANE[r][k] == 'o' ? A(LAP_BLUE) : LAP_UI_FAINT);
        clip(x + 1, ay + 7, w - 2, "No aircraft. DETECTOR starts no receiver:", LAP_UI_DIM);
        clip(x + 1, ay + 8, w - 2, "run ADS-B in its own app and they show here.", LAP_UI_FAINT);
        return;
    }
    const bool wide = w >= 60;
    const int mw = wide ? 20 : 12;              /* the altitude ladder, 0 to 45,000 ft */
    lap_put_str(x + 1, y, "CALL", LAP_UI_FAINT);
    lap_put_str(x + 10, y, "SQK", LAP_UI_FAINT);
    lap_put_str(x + 15, y, "ALTITUDE", LAP_UI_FAINT);
    lap_put_str(x + 16 + mw, y, "   FT", LAP_UI_FAINT);
    lap_put_str(x + 22 + mw, y, "   KM", LAP_UI_FAINT);
    lap_put_str(x + 28 + mw, y, "RULE", LAP_UI_FAINT);
    s.list_y = y + 1; s.list_rows = h - 1;
    for (int i = 0; i < s.air_n && i < h - 1; i++) {
        const lap_aircraft_t *p = &s.air[i];
        const int ry = y + 1 + i;
        det_air_t a = {.icao = p->icao, .emergency = p->emergency, .category = (int8_t)det_air_category_bit(p->category),
                       .squawk = p->squawk, .altitude_ft = p->has_alt ? p->alt_ft : INT32_MIN,
                       .distance_x10_km = p->range_km >= 0 ? (uint16_t)(p->range_km * 10 + 0.5f) : DET_NO_DISTANCE};
        memcpy(a.callsign, p->callsign, sizeof(a.callsign));
        char ids[64] = "";
        size_t n = 0;
        for (unsigned r = 0; r < s.d->n_rules && n + 2 < sizeof(ids); r++)
            if (s.d->rules[r].enabled && s.d->rules[r].sources == DET_ADSB && det_air_match(&s.d->rules[r], &a))
                n += (size_t)snprintf(ids + n, sizeof(ids) - n, "%s%s", n ? "," : "", s.d->rules[r].id);
        const bool alarm = det_air_alarm(a.squawk, a.emergency);
        const bool old = p->age_ms > 60000;
        const uint8_t name_ink = alarm ? LAP_UI_ALARM : old ? LAP_UI_FAINT : ids[0] ? A(LAP_BLUE | LAP_BRIGHT) : LAP_UI_TEXT;
        lap_put_cell(x, ry, alarm ? '!' : ids[0] ? '*' : ' ', alarm ? LAP_UI_ALARM : LAP_UI_VALUE);
        if (p->callsign[0]) snprintf(t, sizeof(t), "%.8s", p->callsign);
        else snprintf(t, sizeof(t), "%06lX", (unsigned long)(p->icao & 0xFFFFFF));
        clip(x + 1, ry, 8, t, name_ink);
        if (p->squawk) { snprintf(t, sizeof(t), "%04u", (unsigned)p->squawk); clip(x + 10, ry, 4, t, alarm ? LAP_UI_ALARM : LAP_UI_DIM); }
        else lap_put_str(x + 10, ry, "----", LAP_UI_FAINT);
        /* [=====------] coloured by height: low green, middle cyan, high blue. */
        lap_put_cell(x + 15, ry, '[', LAP_UI_FAINT);
        lap_put_cell(x + 16 + mw - 2, ry, ']', LAP_UI_FAINT);
        const int lit = p->has_alt ? (int)((p->alt_ft < 0 ? 0 : p->alt_ft > 45000 ? 45000 : p->alt_ft) * (mw - 2) / 45000.0f + 0.5f) : 0;
        const uint8_t band = !p->has_alt ? LAP_UI_FAINT : p->alt_ft < 10000 ? A(LAP_GREEN | LAP_BRIGHT)
                           : p->alt_ft < 25000 ? A(LAP_CYAN | LAP_BRIGHT) : A(LAP_BLUE | LAP_BRIGHT);
        for (int k = 0; k < mw - 2; k++)
            lap_put_cell(x + 16 + k, ry, k < lit ? '=' : '-', k < lit ? (old ? LAP_UI_FAINT : band) : LAP_UI_FAINT);
        if (p->has_alt) { snprintf(t, sizeof(t), "%5ld", (long)p->alt_ft); clip(x + 16 + mw, ry, 5, t, old ? LAP_UI_FAINT : LAP_UI_DIM); }
        else lap_put_str(x + 16 + mw, ry, "   --", LAP_UI_FAINT);
        if (p->range_km >= 0) { snprintf(t, sizeof(t), "%5.1f", (double)p->range_km); clip(x + 22 + mw, ry, 5, t, LAP_UI_DIM); }
        else lap_put_str(x + 22 + mw, ry, "   --", LAP_UI_FAINT);
        clip(x + 28 + mw, ry, w - 29 - mw, ids, alarm ? LAP_UI_ALARM : LAP_UI_VALUE);
    }
}
static void draw_rules(int x, int y, int w, int h) {
    const det_t *d = s.d;
    char t[48];
    snprintf(t, sizeof(t), "%d on of %u", rules_on(), d->n_rules);
    rule(x, y, w, "RULES", t, LAP_UI_TEXT);
    y++; h--;
    const int idw = w >= 70 ? 30 : 20;
    lap_put_str(x + 1, y, "ON", LAP_UI_FAINT);
    lap_put_str(x + 5, y, "RULE", LAP_UI_FAINT);
    lap_put_str(x + 6 + idw, y, "HITS", LAP_UI_FAINT);
    lap_put_str(x + 12 + idw, y, "SOURCES", LAP_UI_FAINT);
    y++; h--;
    const bool split = h >= 14 && d->n_rules;
    const int box = 6;
    if (split) h -= box;
    if (s.pick >= (int)d->n_rules) s.pick = d->n_rules ? (int)d->n_rules - 1 : 0;
    if (s.pick < s.first) s.first = s.pick;
    if (s.pick >= s.first + h) s.first = s.pick - h + 1;
    s.list_x = x; s.list_w = w; s.list_y = y; s.list_rows = h;
    if (split) {
        /* The picked rule in full: where it listens, where it alerts, how often. */
        const det_rule_t *k = &d->rules[s.pick];
        const int by = y + h;
        lap_box(x, by, w, box, k->id, k->enabled ? LAP_UI_LIVE : LAP_UI_DIM);
        char u[128];
        size_t n = (size_t)snprintf(u, sizeof(u), "listens ");
        for (unsigned b = 1; b <= DET_MESH_ALERT && n + 2 < sizeof(u); b <<= 1)
            if (k->sources & b) n += (size_t)snprintf(u + n, sizeof(u) - n, "%s ", det_source_name(b));
        clip(x + 1, by + 1, w - 2, u, LAP_UI_TEXT);
        int cx = x + 1;
        lap_put_str(cx, by + 2, "alerts", LAP_UI_DIM);
        cx += 7;
        for (unsigned q = 0; q < sizeof(SINK_CHIPS) / sizeof(*SINK_CHIPS) && cx < x + w - 2; q++) {
            const uint8_t b = SINK_CHIPS[q].bit;
            const uint8_t at = !(k->sinks & b) ? LAP_UI_FAINT : (d->sinks & b) ? LAP_UI_LIVE : LAP_UI_WARN;
            clip(cx, by + 2, x + w - 1 - cx, SINK_CHIPS[q].name, at);
            cx += (int)strlen(SINK_CHIPS[q].name) + 1;
        }
        snprintf(u, sizeof(u), "cooldown %lus per thing, %lus for the rule", (unsigned long)k->cooldown_s,
                 (unsigned long)k->rule_cooldown_s);
        clip(x + 1, by + 3, w - 2, u, LAP_UI_DIM);
        clip(x + 1, by + 4, w - 2, k->enabled ? "ENTER or TOGGLE turns it off" : "ENTER or TOGGLE turns it on", LAP_UI_FAINT);
    }
    for (int r = 0; r < h && s.first + r < (int)d->n_rules; r++) {
        const int i = s.first + r;
        const det_rule_t *k = &d->rules[i];
        lap_put_cell(x, y + r, i == s.pick ? '>' : ' ', LAP_UI_VALUE);
        lap_put_str(x + 1, y + r, k->enabled ? "[x]" : "[ ]", k->enabled ? LAP_UI_LIVE : LAP_UI_FAINT);
        clip(x + 5, y + r, idw, k->id, i == s.pick ? LAP_UI_VALUE : k->enabled ? LAP_UI_TEXT : LAP_UI_DIM);
        if (i < DET_RULES && s.rule_hits[i]) {
            count_text(t, sizeof(t), s.rule_hits[i]);
            char f[12];
            snprintf(f, sizeof(f), "%4.4s", t);
            lap_put_str(x + 6 + idw, y + r, f, LAP_UI_VALUE);
        } else lap_put_str(x + 6 + idw, y + r, "   -", LAP_UI_FAINT);
        int sx = x + 12 + idw;
        for (int b = 0; b < NSRC && sx < x + w - 1; b++)
            if (k->sources & (1u << b)) {
                clip(sx, y + r, x + w - 1 - sx, LOOK[b].tag, k->enabled ? A(LOOK[b].hue | LAP_BRIGHT) : LAP_UI_FAINT);
                sx += (int)strlen(LOOK[b].tag) + 1;
            }
    }
}
static void draw(void) {
    if (!s.d) { lap_clear(LAP_UI_DIM); lap_put_str(1, 1, "Out of memory", LAP_UI_ALARM); return; }
    if (!lap_service_running()) pump();            /* without the service, only while open */
    const int W = lap_cols(), H = lap_rows();
    const det_t *d = s.d;
    lap_clear(LAP_UI_DIM);
    s.kh = lap_ui_keys_rows(5);
    const bool quiet = det_quiet(d) && d->prefs.quiet_on;
    s.chips[0] = (lap_chip_t){s.running ? (lap_service_running() ? "WATCHING" : "ON SCREEN") : "STOPPED", s.running,
                              s.running ? LAP_UI_LIVE : LAP_UI_WARN};
    s.chips[1] = (lap_chip_t){"QUIET", quiet, LAP_UI_VALUE};
    s.chips[2] = (lap_chip_t){"MESH", (d->sinks & DET_MESH) != 0, LAP_CYAN};
    s.chips[3] = (lap_chip_t){VIEW_NAME[s.view], true, LAP_UI_VALUE};
    lap_ui_rail(0, 0, W, s.chips, 3, &s.chips[3], 1);
    /* The engine's counts, each value in what it means. */
    char v[16];
    int x = 1;
    snprintf(v, sizeof(v), "%d/%u", rules_on(), d->n_rules);
    x = stat(x, 1, W - 1, "rules", v, LAP_UI_TEXT);
    count_text(v, sizeof(v), d->observations);
    x = stat(x, 1, W - 1, "seen", v, d->observations ? LAP_UI_LIVE : LAP_UI_FAINT);
    count_text(v, sizeof(v), d->matches);
    x = stat(x, 1, W - 1, "match", v, d->matches ? LAP_UI_VALUE : LAP_UI_FAINT);
    count_text(v, sizeof(v), d->suppressed);
    x = stat(x, 1, W - 1, "held", v, d->suppressed ? A(LAP_CYAN | LAP_BRIGHT) : LAP_UI_FAINT);
    count_text(v, sizeof(v), d->dropped);
    stat(x, 1, W - 1, "drop", v, d->dropped ? LAP_UI_WARN : LAP_UI_FAINT);
    if (d->error[0] || s.sd_failed) clip(1, 2, W - 2, d->error, LAP_UI_WARN);
    const int top = 3, body_h = H - s.kh - top;
    if (s.view == VIEW_ALERTS) {
        if (W >= 100) {                          /* landscape: sources and activity left, alerts right */
            const int lw = 52;
            int used = draw_sources(0, top, lw, 3, body_h >= 26 ? 1 : 0);
            if (body_h - used - 1 >= 6) draw_activity(0, top + used + 1, lw, body_h - used - 1 > 8 ? 8 : body_h - used - 1);
            draw_alerts(lw + 1, top, W - lw - 1, body_h);
        } else {
            const int cols = W >= 68 ? 4 : 3;
            const int gap = body_h >= 50 ? 1 : 0;
            int used = draw_sources(0, top, W, cols, gap) + 1;
            if (body_h - used >= 20) used += draw_activity(0, top + used, W, 6);
            draw_alerts(0, top + used, W, body_h - used);
        }
    } else if (s.view == VIEW_AIR) draw_air(0, top, W, body_h);
    else draw_rules(0, top, W, body_h);
    snprintf(s.key_text[0], sizeof(s.key_text[0]), "%s", s.running ? "ON" : "OFF");
    snprintf(s.key_text[1], sizeof(s.key_text[1]), "%s", VIEW_NAME[s.view]);
    s.keys[0] = (lap_btn_t){"RUN", s.key_text[0], 'r', s.running, false, false};
    s.keys[1] = (lap_btn_t){"VIEW", s.key_text[1], 'v', false, false, false};
    s.keys[2] = (lap_btn_t){s.view == VIEW_RULES ? "TOGGLE" : "TEST", NULL, 't', false, false, false};
    s.keys[3] = (lap_btn_t){"RELOAD", NULL, 'l', s.rules_job >= 0, false, false};
    s.keys[4] = (lap_btn_t)LAP_BTN_OPTIONS;
    lap_ui_keys(0, H - s.kh, W, s.kh, s.keys, 5, -1);
}

/* ---- input ---------------------------------------------------------------- */

static void toggle_rule(void) {
    if (s.pick < 0 || s.pick >= (int)s.d->n_rules) return;
    det_rule_t r = s.d->rules[s.pick];
    r.enabled = !r.enabled;
    det_rule_set(s.d, (unsigned)s.pick, &r);
}
static bool key(lap_key_t k, char ch) {
    const int count = s.view == VIEW_ALERTS ? (int)s.d->history_n : s.view == VIEW_RULES ? (int)s.d->n_rules : s.air_n;
    if (k == LAP_KEY_UP) { if (s.pick > 0) s.pick--; return true; }
    if (k == LAP_KEY_DOWN) { if (s.pick + 1 < count) s.pick++; return true; }
    if (k == LAP_KEY_ENTER && s.view == VIEW_RULES) { toggle_rule(); return true; }
    if (k != LAP_KEY_CHAR) return false;
    switch (ch | 0x20) {
    case 'r': run(!s.running); return true;
    case 'v': s.view = (s.view + 1) % VIEWS; s.pick = s.first = 0; save(); return true;
    case 't':
        if (s.view == VIEW_RULES) toggle_rule();
        else det_test(s.d, now_us());
        return true;
    case 'l': reload(); return true;
    case 'o': lap_options_open(&OPTIONS); return true;
    default: return false;
    }
}
static bool touch(int col, int row) {
    const int i = lap_ui_key_hit(col, row);
    if (i >= 0) return key(LAP_KEY_CHAR, s.keys[i].key);
    if (row >= s.list_y && row < s.list_y + s.list_rows && col >= s.list_x && col < s.list_x + s.list_w) {
        const int at = s.first + row - s.list_y;
        const int count = s.view == VIEW_ALERTS ? (int)s.d->history_n : (int)s.d->n_rules;
        if (at >= count) return true;
        if (s.view == VIEW_RULES && at == s.pick) toggle_rule();
        else if (s.view != VIEW_AIR) s.pick = at;
        return true;
    }
    return false;
}

static void enter(void) {
    if (s.ready) return;                         /* back from the background: everything is still here */
    memset(&s, 0, sizeof(s));
    s.rules_job = s.log_job = s.dir_job[0] = s.dir_job[1] = -1;
    s.boot = lap_random() | 1;
    s.d = calloc(1, sizeof(det_t));
    s.rules_text = malloc(DET_RULES_BYTES + 1);
    if (!s.d || !s.rules_text) return;
    s.d->deliver = deliver;
    det_init(s.d);
    load();
    reload();
    s.ready = true;
    if (s.running) run(true);                    /* it was watching when it was last closed */
}
static void leave(void) {
    if (lap_service_running()) return;           /* it goes on watching: keep it all */
    if (s.log_n) log_poll();                     /* hand the last lines to the card */
    lap_seen_watch(0);
    s.ready = false;
}

LAP_APP(.id = "detector", .name = "DETECTOR", .sub = "rule matches", .version = "3.1",
        .author = "LakeShark", .icon = LAP_ICON_TOWER, .color = LAP_CYAN,
        .flags = LAP_CAP_RADIO_RX | LAP_CAP_RADIO_TX | LAP_CAP_SERVICE,
        .icon_art = {"  .----.  ", " / .--. \\ ", "| ( () ) |", " \\ '--' / ", "  '----'  ", " DETECTOR "},
        .enter = enter, .leave = leave, .draw = draw, .key = key, .touch = touch);
