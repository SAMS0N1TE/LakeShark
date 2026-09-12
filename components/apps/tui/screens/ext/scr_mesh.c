/* MESH: a messaging app for the MeshCore node. */

#include "../../ls_tui_screen.h"
#include "../../ls_text.h"

#include <stdio.h>
/* strtol and strtod, for the frequency and location fields. Implicit before
   this: the firmware build gets stdlib.h through an IDF header and never
   complained, so strtod was declared to return int and a typed latitude
   would have been truncated. The host bench compiles this file on its own
   and said so. */
#include <stdlib.h>
#include <string.h>

#include "../../ls_tui_ui.h"
#include "../../ls_icons.h"
/* The typed-text overlay. A router concern, opened from here. */
#include "../../ls_keyboard.h"
/* How far away a node that advertises a position is. */
#include "../../ls_geo.h"
/* What this screen reports to the rest of the unit. */
#include "../../ls_notify.h"
/* Where a node with a location goes when you ask to see it. */
#include "../../ls_app.h"
#include "../../ls_map.h"

#include "core/settings.h"
#include "ls_gps.h"
#include "ls_mesh.h"
#include "ls_lora.h"
#include "esp_attr.h"

/* ------------------------------------------------------------------ state */

typedef enum { PAGE_CHAT = 0, PAGE_NODES, PAGE_SETUP, PAGE__COUNT } mesh_page_t;

static mesh_page_t s_page;

static tui_rect s_page_bar;
static tui_rect s_setup_rect;
static int    s_sel;               /* peer cursor on NODES                  */

static bool     s_detail;
static char     s_detail_id[17];
static tui_rect s_node_rect;       /* where the list was drawn, for taps    */
#define DETAIL_ACTS 4
static tui_rect s_act_rect[DETAIL_ACTS];

/* Portrait CHAT: where the "write something" target was drawn. */
static tui_rect s_compose_rect;
/* Where the destination chip was drawn, for the tap that clears it. */
static tui_rect s_lock_chip;

/* The chat can be held on one node. */

static char s_lock_id[17];
static char s_lock_name[LS_MESH_PEER_NAME];

static bool chat_locked(void) { return s_lock_id[0] != 0; }

static const char *lock_label(void)
{
    return s_lock_name[0] ? s_lock_name : s_lock_id;
}

static void chat_lock_to(const char *id, const char *name)
{
    snprintf(s_lock_id, sizeof(s_lock_id), "%s", id ? id : "");
    snprintf(s_lock_name, sizeof(s_lock_name), "%s", name ? name : "");
}

static void chat_unlock(void)
{
    s_lock_id[0] = 0;
    s_lock_name[0] = 0;
}

/* Portrait: where the arm/disarm control was drawn. */
static tui_rect s_tx_rect;
static int    s_field;             /* field cursor on SETUP                 */
/* Which SETUP field the text overlay is editing, or -1. */

static int    s_edit_field = -1;

#define COMPOSE_MAX 64
static char s_compose[COMPOSE_MAX];
static int  s_compose_len;

#define EDIT_MAX 48   /* base64 of 32 bytes is 44 + NUL () */
static char s_edit[EDIT_MAX];

static uint32_t s_seen_msg;        /* last message sequence drawn           */
static uint32_t s_seen_ev;
static int      s_pulse;
static int      s_blink;           /* cursor blink, counts frames           */
static char     s_flash[40];       /* transient result line                 */
static int      s_flash_ttl;

static int      s_air_frames;      /* frames of animation left               */
static int      s_air_total;       /* what it started at, for the bar        */

static void flash(const char *m) { snprintf(s_flash, sizeof(s_flash), "%s", m); s_flash_ttl = 50; }

/* Scratch buffers, and they are NOT on the stack. */

EXT_RAM_BSS_ATTR static ls_mesh_msg_t  s_msg_buf[LS_MESH_MAX_MSGS];
EXT_RAM_BSS_ATTR static ls_mesh_peer_t s_peer_buf[LS_MESH_MAX_PEERS];
EXT_RAM_BSS_ATTR static float          s_scope_buf[LS_MESH_SCOPE_N];

EXT_RAM_BSS_ATTR static ls_mesh_event_t s_ev_buf[LS_MESH_MAX_EVENTS];

/* --------------------------------------------------------------- fragments */

static uint8_t A(uint8_t fg, uint8_t bg) { return TUI_ATTR(fg, bg); }

/* Two greys, and the difference matters. */

#define DIM_FG    TUI_WHITE
#define FAINT_FG  (TUI_BLACK | TUI_BRIGHT)

static bool mesh_here(double *lat, double *lon)
{
    ls_gps_state_t g;
    ls_gps_get(&g);
    if (g.fix) { *lat = g.lat_deg; *lon = g.lon_deg; return true; }
    float hlat, hlon;
    if (settings_get_home(&hlat, &hlon)) {
        *lat = (double)hlat; *lon = (double)hlon; return true;
    }
    return false;
}

static const char *state_word(const ls_mesh_stats_t *st)
{
    if (!st->running)   return "OFFLINE";
    if (st->tx_enabled) return "ARMED";
    return "LISTENING";
}

static uint8_t state_hue(const ls_mesh_stats_t *st)
{
    if (!st->running)   return DIM_FG;   /* offline still has to be legible */
    if (st->tx_enabled) return TUI_RED;
    return TUI_GREEN;
}

/* Shared, since the P25 talkgroup panel wants the same column. This
   was the original and is now one line over ls_age_str, which also carries
   the day suffix this stopped at - a peer last heard yesterday read "26h". */
static void age_str(char *out, size_t cap, uint32_t then, uint32_t now)
{
    ls_age_str(out, cap, then, now);
}

static uint32_t screen_now(void) { return ls_mesh_now(); }

/* A signal meter in `cells` cells, drawn as rising eighth-height
   traces. -120 dBm is what this radio measures as its floor and -20 is a
   neighbour on the desk, so that is the span worth resolving. Bars rather
   than a number because the question is "can I reach them", not "what
   exactly is the level". */
static void sigmeter(tui_surface *sf, tui_rect clip, int x, int y,
                     int cells, float dbm, uint8_t hue)
{
    float v = (dbm + 120.0f) / 100.0f;
    if (v < 0.0f) v = 0.0f;
    if (v > 1.0f) v = 1.0f;
    for (int i = 0; i < cells; i++) {
        /* Each cell's own threshold, so the meter fills left to right and
           each bar is taller than the one before it. */
        const float t = (float)(i + 1) / (float)cells;
        const int   h = 2 + (i * 6) / (cells > 1 ? cells - 1 : 1);   /* 2..8 */
        const bool  lit = v >= t - (0.5f / (float)cells);
        tui_put_char(sf, clip, x + i, y, LS_TUI_TRACE(h),
                     A(lit ? (hue | TUI_BRIGHT) : FAINT_FG, TUI_BLACK));
    }
}

static void header(tui_surface *sf, tui_rect r, const ls_mesh_stats_t *st)
{
    const uint8_t hue = state_hue(st);
    tui_rect band = tui_rect_make(r.x, r.y, r.w, 1);
    tui_fill(sf, band, LS_TUI_SHADE_25, A(hue, TUI_BLACK));

    char left[64];
    snprintf(left, sizeof(left), " %s  %.8s ",
             ls_mesh_name(), st->running ? st->self_id : "--------");
    tui_put_str(sf, r, r.x + 1, r.y, left, A(TUI_WHITE | TUI_BRIGHT, TUI_BLACK));

    const char *w = state_word(st);
    const int wx = r.x + r.w - (int)strlen(w) - 7;
    tui_put_str(sf, r, wx, r.y, w, A(hue | TUI_BRIGHT, TUI_BLACK));
    if (st->running && st->rx_packets)
        sigmeter(sf, r, r.x + r.w - 5, r.y, 4, st->last_rssi, hue);
}

/* The activity ring, kept from the first version because it earns its place:
   a mesh node is silent most of the time and a counter cannot say "alive". */
static void pulse_at(tui_surface *sf, tui_rect r, int cx, int cy)
{
    if (s_pulse <= 0) return;
    static const int8_t CX[12] = { 8, 7, 4, 0, -4, -7, -8, -7, -4, 0, 4, 7 };
    static const int8_t CY[12] = { 0, 4, 7, 8, 7, 4, 0, -4, -7, -8, -7, -4 };
    const int step = 8 - s_pulse;
    const uint8_t c = A(s_pulse > 4 ? (TUI_CYAN | TUI_BRIGHT) : TUI_CYAN, TUI_BLACK);
    for (int i = 0; i < 12; i++) {
        const int x = cx + (CX[i] * step) / 8;
        const int y = cy + (CY[i] * step) / 16;
        if (x < r.x || x >= r.x + r.w || y < r.y || y >= r.y + r.h) continue;
        tui_put_char(sf, r, x, y, LS_TUI_QUAD(1, 1, 0, 0), c);
    }
}

static void draw_scope(tui_surface *sf, tui_rect r)
{
    ls_panel_box(sf, r, "CHANNEL", TUI_GREEN);
    const int w = r.w - 2, h = r.h - 2;
    if (w < 4 || h < 1) return;

    int n = ls_mesh_scope(s_scope_buf, w < LS_MESH_SCOPE_N ? w : LS_MESH_SCOPE_N);
    if (n <= 0) return;

    /* Fixed scale, not auto-ranging: -130 to -30 dBm is the whole span this
       radio reports, and a scope that rescales itself makes a quiet channel
       look identical to a busy one. */
    for (int i = 0; i < n; i++) {
        const float dbm = s_scope_buf[i];
        if (dbm == 0.0f) continue;                 /* never sampled */
        float v = (dbm + 130.0f) / 100.0f;
        if (v < 0.0f) v = 0.0f;
        if (v > 1.0f) v = 1.0f;

        const int total = h * 8;
        int eighths = (int)(v * total + 0.5f);
        if (eighths < 1) eighths = 1;

        const int x = r.x + 1 + i;
        for (int cell = 0; cell < h; cell++) {
            const int y = r.y + r.h - 2 - cell;    /* build upward */
            const int here = eighths - cell * 8;
            if (here <= 0) break;
            const uint8_t c = A(v > 0.55f ? (TUI_YELLOW | TUI_BRIGHT)
                                          : (TUI_GREEN | TUI_BRIGHT), TUI_BLACK);
            tui_put_char(sf, r, x, y,
                         here >= 8 ? LS_TUI_BLOCK_FULL : LS_TUI_TRACE(here), c);
        }
    }
}

/* ------------------------------------------------------------------- chat */

/* One message row. Ours and theirs are told apart by colour and by a
   marker in the gutter, not by indentation: at 48 columns in portrait there
   is no room to indent and still read the text. */
