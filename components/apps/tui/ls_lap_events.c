/* API 2.0 for native apps: decoded events, the board's actions and notices.
 * See lakeshark_app.h for the calls and ls_lap_radio.h for the tasks.
 *
 * Events: the app is one reader of ls_decoded, the board's single decoded
 * timeline. Decoders publish into it (pages, APRS, AIS), ADS-B arrives
 * from the event bus (ls_tmach.c) and the lap_io worker adds MeshCore's messages; none
 * of them ever waits on an app. The app reads its own cursor one event at a
 * time and is told how many it missed by falling behind.
 *
 * Actions go through ls_action_call, the single dispatch point the console
 * and .lsapp files use, with a grant made from the app's capabilities: read
 * always, tune with RADIO_RX, transmit with RADIO_TX. Never UI (an app must
 * not switch the screen away from itself), store or power. */
#include "ls_lap_radio.h"
#include "ls_lap_api.h"
#include "core/ls_decoded.h"
#include "ls_seen.h"
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#if defined(ESP_PLATFORM) && !defined(LS_LAP_HOST_TEST)
#include "esp_attr.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "core/event_bus.h"
#include "ls_action.h"
#include "ls_mesh.h"
#include "ls_notify.h"
#define BIG_ALLOC(n) heap_caps_calloc(1, (n), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)
#define DEVICE 1
static int64_t now_us(void) { return esp_timer_get_time(); }
#else
#define BIG_ALLOC(n) calloc(1, (n))
#define DEVICE 0
int (*ls_lap_host_action)(const char *, const char *, char *, size_t, uint32_t);
uint32_t (*ls_lap_events_host_clock)(void);    /* ms; the bench's, when it has one */
static int64_t now_us(void) { return ls_lap_events_host_clock ? (int64_t)ls_lap_events_host_clock() * 1000 : 0; }
#endif

/* One timeline, one set of kinds: the SDK's bits are the board's. */
_Static_assert((int)LAP_EV_PAGE == (int)LS_DECODED_PAGE && (int)LAP_EV_APRS == (int)LS_DECODED_APRS &&
               (int)LAP_EV_AIS == (int)LS_DECODED_AIS && (int)LAP_EV_ADSB == (int)LS_DECODED_ADSB &&
               (int)LAP_EV_MESH == (int)LS_DECODED_MESH, "LAP_EV_* and LS_DECODED_* must stay equal");
_Static_assert((int)LAP_EV_SAME == (int)LS_DECODED_SAME && (int)LAP_EV_ACARS == (int)LS_DECODED_ACARS &&
               (int)LAP_EV_SUBGHZ == (int)LS_DECODED_SUBGHZ && (int)LAP_EV_LORA == (int)LS_DECODED_LORA &&
               (int)LAP_EV_TEXT == (int)LS_DECODED_TEXT, "the 2.1 kinds too");
_Static_assert((int)LAP_EV_P25 == (int)LS_DECODED_P25 && (int)LAP_EV_REC == (int)LS_DECODED_REC &&
               (int)LAP_EV_DETECT == (int)LS_DECODED_DETECT, "and the 2.2 kinds");
_Static_assert((int)LAP_SEEN_RID == (int)LS_SEEN_RID && (int)LAP_SEEN_BLE == (int)LS_SEEN_BLE && (int)LAP_SEEN_WIFI == (int)LS_SEEN_WIFI &&
               (int)LAP_SEEN_LORA == (int)LS_SEEN_LORA && (int)LAP_SEEN_DRONE == (int)LS_SEEN_DRONE && (int)LAP_SEEN_REC == (int)DET_REC &&
               (int)LAP_SEEN_CATCH == (int)DET_CATCH && (int)LAP_SEEN_PAGER == (int)DET_POCSAG && (int)LAP_SEEN_P25 == (int)DET_P25,
               "LAP_SEEN_* and the board's sources must stay equal");
_Static_assert((int)LAP_SEEN_F_VALIDATED == (int)LS_SEEN_F_VALIDATED && (int)LAP_SEEN_F_FLOOD == (int)LS_SEEN_F_FLOOD &&
               (int)LAP_SEEN_F_NEW == (int)LS_SEEN_F_NEW && (int)LAP_SEEN_F_EMERGENCY == (int)LS_SEEN_F_EMERGENCY &&
               (int)LAP_SEEN_F_APPROACHING == (int)LS_SEEN_F_APPROACHING, "and the flags");
