/* WARDRIVE's HUNT page (wd_page.h): what the signature table (wd_sig.h)
   and the tracker (wd_track.h) found on this drive.

   TARGETS: one row per tag (ALPR, BODYCAM, FOLLOWING...) with its count,
   its strongest, and when this drive first heard each one, minute by
   minute; a tag opens its targets, a target opens what it is, where it was
   heard and where its pattern comes from. VEHICLES: each group of radios
   that moves with the board, how long and how far, and a strip of how
   strong it has been step by step. ALERTS: per tag, OFF / BANNER / BUZZ
   (vibrate only, for discretion) / BOTH / SOUND, each tag with its own
   vibration rhythm.

   The scope in the corner is the board with every target placed where it
   was strongest; a ring runs out from the middle on each real alert and
   nothing moves otherwise. */
#include "../ls_alert.h"
#include "wd_page.h"
#include "wd_core.h"
#include "wd_feed.h"
#include "wd_sig.h"
#include "wd_track.h"
#include "wd_names.h"
#include "../ls_tui_ui.h"
#include "../ls_braille.h"
#include "../ls_notify.h"
#include "../ls_plate.h"
#include "../ls_calls.h"
#include "ls_haptic.h"
#include "core/settings.h"
#include "esp_timer.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

#ifdef ESP_PLATFORM
#include "esp_attr.h"
#else
#define EXT_RAM_BSS_ATTR
#endif

/* on the plates' ground (ls_plate.h) */
#define INK(c)  ls_plate_at((uint8_t)(c))
#define LIT(c)  INK((c) | TUI_BRIGHT)

void wd_hunt_attach(void);

/* ------------------------------------------------------------ the tags */

/* the shared alert modes and words (ls_alert.h) */
enum { M_OFF = LS_ALERT_OFF, M_BANNER = LS_ALERT_BANNER, M_BUZZ = LS_ALERT_BUZZ, M_SOUND = LS_ALERT_SOUND,
       M_BOTH = LS_ALERT_BOTH, M_N = LS_ALERT_N };
#define MODE LS_ALERT_NAMES

enum { C_ALPR, C_BODYCAM, C_POLICE, C_FOLLOW, C_TRACKER, C_SKIMMER, C_DRONE, C_GLASSES, C_HACK, C_CAMERA,
       C_SENSOR, C_VEHICLE, C_WATCH, NCLS };

#define E(x) ((uint8_t)(LS_HAPTIC_##x + 1))

static const char *const ART_ALPR[] = { " .--------.  ", " | (O) ===|= ", " '---++---'  ", "     ||      ", "  ___||___   " };
static const char *const ART_BODY[] = { "   ______    ", "  | (()) |   ", "  |  ==  |   ", "  |__**__|   ", "    |  |     " };
static const char *const ART_POL[]  = { "    _[R][B]_ ", "  _/   ||   \\", " |_o_______o_|", "  POLICE/FLEET", "             " };
static const char *const ART_FOL[]  = { "  [YOU]>     ", "    :  :     ", "    :  [?]>  ", "    :  :     ", "  same road  " };
static const char *const ART_TRK[]  = { "    .---.    ", "   /  o  \\   ", "   \\_____/   ", "     |_|     ", "  in a bag?  " };
static const char *const ART_SKM[]  = { " +---------+ ", " |=[=====]=| ", " |  _____  | ", " | |CARD!| | ", " +---------+ " };
static const char *const ART_UAS[]  = { " (x)-----(x) ", "    \\[]/     ", "    /  \\     ", " (x)     (x) ", "             " };
static const char *const ART_GLS[]  = { "  ___   ___  ", " (_o_)-(_o_) ", "  '  REC  '  ", "             ", "             " };
static const char *const ART_HAK[]  = { " .-------.   ", " | >_ ## |   ", " | [==]  |~~ ", " '-------'   ", "             " };
static const char *const ART_CAM[]  = { "   _____     ", "  |[O]  |>   ", "  '-----'    ", "     ||      ", "    _||_     " };
static const char *const ART_SEN[]  = { "    .-.      ", "  ((( o )))  ", "    '-'      ", "    /|\\      ", "   / | \\     " };
static const char *const ART_VEH[]  = { "    ______   ", "  _/  ||  \\_ ", " |_o______o_|", "  car radio  ", "             " };
static const char *const ART_WAT[]  = { "   .-\"\"-.    ", "  ( (><) )   ", "   '-..-'    ", "  your list  ", "             " };

typedef struct {
    uint16_t bit;
    const char *word, *what;
    uint8_t hue, def;
    uint8_t pat[3];             /* LS_HAPTIC_* + 1, 0 none */
    const char *const *art;
} cls_t;

static const cls_t CLS[NCLS] = {
    { WD_T_ALPR,     "ALPR",      "plate readers",      TUI_RED,     M_BOTH,   { E(WARN) },                    ART_ALPR },
    { WD_T_BODYCAM,  "BODYCAM",   "body cameras",       TUI_MAGENTA, M_BOTH,   { E(ALERT), E(ALERT) },         ART_BODY },
    { WD_T_POLICE,   "POLICE",    "fleet routers",      TUI_BLUE,    M_BOTH,   { E(ALERT), E(CLICK), E(ALERT) }, ART_POL },
    { WD_T_FOLLOW,   "FOLLOWING", "moving with you",    TUI_RED,     M_SOUND,  { E(WARN), E(WARN) },           ART_FOL },
    { WD_T_TRACKER,  "TRACKER",   "tags",               TUI_YELLOW,  M_OFF,    /* every Apple device beacons Find My; FOLLOWING is the alert */ { E(CLICK), E(CLICK), E(CLICK) }, ART_TRK },
    { WD_T_SKIMMER,  "SKIMMER",   "serial modules",     TUI_MAGENTA, M_BANNER, { E(CONFIRM), E(WARN) },        ART_SKM },
    { WD_T_DRONE,    "DRONE",     "drones",             TUI_CYAN,    M_BANNER, { E(CONFIRM), E(CONFIRM) },     ART_UAS },
    { WD_T_WEARABLE, "GLASSES",   "camera glasses",     TUI_MAGENTA, M_BANNER, { E(CLICK), E(ALERT) },         ART_GLS },
    { WD_T_HACKTOOL, "HACKTOOL",  "Flipper, Pineapple", TUI_GREEN,   M_BANNER, { E(CONFIRM) },                 ART_HAK },
    { WD_T_CAMERA,   "CAMERA",    "doorbells, CCTV",    TUI_WHITE,   M_OFF,    { E(CLICK) },                   ART_CAM },
    { WD_T_INFRA,    "SENSOR",    "gunshot sensors",    TUI_YELLOW,  M_BANNER, { E(ALERT), E(TICK), E(ALERT) }, ART_SEN },
    { WD_T_VEHICLE,  "VEHICLE",   "car radios",         TUI_CYAN,    M_OFF,    { E(CLICK), E(WARN) },          ART_VEH },
    { WD_T_WATCH,    "WATCHED",   "your sigs.txt list", TUI_GREEN,   M_SOUND,  { E(TICK), E(TICK), E(WARN) },  ART_WAT },
};

static uint8_t s_mode[NCLS];
static bool s_mode_read;

static void modes_save(void);
static void modes_read(void)
{
    if (s_mode_read) return;
    s_mode_read = true;
    const uint32_t a = settings_get_rec("wd_hunt_al0", 0xFFFFFFFFu), b = settings_get_rec("wd_hunt_al1", 0xFFFFFFFFu);
    for (int c = 0; c < NCLS; c++) {
        const uint32_t w = c < 10 ? a : b, sh = (uint32_t)(c < 10 ? c : c - 10) * 3;
        const unsigned m = w == 0xFFFFFFFFu ? CLS[c].def : (w >> sh) & 7;
        s_mode[c] = (uint8_t)(m < M_N ? m : CLS[c].def);
    }
    /* saved before the shared order (ls_alert.h): BOTH was 3 and SOUND 4 */
    if (settings_get_rec("wd_hunt_alv", 1) < 2) {
        if (a != 0xFFFFFFFFu || b != 0xFFFFFFFFu) {
            for (int c = 0; c < NCLS; c++)
                if (s_mode[c] == 3 || s_mode[c] == 4) s_mode[c] = (uint8_t)(7 - s_mode[c]);
            modes_save();
        }
        settings_set_rec("wd_hunt_alv", 2);
    }
}

static void modes_save(void)
{
    uint32_t a = 0, b = 0;
    for (int c = 0; c < NCLS; c++) {
        if (c < 10) a |= (uint32_t)s_mode[c] << (c * 3);
        else b |= (uint32_t)s_mode[c] << ((c - 10) * 3);
    }
    settings_set_rec("wd_hunt_al0", a);
    settings_set_rec("wd_hunt_al1", b);
}

/* The rhythm as marks: TICK . CLICK - CONFIRM = ALERT # WARN ### */
static void pat_text(int c, char *out, size_t n)
{
    static const char *const MK[] = { ".", "-", "=", "#", "###" };
    out[0] = 0;
    for (int i = 0; i < 3 && CLS[c].pat[i]; i++) {
        const size_t l = strlen(out);
        snprintf(out + l, n - l, "%s%s", i ? " " : "", MK[CLS[c].pat[i] - 1]);
    }
}

static void buzz(int c)
{
    for (int i = 0; i < 3 && CLS[c].pat[i]; i++) ls_haptic_play((ls_haptic_effect_t)(CLS[c].pat[i] - 1));
}