static void msg_row(tui_surface *sf, tui_rect r, int y, const ls_mesh_msg_t *m,
                    uint32_t now)
{
    char age[8];
    age_str(age, sizeof(age), m->t, now);

    /* The delivery mark. */

    char mark = '<';
    uint8_t gut = A(TUI_CYAN, TUI_BLACK);
    if (m->mine) {
        switch (m->state) {
        case LS_MSG_SENDING: mark = '.'; gut = A(TUI_YELLOW, TUI_BLACK); break;
        case LS_MSG_HEARD:   mark = '*'; gut = A(TUI_GREEN | TUI_BRIGHT, TUI_BLACK); break;
        default:             mark = '>'; gut = A(TUI_YELLOW | TUI_BRIGHT, TUI_BLACK); break;
        }
    }
    tui_put_str(sf, r, r.x + 1, y, age, A(DIM_FG, TUI_BLACK));
    tui_put_char(sf, r, r.x + 5, y, mark, gut);

    /* The name is up to the first colon; the rest is what was said. Colouring
       them differently is the whole reason a chat is readable at a glance. */
    const char *colon = strchr(m->text, ':');
    const int avail = r.w - 8;
    if (avail <= 0) return;

    if (colon && (colon - m->text) < 20) {
        const int nlen = (int)(colon - m->text);
        char name[24];
        snprintf(name, sizeof(name), "%.*s", nlen > 20 ? 20 : nlen, m->text);
        tui_put_str(sf, r, r.x + 7, y, name,
                    A(m->mine ? (TUI_YELLOW | TUI_BRIGHT) : (TUI_MAGENTA | TUI_BRIGHT),
                      TUI_BLACK));
        char body[96];
        snprintf(body, sizeof(body), "%.*s", avail - nlen - 2, colon + 1);
        tui_put_str(sf, r, r.x + 7 + nlen + 1, y, body,
                    A(TUI_WHITE | TUI_BRIGHT, TUI_BLACK));
    } else {
        char body[96];
        snprintf(body, sizeof(body), "%.*s", avail, m->text);
        tui_put_str(sf, r, r.x + 7, y, body, A(TUI_WHITE, TUI_BLACK));
    }
}

static void draw_feed(tui_surface *sf, tui_rect r, uint32_t now, bool with_box)
{
    if (with_box) ls_panel_box(sf, r, "PUBLIC CHANNEL", TUI_CYAN);
    const int top  = r.y + (with_box ? 1 : 0);
    const int rows = r.h - (with_box ? 2 : 0);
    if (rows < 1) return;

    const int n = ls_mesh_messages(s_msg_buf, LS_MESH_MAX_MSGS);
    if (!n) {
        tui_put_str(sf, r, r.x + 2, top + 1, "no messages yet",
                    A(DIM_FG, TUI_BLACK));
        tui_put_str(sf, r, r.x + 2, top + 3, "anything sent here is readable by",
                    A(DIM_FG, TUI_BLACK));
        tui_put_str(sf, r, r.x + 2, top + 4, "every MeshCore node in range.",
                    A(DIM_FG, TUI_BLACK));
        return;
    }
    /* Newest at the bottom, like every chat anyone has used. */
    const int first = n > rows ? n - rows : 0;
    for (int i = first; i < n; i++)
        msg_row(sf, r, top + (i - first), &s_msg_buf[i], now);
}

static void draw_compose(tui_surface *sf, tui_rect r, const ls_mesh_stats_t *st)
{
    const bool can = st->running && st->tx_enabled;
    const uint8_t frame = can ? A(TUI_GREEN, TUI_BLACK) : A(FAINT_FG, TUI_BLACK);
    tui_box(sf, r, NULL, frame);

    if (!st->running) {
        tui_put_str(sf, r, r.x + 2, r.y + 1, "not running - type /start",
                    A(DIM_FG, TUI_BLACK));
        return;
    }
    if (!can) {
        tui_put_str(sf, r, r.x + 2, r.y + 1,
                    "disarmed - type /arm to transmit",
                    A(TUI_YELLOW, TUI_BLACK));
        return;
    }

    /* While a frame is on the air the compose line becomes the
       transmit indicator. The bar is the real airtime, counted down. */
    if (s_air_frames > 0 && s_air_total > 0) {
        const int wfill = ((s_air_total - s_air_frames) * (r.w - 6)) / s_air_total;
        tui_put_str(sf, r, r.x + 2, r.y + 1, "TX", A(TUI_YELLOW | TUI_BRIGHT, TUI_BLACK));
        for (int i = 0; i < r.w - 6; i++)
            tui_put_char(sf, r, r.x + 5 + i, r.y + 1,
                         i <= wfill ? LS_TUI_BLOCK_FULL : LS_TUI_TRACE(1),
                         A(i <= wfill ? (TUI_YELLOW | TUI_BRIGHT)
                                      : FAINT_FG, TUI_BLACK));
        return;
    }

    /* Where this message is going, as a chip inside the box. */

    int text_x = r.x + 4;
    s_lock_chip = tui_rect_make(0, -1, 0, 0);
    if (chat_locked()) {
        const char *nm = lock_label();
        int w = (int)strlen(nm) + 2;
        if (w > 12) w = 12;
        if (w > r.w - 10) w = r.w - 10;
        if (w > 3) {
            const bool known = ls_mesh_peer_known(s_lock_id);
            /* Dim when the node cannot be addressed at all, so the chip
               reports the difference between "private to TBAY" and "private
               to a node that has not said anything since boot". */
            const uint8_t at = known ? A(TUI_BLACK, TUI_GREEN)
                                     : A(TUI_BLACK, TUI_YELLOW);
            s_lock_chip = tui_rect_make(r.x + 1, r.y, w, r.h);
            tui_fill(sf, tui_rect_make(r.x + 1, r.y + 1, w, 1), ' ', at);
            char chip[16];
            snprintf(chip, sizeof(chip), "%.*s", w - 2, nm);
            tui_put_str(sf, r, r.x + 2, r.y + 1, chip, at);
            text_x = r.x + 1 + w + 1;
        }
    } else {
        tui_put_str(sf, r, r.x + 2, r.y + 1, ">",
                    A(TUI_GREEN | TUI_BRIGHT, TUI_BLACK));
    }

    char shown[COMPOSE_MAX + 2];
    const int room = r.x + r.w - 2 - text_x;
    snprintf(shown, sizeof(shown), "%.*s", room > 0 ? room : 0, s_compose);
    tui_put_str(sf, r, text_x, r.y + 1, shown, A(TUI_WHITE | TUI_BRIGHT, TUI_BLACK));

    /* A block cursor that blinks. Cheap, and it is the difference between a
       field that looks live and one that looks like a label. */
    if ((s_blink / 12) & 1) {
        const int cx = text_x + (int)strlen(shown);
        if (cx < r.x + r.w - 1)
            tui_put_char(sf, r, cx, r.y + 1, LS_TUI_BLOCK_FULL,
                         A(TUI_GREEN | TUI_BRIGHT, TUI_BLACK));
    }
}

/* ------------------------------------------------------------------ nodes */

/* A node row you can actually hit with a thumb. */

static int node_row_h(bool compact) { return (compact || ls_tui_is_wide()) ? 1 : 3; }

/* Which node a row belongs to, from the rect the list was drawn in. Draw and
   hit test both go through this so they cannot disagree. */
static int node_at(tui_rect r, int row)
{
    const int rh = node_row_h(false);
    const int i = (row - r.y - 1) / rh;
    return i >= 0 ? i : -1;
}

static void draw_nodes(tui_surface *sf, tui_rect r, uint32_t now, bool compact)
{
    ls_panel_box(sf, r, compact ? "HEARD" : "NODES IN RANGE", TUI_MAGENTA);
    if (!compact) s_node_rect = r;

    const int n = ls_mesh_peers(s_peer_buf, LS_MESH_MAX_PEERS);
    if (s_sel >= n) s_sel = n ? n - 1 : 0;

    if (!n) {

        tui_put_str(sf, r, r.x + 2, r.y + 2, "listening...",
                    A(DIM_FG, TUI_BLACK));
        if (!compact && r.h > 6) {
            static const char *WHY =
                "a node appears when IT advertises - there is nothing to ask "
                "for. Still empty? check both ends match on frequency, SF and "
                "bandwidth: a mismatch gives zero packets and zero errors, "
                "which looks exactly like this.";
            char lines[5][64];
            const int room = (r.y + r.h - 1) - (r.y + 4);
            const int k = ls_wrap_text(WHY, r.w - 4, (char *)lines,
                                       sizeof(lines[0]), room < 5 ? room : 5);
            for (int i = 0; i < k; i++)
                tui_put_str(sf, r, r.x + 2, r.y + 4 + i, lines[i],
                            A(FAINT_FG, TUI_BLACK));
        }
        return;
    }

    const int rh = node_row_h(compact);
    /* The full list keeps its last interior row for the hint. The compact
       rail has no hint and no room for one. */
    const int rows = (r.h - (compact ? 2 : 3)) / rh;
    for (int i = 0; i < n && i < rows; i++) {
        const int top = r.y + 1 + i * rh;
        const int y = top + (rh - 1) / 2;
        const bool sel = !compact && i == s_sel;
        if (sel) {
            tui_rect row = tui_rect_make(r.x + 1, top, r.w - 2, rh);
            tui_fill(sf, row, ' ', A(TUI_BLACK, TUI_MAGENTA));
        } else if (!compact && rh > 1) {

            tui_rect row = tui_rect_make(r.x + 1, top, r.w - 2, rh);
            ls_fill_dither(sf, row, LS_DITHER_LIGHT, TUI_MAGENTA);
        }
        const uint8_t idc = sel ? A(TUI_BLACK, TUI_MAGENTA)
                                : A(TUI_WHITE | TUI_BRIGHT, TUI_BLACK);
        const uint8_t dim = sel ? A(TUI_BLACK, TUI_MAGENTA)
                                : A(DIM_FG, TUI_BLACK);
        char age[8];
        age_str(age, sizeof(age), s_peer_buf[i].last_heard, now);

        const bool saved = (s_peer_buf[i].last_heard == 0);
        if (saved) snprintf(age, sizeof(age), "saved");

        if (compact) {
            /* A peer id is 16 hex characters - eight bytes of the public key - and the rail is 22 columns wide. */

            char who[LS_MESH_PEER_NAME > 9 ? LS_MESH_PEER_NAME : 9];
            if (s_peer_buf[i].name[0])
                snprintf(who, sizeof(who), "%s", s_peer_buf[i].name);
            else
                snprintf(who, sizeof(who), "%.8s", s_peer_buf[i].id);
            tui_put_str(sf, r, r.x + 2, y, who, idc);
            tui_put_str(sf, r, r.x + r.w - 11, y, age, dim);
            sigmeter(sf, r, r.x + r.w - 6, y, 4, s_peer_buf[i].rssi, TUI_MAGENTA);
            continue;
        }
        tui_put_str(sf, r, r.x + 2, y, s_peer_buf[i].id, idc);
        if (rh > 1) {
            /* Tall rows: the name under the id, the numbers beside it. A
               hex id tells you a node is there and a name tells you which.
               Column 18 clears a full 16 character id. */
            char meta[48];
            /* No signal numbers for a saved contact: 0 dBm and 0.0 dB are
               readings nobody took, and printed beside a name they read as
               a node sitting on top of the antenna. */
            if (saved)
                snprintf(meta, sizeof(meta), "%s - not heard yet", age);
            else
                snprintf(meta, sizeof(meta), "%s %4.0fdBm %4.1fdB",
                         age, (double)s_peer_buf[i].rssi,
                         (double)s_peer_buf[i].snr);
            tui_put_str(sf, r, r.x + 2 + 18, y, meta, dim);
            sigmeter(sf, r, r.x + r.w - 6, y, 4, s_peer_buf[i].rssi, TUI_MAGENTA);
            if (rh >= 3 && s_peer_buf[i].name[0])
                tui_put_str(sf, r, r.x + 2, y + 1, s_peer_buf[i].name, dim);
        } else {
            char meta[48];
            snprintf(meta, sizeof(meta), "%s  %4.0f dBm  %4.1f dB  x%lu",
                     age, (double)s_peer_buf[i].rssi, (double)s_peer_buf[i].snr,
                     (unsigned long)s_peer_buf[i].adverts);
            tui_put_str(sf, r, r.x + 2 + 18, y, meta, dim);
            sigmeter(sf, r, r.x + r.w - 6, y, 4, s_peer_buf[i].rssi, TUI_MAGENTA);
        }
    }
    if (!compact && r.h >= 4) {
        const char *h = ls_tui_is_wide() ? "ENTER opens the node"
                                         : "tap a node twice to open it";
        tui_put_str(sf, r, r.x + 2, r.y + r.h - 2, h, A(DIM_FG, TUI_BLACK));
    }
}