#define LAP_EV_ALL (LAP_EV_PAGE | LAP_EV_APRS | LAP_EV_AIS | LAP_EV_ADSB | LAP_EV_MESH)
/* A 2.0 app's lap_event_t.kind is a byte and it knows five kinds: it is
   never offered the others. */
#define LAP_EV_ALL21 LS_DECODED_KINDS
#define LAP_EV_ALL22 (LS_DECODED_KINDS | LS_DECODED_P25 | LS_DECODED_REC | LS_DECODED_DETECT)

static int reader = -1;                  /* screen task */
static uint32_t lost, delivered;         /* screen task */
static atomic_uint mesh_wanted;          /* the worker polls MeshCore while set */
static atomic_bool mesh_prime;
static char app_name[16], app_id[32];
static uint8_t app_hue;
static bool api21;                       /* the app knows the 2.1 kinds */
static bool api22;                       /* and the 2.2 ones, and lap_seen_* */
static uint32_t notice_ms, alert_ms;    /* screen task: rate limits */
static char last_source[32];             /* who decoded the event handed over last */
/* lap_emit: a token bucket, EMIT_BURST deep, refilled EMIT_PER_S a second. */
#define EMIT_PER_S 4
#define EMIT_BURST 8
static int64_t emit_at_us;
static int emit_tokens;
static uint32_t emit_dropped;

/* ---- events ------------------------------------------------------------- */

/* ADS-B contacts are published onto ls_decoded once, by TIME MACHINE's event
   bus listener (ls_tmach.c on_bus, subscribed at boot); the app reads them
   from there like every other kind. */
int lap_event_subscribe(uint32_t m) {
    if (!ls_lap_api_inside()) return LAP_RX_EINVAL;
    if (!(ls_lap_granted() & LAP_CAP_RADIO_RX)) return LAP_RX_EACCES;
    m &= api22 ? LAP_EV_ALL22 : api21 ? LAP_EV_ALL21 : LAP_EV_ALL;
    if (m && reader < 0) {
        reader = ls_decoded_open(m);
        if (reader < 0) return LAP_RX_ENOMEM;
    } else if (reader >= 0) {
        ls_decoded_kinds(reader, m);
    }
    if ((m & LAP_EV_MESH) && !atomic_load(&mesh_wanted)) atomic_store(&mesh_prime, true);
    atomic_store(&mesh_wanted, m & LAP_EV_MESH);
    return 0;
}
int lap_event_next(lap_event_t *out) {
    if (!ls_lap_api_inside() || !ls_lap_api_owns(out, sizeof(*out))) return LAP_RX_EINVAL;
    if (!(ls_lap_granted() & LAP_CAP_RADIO_RX)) return LAP_RX_EACCES;
    ls_decoded_t d;
    if (reader < 0 || !ls_decoded_next(reader, &d, &lost)) return 0;
    memset(out, 0, sizeof(*out));
    out->kind = d.kind < 256 ? (uint8_t)d.kind : 0;
    out->kind16 = d.kind;
    out->has_pos = d.has_pos;
    out->seq = ++delivered + lost;
    out->address = d.address;
    const int64_t age = now_us() - d.at_us;
    out->age_ms = age > 0 ? (uint32_t)(age / 1000) : 0;
    out->lat = d.lat;
    out->lon = d.lon;
    out->alt_ft = d.alt_ft;
    memcpy(out->from, d.from, sizeof(out->from) - 1);
    memcpy(out->text, d.text, sizeof(out->text) - 1);
    memcpy(last_source, d.source, sizeof(last_source));
    last_source[sizeof(last_source) - 1] = 0;
    return 1;
}
uint32_t lap_event_dropped(void) { return ls_lap_api_inside() ? lost : 0; }
int lap_event_source(char *out, size_t cap) {
    if (!ls_lap_api_inside() || !out || !cap || !ls_lap_api_owns(out, cap)) return LAP_RX_EINVAL;
    snprintf(out, cap, "%s", last_source);
    return (int)strlen(last_source);
}
/* API 2.1: the app as a decoder. Its decode goes on the board's timeline the
   way pocsag.c and aprs_store.c put theirs (RX_HEAD_DESIGN.md: plugin output
   is ls_decoded), tagged with the app's id and held to a few a second, so a
   runaway app cannot push the board's own decodes out of the ring. */