/* ------------------------------------------------------------ alerts */

static uint32_t real_ms(void) { return (uint32_t)(esp_timer_get_time() / 1000); }

static uint32_t s_cls_ms[NCLS], s_any_ms, s_poll_ms;
static uint16_t s_more[NCLS];
static ls_notice_t s_note;
static bool s_note_ready;
static char s_last[LS_NOTIFY_BODY];         /* the last alert, for the header */
static uint8_t s_last_hue = TUI_RED;
static int s_last_cls = -1;
/* the alerts raised, newest last, for the TARGETS page's foot */
#define LOG_N 8
typedef struct { uint32_t ms; uint8_t cls, mode; char text[56]; } log_t;
static log_t s_log[LOG_N];
static int s_log_head, s_log_n;

static int class_of(uint16_t tags, int why)
{
    if (why == WD_AL_FOLLOW || why == WD_AL_VEHICLE) return C_FOLLOW;
    for (int c = 0; c < NCLS; c++) if (c != C_FOLLOW && (tags & CLS[c].bit)) return c;
    return -1;
}

static void banner(int c, const char *body)
{
    memset(&s_note, 0, sizeof(s_note));
    snprintf(s_note.title, sizeof(s_note.title), "HUNT %s", CLS[c].word);
    snprintf(s_note.body, sizeof(s_note.body), "%s", body);
    s_note.hue = CLS[c].hue;
    s_note.accent = (uint8_t)(CLS[c].hue == TUI_WHITE ? TUI_CYAN : CLS[c].hue);
    s_note.screen = -1;
    s_note.quiet = true;        /* the sound and the buzz are ours, by the tag's choice */
    s_note.notify_visible = true;
    s_note.steady = true;
    s_note_ready = true;
}

/* An alert by its tag's choice, at most one a tag per 15 s and one in all
   per 4 s; what is held back is counted into the next one. */
static void raise(int c, const char *body, uint32_t r, bool test)
{
    const int m = s_mode[c];
    if (m == M_OFF && !test) return;
    const bool held = !test && ((r - s_cls_ms[c] < 15000 && s_cls_ms[c]) || (r - s_any_ms < 4000 && s_any_ms));
    if (!test) {                        /* every one goes in the log; a held one is marked so */
        log_t *l = &s_log[s_log_head];
        s_log_head = (s_log_head + 1) % LOG_N;
        if (s_log_n < LOG_N) s_log_n++;
        l->ms = r;
        l->cls = (uint8_t)c;
        l->mode = (uint8_t)(held ? M_OFF : m);
        snprintf(l->text, sizeof(l->text), "%s", body);
    }
    if (held) { s_more[c]++; return; }
    s_cls_ms[c] = s_any_ms = r;
    char t[LS_NOTIFY_BODY];
    if (s_more[c]) snprintf(t, sizeof(t), "%s  (+%u more)", body, (unsigned)s_more[c]);
    else snprintf(t, sizeof(t), "%s", body);
    s_more[c] = 0;
    snprintf(s_last, sizeof(s_last), "%s", t);
    s_last_hue = CLS[c].hue;
    s_last_cls = c;
    if (ls_alert_banner(m) || (test && m == M_OFF)) banner(c, t);
    if (ls_alert_buzz(m)) buzz(c);
    if (ls_alert_sound(m)) ls_notify_tone();
}

static void poll(void)
{
    const uint32_t r = real_ms();
    if (r - s_poll_ms < 200 && s_poll_ms) return;
    s_poll_ms = r;
    modes_read();
    for (int n = 0; n < 4; n++) {
        wd_alert_t a;
        char what[24] = "", body[96];
        int members = 0;
        wd_lock();
        const bool got = wd_track_alert(&a);
        if (got) {
            const wd_sig_info_t *s = a.sig ? wd_sig_at(a.sig - 1) : NULL;
            if (s) snprintf(what, sizeof(what), "%s", s->label);
            const wd_veh_t *v = a.veh ? wd_track_vehicle_num(a.veh) : NULL;
            if (v) members = v->n;
        }
        wd_unlock();
        if (!got) break;
        if (!what[0]) {
            char lb[LS_BLE_NAMES_LEN];
            if (a.name[0]) snprintf(what, sizeof(what), "%s", a.name);
            else if (a.kind == WD_BLE && ls_ble_names_device(a.addr, LS_BLE_NAMES_TYPE_UNKNOWN, lb, sizeof(lb)))
                snprintf(what, sizeof(what), "%s", lb);                 /* the name it gave before */
            else snprintf(what, sizeof(what), "%02X:%02X:%02X:%02X", a.addr[2], a.addr[3], a.addr[4], a.addr[5]);
        }
        const int c = class_of(a.tags, a.why);
        if (c < 0) continue;
        if (a.why == WD_AL_VEHICLE) snprintf(body, sizeof(body), "VEHICLE %u with you: %d radios, %s", a.veh, members, what);
        else if (a.why == WD_AL_FOLLOW) snprintf(body, sizeof(body), "%s %s is moving with you", wd_kind_short(a.kind), what);
        else snprintf(body, sizeof(body), "%s  %s  %d dBm", what, wd_kind_short(a.kind), a.rssi);
        raise(c, body, r, false);
    }
}

static bool probe(ls_notice_t *out)
{
    poll();
    if (!s_note_ready) return false;
    *out = s_note;
    s_note_ready = false;
    return true;
}

/* Alerts while any screen is up, not only this page: the coordinator calls
   this from WARDRIVE's enter(); the page calls it too on its first frame. */
void wd_hunt_attach(void)
{
    static bool done;
    if (done) return;
    done = true;
    ls_notify_add_probe(probe);
}

/* ------------------------------------------------------------ snapshot */

#define TGT_MAX 256

typedef struct {
    int16_t idx;
    uint8_t kind, addr[6];
    int8_t rssi, last;
    uint16_t tags, sig, veh;
    uint32_t first_ms, last_ms, seen;
    bool placed;
    float x, z;
    char name[33], label[24];
    uint8_t ad_have, ble_type;          /* Bluetooth: the advert's IDs, for names */
    uint16_t company, uuid16, appearance;
} tgt_t;

typedef struct {
    int n, strong;              /* strong: tgt index of the strongest heard lately */
    uint32_t last_ms;
    uint8_t bins[30];           /* first heard, by minute, the newest last */
} grp_t;

static EXT_RAM_BSS_ATTR wd_stats_t st;
static EXT_RAM_BSS_ATTR tgt_t T[TGT_MAX];
static int nt;
static grp_t G[NCLS];
static EXT_RAM_BSS_ATTR wd_veh_t V[WD_VEH_MAX];
static EXT_RAM_BSS_ATTR tgt_t VM[WD_VEH_MAX][WD_VEH_MEMBERS];
static int nv, nfollow;
static uint32_t steps;
/* the detail's device */
static bool d_on, d_found;
static uint8_t d_kind, d_addr[6];
static tgt_t D;
static wd_trk_t DT;
static char d_sig_id[24], d_sig_src[96];
static uint8_t d_alert;
static char s_signote[80];
static int s_nsig;

/* The names a detail shows, collected (wd_names_detail). */
typedef struct { int n; const char *k[5]; char v[5][LS_BLE_NAMES_LEN]; } dnames_t;
static void dn_add(void *ctx, const char *key, const char *value)
{
    dnames_t *d = ctx;
    if (d->n >= 5) return;
    d->k[d->n] = key;
    snprintf(d->v[d->n], sizeof(d->v[0]), "%s", value);
    d->n++;
}

static void label_for(const wd_dev_t *e, char *out, size_t n)
{
    const wd_sig_info_t *s = e->sig ? wd_sig_at(e->sig - 1) : NULL;
    char lb[LS_BLE_NAMES_LEN];
    const char *nm = s ? NULL : WD_DEV_LABEL(e, lb, NULL);     /* its name, else who made it */
    if (s) snprintf(out, n, "%s", s->label);
    else if (nm) snprintf(out, n, "%.*s", (int)n - 1, nm);
    else snprintf(out, n, "%02X:%02X:%02X:%02X:%02X:%02X", e->addr[0], e->addr[1], e->addr[2], e->addr[3], e->addr[4], e->addr[5]);
}

static void fill(tgt_t *t, int i, const wd_dev_t *e)
{
    t->idx = (int16_t)i;
    t->kind = e->kind;
    memcpy(t->addr, e->addr, 6);
    t->rssi = e->rssi;
    t->last = e->last_rssi;
    t->tags = e->tags;
    t->sig = e->sig;
    t->first_ms = e->first_ms;
    t->last_ms = e->last_ms;
    t->seen = e->seen;
    t->placed = e->flags & WD_F_PLACED;
    t->x = e->x;
    t->z = e->z;
    memcpy(t->name, e->name, sizeof(t->name));
    t->ad_have = e->ad_have;
    t->ble_type = e->ble_type;
    t->company = e->company;
    t->uuid16 = e->uuid16;
    t->appearance = e->appearance;
    label_for(e, t->label, sizeof(t->label));
    wd_trk_t k;
    t->veh = wd_track_info(i, &k) ? k.veh : 0;
}

static const uint16_t HUNT_TAGS = (uint16_t)~(WD_T_REGULAR | WD_T_NEWHERE | WD_T_PEER);