/* ----------------------------------------------------------- node detail */

/* One node, and everything it has told us. */

static const char *role_word(uint8_t type)
{
    switch (type) {
    case LS_MESH_ROLE_CHAT:     return "chat node";
    case LS_MESH_ROLE_REPEATER: return "repeater";
    case LS_MESH_ROLE_ROOM:     return "room server";
    case LS_MESH_ROLE_SENSOR:   return "sensor";
    default:                    return "unknown role";
    }
}

static int detail_index(int n)
{
    for (int i = 0; i < n; i++)
        if (!strcmp(s_peer_buf[i].id, s_detail_id)) return i;
    return -1;
}

static const char *const ACT_NAME[DETAIL_ACTS] = { "MESSAGE", "SAVE", "MAP", "PING" };

/* Which action was just pressed, and for how much longer to show it. */

#define ACT_PRESS_FRAMES 8
static int s_act_pressed = -1;
static int s_act_press_ttl;

static void act_press_mark(int which)
{
    s_act_pressed = which;
    s_act_press_ttl = ACT_PRESS_FRAMES;
}
static const char       ACT_KEY[DETAIL_ACTS]   = { 'd', 's', 'm', 'p' };

static void draw_detail(tui_surface *sf, tui_rect r, uint32_t now)
{
    /* The press highlight ages here because this is the one function
       that runs every frame the buttons are on screen. It also has to keep
       asking for a repaint while it is lit: the cell renderer only pushes
       what changed, so the frame that clears the highlight is a frame where
       nothing else moved and the panel would never see it. */
    if (s_act_press_ttl > 0) {
        if (--s_act_press_ttl == 0) s_act_pressed = -1;
        ls_tui_invalidate();
    }

    const bool wide = ls_tui_is_wide();
    const uint8_t dim   = A(DIM_FG, TUI_BLACK);
    const uint8_t faint = A(FAINT_FG, TUI_BLACK);
    const uint8_t val   = A(TUI_WHITE | TUI_BRIGHT, TUI_BLACK);

    for (int i = 0; i < DETAIL_ACTS; i++) s_act_rect[i] = tui_rect_make(0, -1, 0, 0);

    const int n = ls_mesh_peers(s_peer_buf, LS_MESH_MAX_PEERS);
    const int idx = detail_index(n);
    ls_panel_box(sf, r, "NODE", TUI_MAGENTA);
    if (idx < 0) {
        tui_put_str(sf, r, r.x + 2, r.y + 2, s_detail_id, val);
        tui_put_str(sf, r, r.x + 2, r.y + 4,
                    "this node has dropped out of the list", dim);
        tui_put_str(sf, r, r.x + 2, r.y + 6, "ESC goes back", faint);
        return;
    }
    const ls_mesh_peer_t *p = &s_peer_buf[idx];

    int y = r.y + 1;

    if (p->name[0]) {
        tui_put_str(sf, r, r.x + 2, y, p->name, A(TUI_MAGENTA | TUI_BRIGHT, TUI_BLACK));
        y++;
    }
    tui_put_str(sf, r, r.x + 2, y, p->id, val);
    tui_put_str(sf, r, r.x + 2 + 18, y, role_word(p->type), dim);
    y += 2;

    /* Signal, as a number and as the meter the list uses, so the two pages
       agree at a glance. */
    {
        char s[48];
        snprintf(s, sizeof(s), "%.0f dBm   SNR %.1f dB", (double)p->rssi,
                 (double)p->snr);
        tui_put_str(sf, r, r.x + 2, y, s, val);
        sigmeter(sf, r, r.x + r.w - 8, y, 6, p->rssi, TUI_MAGENTA);
        y++;
    }
    {
        char age[8], s[64];
        age_str(age, sizeof(age), p->last_heard, now);
        snprintf(s, sizeof(s), "heard %s ago, %lu adverts", age,
                 (unsigned long)p->adverts);
        tui_put_str(sf, r, r.x + 2, y, s, dim);
        y++;
        if (p->first_heard && p->first_heard != p->last_heard) {
            char first[8];
            age_str(first, sizeof(first), p->first_heard, now);
            snprintf(s, sizeof(s), "first seen %s ago", first);
            tui_put_str(sf, r, r.x + 2, y, s, faint);
            y++;
        }
    }
    y++;

    /* Where it says it is, and how far that is from here. A position in an
       advert is worth nothing on its own: "45.412300 -75.699700" is a fact
       about the node and "3.2 km NE" is a fact about you and the node. */
    if (p->has_loc) {
        char s[64];
        const double plat = p->lat_e6 / 1e6, plon = p->lon_e6 / 1e6;
        snprintf(s, sizeof(s), "%.6f  %.6f", plat, plon);
        tui_put_str(sf, r, r.x + 2, y, s, val);
        y++;

        double clat, clon;
        if (mesh_here(&clat, &clon)) {
            double brg, rng_m;
            ls_geo_bearing_range(clat, clon, plat, plon, &brg, &rng_m);
            if (rng_m < 1000.0)
                snprintf(s, sizeof(s), "%.0f m %s  (%.0f deg)", rng_m,
                         ls_geo_compass(brg), brg);
            else
                snprintf(s, sizeof(s), "%.1f km / %.1f mi %s  (%.0f deg)",
                         rng_m / 1000.0, rng_m / LS_GEO_M_PER_MILE,
                         ls_geo_compass(brg), brg);
            tui_put_str(sf, r, r.x + 2, y, s, A(TUI_CYAN | TUI_BRIGHT, TUI_BLACK));
        } else {
            tui_put_str(sf, r, r.x + 2, y,
                        "no fix and no home set, so no range", faint);
        }
        y += 2;
    } else {
        tui_put_str(sf, r, r.x + 2, y, "no position in its advert", faint);
        y += 2;
    }

    /* The public key, wrapped. It is what actually identifies the node and
       the only way to check one against another device. */
    if (r.h - (y - r.y) > 8) {
        tui_put_str(sf, r, r.x + 2, y, "PUBLIC KEY", dim);
        y++;
        const int per = (r.w - 4) / 2 * 2;      /* whole bytes per row */
        char line[80];
        for (int off = 0; off < 32 && (y - r.y) < r.h - 6; off += per / 2) {
            int k = 0;
            for (int b = off; b < 32 && b < off + per / 2; b++)
                k += snprintf(line + k, sizeof(line) - (size_t)k, "%02x",
                              p->pub_key[b]);
            tui_put_str(sf, r, r.x + 2, y, line, faint);
            y++;
        }
        y++;
    }

    /* Signal history: every frame we have heard from THIS node, in the order they arrived, as a column chart against the same scale the meters use. */

    {
        /* Whatever is left between the key and the action buttons, which on
           a 66 row portrait panel is about twenty rows and was empty. The
           chart is the last thing on the page precisely so it can have
           them: a tall chart of the same six samples reads a great deal
           better than a short one. */
        const int acts_h = (wide ? 1 : 3) * DETAIL_ACTS + 1;
        const int gh = (r.y + r.h - 1 - acts_h) - y - 1;
        if (gh >= 5) {
            const int nev = ls_mesh_events(s_ev_buf, LS_MESH_MAX_EVENTS);
            float hist[LS_MESH_MAX_EVENTS];
            int nh = 0;
            for (int i = 0; i < nev; i++)
                if (s_ev_buf[i].kind == LS_MESH_EV_ADVERT ||
                    s_ev_buf[i].kind == LS_MESH_EV_RX)
                    if (!strcmp(s_ev_buf[i].id, p->id))
                        hist[nh++] = s_ev_buf[i].rssi;

            tui_put_str(sf, r, r.x + 2, y, "SIGNAL HISTORY", dim);
            y++;
            tui_rect g = tui_rect_make(r.x + 2, y, r.w - 4, gh - 2);
            if (nh < 2) {
                tui_put_str(sf, r, g.x, g.y,
                            nh ? "one frame so far - no trend yet"
                               : "nothing from this node in the log", faint);
            } else {
                /* Fixed scale, -120 to -40 dBm, which is the whole of what a
                   SX1262 reports. An autoscaled chart of six samples three
                   dB apart looks like a mountain range and says nothing. */
                const int cw = g.w / nh;
                for (int i = 0; i < nh; i++) {
                    /* OLDEST ON THE LEFT. The event log is newest
                       first, and drawn in log order the chart ran backwards:
                       a node whose signal was collapsing drew a rising
                       staircase, which is the exact opposite of the one
                       thing this is here to show. Time reads left to right
                       on every chart anyone has seen. */
                    const float v = hist[nh - 1 - i];
                    float f = (v + 120.0f) / 80.0f;
                    if (f < 0.0f) f = 0.0f;
                    if (f > 1.0f) f = 1.0f;
                    int bh = (int)(f * (float)g.h + 0.5f);
                    if (bh < 1) bh = 1;
                    const uint8_t hue = f > 0.55f ? (TUI_GREEN | TUI_BRIGHT)
                                      : f > 0.30f ? TUI_YELLOW
                                                  : TUI_RED;
                    for (int c = 0; c < cw - 1 && c < g.w; c++)
                        for (int b = 0; b < bh; b++)
                            tui_put_char(sf, g, g.x + i * cw + c,
                                         g.y + g.h - 1 - b, LS_TUI_BLOCK_FULL,
                                         A(hue, TUI_BLACK));
                }
                char scale[40];
                snprintf(scale, sizeof(scale), "-120 to -40 dBm");
                tui_put_str(sf, r, r.x + 2, y + gh - 2, scale, faint);
                tui_put_str(sf, r, r.x + r.w - 10, y + gh - 2, "now ->", faint);
            }
            y += gh;
        }
    }

    const int ah = wide ? 1 : 3;
    const int need = DETAIL_ACTS * ah + 1;
    int ay = r.y + r.h - 1 - need;
    if (ay < y) ay = y;
    for (int i = 0; i < DETAIL_ACTS && ay + (i + 1) * ah < r.y + r.h; i++) {
        const tui_rect a = tui_rect_make(r.x + 2, ay + i * ah, r.w - 4, ah);
        s_act_rect[i] = a;
        const bool live = (i != 2) || p->has_loc;
        /* A pressed button is filled and its label goes to black on
           green - the same inversion the diagnostics rows use for a value
           that just changed, so "this one, just now" reads the same way
           everywhere. */
        const bool hot = (i == s_act_pressed && s_act_press_ttl > 0);
        if (hot) {
            tui_fill(sf, a, ' ', A(TUI_BLACK, TUI_GREEN | TUI_BRIGHT));
        } else {
            ls_fill_dither(sf, a, LS_DITHER_LIGHT, live ? TUI_GREEN : TUI_BLACK);
        }
        char label[32];
        snprintf(label, sizeof(label), "%c  %s", ACT_KEY[i], ACT_NAME[i]);
        if (hot) {
            const int at = a.x + (a.w - (int)strlen(label)) / 2;
            tui_put_str(sf, a, at, a.y + (ah - 1) / 2, label,
                        A(TUI_BLACK, TUI_GREEN | TUI_BRIGHT));
        } else {
            ls_dither_label(sf, a, (ah - 1) / 2, label,
                            live ? A(TUI_GREEN | TUI_BRIGHT, TUI_BLACK) : faint);
        }
    }
}