int lap_emit(const lap_decoded_t *d) {
    if (!ls_lap_api_inside()) return LAP_RX_EINVAL;
    if (!api21) return LAP_RX_EUNSUPPORTED;      /* a 2.0 app gets what 2.0 promised */
    if (!ls_lap_api_owns(d, sizeof(*d))) return LAP_RX_EINVAL;
    if (!(ls_lap_granted() & LAP_CAP_RADIO_RX)) return LAP_RX_EACCES;
    const lap_decoded_t c = *d;                  /* copy-in: read the app's memory once */
    if (!c.kind || (c.kind & (c.kind - 1)) || (c.kind & ~(api22 ? LAP_EV_ALL22 : LAP_EV_ALL21))) return LAP_RX_EINVAL;
    const int64_t t = now_us();
    if (emit_at_us) {
        const int64_t refill = (t - emit_at_us) * EMIT_PER_S / 1000000;
        if (refill > 0) {
            emit_tokens = emit_tokens + refill > EMIT_BURST ? EMIT_BURST : emit_tokens + (int)refill;
            emit_at_us += refill * 1000000 / EMIT_PER_S;
        }
    } else {
        emit_at_us = t ? t : 1;
        emit_tokens = EMIT_BURST;
    }
    if (emit_tokens <= 0) {
        emit_dropped++;
        return LAP_RX_EAGAIN;
    }
    emit_tokens--;
    ls_decoded_t e = {.kind = c.kind, .has_pos = c.has_pos, .address = c.address, .lat = c.lat,
                      .lon = c.lon, .alt_ft = c.alt_ft, .at_us = t};
    memcpy(e.from, c.from, sizeof(e.from) - 1);
    memcpy(e.text, c.text, sizeof(e.text) - 1);
    snprintf(e.source, sizeof(e.source), "%s", app_id[0] ? app_id : "app");
    ls_decoded_publish(&e);
    return 0;
}
uint32_t lap_emit_dropped(void) { return ls_lap_api_inside() ? emit_dropped : 0; }

/* API 2.2: what the board's watchers see (ls_seen.c). One app watches, the
   one loaded; its sources go when it closes (ls_lap_events_begin). */
int lap_seen_watch(uint32_t sources) {
    if (!ls_lap_api_inside()) return LAP_RX_EINVAL;
    if (!api22) return LAP_RX_EUNSUPPORTED;
    if (!(ls_lap_granted() & LAP_CAP_RADIO_RX)) return LAP_RX_EACCES;
    if (sources & ~(uint32_t)LS_SEEN_ALL) return LAP_RX_EINVAL;
    ls_seen_want(sources);
    return 0;
}
int lap_seen_next(lap_seen_t *out) {
    if (!ls_lap_api_inside() || !api22 || !ls_lap_api_owns(out, sizeof(*out))) return LAP_RX_EINVAL;
    if (!(ls_lap_granted() & LAP_CAP_RADIO_RX)) return LAP_RX_EACCES;
    ls_seen_t s;
    if (!ls_seen_take(&s)) return 0;
    memset(out, 0, sizeof(*out));
    out->source = s.source;
    out->category = s.category;
    out->section = s.section;
    out->rssi = s.rssi;
    out->flags = s.flags;
    out->band = s.band;
    out->entity = s.entity;
    out->a = s.a;
    out->b = s.b;
    out->hits_per_min = s.hits_per_min;
    const int64_t age = now_us() - s.at_us;
    out->age_ms = age > 0 ? (uint32_t)(age / 1000) : 0;
    memcpy(out->text, s.text, sizeof(out->text) - 1);
    return 1;
}
uint32_t lap_seen_dropped(void) { return ls_lap_api_inside() && api22 ? ls_seen_dropped() : 0; }