/* Everything the frame draws, copied under wd_lock. */
static void snapshot(uint32_t now)
{
    wd_lock();
    st = *wd_stats();
    nt = 0;
    memset(G, 0, sizeof(G));
    for (int c = 0; c < NCLS; c++) G[c].strong = -1;
    const int nd = wd_dev_count();
    for (int i = 0; i < nd; i++) {
        const wd_dev_t *e = wd_dev_at(i);
        if (!(e->tags & HUNT_TAGS)) continue;
        int slot = nt < TGT_MAX ? nt++ : -1;
        if (slot < 0) {                     /* full: keep the most recently heard */
            int old = 0;
            for (int k = 1; k < TGT_MAX; k++) if ((int32_t)(T[k].last_ms - T[old].last_ms) < 0) old = k;
            if ((int32_t)(e->last_ms - T[old].last_ms) <= 0) continue;
            slot = old;
        }
        fill(&T[slot], i, e);
    }
    for (int k = 0; k < nt; k++) {
        const tgt_t *t = &T[k];
        for (int c = 0; c < NCLS; c++) {
            if (!(t->tags & CLS[c].bit)) continue;
            grp_t *g = &G[c];
            g->n++;
            if ((int32_t)(t->last_ms - g->last_ms) > 0) g->last_ms = t->last_ms;
            const bool recent = now - t->last_ms < 60000;
            if (g->strong < 0 || (recent && (now - T[g->strong].last_ms >= 60000 || t->last > T[g->strong].last))) g->strong = k;
            const uint32_t ago = (now - t->first_ms) / 60000;
            if (ago < 30 && g->bins[29 - ago] < 255) g->bins[29 - ago]++;
        }
    }
    nv = wd_track_vehicles();
    for (int v = 0; v < nv; v++) {
        V[v] = *wd_track_vehicle(v);
        for (int m = 0; m < V[v].n; m++) {
            const wd_dev_t *e = V[v].idx[m] >= 0 ? wd_dev_at(V[v].idx[m]) : NULL;
            if (e) fill(&VM[v][m], V[v].idx[m], e);
            else {
                memset(&VM[v][m], 0, sizeof(VM[v][m]));
                VM[v][m].idx = -1;
                VM[v][m].kind = V[v].kind[m];
                memcpy(VM[v][m].addr, V[v].addr[m], 6);
                snprintf(VM[v][m].label, sizeof(VM[v][m].label), "(left the table)");
            }
        }
    }
    nfollow = wd_track_following();
    steps = wd_track_steps();
    d_found = false;
    if (d_on) {
        for (int i = 0; i < nd; i++) {
            const wd_dev_t *e = wd_dev_at(i);
            if (e->kind != d_kind || memcmp(e->addr, d_addr, 6)) continue;
            fill(&D, i, e);
            wd_track_info(i, &DT);
            const wd_sig_info_t *s = e->sig ? wd_sig_at(e->sig - 1) : NULL;
            snprintf(d_sig_id, sizeof(d_sig_id), "%s", s ? s->id : "");
            snprintf(d_sig_src, sizeof(d_sig_src), "%s", s ? s->source : (e->tags & WD_T_FOLLOW) ? "WARDRIVE tracker" : "the radio's own report");
            d_alert = s ? s->alert : 1;
            d_found = true;
            break;
        }
    }
    snprintf(s_signote, sizeof(s_signote), "%s", wd_sig_note());
    s_nsig = wd_sig_count();
    wd_unlock();
}

/* ------------------------------------------------------------ drawing kit */

/* everything on the plates' ground (ls_plate.h): values bright white,
   labels plain white, colour for a tag's class or a state only */
#define DIM      ls_plate_at(TUI_WHITE)
#define FAINT    ls_plate_at(TUI_BLUE)
#define TEXT     ls_plate_at(TUI_WHITE | TUI_BRIGHT)
#define SOLID(h) TUI_ATTR(TUI_BLACK, (uint8_t)((h) | TUI_BRIGHT))

static int nz(int v) { return v > 0 ? v : 0; }

static void put(tui_surface *sf, tui_rect clip, int x, int y, const char *s, uint8_t at)
{
    if (y >= clip.y && y < clip.y + clip.h) tui_put_str(sf, clip, x, y, s, at);
}

/* touch: what each drawn row answers to */
enum { H_TAB, H_GROUP, H_TGT, H_VEH, H_VMEM, H_MODE, H_TEST, H_BACK, H_UP, H_DOWN };
typedef struct { tui_rect r; uint8_t what; int16_t a, b; } hit_t;
static EXT_RAM_BSS_ATTR hit_t s_hit[96];
static int s_nhit;
static void hit_add(tui_rect r, int what, int a, int b)
{
    if (s_nhit < (int)(sizeof(s_hit) / sizeof(s_hit[0]))) s_hit[s_nhit++] = (hit_t){ r, (uint8_t)what, (int16_t)a, (int16_t)b };
}

/* A signal level as a thick meter: -100 dBm empty, -40 full. */
static void level(tui_surface *sf, tui_rect clip, int x, int y, int cells, int rssi, uint8_t hue)
{
    ls_plate_meter(sf, clip, x, y, cells, (float)(rssi + 100) / 60.0f, hue);
}

/* A name as a tab w wide: solid when it is the one picked, else in its hue. */
static void chip(tui_surface *sf, tui_rect clip, int x, int y, int w, const char *word, uint8_t hue, bool sel)
{
    char t[96];
    if (w > 90) w = 90;
    snprintf(t, sizeof(t), " %-*.*s", nz(w - 1), nz(w - 1), word);
    put(sf, clip, x, y, t, sel ? SOLID(hue) : LIT(hue));
}

/* A level in dBm as the big dot-matrix figures, the minus drawn as their
   middle stroke. Returns the column after it. */
static int big_dbm(tui_surface *sf, tui_rect clip, int x, int y, int dbm, uint8_t at)
{
    char t[8];
    if (dbm < 0) {
        for (int i = 0; i < 2; i++) ls_braille_put(sf, clip, x + i, y + 1, (int16_t)(0x2800 | 0x36), at);
        x += 3;
    }
    snprintf(t, sizeof(t), "%d", dbm < 0 ? -dbm : dbm);
    return ls_calls_big_text(sf, clip, x, y, t, at);
}

static bool s_imperial;
static void units(void)
{
    const uint32_t u = settings_get_rec("wd_units", 2);
    if (u != 2) s_imperial = u != 0;
    else s_imperial = st.fix.fix && st.fix.lat > 24 && st.fix.lat < 50 && st.fix.lon > -125 && st.fix.lon < -66;
}

static void dist_text(char *o, size_t n, float m)
{
    if (s_imperial) {
        if (m < 300) snprintf(o, n, "%.0f ft", (double)(m * 3.28084f));
        else snprintf(o, n, "%.1f mi", (double)(m / 1609.344f));
    } else {
        if (m < 1000) snprintf(o, n, "%.0f m", (double)m);
        else snprintf(o, n, "%.1f km", (double)(m / 1000));
    }
}

static void ago_text(char *o, size_t n, uint32_t ms)
{
    const unsigned s = ms / 1000;
    if (s < 5) snprintf(o, n, "now");
    else if (s < 60) snprintf(o, n, "%us ago", s);
    else if (s < 3600) snprintf(o, n, "%um ago", s / 60);
    else snprintf(o, n, "%uh%02u ago", s / 3600, s / 60 % 60);
}

static void span_text(char *o, size_t n, uint32_t ms)
{
    const unsigned s = ms / 1000;
    if (s < 3600) snprintf(o, n, "%u:%02u", s / 60, s % 60);
    else snprintf(o, n, "%u:%02u:%02u", s / 3600, s / 60 % 60, s % 60);
}

static int cls_main(uint16_t tags)
{
    for (int c = 0; c < NCLS; c++) if (tags & CLS[c].bit) return c;
    return -1;
}
static uint8_t tags_hue(uint16_t tags) { const int c = cls_main(tags); return c >= 0 ? CLS[c].hue : TUI_WHITE; }

static bool board_placed(void) { return st.origin && st.fix.fix && st.fix_ms; }

/* ------------------------------------------------------------ the scope */

#define SCOPE_W 18
#define SCOPE_H 9
static uint8_t cv[SCOPE_H][SCOPE_W], cva[SCOPE_H][SCOPE_W];

static void dot(int px, int py, int w, int h, uint8_t at)
{
    if (px < 0 || py < 0 || px >= w * 2 || py >= h * 4) return;
    cv[py / 4][px / 2] |= ls_braille_dot((unsigned)(3 - py % 4), px % 2);
    if (at) cva[py / 4][px / 2] = at;
}

static void ring(int cx, int cy, float r, int w, int h, uint8_t at)
{
    const int n = r < 4 ? 16 : (int)(r * 6);
    for (int i = 0; i < n; i++) {
        const float t = (float)i * 6.2831853f / (float)n;
        dot(cx + (int)lroundf(cosf(t) * r), cy + (int)lroundf(sinf(t) * r * 0.95f), w, h, at);
    }
}