/* ------------------------------------------------------------------ setup */

typedef struct { const char *label; const char *unit; } field_t;

typedef enum { F_TEXT = 0, F_TOGGLE } fkind_t;

typedef struct { const char *label; const char *unit; fkind_t kind; } field2_t;

static const field2_t FIELDS[] = {
    { "name",          NULL,  F_TEXT   },
    { "band",          NULL,  F_TOGGLE },
    { "frequency",     "MHz", F_TEXT   },
    { "spreading",     "SF",  F_TEXT   },
    { "bandwidth",     "kHz", F_TEXT   },
    { "coding rate",   "4/x", F_TEXT   },
    { "power",         "dBm", F_TEXT   },
    { "sync word",     NULL,  F_TOGGLE },
    { "crc",           NULL,  F_TOGGLE },
    { "auto listen",   NULL,  F_TOGGLE },
    { "auto transmit", NULL,  F_TOGGLE },
    { "advert on boot",NULL,  F_TOGGLE },
    { "advert every",  "s",   F_TEXT   },
};
#define F_NAME 0
#define F_BAND 1
#define F_FREQ 2
#define F_SF   3
#define F_BW   4
#define F_CR   5
#define F_PWR  6
#define F_SYNC 7
#define F_CRC  8
#define F_ALIS 9
#define F_ATX  10
#define F_ABOOT 11
#define F_AEVERY 12
#define FIELD_COUNT ((int)(sizeof(FIELDS) / sizeof(FIELDS[0])))

static void field_value(int i, char *out, size_t cap)
{
    ls_mesh_radio_t r;
    ls_mesh_get_radio(&r);
    switch (i) {
        case F_NAME:  snprintf(out, cap, "%s", ls_mesh_name()); break;
        case F_BAND:  snprintf(out, cap, "%s", ls_mesh_band_name(ls_mesh_band_current())); break;
        case F_FREQ:  snprintf(out, cap, "%.4f", r.freq_hz / 1e6); break;
        case F_SF:    snprintf(out, cap, "%u", (unsigned)r.sf); break;
        case F_BW:    snprintf(out, cap, "%.1f", r.bw_hz / 1000.0); break;
        case F_CR:    snprintf(out, cap, "%u", (unsigned)r.cr); break;
        case F_PWR:   snprintf(out, cap, "%d", (int)r.power_dbm); break;
        case F_SYNC:  snprintf(out, cap, "0x%02X %s", (unsigned)r.sync_word,
                               r.sync_word == 0x12 ? "mesh" : "public"); break;
        case F_CRC:   snprintf(out, cap, "%s", r.crc_on == 1 ? "off" : "on"); break;
        case F_ALIS:  snprintf(out, cap, "%s", ls_mesh_auto_listen() ? "on" : "off"); break;
        case F_ATX:   snprintf(out, cap, "%s", ls_mesh_auto_tx() ? "ON" : "off"); break;
        case F_ABOOT: snprintf(out, cap, "%s", r.advert_boot ? "on" : "off"); break;
        case F_AEVERY:
            if (r.advert_secs) snprintf(out, cap, "%u", (unsigned)r.advert_secs);
            else               snprintf(out, cap, "never");
            break;
        default: snprintf(out, cap, "--"); break;
    }
}

static const char *toggle_field(int i)
{
    ls_mesh_radio_t r;
    ls_mesh_get_radio(&r);
    switch (i) {
    case F_BAND: {
        const int next = (ls_mesh_band_current() + 1) % LS_MESH_BAND__COUNT;
        return ls_mesh_set_band(next) == ESP_OK ? "band changed - peers will differ"
                                                : "band change failed";
    }
    case F_SYNC:
        r.sync_word = (r.sync_word == 0x12) ? 0x34 : 0x12;
        return ls_mesh_set_radio(&r) == ESP_OK
               ? (r.sync_word == 0x12 ? "sync 0x12 - MeshCore mesh"
                                      : "sync 0x34 - public, NOT MeshCore")
               : "sync change failed";
    case F_CRC:
        r.crc_on = (r.crc_on == 2) ? 1 : 2;
        return ls_mesh_set_radio(&r) == ESP_OK
               ? (r.crc_on == 2 ? "crc on" : "crc OFF - peers will drop us")
               : "crc change failed";
    case F_ALIS:
        ls_mesh_set_auto(!ls_mesh_auto_listen(), ls_mesh_auto_tx());
        return ls_mesh_auto_listen() ? "will start at boot" : "will not start at boot";
    case F_ATX:
        ls_mesh_set_auto(ls_mesh_auto_listen(), !ls_mesh_auto_tx());
        return ls_mesh_auto_tx() ? "ARMED FROM BOOT - can emit unattended"
                                 : "will boot disarmed";
    case F_ABOOT:
        r.advert_boot = !r.advert_boot;
        return ls_mesh_set_radio(&r) == ESP_OK
               ? (r.advert_boot ? "will announce itself on start" : "silent on start")
               : "failed";
    default:
        return NULL;
    }
}

#define CHAN_FIELD_BASE FIELD_COUNT

static void draw_channels(tui_surface *sf, tui_rect r)
{
    ls_panel_box(sf, r, "CHANNELS", TUI_CYAN);

    ls_mesh_chan_t c[LS_MESH_MAX_CHANNELS];
    const int n = ls_mesh_channels(c, LS_MESH_MAX_CHANNELS);
    const int active = ls_mesh_channel_active();

    for (int i = 0; i < n && i * 3 + 3 < r.h; i++) {
        const int y = r.y + 1 + i * 3;
        const bool sel = (s_field == CHAN_FIELD_BASE + i);

        if (sel) {
            tui_rect row = tui_rect_make(r.x + 1, y, r.w - 2, 1);
            tui_fill(sf, row, ' ', A(TUI_BLACK, TUI_CYAN));
        }
        const uint8_t nc = sel ? A(TUI_BLACK, TUI_CYAN)
                               : A(TUI_WHITE | TUI_BRIGHT, TUI_BLACK);
        const uint8_t dc = sel ? A(TUI_BLACK, TUI_CYAN) : A(DIM_FG, TUI_BLACK);

        /* A precision as well as a width. "%-10s" pads a short name
           to ten and does nothing at all to a long one, so a full length
           channel name ran past the end of the buffer's useful part; the
           compiler could not prove the name was terminated either. The
           precision bounds both. */
        char line[48];
        snprintf(line, sizeof(line), "%d %-10.*s", i,
                 LS_MESH_CHAN_NAME - 1,
                 c[i].name[0] ? c[i].name : "(empty)");
        tui_put_str(sf, r, r.x + 2, y, line, nc);

        /* The one that is actually sending, marked where it cannot be
           mistaken for a selection cursor. */
        if (i == active)
            tui_put_str(sf, r, r.x + r.w - 6, y, "SEND",
                        sel ? A(TUI_BLACK, TUI_CYAN)
                            : A(TUI_GREEN | TUI_BRIGHT, TUI_BLACK));

        const char *psk = c[i].psk[0] ? c[i].psk : "no key set";
        char shown[40];
        snprintf(shown, sizeof(shown), "%.*s", r.w - 6, psk);
        tui_put_str(sf, r, r.x + 4, y + 1, shown, dc);

        if (sel && s_edit_field == CHAN_FIELD_BASE + i) {
            char ed[40];
            snprintf(ed, sizeof(ed), "%.*s", r.w - 6, s_edit);
            tui_rect row = tui_rect_make(r.x + 1, y + 1, r.w - 2, 1);
            tui_fill(sf, row, ' ', A(TUI_BLACK, TUI_YELLOW));
            tui_put_str(sf, r, r.x + 4, y + 1, ed, A(TUI_BLACK, TUI_YELLOW));
            if ((s_blink / 12) & 1)
                tui_put_char(sf, r, r.x + 4 + (int)strlen(ed), y + 1,
                             LS_TUI_BLOCK_FULL, A(TUI_BLACK, TUI_YELLOW));
        }
    }

    const int hy = r.y + r.h - 2;
    if (hy > r.y + n * 3)
        tui_put_str(sf, r, r.x + 2, hy,
                    "ENTER or a second tap edits a key   SPACE send here",
                    A(DIM_FG, TUI_BLACK));
}

static int value_col(void)
{
    int longest = 0;
    for (int i = 0; i < FIELD_COUNT; i++) {
        const int n = (int)strlen(FIELDS[i].label);
        if (n > longest) longest = n;
    }
    return 2 + longest + 1;
}

/* A settings row you can actually hit with a thumb. */

static int setup_row_h(void) { return ls_tui_is_wide() ? 1 : 3; }

/* Which field a row inside the SETUP box belongs to, or -1. The one place
   that knows, so touch and draw stay in step. */
static int setup_field_at(tui_rect r, int row)
{
    const int rh = setup_row_h();
    const int i = (row - r.y - 1) / rh;
    return (i >= 0 && i < FIELD_COUNT) ? i : -1;
}