/* Worker: MeshCore's log, polled while an app listens for it. */
bool ls_lap_events_service(void) {
    if (!atomic_load(&mesh_wanted)) return false;
#if DEVICE
    static uint32_t seen_seq, last_id;
    static ls_mesh_msg_t *log;
    if (!log && !(log = BIG_ALLOC(sizeof(ls_mesh_msg_t) * LS_MESH_MAX_MSGS))) return false;
    const bool prime = atomic_exchange(&mesh_prime, false);
    const uint32_t seq = ls_mesh_msg_seq();
    if (seq == seen_seq && !prime) return true;
    seen_seq = seq;
    const int n = ls_mesh_messages(log, LS_MESH_MAX_MSGS);
    uint32_t newest = last_id;
    for (int i = 0; i < n; i++) {
        if ((int32_t)(log[i].id - newest) > 0) newest = log[i].id;
        if (prime || log[i].mine || (int32_t)(log[i].id - last_id) <= 0) continue;
        ls_decoded_t d = {.kind = LS_DECODED_MESH, .at_us = now_us()};
        snprintf(d.from, sizeof(d.from), "%s", log[i].sender);
        snprintf(d.text, sizeof(d.text), "%s", log[i].text);
        ls_decoded_publish(&d);
    }
    last_id = newest;
#endif
    return true;
}

/* ---- actions ------------------------------------------------------------ */

static bool text_ok(const char *s, size_t max) {
    if (!s) return false;
    for (size_t n = 0; n < max; n++) {
        if (!ls_lap_api_owns(s + n, 1)) return false;
        if (!s[n]) return true;
    }
    return false;
}
static uint32_t action_grant(void) {
    const uint32_t g = ls_lap_granted();
#if DEVICE
    return LS_CAP_READ | ((g & LAP_CAP_RADIO_RX) ? LS_CAP_TUNE : 0) | ((g & LAP_CAP_RADIO_TX) ? LS_CAP_TX : 0);
#else
    return 1u | ((g & LAP_CAP_RADIO_RX) ? 2u : 0) | ((g & LAP_CAP_RADIO_TX) ? 32u : 0);
#endif
}
int lap_action(const char *name, const char *args, char *out, size_t cap) {
    if (!ls_lap_api_inside() || !text_ok(name, 48) || (args && !text_ok(args, 192)) ||
        (out && (!cap || !ls_lap_api_owns(out, cap))))
        return LAP_ACT_BADARG;
    if (out) out[0] = 0;
#if DEVICE
    int index = -1;
    for (int i = 0; i < ls_action_count(); i++)
        if (!strcmp(ls_action_name(i), name)) { index = i; break; }
    if (index < 0) return LAP_ACT_UNKNOWN;
    /* Arguments as the console writes them: separated by spaces, except that
       a text argument in the last place takes the rest of the line. */
    const char *sig = ls_action_sig(index);
    const int want = (int)strlen(sig);
    ls_args_t in = {0};
    EXT_RAM_BSS_ATTR static char words[LS_ACT_MAX_ARGS][96];   /* screen task only */
    const char *p = args ? args : "";
    for (int a = 0; a < want; a++) {
        while (*p == ' ') p++;
        if (!*p) return LAP_ACT_BADARG;
        size_t n = (a == want - 1 && sig[a] == 's') ? strlen(p) : strcspn(p, " ");
        if (n >= sizeof(words[0])) return LAP_ACT_BADARG;
        memcpy(words[a], p, n); words[a][n] = 0;
        p += n;
        if (!ls_action_parse_arg(index, a, words[a], &in.v[a])) return LAP_ACT_BADARG;
        in.n++;
    }
    while (*p == ' ') p++;
    if (*p) return LAP_ACT_BADARG;
    ls_val_t v;
    const ls_act_status_t st = ls_action_call(name, &in, &v, (ls_cap_t)action_grant());
    if (out && st == LS_ACT_OK) {
        switch (v.kind) {
        case LS_VAL_INT: snprintf(out, cap, "%ld", v.i); break;
        case LS_VAL_FLOAT: snprintf(out, cap, "%g", (double)v.f); break;
        case LS_VAL_BOOL: snprintf(out, cap, "%s", v.i ? "on" : "off"); break;
        case LS_VAL_TEXT: snprintf(out, cap, "%s", v.s ? v.s : ""); break;
        default: break;
        }
    }
    switch (st) {
    case LS_ACT_OK: return LAP_ACT_OK;
    case LS_ACT_UNKNOWN: return LAP_ACT_UNKNOWN;
    case LS_ACT_DENIED: return LAP_ACT_DENIED;
    case LS_ACT_BADARG: return LAP_ACT_BADARG;
    case LS_ACT_UNAVAILABLE: return LAP_ACT_UNAVAILABLE;
    case LS_ACT_BUSY: return LAP_ACT_BUSY;
    default: return LAP_ACT_FAILED;
    }
#else
    return ls_lap_host_action ? ls_lap_host_action(name, args ? args : "", out, out ? cap : 0, action_grant())
                              : LAP_ACT_UNAVAILABLE;
#endif
}
int lap_action_count(void) {
#if DEVICE
    return ls_lap_api_inside() ? ls_action_count() : 0;
#else
    return 0;
#endif
}
bool lap_action_info(int index, char *name, size_t name_cap, char *sig, size_t sig_cap) {
    if (!ls_lap_api_inside() || (name && (!name_cap || !ls_lap_api_owns(name, name_cap))) ||
        (sig && (!sig_cap || !ls_lap_api_owns(sig, sig_cap))))
        return false;
#if DEVICE
    if (index < 0 || index >= ls_action_count()) return false;
    if (name) snprintf(name, name_cap, "%s", ls_action_name(index));
    if (sig) snprintf(sig, sig_cap, "%s", ls_action_sig(index));
    return true;
#else
    (void)index;
    return false;
#endif
}