static void scope(tui_surface *sf, tui_rect clip, int x, int y, int w, int h)
{
    if (w > SCOPE_W) w = SCOPE_W;
    if (h > SCOPE_H) h = SCOPE_H;
    memset(cv, 0, sizeof(cv));
    memset(cva, 0, sizeof(cva));
    const int cx = w, cy = h * 2;
    const float R = (float)(h * 2 - 1);
    ring(cx, cy, R, w, h, FAINT);
    ring(cx, cy, R * 0.5f, w, h, FAINT);
    /* the targets where they were strongest, out to the farthest one (200 m at least) */
    float far = 200;
    if (board_placed())
        for (int k = 0; k < nt; k++) if (T[k].placed) { const float d = hypotf(T[k].x - st.x, T[k].z - st.z); if (d > far && d < 20000) far = d; }
    if (board_placed())
        for (int k = 0; k < nt; k++) {
            if (!T[k].placed) continue;
            const float dx = T[k].x - st.x, dz = T[k].z - st.z, d = hypotf(dx, dz);
            if (d > far) continue;
            const float s = R / far;
            const uint8_t hue = tags_hue(T[k].tags);
            const uint8_t at = (T[k].tags & WD_T_FOLLOW) ? LIT(TUI_RED) : LIT(hue);
            dot(cx + (int)lroundf(dx * s), cy + (int)lroundf(dz * s * 0.95f), w, h, at);
            if (T[k].tags & (WD_T_ALPR | WD_T_BODYCAM | WD_T_FOLLOW)) dot(cx + (int)lroundf(dx * s) + 1, cy + (int)lroundf(dz * s * 0.95f), w, h, at);
        }
    /* a ring out from the middle for 1.2 s after each real alert */
    const uint32_t age = real_ms() - s_any_ms;
    if (s_any_ms && age < 1200) {
        ring(cx, cy, R * (float)age / 1200.0f, w, h, LIT(s_last_hue));
        ls_tui_invalidate();
    }
    for (int r = 0; r < h; r++)
        for (int c = 0; c < w; c++)
            if (cv[r][c]) ls_braille_put(sf, clip, x + c, y + r, (int16_t)(0x2800 | cv[r][c]), cva[r][c] ? cva[r][c] : FAINT);
    tui_put_char(sf, clip, x + w / 2, y + h / 2, '+', LIT(TUI_GREEN));
    if (board_placed()) {
        char t[16];
        dist_text(t, sizeof(t), far);
        put(sf, clip, x, y + h - 1, t, DIM);
    }
    put(sf, clip, x + w / 2, y, "N", DIM);   /* north up, like MAP */
}

/* ------------------------------------------------------------ header + tabs */

static int s_tab;               /* 0 targets, 1 vehicles, 2 alerts */
static bool s_wide;             /* landscape: the HUNT plate a column at the left */
enum { V_GROUPS, V_TARGETS, V_DETAIL };
static int s_view;
static int s_gsel, s_tsel, s_ttop, s_vsel, s_vtop, s_asel, s_atop;
static int s_cls = -1;          /* the tag TARGETS is showing */
static int s_ord[TGT_MAX], s_nord;

/* The alerts raised so far, newest first, from y down to bottom: one row
   each when w has room, else the tag and when over the text. */
static void recent(tui_surface *sf, tui_rect c, int x, int y, int w, int bottom)
{
    char t[80], u[24];
    ls_plate_tab(sf, c, x, y++, "RECENT ALERTS", TUI_RED);
    const uint32_t rn = real_ms();
    const bool one = w >= 40;
    for (int i = 0; i < s_log_n && y + (one ? 0 : 1) < bottom; i++) {
        const log_t *e = &s_log[(s_log_head - 1 - i + 2 * LOG_N) % LOG_N];
        const uint8_t hue = CLS[e->cls].hue;
        const char *mk = e->mode == M_BUZZ ? "~" : e->mode == M_SOUND ? "!" : e->mode == M_BOTH ? "*" : " ";
        ago_text(u, sizeof(u), rn - e->ms);
        if (one) {
            put(sf, c, x + 1, y, u, DIM);
            put(sf, c, x + 10, y, CLS[e->cls].word, LIT(hue));
            snprintf(t, sizeof(t), "%.*s", nz(w - 23), e->text);
            put(sf, c, x + 20, y, t, TEXT);
            put(sf, c, x + w - 1, y++, mk, LIT(hue));
            continue;
        }
        put(sf, c, x + 1, y, CLS[e->cls].word, LIT(hue));
        put(sf, c, x + 1 + (int)strlen(CLS[e->cls].word), y, mk, LIT(hue));
        put(sf, c, x + w - (int)strlen(u), y++, u, DIM);
        snprintf(t, sizeof(t), "%.*s", nz(w - 2), e->text);
        put(sf, c, x + 1, y++, t, TEXT);
    }
}

/* The HUNT plate: the scope, how many targets in big figures, and the
   loudest, the nearest and the last alert. */
static void header(tui_surface *sf, tui_rect r, uint32_t now)
{
    char t[64], u[24];
    if (st.running) {
        span_text(u, sizeof(u), now - st.start_ms);
        snprintf(t, sizeof(t), "%s %s", st.replay ? "REPLAY" : "DRIVE", u);
    } else snprintf(t, sizeof(t), "%s", st.ever ? "STOPPED" : "NO DRIVE YET");
    const tui_rect c = ls_plate(sf, r, r, TUI_RED, "HUNT", t, st.running ? TUI_GREEN : TUI_WHITE);
    if (c.w < 26 || c.h < 5) return;
    const int sh = c.h - 1 >= SCOPE_H ? SCOPE_H : c.h - 1, sw = sh >= SCOPE_H ? SCOPE_W : 16;
    const int y = c.y + 1;
    scope(sf, c, c.x, y, sw, sh);
    const int x = c.x + sw + 2, rw = c.x + c.w - x;
    /* the headline: how many are on the lists, red when something follows */
    snprintf(t, sizeof(t), "%d", nt);
    const int bw = ls_calls_big_width(t);
    ls_calls_big_text(sf, c, x, y, t, nfollow ? LIT(TUI_RED) : TEXT);
    const bool beside = rw - bw - 2 >= 12;
    const int lx = beside ? x + bw + 2 : x, ly = beside ? y : y + LS_CALLS_BIG_ROWS;
    put(sf, c, lx, ly, "TARGETS", TEXT);
    snprintf(t, sizeof(t), "%d FOLLOWING", nfollow);
    put(sf, c, lx, ly + 1, t, nfollow ? LIT(TUI_RED) : DIM);
    snprintf(t, sizeof(t), "%d VEHICLE%s", nv, nv == 1 ? "" : "S");
    put(sf, c, lx, ly + 2, t, nv ? LIT(TUI_CYAN) : DIM);
    /* strongest heard in the last minute, and the nearest by where it was strongest */
    int best = -1, near = -1;
    float nd = 1e9f;
    for (int k = 0; k < nt; k++) {
        if (now - T[k].last_ms < 60000 && (best < 0 || T[k].last > T[best].last)) best = k;
        if (board_placed() && T[k].placed) { const float d = hypotf(T[k].x - st.x, T[k].z - st.z); if (d < nd) { nd = d; near = k; } }
    }
    /* beside the scope under the count, or under the scope when narrow */
    int fx = x, fy = y + LS_CALLS_BIG_ROWS + 1, fw = rw;
    if (!beside || fw < 26) { fx = c.x; fy = y + sh + 1; fw = c.w; }
    const int fb = c.y + c.h;
    if (best >= 0 && fy < fb) {
        snprintf(u, sizeof(u), "%d", T[best].last);
        put(sf, c, fx, fy, "LOUDEST", DIM);
        snprintf(t, sizeof(t), "%.*s", nz(fw - 9 - (int)strlen(u)), T[best].label);
        put(sf, c, fx + 8, fy, t, LIT(tags_hue(T[best].tags)));
        put(sf, c, fx + fw - (int)strlen(u), fy++, u, TEXT);
    }
    if (near >= 0 && fy < fb) {
        dist_text(u, sizeof(u), nd);
        put(sf, c, fx, fy, "NEAREST", DIM);
        snprintf(t, sizeof(t), "%.*s", nz(fw - 9 - (int)strlen(u)), T[near].label);
        put(sf, c, fx + 8, fy, t, LIT(tags_hue(T[near].tags)));
        put(sf, c, fx + fw - (int)strlen(u), fy++, u, TEXT);
    }
    if (s_log_n && !beside && fb - fy >= 5) recent(sf, c, fx, fy + 1, fw, fb);    /* a tall column: the whole list */
    else if (fy < fb) {
        if (s_last[0]) {
            put(sf, c, fx, fy, "LAST", DIM);
            snprintf(t, sizeof(t), "%.*s", nz(fw - 8), s_last);
            put(sf, c, fx + 8, fy, t, LIT(s_last_hue));
        } else {
            snprintf(t, sizeof(t), "%.*s", nz(fw), s_signote);
            put(sf, c, fx, fy, t, DIM);
        }
    }
}

/* TARGETS / VEHICLES / ALERTS as big keys: solid when it is the page. */
static void tabs(tui_surface *sf, tui_rect r)
{
    static const char *const W[3] = { "TARGETS", "VEHICLES", "ALERTS" };
    static const uint8_t H[3] = { TUI_RED, TUI_CYAN, TUI_YELLOW };
    int on = 0;
    for (int c = 0; c < NCLS; c++) on += s_mode[c] != M_OFF;
    const int cnt[3] = { nt, nv, on };
    const int gap = 1, bw = (r.w - 2 * gap) / 3;
    for (int i = 0; i < 3; i++) {
        const int x = r.x + i * (bw + gap);
        const tui_rect b = tui_rect_make(x, r.y, i == 2 ? r.x + r.w - x : bw, r.h);
        char t[32];
        if (i == 2) snprintf(t, sizeof(t), "%s %d ON", W[i], cnt[i]);
        else snprintf(t, sizeof(t), "%s %d", W[i], cnt[i]);
        ls_plate_key(sf, r, b, t, H[i], i == s_tab, true);
        hit_add(b, H_TAB, i, 0);
    }
}

