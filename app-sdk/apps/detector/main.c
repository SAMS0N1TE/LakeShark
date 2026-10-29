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
    int kh, list_y, list_rows;
    uint32_t test_at;
} s;

static int64_t now_us(void) { return (int64_t)lap_millis() * 1000; }

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
    else det_parse(s.d, s.rules_text, (size_t)r);   /* a file past the limit is refused there */
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
    if (!det_wants(s.d, DET_ADSB)) return;
    for (int i = 0; i < s.air_n; i++) {
        const lap_aircraft_t *p = &s.air[i];
        if (p->age_ms > 60000) continue;           /* stale aircraft never alert */
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
    case O_CLEAR: s.d->history_n = 0; s.pick = s.first = 0; return;
    default: return;
    }
    det_prefs_set(s.d, &p);
    save();
}

/* ---- screen --------------------------------------------------------------- */

static uint8_t event_hue(const det_event_t *e) {
    static const uint8_t cats[] = {LAP_YELLOW, LAP_MAGENTA, LAP_CYAN, LAP_GREEN, LAP_RED};
    if (e->test) return LAP_WHITE;
    if (det_emergency(e)) return LAP_RED | LAP_BRIGHT;
    if (e->source == DET_ADSB) return LAP_BLUE | LAP_BRIGHT;
    if (e->source == DET_MESH_ALERT) return LAP_MAGENTA | LAP_BRIGHT;
    if (e->source == DET_P25) return LAP_YELLOW | LAP_BRIGHT;
    if (e->source & DET_SIGNALS) return LAP_GREEN | LAP_BRIGHT;
    return e->category < 5 ? cats[e->category] | LAP_BRIGHT : LAP_CYAN;
}
static void age_text(int64_t at, char *out, size_t cap) {
    const int64_t a = (now_us() - at) / 1000000;
    if (a < 60) snprintf(out, cap, "%lds", (long)(a < 0 ? 0 : a));
    else if (a < 3600) snprintf(out, cap, "%ldm", (long)(a / 60));
    else snprintf(out, cap, "%ldh", (long)(a / 3600));
}
static void clip(int x, int y, int w, const char *t, uint8_t attr) {
    char b[128];
    if (w <= 0) return;
    snprintf(b, sizeof(b), "%.*s", w < 127 ? w : 127, t);
    lap_put_str(x, y, b, attr);
}
static void draw_alerts(int x, int y, int w, int h) {
    const det_t *d = s.d;
    if (!d->history_n) {
        clip(x + 1, y + 1, w - 2, s.running ? "Watching. Nothing has matched a rule yet." : "Stopped. RUN to watch.", LAP_UI_DIM);
        clip(x + 1, y + 3, w - 2, "TEST tries the sinks; OPTIONS chooses sources and sinks.", LAP_UI_FAINT);
        return;
    }
    const bool split = h >= 12;
    const int rows = split ? h - 6 : h;
    if (s.pick >= (int)d->history_n) s.pick = (int)d->history_n - 1;
    if (s.pick < s.first) s.first = s.pick;
    if (s.pick >= s.first + rows) s.first = s.pick - rows + 1;
    s.list_y = y; s.list_rows = rows;
    for (int r = 0; r < rows && s.first + r < (int)d->history_n; r++) {
        const int i = s.first + r;
        const det_event_t *e = &d->history[i];
        char age[8], line[128];
        age_text(e->at, age, sizeof(age));
        det_describe(e, line, sizeof(line));
        const uint8_t hue = event_hue(e);
        lap_put_cell(x, y + r, i == s.pick ? '>' : ' ', A(hue));
        clip(x + 1, y + r, 4, age, LAP_UI_FAINT);
        clip(x + 6, y + r, w - 7, line, i == s.pick ? A(hue) : LAP_UI_TEXT);
    }
    if (!split) return;
    const det_event_t *e = &d->history[s.pick];
    const int by = y + rows;
    lap_box(x, by, w, 6, det_label(e), A(event_hue(e)));
    char t[128], u[96];
    det_describe(e, t, sizeof(t));
    clip(x + 1, by + 1, w - 2, t, LAP_UI_TEXT);
    snprintf(t, sizeof(t), "sinks %s%s%s%s%s%s%s", e->delivered & DET_BANNER ? "BANNER " : "", e->delivered & DET_TONE ? "CHIRP " : "",
             e->delivered & DET_VIBRATE ? "VIBRATE " : "", e->delivered & DET_TTS ? "SPEECH " : "", e->delivered & DET_SD ? "SD " : "",
             e->delivered & DET_MESH ? "MESH " : "", e->failed ? "(some failed)" : "");
    clip(x + 1, by + 2, w - 2, t, LAP_UI_DIM);
    snprintf(u, sizeof(u), "%s%s%s", e->quiet ? "quiet hours held some back  " : "",
             e->mesh_note ? "mesh " : "", e->mesh_note ? det_note_name(e->mesh_note) : "");
    clip(x + 1, by + 3, w - 2, u, LAP_UI_FAINT);
    if (e->source == DET_MESH_ALERT && e->distance_x10_km != DET_NO_DISTANCE) {
        snprintf(u, sizeof(u), "from %.16s, %u.%u km %s", e->name[0] ? e->name : e->origin, e->distance_x10_km / 10,
                 e->distance_x10_km % 10, compass(e->bearing));
        clip(x + 1, by + 4, w - 2, u, LAP_UI_VALUE);
    } else if (e->coalesced) {
        snprintf(u, sizeof(u), "+%u more held by cooldowns", (unsigned)e->coalesced);
        clip(x + 1, by + 4, w - 2, u, LAP_UI_VALUE);
    }
}
static void draw_air(int x, int y, int w, int h) {
    if (!s.air_n) {
        clip(x + 1, y + 1, w - 2, "No aircraft. DETECTOR starts no receiver:", LAP_UI_DIM);
        clip(x + 1, y + 2, w - 2, "run ADS-B in its own app and they show here.", LAP_UI_FAINT);
        return;
    }
    s.list_y = y; s.list_rows = h;
    for (int i = 0; i < s.air_n && i < h; i++) {
        const lap_aircraft_t *p = &s.air[i];
        det_air_t a = {.icao = p->icao, .emergency = p->emergency, .category = (int8_t)det_air_category_bit(p->category),
                       .squawk = p->squawk, .altitude_ft = p->has_alt ? p->alt_ft : INT32_MIN,
                       .distance_x10_km = p->range_km >= 0 ? (uint16_t)(p->range_km * 10 + 0.5f) : DET_NO_DISTANCE};
        memcpy(a.callsign, p->callsign, sizeof(a.callsign));
        char t[64], ids[64] = "";
        det_air_text(&a, t, sizeof(t));
        size_t n = 0;
        for (unsigned r = 0; r < s.d->n_rules && n + 2 < sizeof(ids); r++)
            if (s.d->rules[r].enabled && s.d->rules[r].sources == DET_ADSB && det_air_match(&s.d->rules[r], &a))
                n += (size_t)snprintf(ids + n, sizeof(ids) - n, "%s%s", n ? "," : "", s.d->rules[r].id);
        const bool alarm = det_air_alarm(a.squawk, a.emergency);
        clip(x + 1, y + i, 28, t, alarm ? A(LAP_RED | LAP_BRIGHT) : ids[0] ? A(LAP_BLUE | LAP_BRIGHT) : LAP_UI_DIM);
        clip(x + 30, y + i, w - 31, ids, LAP_UI_VALUE);
    }
}
static void draw_rules(int x, int y, int w, int h) {
    const det_t *d = s.d;
    if (s.pick >= (int)d->n_rules) s.pick = d->n_rules ? (int)d->n_rules - 1 : 0;
    if (s.pick < s.first) s.first = s.pick;
    if (s.pick >= s.first + h) s.first = s.pick - h + 1;
    s.list_y = y; s.list_rows = h;
    for (int r = 0; r < h && s.first + r < (int)d->n_rules; r++) {
        const int i = s.first + r;
        const det_rule_t *k = &d->rules[i];
        char src[64] = "";
        size_t n = 0;
        for (unsigned b = 1; b <= DET_MESH_ALERT && n + 2 < sizeof(src); b <<= 1)
            if (k->sources & b) n += (size_t)snprintf(src + n, sizeof(src) - n, "%s%s", n ? " " : "", det_source_name(b));
        lap_put_cell(x, y + r, i == s.pick ? '>' : ' ', LAP_UI_VALUE);
        lap_put_str(x + 1, y + r, k->enabled ? "[x]" : "[ ]", k->enabled ? A(LAP_GREEN | LAP_BRIGHT) : LAP_UI_FAINT);
        clip(x + 5, y + r, 22, k->id, k->enabled ? LAP_UI_TEXT : LAP_UI_DIM);
        clip(x + 28, y + r, w - 29, src, LAP_UI_FAINT);
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
    char t[128];
    snprintf(t, sizeof(t), "rules %u  matches %lu  held %lu  dropped %lu  seen %lu", d->n_rules, (unsigned long)d->matches,
             (unsigned long)d->suppressed, (unsigned long)d->dropped, (unsigned long)d->observations);
    clip(1, 1, W - 2, t, LAP_UI_DIM);
    if (d->error[0] || s.sd_failed) clip(1, 2, W - 2, d->error, LAP_UI_WARN);
    const int top = 3, body_h = H - s.kh - top;
    if (s.view == VIEW_ALERTS) draw_alerts(0, top, W, body_h);
    else if (s.view == VIEW_AIR) draw_air(0, top, W, body_h);
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
    if (row >= s.list_y && row < s.list_y + s.list_rows) {
        const int at = s.first + row - s.list_y;
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

LAP_APP(.id = "detector", .name = "DETECTOR", .sub = "rule matches", .version = "3.0",
        .author = "LakeShark", .icon = LAP_ICON_TOWER, .color = LAP_CYAN,
        .flags = LAP_CAP_RADIO_RX | LAP_CAP_SERVICE,
        .icon_art = {"  .----.  ", " / .--. \\ ", "| ( () ) |", " \\ '--' / ", "  '----'  ", " DETECTOR "},
        .enter = enter, .leave = leave, .draw = draw, .key = key, .touch = touch);