static void draw_setup(tui_surface *sf, tui_rect r)
{
    ls_panel_box(sf, r, "SETUP", TUI_YELLOW);

    const int vx = value_col();
    const int rh = setup_row_h();

    /* One row per field now that there are thirteen. Two rows each
       looked better with five and does not fit thirteen on any geometry the
       gate builds. */
    for (int i = 0; i < FIELD_COUNT && (i + 1) * rh + 1 < r.h; i++) {

        const int top = r.y + 1 + i * rh;
        const int y   = top + (rh - 1) / 2;
        const bool sel = (i == s_field);
        if (sel) {
            tui_rect row = tui_rect_make(r.x + 1, top, r.w - 2, rh);
            tui_fill(sf, row, ' ', A(TUI_BLACK, TUI_YELLOW));
        } else if (rh > 1) {
            /* Unselected fields get the quarter-block texture the
               rest of this interface uses for "a field you may press"
               (). Without it thirteen tall rows read as one large
               empty panel with some words in it, and nothing says where one
               target ends and the next begins. */
            ls_fill_dither(sf, tui_rect_make(r.x + 1, top, r.w - 2, rh),
                           LS_DITHER_LIGHT, TUI_YELLOW);
        }
        const uint8_t lab = sel ? A(TUI_BLACK, TUI_YELLOW) : A(TUI_WHITE, TUI_BLACK);
        const uint8_t val = sel ? A(TUI_BLACK, TUI_YELLOW)
                                : A(TUI_WHITE | TUI_BRIGHT, TUI_BLACK);

        tui_put_str(sf, r, r.x + 2, y, FIELDS[i].label, lab);

        /* Big enough for the longest thing that can be in it. */

        char v[EDIT_MAX + 8];
        if (sel && s_edit_field == i) {
            snprintf(v, sizeof(v), "%s", s_edit);
        } else {
            field_value(i, v, sizeof(v));
        }
        const int vlen = (int)strlen(v);
        const bool wrap = (vx + vlen >= r.w - 1);
        /* Auto transmit reads in red when it is on, wherever the
           cursor happens to be. A setting that lets the board emit
           unattended should not look like every other row. */
        const uint8_t vcol = (i == F_ATX && ls_mesh_auto_tx() && !sel)
                             ? A(TUI_RED | TUI_BRIGHT, TUI_BLACK) : val;
        if (wrap) {

            tui_put_str(sf, r, r.x + 4, y + 1, v, vcol);
        } else {
            tui_put_str(sf, r, r.x + vx, y, v, vcol);
        }
        if (FIELDS[i].unit && !wrap)
            tui_put_str(sf, r, r.x + vx + vlen + 1, y, FIELDS[i].unit,
                        sel ? A(TUI_BLACK, TUI_YELLOW) : A(DIM_FG, TUI_BLACK));

        if (sel && s_edit_field == i && ((s_blink / 12) & 1))
            tui_put_char(sf, r, wrap ? r.x + 4 + vlen : r.x + vx + vlen,
                         wrap ? y + 1 : y, LS_TUI_BLOCK_FULL,
                         A(TUI_BLACK, TUI_YELLOW));
    }

    const int hy = r.y + r.h - 2;
    if (hy > r.y + FIELD_COUNT)
        tui_put_str(sf, r, r.x + 2, hy,
                    "ENTER or a second tap edits   UP/DOWN choose",
                    A(DIM_FG, TUI_BLACK));
}

/* --------------------------------------------------------------- portrait */

static const char *const PAGE_NAMES[PAGE__COUNT] = { "CHAT", "NODES", "SETUP" };

static tui_rect page_tab_rect(tui_rect bar, int i)
{
    const int cw = bar.w / PAGE__COUNT;
    const int x0 = bar.x + i * cw;
    const int w  = (i == PAGE__COUNT - 1) ? bar.x + bar.w - x0 : cw;
    return tui_rect_make(x0, bar.y, w, bar.h);
}

static void portrait_tabs(tui_surface *sf, tui_rect bar)
{
    for (int i = 0; i < PAGE__COUNT; i++) {
        const tui_rect t = page_tab_rect(bar, i);
        const bool on = ((mesh_page_t)i == s_page);
        const uint8_t hue = on ? (TUI_GREEN | TUI_BRIGHT) : TUI_CYAN;

        ls_panel_box(sf, t, NULL, hue);
        tui_rect f = tui_rect_make(t.x + 1, t.y + 1, t.w - 2, t.h - 2);
        if (f.w <= 0 || f.h <= 0) {
            /* Too short for a box; the word alone still has to be readable. */
            const int len = (int)strlen(PAGE_NAMES[i]);
            tui_put_str(sf, bar, t.x + (t.w - len) / 2, t.y, PAGE_NAMES[i],
                        A(TUI_WHITE | TUI_BRIGHT, TUI_BLACK));
            continue;
        }
        ls_fill_dither(sf, f, on ? LS_DITHER_MEDIUM : LS_DITHER_LIGHT, hue);
        ls_dither_label(sf, f, (f.h - 1) / 2, PAGE_NAMES[i],
                        A(TUI_WHITE | TUI_BRIGHT, TUI_BLACK));
    }
}

/* -------------------------------------------------------------- landscape */

static void page_tabs(tui_surface *sf, tui_rect r)
{
    int x = r.x + 1;
    tui_put_char(sf, r, x - 1, r.y, '<', A(FAINT_FG, TUI_BLACK));
    for (int i = 0; i < PAGE__COUNT; i++) {
        const bool on = ((mesh_page_t)i == s_page);
        const int w = (int)strlen(PAGE_NAMES[i]) + 2;
        tui_rect tab = tui_rect_make(x, r.y, w, 1);
        tui_fill(sf, tab, ' ', on ? A(TUI_BLACK, TUI_CYAN) : A(TUI_WHITE, TUI_BLACK));
        tui_put_str(sf, r, x + 1, r.y, PAGE_NAMES[i],
                    on ? A(TUI_BLACK, TUI_CYAN) : A(TUI_CYAN, TUI_BLACK));
        x += w + 1;
    }
    tui_put_char(sf, r, x, r.y, '>', A(FAINT_FG, TUI_BLACK));
    if (s_flash_ttl > 0)
        tui_put_str(sf, r, x + 3, r.y, s_flash, A(TUI_GREEN | TUI_BRIGHT, TUI_BLACK));
    else
        tui_put_str(sf, r, x + 3, r.y, "/help for commands",
                    A(DIM_FG, TUI_BLACK));
}

static void draw_landscape(tui_surface *sf, tui_rect area,
                           const ls_mesh_stats_t *st, uint32_t now)
{
    tui_rect head = tui_rect_make(area.x, area.y, area.w, 1);
    tui_rect tabs = tui_rect_make(area.x, area.y + area.h - 1, area.w, 1);
    tui_rect body = tui_rect_make(area.x, area.y + 1, area.w, area.h - 2);

    header(sf, head, st);
    page_tabs(sf, tabs);

    switch (s_page) {
    case PAGE_CHAT: {
        /* Feed and a compact node rail. The rail is a quarter of the width -
           enough for an id and a signal meter, which is what you glance at
           while typing. */
        const int railw = body.w / 4 < 22 ? body.w / 4 : 22;
        const int scopeh = body.h >= 14 ? 5 : 0;   /* only when there is room */
        tui_rect feed = tui_rect_make(body.x, body.y, body.w - railw, body.h - 3);
        tui_rect comp = tui_rect_make(body.x, body.y + feed.h, body.w - railw, 3);
        tui_rect rail = tui_rect_make(body.x + body.w - railw, body.y,
                                      railw, body.h - scopeh);
        tui_rect scope = tui_rect_make(body.x + body.w - railw,
                                       body.y + body.h - scopeh, railw, scopeh);
        draw_feed(sf, feed, now, true);
        draw_compose(sf, comp, st);
        /* Landscape can open the keyboard too. The QWERTY is
           detachable, so "landscape" is not a promise that one is
           attached - which is how the compose line came to be unusable on a
           unit being carried without it. */
        s_compose_rect = comp;
        draw_nodes(sf, rail, now, true);
        pulse_at(sf, rail, rail.x + rail.w / 2, rail.y + rail.h - 2);
        if (scopeh) draw_scope(sf, scope);
        break;
    }
    case PAGE_NODES:

        if (s_detail) {
            const int lw = body.w * 45 / 100;
            draw_nodes(sf, tui_rect_make(body.x, body.y, lw, body.h), now, false);
            draw_detail(sf, tui_rect_make(body.x + lw, body.y,
                                          body.w - lw, body.h), now);
        } else {
            draw_nodes(sf, body, now, false);
        }
        break;
    case PAGE_SETUP: {

        const int lw = body.w * 55 / 100;
        tui_rect left  = tui_rect_make(body.x, body.y, lw, body.h);
        tui_rect right = tui_rect_make(body.x + lw, body.y, body.w - lw, body.h);
        draw_setup(sf, left);
        draw_channels(sf, right);
        break;
    }
    default: break;
    }
}

/* --------------------------------------------------------------- portrait */