/* ------------------------------------------------------------ TARGETS */

static int group_list(int *out)
{
    int n = 0;
    for (int c = 0; c < NCLS; c++) if (G[c].n) out[n++] = c;
    return n;
}

static void empty_state(tui_surface *sf, tui_rect r)
{
    static const char *const ART[] = {
        "        .   :   .         ",
        "    .  ( listening )  .   ",
        "        '   :   '         ",
        "  ~~ ~~~ ~~ ~~~~ ~~ ~~~ ~~ ",
    };
    const tui_rect c = ls_plate(sf, r, r, TUI_RED, "TARGETS", NULL, 0);
    if (c.w < 30 || c.h < 4) return;
    int y = c.y + 1;
    for (int i = 0; i < 4; i++) put(sf, c, c.x + (c.w - 26) / 2, y++, ART[i], i == 3 ? LIT(TUI_BLUE) : LIT(TUI_CYAN));
    y++;
    char t[96];
    put(sf, c, c.x, y++, st.running ? "Nothing on the lists yet." : "Start a drive (S) to hunt.", TEXT);
    y++;
    snprintf(t, sizeof(t), "%d signatures: plate readers, body cams,", s_nsig);
    put(sf, c, c.x, y++, t, DIM);
    put(sf, c, c.x, y++, "trackers, drones, skimmers, glasses, cars.", DIM);
    put(sf, c, c.x, y++, "FOLLOWING needs a fix and a few km together.", DIM);
    snprintf(t, sizeof(t), "%.*s", nz(c.w), s_signote);
    put(sf, c, c.x, y + 1, t, DIM);
}

/* One row a tag: its name as a tab, how many, how loud its loudest is now
   and when last; its loudest one's name under it (beside it when wide);
   the tags with nothing yet; the alerts raised, newest first. */
static void groups(tui_surface *sf, tui_rect r, uint32_t now)
{
    int list[NCLS];
    const int n = group_list(list);
    if (!n) { empty_state(sf, r); return; }
    if (s_gsel >= n) s_gsel = n - 1;
    if (r.w < 40 || r.h < 8) return;
    const tui_rect c = tui_rect_make(r.x + 2, r.y + 1, r.w - 4, r.h - 2);     /* as ls_plate returns it */
    const int x0 = c.x, top_y = c.y + 3, bottom = c.y + c.h, avail = bottom - top_y;
    const int none = n < NCLS ? 2 : 0;
    const int airy = s_wide ? 2 : 3, tight = s_wide ? 1 : 2;
    const int rh = n * airy - 1 + none <= avail ? airy : tight;      /* air first; RECENT only in what is left */
    int fit = (avail - none + (rh == airy ? 1 : 0)) / rh;
    if (fit < 1) fit = 1;
    const int top = s_gsel >= fit ? s_gsel - fit + 1 : 0;
    char t[160], u[40];
    if (n > fit) snprintf(u, sizeof(u), "%d-%d OF %d", top + 1, top + fit < n ? top + fit : n, n);
    ls_plate(sf, r, r, TUI_RED, "BY TAG", n > fit ? u : NULL, TUI_WHITE);
    put(sf, c, x0 + 1, c.y + 1, "TAG", DIM);
    put(sf, c, x0 + 10, c.y + 1, "FOUND", DIM);
    put(sf, c, x0 + 17, c.y + 1, "LOUDEST", DIM);
    put(sf, c, x0 + 30, c.y + 1, "dBm", DIM);
    put(sf, c, x0 + 35, c.y + 1, "HEARD", DIM);
    if (s_wide) put(sf, c, x0 + 45, c.y + 1, "LOUDEST ONE", DIM);
    int y = top_y;
    for (int i = top; i < n && i < top + fit && y < bottom; i++) {
        const int k = list[i];
        const grp_t *g = &G[k];
        const uint8_t hue = CLS[k].hue;
        chip(sf, c, x0, y, 11, CLS[k].word, hue, i == s_gsel);
        snprintf(t, sizeof(t), "%3d", g->n);
        put(sf, c, x0 + 12, y, t, TEXT);
        const tgt_t *s = g->strong >= 0 ? &T[g->strong] : NULL;
        if (s) {
            const bool live = now - s->last_ms < 60000;
            level(sf, c, x0 + 17, y, 10, live ? s->last : -100, hue);
            snprintf(t, sizeof(t), "%4d", live ? s->last : s->rssi);
            put(sf, c, x0 + 29, y, t, live ? TEXT : DIM);
            ago_text(u, sizeof(u), now - g->last_ms);
            put(sf, c, x0 + 35, y, u, DIM);
            const int nx = s_wide ? x0 + 45 : x0 + 1, ny = s_wide ? y : y + 1;
            snprintf(t, sizeof(t), "%.*s", nz(c.x + c.w - nx), s->label);
            put(sf, c, nx, ny, t, DIM);
        }
        hit_add(tui_rect_make(r.x, y, r.w, rh), H_GROUP, i, k);
        y += rh;
    }
    if (rh == airy) y--;                /* no blank owed after the last */
    /* the tags with nothing yet */
    t[0] = 0;
    for (int k = 0; k < NCLS; k++) if (!G[k].n) { strncat(t, " ", sizeof(t) - strlen(t) - 1); strncat(t, CLS[k].word, sizeof(t) - strlen(t) - 1); }
    if (t[0] && y + 1 < bottom) {
        char line[180];
        snprintf(line, sizeof(line), "%.*s", nz(c.w - 9), t);
        put(sf, c, x0 + 1, y + 1, "NONE YET", DIM);
        put(sf, c, x0 + 9, y + 1, line, FAINT);
        y += 2;
    }
    /* the alerts raised so far in the room that is left (wide: the HUNT plate has them) */
    if (s_log_n && !s_wide && y + 3 <= bottom) recent(sf, c, x0, y + 1, c.w, bottom);
}

static void order_targets(void)
{
    s_nord = 0;
    if (s_cls < 0) return;
    for (int k = 0; k < nt; k++) if (T[k].tags & CLS[s_cls].bit) s_ord[s_nord++] = k;
    /* most recently heard first; a tie by strength */
    for (int i = 1; i < s_nord; i++) {
        const int v = s_ord[i];
        int j = i - 1;
        while (j >= 0 && ((int32_t)(T[s_ord[j]].last_ms - T[v].last_ms) < 0 ||
                          (T[s_ord[j]].last_ms == T[v].last_ms && T[s_ord[j]].last < T[v].last))) { s_ord[j + 1] = s_ord[j]; j--; }
        s_ord[j + 1] = v;
    }
}

/* A view under TARGETS: its plate with BACK and the title as tabs. */
static tui_rect sub_plate(tui_surface *sf, tui_rect r, const char *title, uint8_t hue, const char *right)
{
    const tui_rect c = ls_plate(sf, r, r, hue, NULL, right, TUI_WHITE);
    const int bw = ls_plate_tab(sf, r, r.x + 2, r.y, "< BACK", TUI_WHITE);
    hit_add(tui_rect_make(r.x, r.y, bw + 3, 2), H_BACK, 0, 0);
    ls_plate_tab(sf, r, r.x + 3 + bw, r.y, title, hue);
    return c;
}

/* One tag's targets: two rows each, the name as a tab and its level; then
   its name or address, when, and whether it came along. */
static void targets(tui_surface *sf, tui_rect r, uint32_t now)
{
    order_targets();
    const int k = s_cls;
    char t[96], u[40], w[24];
    if (r.w < 40 || r.h < 8) return;
    const tui_rect c = tui_rect_make(r.x + 2, r.y + 1, r.w - 4, r.h - 2);
    const int top_y = c.y + 3, avail = c.y + c.h - top_y;
    const int rh = s_nord * 3 - 1 <= avail ? 3 : 2;
    int fit = (avail + (rh == 3 ? 1 : 0)) / rh;
    if (fit < 1) fit = 1;
    if (s_tsel >= s_nord) s_tsel = s_nord - 1;
    if (s_tsel < 0) s_tsel = 0;
    if (s_tsel < s_ttop) s_ttop = s_tsel;
    if (s_tsel >= s_ttop + fit) s_ttop = s_tsel - fit + 1;
    if (s_nord > fit) snprintf(u, sizeof(u), "%d-%d OF %d", s_ttop + 1, s_ttop + fit < s_nord ? s_ttop + fit : s_nord, s_nord);
    else {                              /* all of them: the plate only as tall as they are */
        const int h = 2 + 3 + s_nord * rh - (rh == 3 ? 1 : 0) + 1;
        if (h < r.h) r.h = h;
    }
    sub_plate(sf, r, CLS[k].word, CLS[k].hue, s_nord > fit ? u : NULL);
    snprintf(t, sizeof(t), "%d", s_nord);
    put(sf, c, c.x, c.y + 1, t, TEXT);
    put(sf, c, c.x + (int)strlen(t) + 1, c.y + 1, CLS[k].what, DIM);
    const int lw = c.w - 20 < 32 ? c.w - 20 : 32;                 /* the name's tab, not the whole row */
    int y = top_y;
    for (int i = s_ttop; i < s_nord && i < s_ttop + fit && y + 1 < c.y + c.h; i++) {
        const tgt_t *g = &T[s_ord[i]];
        const bool sel = i == s_tsel, live = now - g->last_ms < 60000;
        const uint8_t hue = (g->tags & WD_T_FOLLOW) ? TUI_RED : CLS[k].hue;
        put(sf, c, c.x, y, wd_kind_tag(g->kind), DIM);
        chip(sf, c, c.x + 5, y, lw, g->label, hue, sel);
        level(sf, c, c.x + c.w - 13, y, 8, live ? g->last : -100, hue);
        snprintf(t, sizeof(t), "%4d", live ? g->last : g->rssi);
        put(sf, c, c.x + c.w - 4, y, t, live ? TEXT : DIM);
        if (g->sig && g->name[0]) snprintf(w, sizeof(w), "%.20s", g->name);
        else snprintf(w, sizeof(w), "%02X:%02X:%02X:%02X:%02X:%02X", g->addr[0], g->addr[1], g->addr[2], g->addr[3], g->addr[4], g->addr[5]);
        ago_text(u, sizeof(u), now - g->last_ms);
        snprintf(t, sizeof(t), "%s  %s", w, u);
        put(sf, c, c.x + 6, y + 1, t, DIM);
        if (g->tags & WD_T_FOLLOW) put(sf, c, c.x + c.w - 8, y + 1, "WITH YOU", LIT(TUI_RED));
        else if (g->veh) { snprintf(t, sizeof(t), "VEH %u", g->veh); put(sf, c, c.x + c.w - (int)strlen(t), y + 1, t, LIT(TUI_CYAN)); }
        hit_add(tui_rect_make(r.x, y, r.w, rh), H_TGT, i, s_ord[i]);
        y += rh;
    }
}