/* ---- notices ------------------------------------------------------------ */

static bool notice(const char *title, const char *body, int level, bool alert) {
    if (!ls_lap_api_inside() || !text_ok(title, 64) || !text_ok(body, 256) || level < 0 || level > LAP_ALERT_ALARM)
        return false;
    const uint32_t t = (uint32_t)(now_us() / 1000);
    uint32_t *last = alert ? &alert_ms : &notice_ms;
    const uint32_t gap = alert ? 5000u : 1000u;
    if (*last && t - *last < gap) return false;
    *last = t ? t : 1;
#if DEVICE
    static const uint8_t HUE[8] = {TUI_WHITE, TUI_RED, TUI_GREEN, TUI_YELLOW, TUI_BLUE, TUI_MAGENTA, TUI_CYAN, TUI_WHITE};
    ls_notice_t n = {0};
    snprintf(n.title, sizeof(n.title), "%.9s %s", app_name, title);
    snprintf(n.body, sizeof(n.body), "%s", body);
    n.screen = -1;
    n.hue = HUE[app_hue & 7];
    if (alert) {
        n.accent = level == LAP_ALERT_ALARM ? TUI_RED : level == LAP_ALERT_WARN ? TUI_YELLOW : TUI_CYAN;
        n.steady = true;
        n.notify_visible = true;
        ls_notify_post(&n);              /* the board's ring and vibrate settings */
    } else {
        n.accent = n.hue;
        n.quiet = true;
        ls_notify_post_quiet(&n);
    }
#else
    ls_lap_hub_host_t *h = ls_lap_hub_host();
    if (alert) h->alerts++; else h->notices++;
    snprintf(h->last_title, sizeof(h->last_title), "%s", title);
    snprintf(h->last_body, sizeof(h->last_body), "%s", body);
    h->last_level = level;
    (void)app_name; (void)app_hue;
#endif
    return true;
}
bool lap_notify(const char *title, const char *body) { return notice(title, body, 0, false); }
bool lap_alert(const char *title, const char *body, int level) { return notice(title, body, level, true); }

/* ---- lifetime ----------------------------------------------------------- */

void ls_lap_events_begin(const lap_app_t *app) {
    if (reader >= 0) ls_decoded_close(reader);
    reader = -1;
    lost = delivered = 0;
    atomic_store(&mesh_wanted, 0u);
    notice_ms = alert_ms = 0;
    snprintf(app_name, sizeof(app_name), "%s", app ? app->name : "");
    snprintf(app_id, sizeof(app_id), "%s", app ? app->id : "");
    app_hue = app ? app->color : 0;
    api21 = app && app->api_major == 2 && app->api_minor >= 1;
    api22 = app && app->api_major == 2 && app->api_minor >= 2;
    ls_seen_want(0);
    last_source[0] = 0;
    emit_at_us = 0;
    emit_tokens = 0;
    emit_dropped = 0;
}
void ls_lap_events_end(void) { ls_lap_events_begin(NULL); }