static void draw_portrait(tui_surface *sf, tui_rect area,
                          const ls_mesh_stats_t *st, uint32_t now)
{
    const uint8_t dim = A(DIM_FG, TUI_BLACK);
    const uint8_t val = A(TUI_WHITE | TUI_BRIGHT, TUI_BLACK);

    const int armed_rows = 4;

    /* Three rows, because one row of a 10x17 cell is a 3 mm target
       and this is the control a thumb reaches for most - and because two
       rows have no middle to put the name on. At two, every tab drew its
       label on the top row with a row of solid colour underneath it, which
       is the fault the tabs, the tiles and both page bars all had. */
    const int bar_h = 3;
    s_page_bar = tui_rect_make(area.x, area.y, area.w, bar_h);
    portrait_tabs(sf, s_page_bar);

    const int bottom = area.y + area.h - armed_rows;
    const int body_y = area.y + bar_h + 1;
    tui_rect body = tui_rect_make(area.x, body_y, area.w, bottom - body_y);
    s_setup_rect = tui_rect_make(0, -1, 0, 0);

    if (body.h < 3) return;

    if (s_page == PAGE_NODES) {

        if (s_detail) {
            draw_detail(sf, body, now);
        } else {
            /* The peer list gets the height, and the scope goes under it:
               both answer the same question, which is whether anything is
               out there. */
            const int scopeh = body.h >= 16 ? 6 : 0;
            tui_rect list = tui_rect_make(body.x, body.y, body.w, body.h - scopeh);
            draw_nodes(sf, list, now, false);
            if (scopeh)
                draw_scope(sf, tui_rect_make(body.x, body.y + list.h,
                                             body.w, scopeh));
        }
    } else if (s_page == PAGE_SETUP) {

        /* The fields take what they need and the channels take the rest.
           Sized the other way round it was the fields box that grew, so
           thirteen rows of settings sat in a forty-five row frame above a
           channel list with no room to say anything about a channel. A list
           is the half that can use more room. */
        /* Sized from the row height the fields actually use, not from
           the field count: three rows each in portrait is what makes them
           hittable, and the channel list still gets what it needs below. */
        const int fields_h = FIELD_COUNT * setup_row_h() + 3;
        const int chan_h = body.h - fields_h;
        if (chan_h >= 1 + LS_MESH_MAX_CHANNELS * 3 + 1) {
            s_setup_rect = tui_rect_make(body.x, body.y, body.w, fields_h);
            draw_setup(sf, s_setup_rect);
            draw_channels(sf, tui_rect_make(body.x, body.y + fields_h,
                                            body.w, chan_h));
        } else {
            s_setup_rect = body;
            draw_setup(sf, body);
        }
    } else {
        int y = body.y;

        /* Identity block: the icon makes the screen recognisable across a
           bench before any of the text is legible. */
        ls_icon_draw(sf, body, body.x + 1, y, LS_ICON_MESH,
                     A(TUI_CYAN | TUI_BRIGHT, TUI_BLACK));
        tui_put_str(sf, body, body.x + LS_ICON_COLS + 3, y + 1,
                    ls_mesh_name(), val);
        tui_put_str(sf, body, body.x + LS_ICON_COLS + 3, y + 3,
                    st->running ? st->self_id : "--------", dim);
        pulse_at(sf, body, body.x + body.w - 5, y + 3);
        y += LS_ICON_ROWS + 1;

        /* State, as the biggest thing on the screen, on a shaded band. */
        {
            const char *w = state_word(st);
            tui_rect band = tui_rect_make(body.x, y, body.w, 3);
            tui_fill(sf, band, LS_TUI_SHADE_25, A(state_hue(st), TUI_BLACK));
            tui_put_str(sf, body, body.x + (body.w - (int)strlen(w)) / 2,
                        y + 1, w, A(state_hue(st) | TUI_BRIGHT, TUI_BLACK));
            y += 4;
        }

        /* Counters and signal, one line each, large. */
        {
            char a[24], b[24];
            snprintf(a, sizeof(a), "%lu", (unsigned long)st->rx_packets);
            snprintf(b, sizeof(b), "%lu", (unsigned long)st->tx_packets);
            tui_put_str(sf, body, body.x + 2,              y, "HEARD", dim);
            tui_put_str(sf, body, body.x + body.w / 2 + 1, y, "SENT",  dim);
            tui_put_str(sf, body, body.x + 2,              y + 1,
                        st->running ? a : "--", val);
            tui_put_str(sf, body, body.x + body.w / 2 + 1, y + 1,
                        st->running ? b : "--", val);
            y += 2;
            if (st->running && st->rx_packets) {
                char sig[32];
                snprintf(sig, sizeof(sig), "%.0f dBm  SNR %.1f",
                         (double)st->last_rssi, (double)st->last_snr);
                tui_put_str(sf, body, body.x + 2, y, sig, dim);
                sigmeter(sf, body, body.x + body.w - 7, y, 5, st->last_rssi,
                         TUI_GREEN);
                y += 1;
            }
            y += 1;
        }

        /* The compose target sits at the bottom of the page, where a chat window's compose line always is, and the feed takes what is left above it. */

        const int comp_h = 4;
        tui_rect feed = tui_rect_make(body.x, y, body.w,
                                      body.y + body.h - y - comp_h);
        if (feed.h > 3) draw_feed(sf, feed, now, true);

        s_compose_rect = tui_rect_make(body.x, body.y + body.h - comp_h,
                                       body.w, comp_h);
        if (feed.h > 3) {
            const bool can = st->running && st->tx_enabled;
            const uint8_t hue = can ? TUI_GREEN : TUI_YELLOW;
            tui_rect c = s_compose_rect;

            /* While a frame is on the air this becomes the transmit
               indicator, exactly as the landscape compose line does: the bar
               is the real airtime, counted down. */
            if (s_air_frames > 0 && s_air_total > 0) {
                ls_panel_box(sf, c, NULL, TUI_YELLOW | TUI_BRIGHT);
                const int wfill = ((s_air_total - s_air_frames) * (c.w - 6))
                                  / s_air_total;
                tui_put_str(sf, c, c.x + 2, c.y + 1, "TX",
                            A(TUI_YELLOW | TUI_BRIGHT, TUI_BLACK));
                for (int i = 0; i < c.w - 6; i++)
                    tui_put_char(sf, c, c.x + 5 + i, c.y + 1,
                                 i <= wfill ? LS_TUI_BLOCK_FULL : LS_TUI_TRACE(1),
                                 A(i <= wfill ? (TUI_YELLOW | TUI_BRIGHT)
                                              : FAINT_FG, TUI_BLACK));
            } else {
                ls_panel_box(sf, c, NULL, hue);
                tui_rect inner = tui_rect_make(c.x + 1, c.y + 1, c.w - 2, c.h - 2);

                /* The destination, in the posture that has no keyboard to type /pub with. */

                s_lock_chip = tui_rect_make(0, -1, 0, 0);
                if (chat_locked()) {
                    const char *nm = lock_label();
                    int w = (int)strlen(nm) + 2;
                    if (w > 12) w = 12;
                    if (w > inner.w / 2) w = inner.w / 2;
                    if (w > 3) {
                        const bool known = ls_mesh_peer_known(s_lock_id);
                        const uint8_t at = known ? A(TUI_BLACK, TUI_GREEN)
                                                 : A(TUI_BLACK, TUI_YELLOW);
                        s_lock_chip = tui_rect_make(c.x, c.y, w + 2, c.h);
                        tui_fill(sf, tui_rect_make(inner.x, inner.y, w, inner.h),
                                 ' ', at);
                        char chip[16];
                        snprintf(chip, sizeof(chip), "%.*s", w - 2, nm);
                        tui_put_str(sf, inner, inner.x + 1,
                                    inner.y + (inner.h - 1) / 2, chip, at);
                        inner = tui_rect_make(inner.x + w + 1, inner.y,
                                              inner.w - w - 1, inner.h);
                    }
                }

                ls_fill_dither(sf, inner, LS_DITHER_LIGHT, hue);
                /* It never names a key. This bar sits directly
                   above the ARM button, so "arm it below" points at
                   something on the glass; "MIC arms it" pointed at a key on
                   a keyboard that is not attached in this posture. */
                const char *what =
                    !st->running       ? "TAP TO TYPE - not running yet" :
                    !st->tx_enabled    ? "TAP TO TYPE - ARM BELOW TO SEND" :
                    s_compose_len      ? s_compose :
                    chat_locked()      ? "TAP TO WRITE - PRIVATE"
                                       : "TAP TO WRITE A MESSAGE";
                ls_dither_label(sf, inner, (inner.h - 1) / 2, what,
                                A(hue | TUI_BRIGHT, TUI_BLACK));
            }
        }
    }

    {

        tui_rect bar = tui_rect_make(area.x, area.y + area.h - 4, area.w, 4);
        s_tx_rect = bar;

        const bool on = st->tx_enabled;
        const uint8_t hue = !st->running ? DIM_FG : on ? TUI_RED : TUI_GREEN;
        ls_panel_box(sf, bar, NULL, hue | (st->running ? TUI_BRIGHT : 0));
        tui_rect in = tui_rect_make(bar.x + 1, bar.y + 1, bar.w - 2, bar.h - 2);
        ls_fill_dither(sf, in, on ? LS_DITHER_MEDIUM : LS_DITHER_LIGHT, hue);

        const char *word = !st->running ? "NOT RUNNING"
                         : on           ? "DISARM TX"
                                        : "ARM TX";
        ls_dither_label(sf, in, 0, word, A(hue | TUI_BRIGHT, TUI_BLACK));

        char sub[48];
        if (!st->running) {
            snprintf(sub, sizeof(sub), "open SETUP to start the stack");
        } else if (on) {
            snprintf(sub, sizeof(sub), "this radio is emitting");
        } else {
            ls_mesh_radio_t rc;
            ls_mesh_get_radio(&rc);
            snprintf(sub, sizeof(sub), "%.3f MHz at %d dBm",
                     rc.freq_hz / 1e6, (int)rc.power_dbm);
        }
        if (in.h >= 2)
            ls_dither_label(sf, in, 1, sub, A(DIM_FG, TUI_BLACK));
    }
}

/* ---------------------------------------------------------------- screen */

static void enter(void)
{
    s_page = PAGE_CHAT;
    s_sel = 0; s_field = 0; s_edit_field = -1;
    s_detail = false; s_detail_id[0] = 0;
    s_compose_len = 0; s_compose[0] = 0;
    s_seen_msg = ls_mesh_msg_seq();
    s_seen_ev  = ls_mesh_event_seq();
    s_pulse = 0; s_flash_ttl = 0;
}

static void draw(tui_surface *sf, tui_rect area)
{
    ls_mesh_stats_t st;
    ls_mesh_get_stats(&st);

    const uint32_t ev = ls_mesh_event_seq();
    if (ev != s_seen_ev) { s_seen_ev = ev; s_pulse = 8; }
    else if (s_pulse > 0) s_pulse--;

    const uint32_t ms = ls_mesh_msg_seq();
    if (ms != s_seen_msg) { s_seen_msg = ms; s_pulse = 8; }

    s_blink++;
    if (s_flash_ttl > 0) s_flash_ttl--;
    if (s_air_frames > 0) s_air_frames--;

    /*/Every touch rect is cleared before the frame that
       fills it, for the same reason s_setup_rect is cleared in
       draw_portrait: a rect left over from the page you were on last is a
       tap that lands on something no longer drawn. The draw puts back
       whatever it actually put on the screen. */
    s_compose_rect = tui_rect_make(0, -1, 0, 0);
    s_node_rect    = tui_rect_make(0, -1, 0, 0);
    s_tx_rect      = tui_rect_make(0, -1, 0, 0);
    for (int i = 0; i < DETAIL_ACTS; i++) s_act_rect[i] = tui_rect_make(0, -1, 0, 0);

    const uint32_t now = screen_now();
    if (ls_tui_is_wide()) draw_landscape(sf, area, &st, now);
    else                  draw_portrait(sf, area, &st, now);
}

/* ----------------------------------------------------------------- input */

static void act_start_on(void)
{
    flash(ls_mesh_start() == ESP_OK ? "started" : "start failed");
}

static void act_stop(void)
{
    ls_mesh_stop();
    flash("stopped");
}

static void act_set_tx(bool on)
{
    if (!ls_mesh_running()) { flash("not running - /start"); return; }
    ls_mesh_set_tx(on);
    flash(on ? "TRANSMIT ARMED" : "transmit disarmed");
}

static bool act_send(void)
{
    if (!s_compose_len) return true;

    esp_err_t err;
    if (chat_locked()) {
        if (!ls_mesh_peer_known(s_lock_id)) {
            flash("not heard since boot - waiting for its advert");
            return true;
        }
        err = ls_mesh_send_dm_id(s_lock_id, s_compose);
    } else {
        err = ls_mesh_send_text(s_compose);
    }
    if (err == ESP_ERR_NOT_ALLOWED) { flash("disarmed - type /arm"); return true; }
    if (err == ESP_ERR_NOT_FOUND)   { flash("that node is not in the list"); return true; }
    if (err != ESP_OK)              { flash("send failed"); return true; }
    ls_mesh_stats_t st;
    ls_mesh_get_stats(&st);
    s_air_total = 18;
    const uint32_t air = ls_lora_airtime_ms((int)strlen(s_compose) + 24);
    if (air) s_air_total = (int)(air / 40) + 2;
    s_air_frames = s_air_total;
    s_compose_len = 0; s_compose[0] = 0;
    return true;
}

/* Typing without a keyboard attached.

   The compose line took characters from the router and there was no way to
   produce one in portrait, so portrait could read the channel and not answer
   on it. The overlay fills the same buffer the physical keyboard fills and
   then goes down the same path, slash commands included: a message typed on
   glass and a message typed on keys must not be able to behave differently,
   which they would the moment there were two send functions. */
static bool slash(const char *text);