static void kv(tui_surface *sf, tui_rect r, int *y, const char *k, const char *v, uint8_t at)
{
    if (*y >= r.y + r.h) return;
    put(sf, r, r.x, *y, k, DIM);
    char t[128];
    snprintf(t, sizeof(t), "%.*s", nz(r.w - 11), v);
    put(sf, r, r.x + 11, (*y)++, t, at);
}

/* the tag words a device carries */
static void tag_words(uint16_t tags, char *o, size_t n)
{
    o[0] = 0;
    for (int b = 0; b < 16; b++) {
        if (!(tags & (1u << b))) continue;
        const char *w = wd_tag_word((uint16_t)(1u << b));
        if (!w[0]) continue;
        const size_t l = strlen(o);
        snprintf(o + l, n - l, "%s%s", l ? " " : "", w);
    }
}

/* One target: its class's art, its name, how loud in big figures; then
   what it is, when and where, in groups (a column of their own when wide). */
static void detail(tui_surface *sf, tui_rect r, uint32_t now)
{
    if (r.w < 40 || r.h < 8) return;
    if (!d_found) {
        const tui_rect c = sub_plate(sf, r, "GONE", TUI_RED, NULL);
        put(sf, c, c.x, c.y + 2, "That device left the session's table.", TEXT);
        return;
    }
    const int k = cls_main(D.tags);
    const uint8_t hue = k >= 0 ? CLS[k].hue : TUI_WHITE;
    /* the plate only as tall as the record: the art's eight rows, the record
       under them (beside them when wide), a blank between its groups if room */
    const bool two = r.w - 4 >= 70, air = (two ? r.h - 3 : r.h - 11) >= 16;
    /* Bluetooth: what the names say of it (its name before, company, service, looks, maker) */
    dnames_t dn = {0};
    if (D.kind == WD_BLE) {
        wd_dev_t e;
        memset(&e, 0, sizeof(e));
        e.kind = D.kind;
        memcpy(e.addr, D.addr, 6);
        memcpy(e.name, D.name, sizeof(e.name));
        e.ad_have = D.ad_have; e.ble_type = D.ble_type;
        e.company = D.company; e.uuid16 = D.uuid16; e.appearance = D.appearance;
        wd_names_detail(&e, dn_add, &dn);
    }
    const int nkv = (d_sig_id[0] ? 1 : 0) + 3 + (D.name[0] ? 1 : 0) + dn.n + 1 + (D.placed ? 1 + (board_placed() ? 1 : 0) : 0) +
                    (DT.placed ? 1 : 0) + (D.veh ? 1 : 0) + (DT.steps ? 1 : 0) + (air ? 2 : 0);
    const int need = (two ? (1 + nkv > 9 ? 1 + nkv : 9) : 9 + nkv) + 3;
    if (need < r.h) r.h = need;
    const tui_rect c = sub_plate(sf, r, k >= 0 ? CLS[k].word : "TARGET", hue, wd_kind_name(D.kind));
    const int lim = two ? c.x + 36 : c.x + c.w;            /* the left part's right edge */
    int y = c.y + 1;
    /* the art, and beside it the name, its tags and the level big */
    const char *const *art = k >= 0 ? CLS[k].art : ART_CAM;
    const bool live = now - D.last_ms < 60000;
    for (int i = 0; i < 5; i++) put(sf, c, c.x, y + i, art[i], live ? LIT(hue) : INK(hue));
    char t[128], u[32], v[160];
    const int tx = c.x + 15, tw = lim - tx;
    snprintf(t, sizeof(t), "%.*s", nz(tw), D.label);
    put(sf, c, tx, y, t, LIT(hue));
    tag_words(D.tags, v, sizeof(v));
    snprintf(t, sizeof(t), "%.*s", nz(tw), v);
    put(sf, c, tx, y + 1, t, DIM);
    const int bx = big_dbm(sf, c, tx, y + 2, live ? D.last : D.rssi, live ? TEXT : DIM);
    put(sf, c, bx + 1, y + 2, live ? "dBm NOW" : "dBm BEST", DIM);
    if (live) { snprintf(t, sizeof(t), "best %d", D.rssi); put(sf, c, bx + 1, y + 3, t, DIM); }
    y += 6;
    level(sf, c, c.x, y, 13, live ? D.last : -100, hue);
    put(sf, c, tx, y, live ? "IN RANGE" : "OUT OF RANGE", live ? LIT(TUI_GREEN) : DIM);
    snprintf(t, sizeof(t), "heard %u times", (unsigned)D.seen);
    const int hx = tx + (live ? 10 : 14);
    if (hx + (int)strlen(t) <= lim) put(sf, c, hx, y, t, DIM);
    else put(sf, c, tx, y + 1, t, DIM);
    /* the record: a column of its own when wide, else under the art */
    const tui_rect kr = two ? tui_rect_make(c.x + 37, c.y + 1, c.w - 37, c.h - 1)
                            : tui_rect_make(c.x, y + 2, c.w, c.y + c.h - (y + 2));
    y = kr.y;
    if (d_sig_id[0]) {
        snprintf(t, sizeof(t), "%s  (alert %u)", d_sig_id, d_alert);
        kv(sf, kr, &y, "SIGNATURE", t, TEXT);
    }
    kv(sf, kr, &y, "SOURCE", d_sig_src, TEXT);
    snprintf(t, sizeof(t), "%02X:%02X:%02X:%02X:%02X:%02X", D.addr[0], D.addr[1], D.addr[2], D.addr[3], D.addr[4], D.addr[5]);
    kv(sf, kr, &y, "ADDRESS", t, TEXT);
    if (D.name[0]) kv(sf, kr, &y, "NAME", D.name, TEXT);
    for (int i = 0; i < dn.n; i++) kv(sf, kr, &y, dn.k[i], dn.v[i], LIT(TUI_CYAN));
    if (air) y++;
    span_text(u, sizeof(u), D.first_ms - st.start_ms);
    ago_text(t, sizeof(t), now - D.first_ms);
    snprintf(v, sizeof(v), "+%s into the drive, %.40s", u, t);
    kv(sf, kr, &y, "FIRST", v, TEXT);
    ago_text(t, sizeof(t), now - D.last_ms);
    kv(sf, kr, &y, "LAST", t, TEXT);
    if (air) y++;
    if (D.placed) {
        const double lat = st.lat0 - D.z / st.m_lat, lon = st.lon0 + D.x / st.m_lon;
        snprintf(t, sizeof(t), "%.5f %.5f", lat, lon);
        kv(sf, kr, &y, "STRONGEST", t, TEXT);
        if (board_placed()) {
            const float dx = D.x - st.x, dz = D.z - st.z, d = hypotf(dx, dz);
            static const char *const P[8] = { "N", "NE", "E", "SE", "S", "SW", "W", "NW" };
            const float b = fmodf(atan2f(dx, -dz) * 57.29578f + 360.0f, 360.0f);
            dist_text(u, sizeof(u), d);
            snprintf(t, sizeof(t), "%s %s of here", u, P[(int)((b + 22.5f) / 45.0f) % 8]);
            kv(sf, kr, &y, "FROM HERE", t, LIT(TUI_GREEN));
        }
    }
    if (DT.placed) {
        char a[24], b2[24];
        dist_text(a, sizeof(a), DT.span_m);
        dist_text(b2, sizeof(b2), DT.with_m);
        snprintf(t, sizeof(t), "%s apart, %s of road, %u places", a, b2, DT.places);
        kv(sf, kr, &y, "HEARD ON", t, (D.tags & WD_T_FOLLOW) ? LIT(TUI_RED) : TEXT);
    }
    if (D.veh) { snprintf(t, sizeof(t), "VEHICLE %u", D.veh); kv(sf, kr, &y, "PART OF", t, LIT(TUI_CYAN)); }
    if (DT.steps) {
        snprintf(t, sizeof(t), "heard %u of the last 64 steps", DT.steps);
        kv(sf, kr, &y, "STEPS", t, DIM);
    }
}

/* ------------------------------------------------------------ VEHICLES */

/* The road: YOU at the front, each vehicle behind by how strong it was in
   the last step it was heard (stronger is nearer). */
static void convoy(tui_surface *sf, tui_rect c, int y)
{
    const int x0 = c.x, w = c.w;
    tui_hline(sf, c, x0, y - 1, w, '_', FAINT);
    tui_hline(sf, c, x0, y, w, '-', FAINT);
    tui_hline(sf, c, x0, y + 1, w, '_', FAINT);
    put(sf, c, x0, y, "[YOU]>", LIT(TUI_GREEN));
    for (int v = 0; v < nv; v++) {
        int last = -128;
        for (int i = WD_VEH_TRAIL - 1; i >= 0; i--) if (V[v].trail[i] > -128) { last = V[v].trail[i]; break; }
        const float f = last > -128 ? fmaxf(0, fminf(1, (-40 - last) / 60.0f)) : 1.0f;
        char t[12];
        snprintf(t, sizeof(t), "[%u]>", V[v].num);
        const int x = x0 + 7 + (int)(f * (float)(w - 12));
        put(sf, c, x, y, t, last > -128 ? LIT((V[v].tags & WD_T_FOLLOW) ? TUI_RED : TUI_CYAN) : DIM);
    }
}

/* Its strongest member each road step, oldest left: solid columns two rows
   tall in half-cell steps; a step it was not heard leaves the floor. */
static void trail(tui_surface *sf, tui_rect c, int x, int y, const wd_veh_t *e, uint8_t hue)
{
    for (int i = 0; i < WD_VEH_TRAIL; i++) {
        const int lv = e->trail[i];
        int h = 0;
        if (lv > -128) { h = 1 + (lv + 100) * 3 / 60; if (h < 1) h = 1; if (h > 4) h = 4; }
        for (int row = 0; row < 2; row++) {
            const int above = h - row * 2, yy = y + 1 - row;
            if (above >= 1) ls_plate_col(sf, c, x + i, yy, above, LIT(hue));
            else if (!row) tui_put_char(sf, c, x + i, yy, '.', INK(hue));      /* not heard: the track */
        }
    }
    char t[24];
    put(sf, c, x + WD_VEH_TRAIL + 2, y, "loudest", DIM);
    snprintf(t, sizeof(t), "last %u steps", (unsigned)WD_VEH_TRAIL);
    put(sf, c, x + WD_VEH_TRAIL + 2, y + 1, t, DIM);
}

static void vehicles(tui_surface *sf, tui_rect r, uint32_t now)
{
    char t[96], u[24], w[24];
    if (r.w < 40 || r.h < 8) return;
    if (!nv) {
        static const char *const ART[] = {
            "      ______               ______      ",
            "    _/  ||  \\_    . . .   _/  ||  \\_   ",
            "   |_o______o_|          |_o______o_|  ",
            "  ==============================        ",
        };
        const tui_rect c = ls_plate(sf, r, r, TUI_CYAN, "VEHICLES", NULL, 0);
        if (c.w < 40 || c.h < 4) return;
        int y = c.y + 1;
        for (int i = 0; i < 4; i++) put(sf, c, c.x + (c.w - 40) / 2, y++, ART[i], i == 3 ? FAINT : LIT(TUI_CYAN));
        y++;
        put(sf, c, c.x, y++, "No vehicle has come along yet.", TEXT);
        y++;
        put(sf, c, c.x, y++, "Radios heard together over km of road", DIM);
        put(sf, c, c.x, y++, "(a hotspot, a phone key, four TPMS) become", DIM);
        put(sf, c, c.x, y++, "one numbered vehicle here.", DIM);
        snprintf(t, sizeof(t), "%u road steps of %.0f m so far", (unsigned)steps, (double)WD_TRK_STEP_M);
        put(sf, c, c.x, y + 1, t, DIM);
        return;
    }
    int all = 2 + 5;                    /* the plate only as tall as they are when all fit */
    for (int v = 0; v < nv; v++) all += 5 + V[v].n;
    if (all < r.h) r.h = all;
    const tui_rect c = ls_plate(sf, r, r, TUI_CYAN, "VEHICLES", NULL, 0);
    convoy(sf, c, c.y + 2);
    /* blocks, the selected one kept on screen */
    const int bottom = c.y + c.h;
    if (s_vsel >= nv) s_vsel = nv - 1;
    if (s_vsel < s_vtop) s_vtop = s_vsel;
    int y = c.y + 5;
    for (int v = s_vtop; v < nv; v++) {
        const wd_veh_t *e = &V[v];
        const int need = 4 + e->n;
        if (y + need > bottom && v > s_vsel) break;
        if (y + need > bottom && v == s_vsel && v > s_vtop) { s_vtop++; break; }
        const bool sel = v == s_vsel, live = now - e->last_ms < 60000;
        const uint8_t hue = (e->tags & WD_T_FOLLOW) ? TUI_RED : TUI_CYAN;
        snprintf(t, sizeof(t), "VEHICLE %u", e->num);
        chip(sf, c, c.x, y, 12, t, hue, sel);
        span_text(u, sizeof(u), e->last_ms - e->first_ms);
        dist_text(w, sizeof(w), e->with_m);
        snprintf(t, sizeof(t), "with you %s, %s", u, w);
        put(sf, c, c.x + 13, y, t, TEXT);
        put(sf, c, c.x + c.w - (live ? 8 : 4), y, live ? "IN RANGE" : "GONE", live ? LIT(TUI_GREEN) : DIM);
        hit_add(tui_rect_make(r.x, y, r.w, 3), H_VEH, v, 0);
        trail(sf, c, c.x + 1, y + 1, e, hue);
        y += 4;
        for (int m = 0; m < e->n && y < bottom; m++) {
            const tgt_t *g = &VM[v][m];
            const bool ml = g->idx >= 0 && now - g->last_ms < 60000;
            put(sf, c, c.x + 1, y, wd_kind_tag(g->kind), DIM);
            snprintf(t, sizeof(t), "%.*s", nz(c.w - 21), g->label);
            put(sf, c, c.x + 6, y, t, LIT(tags_hue(g->tags)));
            if (g->idx >= 0) {
                level(sf, c, c.x + c.w - 13, y, 8, ml ? g->last : -100, hue);
                snprintf(t, sizeof(t), "%4d", ml ? g->last : g->rssi);
                put(sf, c, c.x + c.w - 4, y, t, ml ? TEXT : DIM);
            }
            hit_add(tui_rect_make(r.x, y, r.w, 1), H_VMEM, v, m);
            y++;
        }
        y++;
    }
}

/* ------------------------------------------------------------ ALERTS */

/* The width the mode keys need on one row. */
static int mode_keys_w(void)
{
    int sum = 0;
    for (int m = 0; m <= M_N; m++) sum += (int)strlen(m < M_N ? MODE[m] : "TEST") + 2 + 1;
    return sum - 1;
}

/* The selected tag's choice as big keys, the shared modes (ls_alert.h) and
   TEST, the whole width; on two rows of three when one row is too narrow. */
static void mode_keys(tui_surface *sf, tui_rect c, int x, int y, int w, int h, int k, int rows)
{
    enum { NK = M_N + 1 };
    const int per = rows > 1 ? (NK + 1) / 2 : NK, gap = 1;
    int bw[NK];
    for (int row = 0; row * per < NK; row++) {
        const int m0 = row * per, m1 = m0 + per < NK ? m0 + per : NK;
        int sum = 0;
        for (int m = m0; m < m1; m++) { bw[m] = (int)strlen(m < M_N ? MODE[m] : "TEST") + 2; sum += bw[m]; }
        const int extra = w - sum - gap * (m1 - m0 - 1);
        if (extra > 0) for (int m = m0; m < m1; m++) bw[m] += extra / (m1 - m0) + (m - m0 < extra % (m1 - m0) ? 1 : 0);
    }
    int bx = x;
    for (int m = 0; m < NK; m++) {
        if (m && m % per == 0) { bx = x; y += h; }
        const tui_rect b = tui_rect_make(bx, y, bw[m], h);
        if (m < M_N) {
            ls_plate_key(sf, c, b, MODE[m], m == M_OFF ? TUI_WHITE : CLS[k].hue, s_mode[k] == m, true);
            hit_add(b, H_MODE, k, m);
        } else {
            ls_plate_key(sf, c, b, "TEST", TUI_GREEN, false, true);
            hit_add(b, H_TEST, k, 0);
        }
        bx += bw[m] + gap;
    }
}

/* One row a tag with its choice as a chip; the selected one opens into big
   keys. Tapping a row picks it. */