static void compose_done(const char *text)
{
    snprintf(s_compose, sizeof(s_compose), "%s", text ? text : "");
    s_compose_len = (int)strlen(s_compose);
    if (!s_compose_len) return;
    if (slash(s_compose)) { s_compose_len = 0; s_compose[0] = 0; return; }
    act_send();
}

static void open_compose(void)
{
    ls_keyboard_open("MESSAGE", s_compose, COMPOSE_MAX - 1, compose_done);
}

/* The one-shot direct message is gone with the row it used. */

/* One of the four things a node's page can do. Shared by the letter keys and
   by the buttons, so a tap and a keystroke cannot drift apart. */
static void detail_act(int which)
{

    act_press_mark(which);

    const int n = ls_mesh_peers(s_peer_buf, LS_MESH_MAX_PEERS);
    const int idx = detail_index(n);
    if (idx < 0) { flash("that node is no longer in the list"); return; }
    const ls_mesh_peer_t *p = &s_peer_buf[idx];

    switch (which) {
    case 0:

        chat_lock_to(p->id, p->name);
        s_detail = false;
        s_page = PAGE_CHAT;
        flash("chat locked - ESC or tap the name to leave");
        return;
    case 1: {
        const esp_err_t e = ls_mesh_add_contact(p->id, p->name);
        flash(e == ESP_OK ? "saved as a contact" : "could not save it");
        return;
    }
    case 2: {
        if (!p->has_loc) { flash("this node sends no position"); return; }

        /* Do not navigate to a map that cannot draw this node. */

        const double nlat = p->lat_e6 / 1e6, nlon = p->lon_e6 / 1e6;
        const int z = ls_map_zoom_covering(nlat, nlon);
        if (z < 0) {
            flash("that node is outside this map archive");
            return;
        }
        if (z != ls_map_zoom()) ls_map_zoom_by(z - ls_map_zoom());
        ls_map_center(nlat, nlon);
        const ls_app_t *app = ls_app_by_id("map");
        const int si = (app && app->screen) ? ls_tui_screen_index_of(app->screen)
                                            : -1;
        if (si >= 0) ls_tui_screen_show(si);
        else flash("this build has no map screen");
        return;
    }
    case 3: {
        const esp_err_t e = ls_mesh_advertise();
        /* An advert is an announcement, not a question. */

        flash(e == ESP_OK ? "advert sent - this puts us in their list, "
                            "there is no reply to wait for"
                          : "cannot advertise while disarmed");
        return;
    }
    default: return;
    }
}

static void apply_field(void);
static void field_value(int i, char *out, size_t n);

static void setup_edit_done(const char *text)
{
    if (s_edit_field < 0) return;
    snprintf(s_edit, sizeof(s_edit), "%s", text ? text : "");
    s_field = s_edit_field;
    s_edit_field = -1;
    apply_field();
}

/* Open the overlay on a field, seeded with what the field says now.

   Titled with the field name, because the overlay covers the row it came
   from: the inline editor answered "what am I typing into" by being drawn on
   that row, and an overlay has to say it.  */
static void open_field_edit(int field)
{
    char title[24];
    if (field >= CHAN_FIELD_BASE) {
        ls_mesh_chan_t c[LS_MESH_MAX_CHANNELS];
        ls_mesh_channels(c, LS_MESH_MAX_CHANNELS);
        const int idx = field - CHAN_FIELD_BASE;
        snprintf(s_edit, sizeof(s_edit), "%s", c[idx].psk);
        snprintf(title, sizeof(title), "CHANNEL %d KEY", idx);
    } else {
        field_value(field, s_edit, sizeof(s_edit));
        snprintf(title, sizeof(title), "%s", FIELDS[field].label);
        for (char *q = title; *q; q++)
            if (*q >= 0x61 && *q <= 0x7A) *q = (char)(*q - 32);
    }
    s_edit_field = field;
    ls_keyboard_open(title, s_edit, EDIT_MAX - 1, setup_edit_done);
}

/* SETUP: apply whatever is in the edit buffer to the selected field. */
static void apply_field(void)
{
    /* A channel slot first: its edit buffer is a base64 key, not a
       number, and falling through to the radio switch would misread it. */
    if (s_field >= CHAN_FIELD_BASE) {
        const int idx = s_field - CHAN_FIELD_BASE;
        char name[LS_MESH_CHAN_NAME];
        snprintf(name, sizeof(name), "chan%d", idx);
        const esp_err_t e = ls_mesh_set_channel(idx, name, s_edit);
        flash(e == ESP_OK ? (s_edit[0] ? "channel key saved" : "channel cleared")
                          : "key must be base64, 16 or 32 bytes");
        return;
    }

    ls_mesh_radio_t r;
    ls_mesh_get_radio(&r);
    char *end;
    switch (s_field) {
    case F_NAME:
        flash(ls_mesh_set_name(s_edit) == ESP_OK ? "name saved" : "bad name");
        return;
    case F_CR: {
        const long cr = strtol(s_edit, &end, 10);
        if (end == s_edit || cr < 5 || cr > 8) { flash("coding rate 5-8"); return; }
        r.cr = (uint8_t)cr;
        break;
    }
    case F_AEVERY: {
        const long secs = strtol(s_edit, &end, 10);
        /* 0 is off. Below 30 s a node floods its own neighbours with
           adverts and eats the duty cycle it needs for messages. */
        if (end == s_edit || secs < 0 || secs > 3600) { flash("0-3600 s, 0 = never"); return; }
        if (secs > 0 && secs < 30) { flash("30 s minimum, or 0"); return; }
        r.advert_secs = (uint16_t)secs;
        break;
    }
    case F_FREQ: {
        const double mhz = strtod(s_edit, &end);
        if (end == s_edit || mhz < 137.0 || mhz > 1020.0) { flash("137-1020 MHz"); return; }
        r.freq_hz = (uint32_t)(mhz * 1e6 + 0.5);
        break;
    }
    case F_SF: {
        const long sf = strtol(s_edit, &end, 10);
        if (end == s_edit || sf < 5 || sf > 12) { flash("SF 5-12"); return; }
        r.sf = (uint8_t)sf;
        break;
    }
    case F_BW: {
        const double khz = strtod(s_edit, &end);
        if (end == s_edit || khz < 7.0 || khz > 500.0) { flash("7.8-500 kHz"); return; }
        r.bw_hz = (uint32_t)(khz * 1000.0 + 0.5);
        break;
    }
    case F_PWR: {
        const long p = strtol(s_edit, &end, 10);
        if (end == s_edit || p < -9 || p > 22) { flash("-9..22 dBm"); return; }
        r.power_dbm = (int8_t)p;
        break;
    }
    default: return;
    }

    flash(ls_mesh_set_radio(&r) == ESP_OK ? "applied and saved" : "apply failed");
}

/* Slash commands, so the chat page needs no key that anything else
   wants. Typed into the same compose line as a message, which makes them
   discoverable - `/help` is one keystroke away from anyone already typing -
   and impossible to hit by accident. Returns true when the text was a
   command and has been dealt with. */
static bool slash(const char *text)
{
    if (text[0] != '/') return false;
    const char *cmd = text + 1;

    if (!strncasecmp(cmd, "help", 4)) {
        flash("/start /stop /arm /disarm /advert /name X /to X /pub");
        return true;
    }

    if (!strncasecmp(cmd, "pub", 3)) {
        chat_unlock();
        flash("back on the channel");
        return true;
    }
    if (!strncasecmp(cmd, "to ", 3)) {
        const char *want = cmd + 3;
        while (*want == ' ') want++;
        if (!*want) { flash("/to <node id or name>"); return true; }

        const int n = ls_mesh_peers(s_peer_buf, LS_MESH_MAX_PEERS);
        int found = -1, hits = 0;
        for (int i = 0; i < n; i++) {
            if (!strcasecmp(s_peer_buf[i].id, want)) { found = i; hits = 1; break; }
            const size_t w = strlen(want);
            if (!strncasecmp(s_peer_buf[i].name, want, w) ||
                !strncasecmp(s_peer_buf[i].id, want, w)) {
                found = i;
                hits++;
            }
        }
        if (hits == 0) { flash("no node here by that name"); return true; }
        if (hits > 1)  { flash("more than one node matches - use the id"); return true; }
        chat_lock_to(s_peer_buf[found].id, s_peer_buf[found].name);
        flash("chat locked - /pub or tap the name to leave");
        return true;
    }
    if (!strncasecmp(cmd, "start", 5))  { act_start_on();  return true; }
    if (!strncasecmp(cmd, "stop", 4))   { act_stop();      return true; }
    if (!strncasecmp(cmd, "arm", 3))    { act_set_tx(true);  return true; }
    if (!strncasecmp(cmd, "disarm", 6)) { act_set_tx(false); return true; }
    if (!strncasecmp(cmd, "advert", 6)) {
        const esp_err_t e = ls_mesh_advertise();
        flash(e == ESP_ERR_NOT_ALLOWED ? "arm first: /arm"
                                       : (e == ESP_OK ? "advert sent" : "advert failed"));
        return true;
    }
    if (!strncasecmp(cmd, "name", 4)) {
        const char *arg = cmd + 4;
        while (*arg == ' ') arg++;
        if (!*arg) { flash("usage: /name <name>"); return true; }
        flash(ls_mesh_set_name(arg) == ESP_OK ? "name saved" : "bad name");
        return true;
    }
    flash("unknown command - /help");
    return true;
}