static void alerts(tui_surface *sf, tui_rect r)
{
    char t[64], u[40];
    if (r.w < 46 || r.h < 10) return;
    const tui_rect c = tui_rect_make(r.x + 2, r.y + 1, r.w - 4, r.h - 2);
    const int top_y = c.y + 4, bottom = c.y + c.h, avail = bottom - top_y;
    const int krows = mode_keys_w() > c.w ? 2 : 1;                   /* the keys wrap when narrow */
    const int key_h = (avail >= 8 + 3 * (krows - 1) ? 3 : 1) * krows, exp_h = 2 + key_h;   /* name, keys, a blank */
    const int rhc = exp_h + 2 * (NCLS - 1) - 1 <= avail ? 2 : 1;
    if (s_asel < s_atop) s_atop = s_asel;
    for (;;) {                          /* the selected one wholly on screen */
        int need = 1 + key_h;
        for (int k = s_atop; k < s_asel; k++) need += rhc;
        if (need <= avail || s_atop >= s_asel) break;
        s_atop++;
    }
    int last = s_atop, yy = 0;
    for (int k = s_atop; k < NCLS; k++) {
        if (yy + (k == s_asel ? 1 + key_h : 1) > avail) break;
        last = k;
        yy += k == s_asel ? exp_h : rhc;
    }
    const bool more = s_atop > 0 || last < NCLS - 1;
    if (more) snprintf(u, sizeof(u), "%d-%d OF %d", s_atop + 1, last + 1, NCLS);
    ls_plate(sf, r, r, TUI_YELLOW, "ALERTS", more ? u : NULL, TUI_WHITE);
    put(sf, c, c.x, c.y + 1, "BUZZ: vibrate only, no banner, no sound.", TEXT);
    put(sf, c, c.x, c.y + 2, "ENTER or TEST plays a tag's rhythm.", DIM);
    int y = top_y;
    for (int k = s_atop; k <= last; k++) {
        const bool sel = k == s_asel;
        const uint8_t hue = CLS[k].hue;
        char p[24];
        pat_text(k, p, sizeof(p));
        const bool felt = ls_alert_buzz(s_mode[k]);
        chip(sf, c, c.x, y, 11, CLS[k].word, hue, sel);
        const int px = sel ? c.x + c.w - (int)strlen(p) : c.x + c.w - 10 - (int)strlen(p);
        snprintf(t, sizeof(t), "%.*s", nz(px - c.x - 13), CLS[k].what);
        put(sf, c, c.x + 12, y, t, DIM);
        put(sf, c, px, y, p, felt ? LIT(hue) : FAINT);
        if (sel) {
            mode_keys(sf, c, c.x, y + 1, c.w, key_h / krows, k, krows);
            y += exp_h;
            continue;
        }
        snprintf(t, sizeof(t), " %s ", MODE[s_mode[k]]);
        put(sf, c, c.x + c.w - (int)strlen(t), y, t, s_mode[k] == M_OFF ? DIM : SOLID(hue));
        hit_add(tui_rect_make(r.x, y, r.w, rhc), H_MODE, k, s_mode[k]);
        y += rhc;
    }
}

static void test_alert(int c)
{
    char p[24], body[96];
    pat_text(c, p, sizeof(p));
    snprintf(body, sizeof(body), "TEST: %s alerts look like this  %s", CLS[c].word, p);
    raise(c, body, real_ms(), true);
}

static void set_mode(int c, int m)
{
    s_mode[c] = (uint8_t)((m + M_N) % M_N);
    modes_save();
}

/* ------------------------------------------------------------ page */

static void open_detail(const tgt_t *g)
{
    d_on = true;
    d_kind = g->kind;
    memcpy(d_addr, g->addr, 6);
    s_view = V_DETAIL;
}

static void draw(tui_surface *sf, tui_rect a, uint32_t now)
{
    wd_hunt_attach();
    modes_read();
    poll();
    if (s_note_ready) { ls_notify_post(&s_note); s_note_ready = false; }
    snapshot(now);
    units();
    s_nhit = 0;
    tui_rect head, tb, body;
    const bool wide = s_wide = a.w >= 90;
    if (wide) {                         /* the HUNT plate a column; the tabs over the page */
        const int lw = 36, bx = a.x + lw + 1, bw = a.w - lw - 1, th = a.h >= 16 ? 3 : 1;
        head = tui_rect_make(a.x, a.y, lw, a.h);
        tb = tui_rect_make(bx, a.y, bw, th);
        body = tui_rect_make(bx, a.y + th + 1, bw, a.h - th - 1);
    } else {
        const int hh = 10, th = a.h >= 40 ? 3 : 1;
        head = tui_rect_make(a.x, a.y, a.w, hh);
        tb = tui_rect_make(a.x, a.y + hh + 1, a.w, th);
        body = tui_rect_make(a.x, tb.y + th + 1, a.w, a.y + a.h - (tb.y + th + 1));
    }
    header(sf, head, now);
    tabs(sf, tb);
    if (s_tab == 1) vehicles(sf, body, now);
    else if (s_tab == 2) alerts(sf, body);
    else if (s_view == V_DETAIL) detail(sf, body, now);
    else if (s_view == V_TARGETS && s_cls >= 0 && G[s_cls].n) targets(sf, body, now);
    else { s_view = V_GROUPS; groups(sf, body, now); }
}

static void tab(int t)
{
    s_tab = (t + 3) % 3;
    if (s_tab != 0) d_on = false;
    if (s_tab == 0 && s_view == V_DETAIL) s_view = s_cls >= 0 ? V_TARGETS : V_GROUPS;
}

static bool back(void)
{
    if (s_tab == 0 && s_view == V_DETAIL) { s_view = s_cls >= 0 && s_cls < NCLS ? V_TARGETS : V_GROUPS; d_on = false; return true; }
    if (s_tab == 0 && s_view == V_TARGETS) { s_view = V_GROUPS; return true; }
    if (s_tab != 0) { s_tab = 0; d_on = false; return true; }
    return false;
}

static bool key(ls_tk_t k, char ch, uint32_t now)
{
    (void)now;
    modes_read();
    if (k == LS_TK_ESC || k == LS_TK_BACKSPACE) return back();
    if (k == LS_TK_TAB) { tab(s_tab + 1); return true; }
    if (ch == 't' || ch == 'T') { if (s_tab == 2) test_alert(s_asel); else tab(0); return true; }
    if (ch == 'v' || ch == 'V') { tab(1); return true; }
    if (ch == 'a' || ch == 'A') { tab(2); return true; }
    const int dy = k == LS_TK_UP ? -1 : k == LS_TK_DOWN ? 1 : k == LS_TK_PGUP ? -8 : k == LS_TK_PGDN ? 8 : 0;
    if (s_tab == 2) {
        if (dy) { s_asel += dy; if (s_asel < 0) s_asel = 0; if (s_asel >= NCLS) s_asel = NCLS - 1; return true; }
        if (k == LS_TK_LEFT || k == LS_TK_RIGHT) { set_mode(s_asel, s_mode[s_asel] + (k == LS_TK_LEFT ? -1 : 1)); return true; }
        if (k == LS_TK_ENTER) { test_alert(s_asel); return true; }
        return false;
    }
    if (s_tab == 1) {
        if (dy) { s_vsel += dy; if (s_vsel < 0) s_vsel = 0; if (s_vsel >= nv) s_vsel = nv > 0 ? nv - 1 : 0; return true; }
        if (k == LS_TK_LEFT || k == LS_TK_RIGHT) { tab(s_tab + (k == LS_TK_LEFT ? -1 : 1)); return true; }
        if (k == LS_TK_ENTER && s_vsel < nv && V[s_vsel].n) {
            tab(0);
            open_detail(&VM[s_vsel][0]);
            s_cls = C_VEHICLE;
            return true;
        }
        return false;
    }
    if (s_view == V_GROUPS) {
        int list[NCLS];
        const int n = group_list(list);
        if (dy) { s_gsel += dy; if (s_gsel < 0) s_gsel = 0; if (s_gsel >= n) s_gsel = n > 0 ? n - 1 : 0; return true; }
        if (k == LS_TK_LEFT || k == LS_TK_RIGHT) { tab(s_tab + (k == LS_TK_LEFT ? -1 : 1)); return true; }
        if (k == LS_TK_ENTER && n) { s_cls = list[s_gsel < n ? s_gsel : 0]; s_view = V_TARGETS; s_tsel = s_ttop = 0; return true; }
        return false;
    }
    if (s_view == V_TARGETS) {
        if (dy) { s_tsel += dy; if (s_tsel < 0) s_tsel = 0; if (s_tsel >= s_nord) s_tsel = s_nord > 0 ? s_nord - 1 : 0; return true; }
        if (k == LS_TK_LEFT) return back();
        if (k == LS_TK_ENTER && s_tsel < s_nord) { open_detail(&T[s_ord[s_tsel]]); return true; }
        return false;
    }
    if (s_view == V_DETAIL && k == LS_TK_LEFT) return back();
    return false;
}

static bool touch(int x, int y, uint32_t now)
{
    (void)now;
    for (int i = 0; i < s_nhit; i++) {
        const hit_t *h = &s_hit[i];
        if (!tui_rect_contains(h->r, x, y)) continue;
        switch (h->what) {
        case H_TAB: tab(h->a); return true;
        case H_BACK: back(); return true;
        case H_GROUP: s_gsel = h->a; s_cls = h->b; s_view = V_TARGETS; s_tsel = s_ttop = 0; return true;
        case H_TGT: s_tsel = h->a; open_detail(&T[h->b]); return true;
        case H_VEH: s_vsel = h->a; return true;
        case H_VMEM: if (VM[h->a][h->b].idx >= 0) { tab(0); open_detail(&VM[h->a][h->b]); s_cls = C_VEHICLE; } return true;
        case H_MODE: s_asel = h->a; set_mode(h->a, h->b); return true;
        case H_TEST: s_asel = h->a; test_alert(h->a); return true;
        default: break;
        }
    }
    return false;
}

const wd_page_t wd_page_hunt = { draw, key, touch, NULL };