static bool key(ls_tk_t k, char ch)
{
    /* NO FUNCTION KEY IS CONSUMED HERE, on purpose. F1..F8 are the
       global app switches and the router gives this screen first refusal, so
       taking one would silently disable switching apps while standing on
       this screen. Pages move on LEFT/RIGHT, which the router does not
       bind. */
    /* MIC arms and disarms the transmitter, on every page. */

    if (k == LS_TK_MIC) {
        act_set_tx(!ls_mesh_tx_enabled());
        return true;
    }
    /* Hold arms it for good: auto transmit, which survives a reboot.
       The two live on one key because they are the same intention at two
       time scales - "transmit now" and "transmit from now on" - and because
       the second is the one you want to be slightly harder to do by
       accident. The confirmation line says which happened, in as many words,
       because the difference matters. */
    if (k == LS_TK_MIC_HOLD) {
        const bool on = !ls_mesh_auto_tx();
        ls_mesh_set_auto(ls_mesh_auto_listen(), on);
        flash(on ? "AUTO TRANSMIT ON - armed from every boot"
                 : "auto transmit off - boots disarmed");
        return true;
    }

    if (k == LS_TK_LEFT) {
        s_page = (mesh_page_t)((s_page + PAGE__COUNT - 1) % PAGE__COUNT);
        s_edit_field = -1;
        s_detail = false;
        return true;
    }
    if (k == LS_TK_RIGHT) {
        s_page = (mesh_page_t)((s_page + 1) % PAGE__COUNT);
        s_edit_field = -1;
        s_detail = false;
        return true;
    }

    if (s_page == PAGE_SETUP) {
        switch (k) {
        case LS_TK_UP:   if (s_field > 0) s_field--; return true;
        case LS_TK_DOWN:
            if (s_field < FIELD_COUNT + LS_MESH_MAX_CHANNELS - 1) s_field++;
            return true;
        case LS_TK_CHAR:
            /* SPACE selects the channel to send on. A letter would be
               ambiguous next to the S/T shortcuts below. */
            if (ch == ' ' && s_field >= CHAN_FIELD_BASE) {
                const int idx = s_field - CHAN_FIELD_BASE;
                const esp_err_t e = ls_mesh_set_channel_active(idx);
                flash(e == ESP_OK ? "sending on this channel"
                      : e == ESP_ERR_INVALID_STATE ? "no key set on that channel"
                      : "cannot select");
                return true;
            }
            break;
        case LS_TK_ENTER:
            if (s_field >= CHAN_FIELD_BASE) {
                if (s_field == CHAN_FIELD_BASE) {
                    flash("the public channel is fixed");
                    return true;
                }
                open_field_edit(s_field);
                return true;
            }
            if (FIELDS[s_field].kind == F_TOGGLE) {
                const char *msg = toggle_field(s_field);
                if (msg) flash(msg);
                return true;
            }
            open_field_edit(s_field);
            return true;
        default: return false;
        }
    }

    if (s_page == PAGE_NODES) {
        /* The node's own page. ESC leaves it, the four letters
           printed on its buttons do the four things, and UP/DOWN still move
           the list underneath in landscape - where the list is still on
           screen beside it - so you can walk the nodes without closing and
           reopening the page. */
        if (s_detail) {
            if (k == LS_TK_ESC) { s_detail = false; return true; }
            if (k == LS_TK_CHAR) {
                for (int i = 0; i < DETAIL_ACTS; i++)
                    if (ch == ACT_KEY[i] || ch == (char)(ACT_KEY[i] - 32)) {
                        detail_act(i);
                        return true;
                    }
            }
            if (k == LS_TK_UP || k == LS_TK_DOWN) {
                const int n = ls_mesh_peers(s_peer_buf, LS_MESH_MAX_PEERS);
                if (k == LS_TK_UP && s_sel > 0) s_sel--;
                if (k == LS_TK_DOWN && s_sel < n - 1) s_sel++;
                if (s_sel >= 0 && s_sel < n)
                    snprintf(s_detail_id, sizeof(s_detail_id), "%s",
                             s_peer_buf[s_sel].id);
                return true;
            }
            return true;      /* the page owns the rest while it is open */
        }
        switch (k) {
        case LS_TK_UP:   if (s_sel > 0) s_sel--; return true;
        case LS_TK_DOWN: s_sel++; return true;      /* clamped in draw */
        case LS_TK_ENTER: {
            const int n = ls_mesh_peers(s_peer_buf, LS_MESH_MAX_PEERS);
            if (s_sel < 0 || s_sel >= n) { flash("nothing heard yet"); return true; }
            snprintf(s_detail_id, sizeof(s_detail_id), "%s", s_peer_buf[s_sel].id);
            s_detail = true;
            return true;
        }
        default: break;
        }
        /* Letters are free on this page - nothing here is a text field. */
        if (k == LS_TK_CHAR) {
            switch (ch) {
            case 's': case 'S': ls_mesh_running() ? act_stop() : act_start_on(); return true;
            case 't': case 'T': act_set_tx(!ls_mesh_tx_enabled()); return true;
            case 'a': case 'A': ls_mesh_advertise(); flash("advert sent"); return true;
            default: break;
            }
        }
        return false;
    }

    /* CHAT: the compose line has the keyboard. Letters are text. */
    switch (k) {
    case LS_TK_ENTER:
        if (!s_compose_len) return true;
        if (slash(s_compose)) { s_compose_len = 0; s_compose[0] = 0; return true; }
        return act_send();
    case LS_TK_BACKSPACE:
        if (s_compose_len) s_compose[--s_compose_len] = 0;
        return true;
    case LS_TK_ESC:
        if (s_compose_len) { s_compose_len = 0; s_compose[0] = 0; return true; }
        /* Then the lock, so ESC walks out of the conversation the
           same way it walks out of everything else here: the draft first,
           then the destination, then the screen. */
        if (chat_locked()) { chat_unlock(); flash("back on the channel"); return true; }
        return false;                     /* empty: let the router take it */
    case LS_TK_CHAR:
        if (ch >= 0x20 && ch < 0x7F && s_compose_len < COMPOSE_MAX - 1) {
            s_compose[s_compose_len++] = ch;
            s_compose[s_compose_len] = 0;
        }
        return true;
    default:
        return false;
    }
}

static bool touch(int col, int row)
{
    if (!ls_tui_is_wide()) {
        /* The page bar first: it is drawn over everything else and is the
           only way to reach NODES and SETUP without a keyboard. */
        if (row >= s_page_bar.y && row < s_page_bar.y + s_page_bar.h) {
            for (int i = 0; i < PAGE__COUNT; i++) {
                const tui_rect t = page_tab_rect(s_page_bar, i);
                if (col >= t.x && col < t.x + t.w) {
                    s_page = (mesh_page_t)i;
                    s_edit_field = -1;
                    s_detail = false;
                    return true;
                }
            }
            return true;     /* a miss on the strip is not the body's tap */
        }

        if (s_setup_rect.h > 0 && row > s_setup_rect.y &&
            row < s_setup_rect.y + s_setup_rect.h - 1 &&
            col >= s_setup_rect.x && col < s_setup_rect.x + s_setup_rect.w) {
            /* Through setup_field_at, which is also what the draw
               uses - a hit test that recomputed the row height separately is
               how a tap comes to land one field off the moment either
               changes. */
            const int i = setup_field_at(s_setup_rect, row);
            if (i >= 0) {
                if (i == s_field) return key(LS_TK_ENTER, 0);
                s_field = i;
                return true;
            }
            return true;
        }

        /* A node's page: the four action buttons, then anything
           else on the page closes nothing - ESC and the page bar are the
           ways out, so a stray tap cannot lose the page you opened. */
        if (s_detail) {
            for (int i = 0; i < DETAIL_ACTS; i++) {
                const tui_rect a = s_act_rect[i];
                if (a.h > 0 && row >= a.y && row < a.y + a.h &&
                    col >= a.x && col < a.x + a.w) {
                    detail_act(i);
                    return true;
                }
            }
        }

        /* A node in the list: the first tap picks it and a second
           tap on the same node opens it, which is the rule SETUP's fields
           already use - one tap must never fire an action, because a tap
           that both moves the cursor and does something is a tap you cannot
           take back. */
        if (!s_detail && s_node_rect.h > 0 && row > s_node_rect.y &&
            row < s_node_rect.y + s_node_rect.h - 1 &&
            col >= s_node_rect.x && col < s_node_rect.x + s_node_rect.w) {
            const int n = ls_mesh_peers(s_peer_buf, LS_MESH_MAX_PEERS);
            const int i = node_at(s_node_rect, row);
            if (i >= 0 && i < n) {
                if (i == s_sel) return key(LS_TK_ENTER, 0);
                s_sel = i;
            }
            return true;
        }

        /* The destination chip first, because it sits inside the
           compose box and the box below takes everything else. */
        if (chat_locked() && s_lock_chip.h > 0 &&
            row >= s_lock_chip.y && row < s_lock_chip.y + s_lock_chip.h &&
            col >= s_lock_chip.x && col < s_lock_chip.x + s_lock_chip.w) {
            chat_unlock();
            flash("back on the channel");
            return true;
        }

        /* The compose target opens the keyboard. */
        if (s_compose_rect.h > 0 && row >= s_compose_rect.y &&
            row < s_compose_rect.y + s_compose_rect.h) {
            open_compose();
            return true;
        }

        /* The one button, and only when armed. */

        if (s_tx_rect.h > 0 && row >= s_tx_rect.y &&
            row < s_tx_rect.y + s_tx_rect.h) {
            if (!ls_mesh_running()) {
                flash("not running - open SETUP or type /start");
                return true;
            }
            act_set_tx(!ls_mesh_tx_enabled());
            return true;
        }
        return false;
    }

    /* Landscape: the tab strip is the bottom row, and it is the one thing
       worth hit-testing here - the rest of the page is a display. */

    int x = 1;
    for (int i = 0; i < PAGE__COUNT; i++) {
        const int w = (int)strlen(PAGE_NAMES[i]) + 2;
        if (col >= x && col < x + w) {
            s_page = (mesh_page_t)i; s_edit_field = -1; s_detail = false;
            return true;
        }
        x += w + 1;
    }

    for (int i = 0; i < DETAIL_ACTS; i++) {
        const tui_rect a = s_act_rect[i];
        if (a.h > 0 && row >= a.y && row < a.y + a.h &&
            col >= a.x && col < a.x + a.w) {
            detail_act(i);
            return true;
        }
    }
    if (s_page == PAGE_CHAT && s_compose_rect.h > 0 &&
        row >= s_compose_rect.y && row < s_compose_rect.y + s_compose_rect.h &&
        col >= s_compose_rect.x && col < s_compose_rect.x + s_compose_rect.w) {
        open_compose();
        return true;
    }
    if (s_page == PAGE_NODES && s_node_rect.h > 0 &&
        row > s_node_rect.y && row < s_node_rect.y + s_node_rect.h - 1 &&
        col >= s_node_rect.x && col < s_node_rect.x + s_node_rect.w) {
        const int n = ls_mesh_peers(s_peer_buf, LS_MESH_MAX_PEERS);
        const int i = node_at(s_node_rect, row);
        if (i >= 0 && i < n) {
            if (i == s_sel && !s_detail) {
                snprintf(s_detail_id, sizeof(s_detail_id), "%s", s_peer_buf[i].id);
                s_detail = true;
            } else {
                s_sel = i;
                if (s_detail)
                    snprintf(s_detail_id, sizeof(s_detail_id), "%s",
                             s_peer_buf[i].id);
            }
        }
        return true;
    }
    return false;
}

/* What MESH tells the rest of the unit while nobody is looking at it. */

extern const ls_tui_screen_t ls_scr_mesh;
static uint32_t s_told_msg;
static bool     s_told_init;

bool ls_scr_mesh_notice(ls_notice_t *out)
{
    const uint32_t seq = ls_mesh_msg_seq();
    if (!s_told_init) { s_told_init = true; s_told_msg = seq; return false; }
    if (seq == s_told_msg) return false;
    s_told_msg = seq;

    const int n = ls_mesh_messages(s_msg_buf, LS_MESH_MAX_MSGS);
    if (n < 1) return false;
    const ls_mesh_msg_t *m = &s_msg_buf[n - 1];
    if (m->mine) return false;

    snprintf(out->title, sizeof(out->title), "%s",
             m->direct ? "DIRECT MESSAGE" : "MESH");
    snprintf(out->body, sizeof(out->body), "%s", m->text);
    out->hue = TUI_CYAN;
    out->screen = ls_tui_screen_index_of(&ls_scr_mesh);
    return true;
}

const ls_tui_screen_t ls_scr_mesh = {
    .name = "MESH",
    .hint = "MIC arms  hold auto  LEFT/RIGHT page  ENTER sends",
    .enter = enter,
    .draw = draw,
    .key = key,
    .touch = touch,
};
