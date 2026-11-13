/* MESH as a desktop program: a messenger.

       Mesh  Chat  View  Node                  SHARK 62E29FE6  LISTENING
       [Chats] [Nodes]  [Stop] [Arm] [Advert]  [Find] [Options]
       +CHATS--------+ +# Public---------- 7 messages-+ +ON THIS CHANNEL-+
       | CHANNELS    | | 20:31 NORTHFIELD  repeater...| | who spoke, the |
       | # Public  2 | | 20:32 SHARK  copy    relayed | | radio, or the  |
       | DIRECT      | | ------------ new ----------- | | node you are   |
       | @ KB1QWE  1 | | 20:36 KB1QWE  are you on...  | | writing to     |
       | ^ NORTHFIELD| |------------------------------| |                |
       | ...         | | To # Public [type here] [Send]| |                |
       +-------------+ +------------------------------+ +----------------+
       status: chats, unread, nodes, the last thing said

   Nodes swaps the middle for a table of every node heard, sorted by any
   column. The radio, the message ring and every action are the handheld's
   (scr_mesh.c, ls_mesh.h); what a conversation is and what is unread is
   ls_mesh_desk.c; the widgets are ls_deskui.c. Unread counts are since
   boot: the ring is in RAM and holds the last LS_MESH_MAX_MSGS messages. */
#include "scr_mesh_desk.h"

#if LS_TUI_HAS_COMPACT

#include "../../ls_deskui.h"
#include "../../ls_mesh_desk.h"
#include "../../ls_box.h"
#include "../../ls_geo.h"
#include "../../ls_text.h"
#include "../../ls_input_ev.h"
#include "../../ls_tui_touch.h"
#include "../../ls_tui_screen.h"
#include "ls_gps.h"
#include "core/settings.h"
#include "esp_attr.h"
#include "esp_timer.h"
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#ifdef _WIN32                   /* the bench on Windows */
#include <direct.h>
#define make_dir(p) _mkdir(p)
#else
#define make_dir(p) mkdir((p), 0775)
#endif

#define HUE TUI_CYAN
#define WHITE TUI_ATTR(TUI_WHITE | TUI_BRIGHT, TUI_BLACK)
#define INK(h) TUI_ATTR((h) | TUI_BRIGHT, TUI_BLACK)
#define SOFT(h) TUI_ATTR((h), TUI_BLACK)

/* ----------------------------------------------------------------- state -- */

#define CONVS 20
#define LINES 320
static EXT_RAM_BSS_ATTR ls_mesh_msg_t s_msg[LS_MESH_MAX_MSGS];
static EXT_RAM_BSS_ATTR ls_mesh_peer_t s_peer[LS_MESH_MAX_PEERS];
static EXT_RAM_BSS_ATTR ls_mesh_chan_t s_chan[LS_MESH_MAX_CHANNELS];
static EXT_RAM_BSS_ATTR ls_mesh_event_t s_ev[LS_MESH_MAX_EVENTS];
static EXT_RAM_BSS_ATTR float s_scope[LS_MESH_SCOPE_N];
static EXT_RAM_BSS_ATTR ls_meshd_conv_t s_conv[CONVS];
static EXT_RAM_BSS_ATTR ls_meshd_read_t s_read;
static EXT_RAM_BSS_ATTR float s_dist[LS_MESH_MAX_PEERS], s_brg[LS_MESH_MAX_PEERS];
static EXT_RAM_BSS_ATTR uint8_t s_order[LS_MESH_MAX_PEERS];
static EXT_RAM_BSS_ATTR ls_mesh_stats_t s_st;
static EXT_RAM_BSS_ATTR int s_nmsg, s_npeer, s_nchan, s_nconv, s_nev, s_nscope;
static EXT_RAM_BSS_ATTR ls_mesh_radio_t s_cfg;
static EXT_RAM_BSS_ATTR int64_t s_slow_us;
static EXT_RAM_BSS_ATTR bool s_here;
static EXT_RAM_BSS_ATTR uint32_t s_now;

/* The conversation that is open, by what it is: the list is rebuilt every
   frame and its rows move. */
static EXT_RAM_BSS_ATTR ls_meshd_conv_t s_open;
static EXT_RAM_BSS_ATTR int s_cur;                       /* its row in s_conv */
static EXT_RAM_BSS_ATTR int s_list_cur, s_list_first;    /* the keyboard's row in the list */
static EXT_RAM_BSS_ATTR uint32_t s_new_mark;             /* the first message that was unread when it was opened */
static EXT_RAM_BSS_ATTR uint32_t s_pick_mark;            /* the message a click chose */

enum { TAB_CHAT, TAB_NODES };
enum { F_COMPOSE, F_LIST, F_FIND, F_TABLE };
static EXT_RAM_BSS_ATTR int s_tab, s_focus;
static EXT_RAM_BSS_ATTR bool s_classic, s_show_list, s_show_info;
static EXT_RAM_BSS_ATTR bool s_find_on;
static EXT_RAM_BSS_ATTR ls_dui_edit_t s_compose, s_find;
static EXT_RAM_BSS_ATTR int s_back;                      /* feed lines scrolled up from the newest */

/* The feed, a row at a time. */
enum { L_HEAD, L_MORE, L_DAY, L_NEW };
typedef struct { int8_t msg; uint8_t kind; uint16_t off; } line_t;
static EXT_RAM_BSS_ATTR line_t s_line[LINES];
static EXT_RAM_BSS_ATTR int s_nline, s_feed_first, s_text_w, s_name_w, s_shown;

/* The node table. */
static const ls_dui_col_t NCOLS[LS_MN__COUNT] = {
    [LS_MN_NAME]    = { "NAME",    18, 10, LS_DUI_FLEX, 0 },
    [LS_MN_ID]      = { "ID",      16, 16, 0, 5 },
    [LS_MN_ROLE]    = { "ROLE",    8,  8, 0, 3 },
    [LS_MN_HEARD]   = { "HEARD",   6,  6, LS_DUI_RIGHT, 0 },
    [LS_MN_RSSI]    = { "RSSI",    12, 12, 0, 0 },
    [LS_MN_SNR]     = { "SNR",     6,  6, LS_DUI_RIGHT, 2 },
    [LS_MN_DIST]    = { "RANGE",   9,  9, LS_DUI_RIGHT, 1 },
    [LS_MN_ADVERTS] = { "ADVERTS", 7,  7, LS_DUI_RIGHT, 6 },
    [LS_MN_FIRST]   = { "SINCE",   6,  6, LS_DUI_RIGHT, 7 },
};
static EXT_RAM_BSS_ATTR ls_dui_table_t s_t;
static EXT_RAM_BSS_ATTR int s_nsort, s_nsel;
static EXT_RAM_BSS_ATTR bool s_ndesc;
static EXT_RAM_BSS_ATTR char s_nsel_id[17];
static EXT_RAM_BSS_ATTR uint32_t s_nshown;

static EXT_RAM_BSS_ATTR tui_rect s_area, s_r_menu, s_r_tools, s_r_list, s_r_main, s_r_info, s_r_status, s_r_feed,
                                 s_r_compose, s_r_find, s_r_send, s_r_edit;
static EXT_RAM_BSS_ATTR int16_t s_bar_x[8];
static EXT_RAM_BSS_ATTR int s_list_y, s_list_rows;
static EXT_RAM_BSS_ATTR int8_t s_list_row[64];          /* a drawn list row: the conversation, or -1 */

enum {
    C_RUN, C_ARM, C_ADVERT, C_OPTIONS, C_RADIO, C_HELP, C_CHATS, C_NODES, C_FIND, C_READ_ALL, C_EXPORT,
    C_CLASSIC, C_PANE_LIST, C_PANE_INFO, C_NEXT_UNREAD, C_PUBLIC, C_SEND, C_PASTE,
    C_MSG_COPY, C_MSG_QUOTE, C_MSG_DM, C_MSG_WHO,
    C_NODE_DM, C_NODE_MAP, C_NODE_SAVE, C_NODE_INSPECT, C_NODE_PAGE, C_NODE_COPY, C_NODE_COPY_POS,
    C_GO = 60,              /* + conversation row */
    C_SORT = 100,           /* + column */
    C_COL = 120,            /* + column: show or hide */
};

typedef struct { tui_rect r; int16_t cmd; } hit_t;
static EXT_RAM_BSS_ATTR hit_t s_hit[16];
static EXT_RAM_BSS_ATTR int s_nhit;

#define ITEMS 36
static EXT_RAM_BSS_ATTR ls_dui_popup_t s_pop;
static EXT_RAM_BSS_ATTR ls_dui_item_t s_items[ITEMS];
static EXT_RAM_BSS_ATTR char s_label[ITEMS][40], s_keys[ITEMS][12];
static EXT_RAM_BSS_ATTR int s_nitems;
static EXT_RAM_BSS_ATTR char s_menu_id[17];             /* the node a menu was opened on */

static EXT_RAM_BSS_ATTR char s_say[96];
static EXT_RAM_BSS_ATTR int64_t s_said_us;
static EXT_RAM_BSS_ATTR char s_dir[96];

static void run(int cmd);

static void say(const char *text) { snprintf(s_say, sizeof(s_say), "%s", text); s_said_us = esp_timer_get_time(); }

bool ls_mesh_desk_classic(void) { return s_classic; }
void ls_mesh_desk_export_dir(const char *dir) { snprintf(s_dir, sizeof(s_dir), "%s", dir ? dir : ""); }

/* ------------------------------------------------------------------ data -- */

static int peer_of(const char *id)
{
    for (int i = 0; i < s_npeer; i++)
        if (!strcasecmp(s_peer[i].id, id)) return i;
    return -1;
}

static const ls_mesh_peer_t *node_sel(void)
{
    return s_nsel >= 0 && s_nsel < s_npeer ? &s_peer[s_order[s_nsel]] : NULL;
}

static bool same_conv(const ls_meshd_conv_t *a, const ls_meshd_conv_t *b)
{
    if (a->kind != b->kind) return false;
    if (a->kind == LS_MC_DM) return !strcasecmp(a->id, b->id);
    return a->kind == LS_MC_PUBLIC || a->chan == b->chan;
}

static const ls_meshd_conv_t *cur(void) { return s_cur >= 0 && s_cur < s_nconv ? &s_conv[s_cur] : &s_open; }

static uint8_t role_hue(uint8_t role)
{
    switch (role) {
    case LS_MESH_ROLE_REPEATER: return TUI_YELLOW;
    case LS_MESH_ROLE_ROOM:     return TUI_MAGENTA;
    case LS_MESH_ROLE_SENSOR:   return TUI_BLUE;
    default:                    return TUI_GREEN;
    }
}

static char conv_glyph(const ls_meshd_conv_t *c)
{
    if (c->kind != LS_MC_DM) return '#';
    switch (c->role) {
    case LS_MESH_ROLE_REPEATER: return '^';
    case LS_MESH_ROLE_ROOM:     return '&';
    case LS_MESH_ROLE_SENSOR:   return '~';
    default:                    return '@';
    }
}

/* A colour a name keeps: the same node is the same colour in every line. */
static uint8_t name_hue(const char *name)
{
    static const uint8_t HUES[] = { TUI_MAGENTA, TUI_GREEN, TUI_CYAN, TUI_BLUE, TUI_RED };
    uint32_t h = 2166136261u;
    for (const char *c = name; *c; c++) h = (h ^ (uint8_t)*c) * 16777619u;
    return HUES[h % (sizeof(HUES) / sizeof(HUES[0]))];
}

static void fetch(void)
{
    s_now = ls_mesh_now();
    s_nmsg = ls_mesh_messages(s_msg, LS_MESH_MAX_MSGS);
    s_npeer = ls_mesh_peers(s_peer, LS_MESH_MAX_PEERS);
    /* The rest moves slowly and each is a question put to the mesh task:
       twice a second is as often as anyone reads them. */
    const int64_t now_us = esp_timer_get_time();
    if (!s_slow_us || now_us < s_slow_us || now_us - s_slow_us >= 500000) {
        s_slow_us = now_us;
        s_nchan = ls_mesh_channels(s_chan, LS_MESH_MAX_CHANNELS);
        s_nev = ls_mesh_events(s_ev, LS_MESH_MAX_EVENTS);
        s_nscope = ls_mesh_scope(s_scope, LS_MESH_SCOPE_N);
        ls_mesh_get_radio(&s_cfg);
    }
    if (s_nmsg < 0) s_nmsg = 0;
    if (s_npeer < 0) s_npeer = 0;
    if (s_nchan < 0) s_nchan = 0;
    if (s_nev < 0) s_nev = 0;

    double lat = 0, lon = 0;
    ls_gps_state_t g;
    ls_gps_get(&g);
    float hlat, hlon;
    s_here = false;
    if (g.fix) { lat = g.lat_deg; lon = g.lon_deg; s_here = true; }
    else if (settings_get_home(&hlat, &hlon)) { lat = (double)hlat; lon = (double)hlon; s_here = true; }
    for (int i = 0; i < s_npeer; i++) {
        s_dist[i] = s_brg[i] = -1;
        if (!s_here || !s_peer[i].has_loc) continue;
        double brg = 0, range = 0;
        ls_geo_bearing_range(lat, lon, s_peer[i].lat_e6 / 1e6, s_peer[i].lon_e6 / 1e6, &brg, &range);
        s_dist[i] = (float)range;
        s_brg[i] = (float)brg;
    }
    ls_meshd_sort(s_order, s_peer, s_npeer, s_nsort, s_ndesc, s_dist);
    /* The cursor stays on its node while the order moves under it. */
    if (s_nsel_id[0]) {
        for (int k = 0; k < s_npeer; k++)
            if (!strcasecmp(s_peer[s_order[k]].id, s_nsel_id)) { s_nsel = k; break; }
    }
    if (s_nsel >= s_npeer) s_nsel = s_npeer ? s_npeer - 1 : 0;
    if (s_nsel < 0) s_nsel = 0;
    if (node_sel()) snprintf(s_nsel_id, sizeof(s_nsel_id), "%s", node_sel()->id);

    s_nconv = ls_meshd_convs(s_conv, CONVS, s_msg, s_nmsg, s_chan, s_nchan, s_peer, s_npeer, &s_read);
    s_cur = -1;
    for (int i = 0; i < s_nconv && s_cur < 0; i++)
        if (same_conv(&s_conv[i], &s_open)) s_cur = i;
    if (s_cur < 0) {
        /* A DM with a node that is gone and left nothing: back to the channel. */
        memset(&s_open, 0, sizeof(s_open));
        s_open.kind = LS_MC_PUBLIC;
        s_cur = 0;
        ls_scr_mesh_dest_set("", "", 0);
    }
    s_open = s_conv[s_cur];
    /* What is on the screen has been read. */
    if (s_tab == TAB_CHAT && s_conv[s_cur].unread && !s_back) {
        ls_meshd_read_all(&s_read, &s_conv[s_cur], s_msg, s_nmsg);
        s_conv[s_cur].unread = 0;
    }
    if (s_list_cur >= s_nconv) s_list_cur = s_nconv - 1;
    if (s_list_cur < 0) s_list_cur = 0;
}

static void open_conv(int i)
{
    if (i < 0 || i >= s_nconv) return;
    const ls_meshd_conv_t *c = &s_conv[i];
    /* The line the new ones start at, kept while this conversation is open. */
    s_new_mark = 0;
    if (c->unread) {
        int left = c->unread;
        for (int k = s_nmsg - 1; k >= 0 && left > 0; k--) {
            if (!ls_meshd_belongs(&s_msg[k], c) || s_msg[k].mine) continue;
            s_new_mark = ls_meshd_mark(&s_msg[k]);
            left--;
        }
    }
    s_open = *c;
    s_cur = s_list_cur = i;
    s_back = 0;
    s_pick_mark = 0;
    s_tab = TAB_CHAT;
    s_focus = F_COMPOSE;
    ls_meshd_read_all(&s_read, c, s_msg, s_nmsg);
    ls_scr_mesh_dest_set(c->kind == LS_MC_DM ? c->id : "", c->name, c->kind == LS_MC_PUBLIC ? 0 : c->chan);
}

static void open_dm(const char *id)
{
    for (int i = 0; i < s_nconv; i++)
        if (s_conv[i].kind == LS_MC_DM && !strcasecmp(s_conv[i].id, id)) { open_conv(i); return; }
    say("That node is no longer in the list");
}

static int unread_total(void)
{
    int n = 0;
    for (int i = 0; i < s_nconv; i++) n += s_conv[i].unread;
    return n;
}

/* The message a click chose, if it is still in the ring. */
static const ls_mesh_msg_t *picked(void)
{
    if (!s_pick_mark) return NULL;
    for (int i = 0; i < s_nmsg; i++)
        if (ls_meshd_belongs(&s_msg[i], cur()) && ls_meshd_mark(&s_msg[i]) == s_pick_mark) return &s_msg[i];
    return NULL;
}

/* ----------------------------------------------------------------- menus -- */

static void items_begin(void) { s_nitems = 0; }

static void item(const char *label, const char *keys, int id, int flags)
{
    if (s_nitems >= ITEMS) return;
    ls_dui_item_t *it = &s_items[s_nitems];
    memset(it, 0, sizeof(*it));
    if (label) { snprintf(s_label[s_nitems], sizeof(s_label[0]), "%s", label); it->label = s_label[s_nitems]; }
    if (keys) { snprintf(s_keys[s_nitems], sizeof(s_keys[0]), "%s", keys); it->keys = s_keys[s_nitems]; }
    it->id = (int16_t)id;
    it->flags = (uint8_t)flags;
    s_nitems++;
}

static void rule(void) { item(NULL, NULL, -1, LS_DUI_SEP); }
static void popup(int x, int y, int bar) { ls_dui_popup_open(&s_pop, s_items, s_nitems, x, y, bar, HUE); }

static const char *const MENUS[] = { "Mesh", "Chat", "View", "Node" };
#define NMENUS 4

static void node_items(const ls_mesh_peer_t *p)
{
    const int off = p ? 0 : LS_DUI_OFF;
    snprintf(s_menu_id, sizeof(s_menu_id), "%s", p ? p->id : "");
    item("Message", "Enter", C_NODE_DM, off);
    item("Show on MAP", "M", C_NODE_MAP, off | (p && p->has_loc ? 0 : LS_DUI_OFF));
    item("Inspect: ping, trace, telemetry", "I", C_NODE_INSPECT, off);
    item("Node page", "P", C_NODE_PAGE, off);
    item("Save as a contact", NULL, C_NODE_SAVE, off);
    rule();
    item("Copy id", "Ctrl+C", C_NODE_COPY, off);
    item("Copy position", NULL, C_NODE_COPY_POS, off | (p && p->has_loc ? 0 : LS_DUI_OFF));
}

static void build_menu(int which)
{
    items_begin();
    switch (which) {
    case 0:
        item(s_st.running ? "Stop the mesh radio" : "Start the mesh radio", "S", C_RUN, 0);
        item(s_st.tx_enabled ? "Disarm transmit" : "Arm transmit", "MIC", C_ARM, s_st.running ? 0 : LS_DUI_OFF);
        item("Send an advert", "A", C_ADVERT, s_st.running && s_st.tx_enabled ? 0 : LS_DUI_OFF);
        rule();
        item("Radio...", "R", C_RADIO, 0);
        item("Options...", "O", C_OPTIONS, 0);
        item("Commands (/help)", NULL, C_HELP, 0);
        break;
    case 1:
        item("Find in this chat", "Ctrl+F", C_FIND, 0);
        item("Next unread", "Ctrl+U", C_NEXT_UNREAD, unread_total() ? 0 : LS_DUI_OFF);
        item("Mark everything read", NULL, C_READ_ALL, unread_total() ? 0 : LS_DUI_OFF);
        item("Back to the public channel", NULL, C_PUBLIC, cur()->kind == LS_MC_PUBLIC ? LS_DUI_OFF : 0);
        rule();
        item("Copy the chosen message", "Ctrl+C", C_MSG_COPY, picked() ? 0 : LS_DUI_OFF);
        item("Paste", "Ctrl+V", C_PASTE, ls_dui_clip_get()[0] ? 0 : LS_DUI_OFF);
        item("Save this chat to the card", "Ctrl+E", C_EXPORT, cur()->total ? 0 : LS_DUI_OFF);
        if (!s_r_list.w) {
            /* No room for the list in a window this narrow: it is here. */
            rule();
            for (int i = 0; i < s_nconv && s_nitems < ITEMS; i++) {
                char text[40], n[8];
                snprintf(text, sizeof(text), "%c %s", conv_glyph(&s_conv[i]), s_conv[i].name);
                snprintf(n, sizeof(n), "%d", s_conv[i].unread);
                item(text, s_conv[i].unread ? n : NULL, C_GO + i, i == s_cur ? LS_DUI_CHECK : 0);
            }
        }
        break;
    case 2:
        item("Chats", "Ctrl+1", C_CHATS, s_tab == TAB_CHAT ? LS_DUI_CHECK : 0);
        item("Nodes", "Ctrl+2", C_NODES, s_tab == TAB_NODES ? LS_DUI_CHECK : 0);
        rule();
        item("Chat list", NULL, C_PANE_LIST, s_show_list ? LS_DUI_CHECK : 0);
        item("Details", NULL, C_PANE_INFO, s_show_info ? LS_DUI_CHECK : 0);
        rule();
        for (int c = 0; c < LS_MN__COUNT; c++) {
            char text[40];
            snprintf(text, sizeof(text), "Sort nodes by %s", NCOLS[c].title);
            item(text, NULL, C_SORT + c, s_nsort == c ? LS_DUI_CHECK : 0);
        }
        rule();
        item("Handheld layout", "Ctrl+L", C_CLASSIC, 0);
        break;
    default: {
        const ls_mesh_peer_t *p = s_tab == TAB_NODES ? node_sel()
                                : cur()->kind == LS_MC_DM && peer_of(cur()->id) >= 0 ? &s_peer[peer_of(cur()->id)] : NULL;
        item(p ? (p->name[0] ? p->name : p->id) : "NO NODE CHOSEN", NULL, -1, LS_DUI_HEAD);
        node_items(p);
        break;
    }
    }
}

static void open_menu(int which)
{
    if (which < 0) which = NMENUS - 1;
    if (which >= NMENUS) which = 0;
    build_menu(which);
    popup(s_bar_x[which], s_r_menu.y + 1, which);
}

static void node_menu(const ls_mesh_peer_t *p, int x, int y)
{
    if (!p) return;
    items_begin();
    item(p->name[0] ? p->name : p->id, NULL, -1, LS_DUI_HEAD);
    node_items(p);
    popup(x, y, -1);
}

static void conv_menu(int i, int x, int y)
{
    const ls_meshd_conv_t *c = &s_conv[i];
    items_begin();
    item(c->name, NULL, -1, LS_DUI_HEAD);
    item("Open", "Enter", C_GO + i, 0);
    if (c->kind == LS_MC_DM) {
        const int p = peer_of(c->id);
        rule();
        node_items(p >= 0 ? &s_peer[p] : NULL);
        snprintf(s_menu_id, sizeof(s_menu_id), "%s", c->id);
        /* The id can be copied whether or not the node is still heard. */
        for (int k = 0; k < s_nitems; k++)
            if (s_items[k].id == C_NODE_COPY) s_items[k].flags &= (uint8_t)~LS_DUI_OFF;
    } else {
        item("Mark everything read", NULL, C_READ_ALL, unread_total() ? 0 : LS_DUI_OFF);
    }
    popup(x, y, -1);
}

static void msg_menu(const ls_mesh_msg_t *m, int x, int y)
{
    char who[LS_MESH_PEER_NAME + 4], text[12];
    ls_meshd_split(m, who, sizeof(who));
    items_begin();
    ls_meshd_time_text(m->t, text, sizeof(text));
    char head[40];
    snprintf(head, sizeof(head), "%.10s %.24s", text, who[0] ? who : "message");
    item(head, NULL, -1, LS_DUI_HEAD);
    item("Copy text", "Ctrl+C", C_MSG_COPY, 0);
    item("Copy sender", NULL, C_MSG_WHO, who[0] ? 0 : LS_DUI_OFF);
    item("Quote in the reply", NULL, C_MSG_QUOTE, 0);
    bool can = false;
    if (!m->mine) {
        if (m->direct) can = cur()->kind != LS_MC_DM;
        else for (int i = 0; i < s_npeer; i++) can = can || (who[0] && !strcasecmp(s_peer[i].name, who));
    }
    item("Message the sender", NULL, C_MSG_DM, can ? 0 : LS_DUI_OFF);
    popup(x, y, -1);
}

static void head_menu(int x, int y)
{
    items_begin();
    item("COLUMNS", NULL, -1, LS_DUI_HEAD);
    for (int c = 0; c < LS_MN__COUNT; c++)
        item(NCOLS[c].title, NULL, C_COL + c, ((s_nshown >> c) & 1 ? LS_DUI_CHECK : 0) | (NCOLS[c].drop ? 0 : LS_DUI_OFF));
    popup(x, y, -1);
}

/* --------------------------------------------------------------- actions -- */

static void send(void)
{
    if (!s_compose.len) return;
    if (s_compose.text[0] == '/') {
        if (ls_scr_mesh_slash(s_compose.text)) ls_dui_edit_set(&s_compose, "", 140);
        return;
    }
    const ls_meshd_conv_t *c = cur();
    if (!s_st.running) { say("Not running: Start first. The draft is kept."); return; }
    esp_err_t err;
    if (c->kind == LS_MC_DM) {
        if (!ls_mesh_peer_known(c->id)) { say("Not heard since boot: waiting for its advert. The draft is kept."); return; }
        err = ls_mesh_send_dm_id(c->id, s_compose.text);
    } else err = ls_mesh_send_text_on(c->kind == LS_MC_PUBLIC ? 0 : c->chan, s_compose.text);
    if (err == ESP_ERR_NOT_ALLOWED) { say("Disarmed: Arm transmit first. The draft is kept."); return; }
    if (err == ESP_ERR_INVALID_SIZE) { say("Too long for one packet: shorten it."); return; }
    if (err == ESP_ERR_INVALID_STATE && c->kind == LS_MC_CHANNEL) { say("That channel has no key here."); return; }
    if (err != ESP_OK) { say("Send failed. The draft is kept."); return; }
    say("Message queued");
    ls_dui_edit_set(&s_compose, "", 140);
    s_back = 0;
}

static void copy(const char *text, const char *what)
{
    char line[96];
    ls_dui_clip_set(text);
    snprintf(line, sizeof(line), "Copied %s", what);
    say(line);
}

static void export_chat(void)
{
    const ls_meshd_conv_t *c = cur();
    char path[160], stem[LS_MESH_PEER_NAME + 2], line[64];
    const char *dir = s_dir[0] ? s_dir : "/sdcard/mesh";
    int k = 0;
    for (const char *p = c->name; *p && k < (int)sizeof(stem) - 1; p++)
        stem[k++] = (*p >= 'A' && *p <= 'Z') ? (char)(*p + 32) : ((*p >= 'a' && *p <= 'z') || (*p >= '0' && *p <= '9')) ? *p : '_';
    stem[k] = 0;
    make_dir(dir);
    snprintf(path, sizeof(path), "%s/%s-%lu.txt", dir, stem[0] ? stem : "chat", (unsigned long)s_now);
    FILE *f = fopen(path, "w");
    if (!f) { say("No card to save to"); return; }
    fprintf(f, "MESH %s%s  saved %lu\n", c->kind == LS_MC_DM ? "direct with " : "channel ", c->name, (unsigned long)s_now);
    int n = 0;
    for (int i = 0; i < s_nmsg; i++) {
        const ls_mesh_msg_t *m = &s_msg[i];
        if (!ls_meshd_belongs(m, c)) continue;
        char who[LS_MESH_PEER_NAME + 4], when[12];
        const char *body = ls_meshd_split(m, who, sizeof(who));
        ls_meshd_time_text(m->t, when, sizeof(when));
        fprintf(f, "%lu\t%s\t%s\t%s\t%s\n", (unsigned long)m->t, when, who[0] ? who : (m->mine ? ls_mesh_name() : "-"), body,
                m->mine ? ls_meshd_state_word(m) : "received");
        n++;
    }
    fclose(f);
    snprintf(line, sizeof(line), "Saved %d message%s to %.40s", n, n == 1 ? "" : "s", strrchr(path, '/') ? strrchr(path, '/') + 1 : path);
    say(line);
}

static const ls_mesh_peer_t *menu_node(void)
{
    const int p = peer_of(s_menu_id);
    return p >= 0 ? &s_peer[p] : NULL;
}

static void run(int cmd)
{
    char text[LS_DUI_TEXT];
    const ls_mesh_msg_t *m = picked();
    if (cmd >= C_COL) {
        const int c = cmd - C_COL;
        if (c < LS_MN__COUNT) s_nshown ^= 1u << c;
        return;
    }
    if (cmd >= C_SORT) {
        const int c = cmd - C_SORT;
        if (c >= LS_MN__COUNT) return;
        if (s_nsort == c) s_ndesc = !s_ndesc;
        else { s_nsort = c; s_ndesc = false; }
        return;
    }
    if (cmd >= C_GO) { open_conv(cmd - C_GO); return; }
    switch (cmd) {
    case C_RUN: ls_scr_mesh_start(!s_st.running); break;
    case C_ARM: ls_scr_mesh_arm(!s_st.tx_enabled); break;
    case C_ADVERT: ls_scr_mesh_slash("/advert"); break;
    case C_OPTIONS: ls_scr_mesh_options(); break;
    case C_RADIO: ls_scr_mesh_radio(); break;
    case C_HELP: ls_scr_mesh_slash("/help"); break;
    case C_CHATS: s_tab = TAB_CHAT; s_focus = F_COMPOSE; break;
    case C_NODES: s_tab = TAB_NODES; s_focus = F_TABLE; s_find_on = false; break;
    case C_FIND:
        s_tab = TAB_CHAT;
        s_find_on = true;
        s_focus = F_FIND;
        s_find.caret = s_find.len;
        break;
    case C_READ_ALL:
        for (int i = 0; i < s_nconv; i++) ls_meshd_read_all(&s_read, &s_conv[i], s_msg, s_nmsg);
        s_new_mark = 0;
        say("Everything is marked read");
        break;
    case C_NEXT_UNREAD:
        for (int k = 1; k <= s_nconv; k++) {
            const int i = (s_cur + k) % s_nconv;
            if (s_conv[i].unread) { open_conv(i); return; }
        }
        say("Nothing unread");
        break;
    case C_PUBLIC: open_conv(0); break;
    case C_EXPORT: export_chat(); break;
    case C_CLASSIC:
        s_classic = true;
        s_pop.open = false;
        ls_scr_mesh_say("Handheld layout: Ctrl+L brings the desktop one back");
        break;
    case C_PANE_LIST: s_show_list = !s_show_list; if (s_focus == F_LIST) s_focus = F_COMPOSE; break;
    case C_PANE_INFO: s_show_info = !s_show_info; break;
    case C_SEND: send(); break;
    case C_PASTE:
        if (s_tab != TAB_CHAT) break;
        if (s_focus != F_FIND) s_focus = F_COMPOSE;
        if (!ls_dui_edit_paste(s_focus == F_FIND ? &s_find : &s_compose)) say("Nothing to paste");
        break;
    case C_MSG_COPY:
        if (m) copy(ls_meshd_split(m, NULL, 0), "the message");
        else if (s_compose.len) copy(s_compose.text, "the draft");
        else say("Click a message to choose it first");
        break;
    case C_MSG_WHO:
        if (m) { char who[LS_MESH_PEER_NAME + 4]; ls_meshd_split(m, who, sizeof(who)); copy(who, who); }
        break;
    case C_MSG_QUOTE:
        if (m) {
            snprintf(text, sizeof(text), "\"%.60s\" ", ls_meshd_split(m, NULL, 0));
            ls_dui_edit_set(&s_compose, text, 140);
            s_focus = F_COMPOSE;
        }
        break;
    case C_MSG_DM:
        if (m && !m->mine) {
            char who[LS_MESH_PEER_NAME + 4];
            ls_meshd_split(m, who, sizeof(who));
            if (m->direct) { open_dm(m->peer); break; }
            for (int i = 0; i < s_npeer; i++)
                if (!strcasecmp(s_peer[i].name, who)) { open_dm(s_peer[i].id); return; }
            say("That sender has not advertised: nothing to address yet");
        }
        break;
    case C_NODE_DM: if (s_menu_id[0]) open_dm(s_menu_id); break;
    case C_NODE_MAP: if (s_menu_id[0]) ls_scr_mesh_node_act(s_menu_id, 2); break;
    case C_NODE_SAVE: if (s_menu_id[0]) ls_scr_mesh_node_act(s_menu_id, 1); break;
    case C_NODE_INSPECT: if (s_menu_id[0]) ls_scr_mesh_node_act(s_menu_id, 3); break;
    case C_NODE_PAGE: if (s_menu_id[0]) ls_scr_mesh_node_act(s_menu_id, 4); break;
    case C_NODE_COPY: if (s_menu_id[0]) copy(s_menu_id, "the node id"); break;
    case C_NODE_COPY_POS: {
        const ls_mesh_peer_t *p = menu_node();
        if (p && p->has_loc) { snprintf(text, sizeof(text), "%.5f, %.5f", p->lat_e6 / 1e6, p->lon_e6 / 1e6); copy(text, text); }
        break;
    }
    default: break;
    }
}

/* A node command from a key: on the table's node, or the chat's. */
static void node_cmd(int cmd)
{
    const ls_mesh_peer_t *p = s_tab == TAB_NODES ? node_sel() : NULL;
    if (!p && s_conv[s_list_cur].kind == LS_MC_DM) {
        snprintf(s_menu_id, sizeof(s_menu_id), "%s", s_conv[s_list_cur].id);
        run(cmd);
        return;
    }
    if (!p) { say("Choose a node first"); return; }
    snprintf(s_menu_id, sizeof(s_menu_id), "%s", p->id);
    run(cmd);
}

/* ---------------------------------------------------------------- layout -- */

static void layout(tui_rect a)
{
    const tui_rect none = tui_rect_make(0, 0, 0, 0);
    int y = a.y;
    s_r_menu = tui_rect_make(a.x, y++, a.w, 1);
    s_r_tools = a.h >= 16 ? tui_rect_make(a.x, y++, a.w, 1) : none;
    s_r_status = tui_rect_make(a.x, a.y + a.h - 1, a.w, 1);
    const tui_rect body = tui_rect_make(a.x, y, a.w, a.y + a.h - 1 - y);
    const int lw = s_show_list && a.w >= 72 ? (a.w >= 120 ? 26 : 22) : 0;
    const int iw = s_show_info && a.w - lw >= 92 ? (a.w >= 150 ? 38 : 32) : 0;
    const int mx = lw ? body.x + lw - 1 : body.x;
    s_r_list = lw ? tui_rect_make(body.x, body.y, lw, body.h) : none;
    s_r_main = tui_rect_make(mx, body.y, body.x + body.w - mx - (iw ? iw - 1 : 0), body.h);
    s_r_info = iw ? tui_rect_make(body.x + body.w - iw, body.y, iw, body.h) : none;
    ls_dui_table_layout(&s_t, tui_rect_make(s_r_main.x + 1, s_r_main.y + 1, s_r_main.w - 2, s_r_main.h - 2),
                        NCOLS, LS_MN__COUNT, s_nshown | (1u << LS_MN_NAME) | (1u << LS_MN_HEARD) | (1u << LS_MN_RSSI));
}

static void hit(tui_rect r, int cmd)
{
    if (s_nhit < (int)(sizeof(s_hit) / sizeof(s_hit[0]))) { s_hit[s_nhit].r = r; s_hit[s_nhit].cmd = (int16_t)cmd; s_nhit++; }
}

static int tool(tui_surface *sf, tui_rect row, int x, const char *label, int cmd, bool on, bool off, uint8_t hue)
{
    tui_rect r;
    if (x + (int)strlen(label) + 2 > row.x + row.w) return 0;
    const int w = ls_dui_key(sf, row, x, row.y, label, on, off, hue, &r);
    if (!off) hit(r, cmd);
    return w + 1;
}

/* ------------------------------------------------------------------ draw -- */

static const char *state_word(void)
{
    if (!s_st.running) return "OFFLINE";
    if (s_st.held) return "RADIO HELD";
    if (s_st.on_air) return "ON AIR";
    if (s_st.rx_fault || !s_st.receiving) return "RX FAULT";
    return s_st.tx_enabled ? "ARMED" : "LISTENING";
}

static uint8_t state_hue(void)
{
    if (!s_st.running) return TUI_WHITE;
    if (s_st.rx_fault || !s_st.receiving) return TUI_RED;
    return s_st.tx_enabled ? TUI_RED : TUI_GREEN;
}

static void draw_menu(tui_surface *sf)
{
    ls_dui_bar_draw(sf, s_r_menu, MENUS, NMENUS, s_pop.open ? s_pop.bar : -1, HUE, s_bar_x);
    char text[64];
    snprintf(text, sizeof(text), "%s %.8s", ls_mesh_name(), s_st.self_id);
    const char *word = state_word();
    int x = s_r_menu.x + s_r_menu.w - (int)strlen(word) - 1;
    if (x <= s_bar_x[NMENUS] + 2) return;
    tui_put_str(sf, s_r_menu, x, s_r_menu.y, word, INK(state_hue()));
    x -= (int)strlen(text) + 2;
    if (x > s_bar_x[NMENUS] + 2) tui_put_str(sf, s_r_menu, x, s_r_menu.y, text, SOFT(TUI_YELLOW));
}

static void draw_tools(tui_surface *sf)
{
    const tui_rect r = s_r_tools;
    if (!r.h) return;
    char label[24];
    const int unread = unread_total();
    int x = r.x + 1;
    if (unread) snprintf(label, sizeof(label), "Chats %d", unread);
    else snprintf(label, sizeof(label), "Chats");
    x += tool(sf, r, x, label, C_CHATS, s_tab == TAB_CHAT, false, HUE);
    snprintf(label, sizeof(label), "Nodes %d", s_npeer);
    x += tool(sf, r, x, label, C_NODES, s_tab == TAB_NODES, false, TUI_MAGENTA);
    x += 1;
    x += tool(sf, r, x, s_st.running ? "Stop" : "Start", C_RUN, false, false, s_st.running ? TUI_YELLOW : TUI_GREEN);
    x += tool(sf, r, x, s_st.tx_enabled ? "Disarm" : "Arm", C_ARM, s_st.tx_enabled, !s_st.running, TUI_RED);
    x += tool(sf, r, x, "Advert", C_ADVERT, false, !s_st.running || !s_st.tx_enabled, TUI_GREEN);
    x += 1;
    x += tool(sf, r, x, "Find", C_FIND, s_find_on, false, TUI_YELLOW);
    x += tool(sf, r, x, "Save chat", C_EXPORT, false, !cur()->total, TUI_GREEN);
    x += tool(sf, r, x, "Options", C_OPTIONS, false, false, TUI_BLUE);
    /* The radio's own numbers, at the right while there is room. */
    char text[72];
    snprintf(text, sizeof(text), "rx %lu  tx %lu  bad %lu  %.0f dBm  %.1f dB", (unsigned long)s_st.rx_packets,
             (unsigned long)s_st.tx_packets, (unsigned long)s_st.rx_bad, (double)s_st.last_rssi, (double)s_st.last_snr);
    const int tx = r.x + r.w - (int)strlen(text) - 1;
    if (s_st.running && tx > x + 1) tui_put_str(sf, r, tx, r.y, text, LS_ATTR_DIM);
}

static void draw_list(tui_surface *sf)
{
    const tui_rect r = s_r_list;
    if (!r.w) return;
    char note[16];
    const int unread = unread_total();
    snprintf(note, sizeof(note), unread ? "%d unread" : "", unread);
    const tui_rect in = ls_dui_pane(sf, r, "CHATS", note, HUE, s_focus == F_LIST);
    s_list_y = in.y;
    s_list_rows = in.h < (int)sizeof(s_list_row) ? in.h : (int)sizeof(s_list_row);
    memset(s_list_row, -1, sizeof(s_list_row));
    /* Rows: a caption, the channels, a caption, the nodes. */
    int first_dm = s_nconv;
    for (int i = 0; i < s_nconv && first_dm == s_nconv; i++)
        if (s_conv[i].kind == LS_MC_DM) first_dm = i;
    const int total = s_nconv + 1 + (first_dm < s_nconv ? 1 : 0);
    /* The window keeps the keyboard's row in sight. */
    const int cur_row = s_list_cur + 1 + (s_list_cur >= first_dm ? 1 : 0);
    if (cur_row < s_list_first + 1) s_list_first = cur_row - 1;
    if (cur_row >= s_list_first + s_list_rows) s_list_first = cur_row - s_list_rows + 1;
    if (s_list_first > total - s_list_rows) s_list_first = total - s_list_rows;
    if (s_list_first < 0) s_list_first = 0;
    int row = -s_list_first;
    for (int i = 0; i <= s_nconv; i++) {
        if (i == 0 || i == first_dm) {
            if (row >= 0 && row < s_list_rows)
                tui_put_str(sf, in, in.x + 1, in.y + row, i == 0 ? "CHANNELS" : "NODES", LS_ATTR_FAINT);
            row++;
        }
        if (i == s_nconv) break;
        const ls_meshd_conv_t *c = &s_conv[i];
        const int y = in.y + row;
        if (row >= 0 && row < s_list_rows) {
            const bool open = i == s_cur, at = i == s_list_cur && s_focus == F_LIST;
            const uint8_t hue = c->kind == LS_MC_DM ? role_hue(c->role) : HUE;
            s_list_row[row] = (int8_t)i;
            if (open) ls_fill_dither(sf, tui_rect_make(in.x, y, in.w, 1), LS_DITHER_MEDIUM, HUE);
            else if (at) ls_fill_dither(sf, tui_rect_make(in.x, y, in.w, 1), LS_DITHER_LIGHT, HUE);
            if (at || open) ls_box_put(sf, in, in.x, y, LS_SHAPE_RIGHT, at ? WHITE : INK(HUE));
            tui_put_char(sf, in, in.x + 1, y, conv_glyph(c), c->kind == LS_MC_DM && !c->heard ? LS_ATTR_FAINT : INK(hue));
            char name[LS_MESH_PEER_NAME + 2], tail[12];
            tail[0] = 0;
            if (c->unread) snprintf(tail, sizeof(tail), " %d ", c->unread);
            else if (c->kind == LS_MC_DM && c->heard) {
                const int p = peer_of(c->id);
                if (p >= 0 && s_peer[p].last_heard) ls_age_str(tail, sizeof(tail), s_peer[p].last_heard, s_now);
            }
            const int tw = (int)strlen(tail), room = in.w - 4 - tw;
            snprintf(name, sizeof(name), "%.*s", room > 0 ? room : 0, c->name);
            const uint8_t ink = c->unread || open ? WHITE : c->total ? TUI_ATTR(TUI_WHITE, TUI_BLACK)
                              : c->kind == LS_MC_DM && !c->heard ? LS_ATTR_FAINT : LS_ATTR_DIM;
            tui_put_str(sf, in, in.x + 3, y, name, ink);
            if (tw) tui_put_str(sf, in, in.x + in.w - tw, y, tail,
                                c->unread ? TUI_ATTR(TUI_BLACK, TUI_YELLOW) : LS_ATTR_DIM);
        }
        row++;
    }
}

/* The feed's rows for this conversation and this width. */
static void build_lines(int w)
{
    const ls_meshd_conv_t *c = cur();
    s_name_w = w >= 70 ? 12 : 9;
    s_text_w = w - 6 - s_name_w - 1 - 14;
    if (s_text_w < 12) s_text_w = w - 6 - s_name_w - 1;
    if (s_text_w < 4) s_text_w = 4;
    s_nline = s_shown = 0;
    uint32_t day = 0;
    for (int i = 0; i < s_nmsg && s_nline < LINES - 3; i++) {
        const ls_mesh_msg_t *m = &s_msg[i];
        if (!ls_meshd_belongs(m, c)) continue;
        if (s_find_on && !ls_meshd_match(m, s_find.text)) continue;
        s_shown++;
        if (m->t >= 1000000000u && m->t / 86400u != day) {
            /* A rule where the date turns over, but not above the first message. */
            if (day) s_line[s_nline++] = (line_t){ (int8_t)i, L_DAY, 0 };
            day = m->t / 86400u;
        }
        if (s_new_mark && ls_meshd_mark(m) == s_new_mark) s_line[s_nline++] = (line_t){ (int8_t)i, L_NEW, 0 };
        const char *body = ls_meshd_split(m, NULL, 0);
        int off = 0;
        bool first = true;
        do {
            s_line[s_nline++] = (line_t){ (int8_t)i, first ? L_HEAD : L_MORE, (uint16_t)off };
            off = ls_meshd_wrap(body, off, s_text_w, NULL, 0);
            first = false;
        } while (body[off] && s_nline < LINES - 3);
    }
}

static void draw_feed(tui_surface *sf, tui_rect r)
{
    s_r_feed = r;
    build_lines(r.w);
    const ls_meshd_conv_t *c = cur();
    if (!s_nline) {
        const char *why = s_find_on && s_find.len ? "Nothing here matches." :
            c->kind == LS_MC_DM ? "No messages with this node yet." : "No messages here yet.";
        tui_put_str(sf, r, r.x + 2, r.y + 1, why, LS_ATTR_DIM);
        if (!(s_find_on && s_find.len)) {
            const char *who = c->kind == LS_MC_DM ? "Only this node can read what is sent here."
                            : c->kind == LS_MC_CHANNEL ? "Only holders of this channel's key can read it."
                            : "Anything sent here is readable by every MeshCore node in range.";
            tui_put_str(sf, r, r.x + 2, r.y + 3, who, LS_ATTR_DIM);
            static const struct { const char *key, *what; uint8_t hue; } HOW[] = {
                { "Start", "brings the mesh radio up", TUI_GREEN },
                { "Arm", "lets it transmit; nothing is sent while disarmed", TUI_RED },
                { "Enter", "sends the line you typed", TUI_YELLOW },
                { "/help", "every command the handheld has", TUI_CYAN },
            };
            for (int i = 0; i < 4 && r.y + 5 + i < r.y + r.h; i++) {
                tui_put_str(sf, r, r.x + 2, r.y + 5 + i, HOW[i].key, INK(HOW[i].hue));
                tui_put_str(sf, r, r.x + 9, r.y + 5 + i, HOW[i].what, TUI_ATTR(TUI_WHITE, TUI_BLACK));
            }
        }
        return;
    }
    int deepest = s_nline - r.h;
    if (deepest < 0) deepest = 0;
    if (s_back > deepest) s_back = deepest;
    if (s_back < 0) s_back = 0;
    /* The newest sits on the line above where you type; a short chat
       hangs from the bottom, as a messenger's does. */
    s_feed_first = deepest - s_back - (s_nline < r.h ? r.h - s_nline : 0);
    const int tx = r.x + 6 + s_name_w + 1;
    for (int row = 0; row < r.h && s_feed_first + row < s_nline; row++) {
        if (s_feed_first + row < 0) continue;
        const line_t *l = &s_line[s_feed_first + row];
        const ls_mesh_msg_t *m = &s_msg[l->msg];
        const int y = r.y + row;
        if (l->kind == L_DAY || l->kind == L_NEW) {
            char text[32];
            if (l->kind == L_NEW) snprintf(text, sizeof(text), " new ");
            else {
                /* Days since 1970 to a date, the civil-calendar way. */
                long z = (long)(m->t / 86400u) + 719468;
                const long era = z / 146097;
                const unsigned doe = (unsigned)(z - era * 146097);
                const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
                const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
                const unsigned mp = (5 * doy + 2) / 153;
                const unsigned d = doy - (153 * mp + 2) / 5 + 1, mo = mp < 10 ? mp + 3 : mp - 9;
                snprintf(text, sizeof(text), " %04ld-%02u-%02u ", (long)yoe + era * 400 + (mo <= 2), mo, d);
            }
            const uint8_t at = l->kind == L_NEW ? INK(TUI_YELLOW) : LS_ATTR_DIM;
            for (int x = r.x + 1; x < r.x + r.w - 1; x++) ls_box_put(sf, r, x, y, 0x2500, l->kind == L_NEW ? SOFT(TUI_YELLOW) : LS_ATTR_FAINT);
            tui_put_str(sf, r, r.x + (r.w - (int)strlen(text)) / 2, y, text, at);
            continue;
        }
        const bool chosen = s_pick_mark && ls_meshd_mark(m) == s_pick_mark;
        if (chosen) ls_fill_dither(sf, tui_rect_make(r.x, y, r.w, 1), LS_DITHER_LIGHT, HUE);
        char who[LS_MESH_PEER_NAME + 4], line[LS_MESH_MSG_LEN + 1];
        const char *body = ls_meshd_split(m, who, sizeof(who));
        if (l->kind == L_HEAD) {
            char when[12];
            ls_meshd_time_text(m->t, when, sizeof(when));
            tui_put_str(sf, tui_rect_make(r.x, y, 6, 1), r.x, y, when, LS_ATTR_DIM);
            const char *name = who[0] ? who : m->mine ? ls_mesh_name() : m->direct ? cur()->name : "?";
            tui_put_str(sf, tui_rect_make(r.x + 6, y, s_name_w, 1), r.x + 6, y, name,
                        INK(m->mine ? TUI_YELLOW : name_hue(name)));
            if (m->mine && s_text_w + 14 <= r.w - 6 - s_name_w - 1) {
                /* Where ours got to, at the right edge. */
                char st[16];
                if (m->state == LS_MSG_HEARD && m->relays > 1) snprintf(st, sizeof(st), "relayed x%d", m->relays);
                else snprintf(st, sizeof(st), "%s", ls_meshd_state_word(m));
                const uint8_t hue = m->state == LS_MSG_FAILED ? TUI_RED
                                  : m->state == LS_MSG_ACKED || m->state == LS_MSG_HEARD ? TUI_GREEN : TUI_YELLOW;
                const int sx = r.x + r.w - (int)strlen(st) - 1;
                tui_put_str(sf, r, sx, y, st, m->state == LS_MSG_SENT ? SOFT(hue) : INK(hue));
                ls_box_put(sf, r, sx - 2, y, m->state == LS_MSG_FAILED ? LS_SHAPE_TIMES
                           : m->state == LS_MSG_ACKED ? LS_SHAPE_CHECK : LS_SHAPE_DOT, INK(hue));
            }
        }
        ls_meshd_wrap(body, l->off, s_text_w, line, sizeof(line));
        tui_put_str(sf, tui_rect_make(tx, y, s_text_w, 1), tx, y, line,
                    m->mine ? TUI_ATTR(TUI_WHITE, TUI_BLACK) : WHITE);
    }
    /* More below, when the feed has been scrolled back. */
    if (s_back) {
        char text[32];
        snprintf(text, sizeof(text), " %d more below: End ", s_back);
        tui_put_str(sf, r, r.x + r.w - (int)strlen(text) - 1, r.y + r.h - 1, text, TUI_ATTR(TUI_BLACK, TUI_YELLOW));
    }
}

static void draw_chat(tui_surface *sf)
{
    const ls_meshd_conv_t *c = cur();
    char title[40], note[40];
    snprintf(title, sizeof(title), "%c %s", conv_glyph(c), c->name);
    if (s_find_on && s_find.len) snprintf(note, sizeof(note), "%d of %d match", s_shown, c->total);
    else snprintf(note, sizeof(note), "%d message%s", c->total, c->total == 1 ? "" : "s");
    tui_rect in = ls_dui_pane(sf, s_r_main, title, note, HUE, s_focus == F_COMPOSE || s_focus == F_FIND);
    s_r_find = tui_rect_make(0, 0, 0, 0);
    if (s_find_on && in.h > 5) {
        tui_put_str(sf, in, in.x + 1, in.y, "Find", s_focus == F_FIND ? INK(TUI_YELLOW) : LS_ATTR_DIM);
        const int fw = in.w > 50 ? 32 : in.w - 14;
        s_r_find = tui_rect_make(in.x + 6, in.y, fw, 1);
        ls_dui_edit_draw(sf, in, s_r_find.x, in.y, fw, &s_find, s_focus == F_FIND, TUI_YELLOW);
        tui_put_str(sf, in, s_r_find.x + fw + 1, in.y, "Esc", LS_ATTR_FAINT);
        in.y++; in.h--;
    }
    /* The compose line and the rule above it. */
    const int cy = in.y + in.h - 1;
    for (int x = in.x; x < in.x + in.w; x++) ls_box_put(sf, in, x, cy - 1, 0x2500, SOFT(HUE));
    draw_feed(sf, tui_rect_make(in.x, in.y, in.w, in.h - 2));
    s_r_compose = tui_rect_make(in.x, cy, in.w, 1);
    char to[LS_MESH_PEER_NAME + 8];
    snprintf(to, sizeof(to), "To %c %.12s", conv_glyph(c), c->name);
    const bool can = s_st.running && s_st.tx_enabled && (c->kind != LS_MC_DM || c->heard);
    tui_put_str(sf, in, in.x + 1, cy, to, can ? INK(c->kind == LS_MC_DM ? TUI_GREEN : HUE) : LS_ATTR_DIM);
    const int ex = in.x + 2 + (int)strlen(to);
    char count[12];
    snprintf(count, sizeof(count), "%d/%d", s_compose.len, s_compose.max);
    const int sw = 6, cw = in.w >= 70 ? (int)strlen(count) + 1 : 0;
    const int ew = in.x + in.w - ex - sw - cw - 1;
    s_r_edit = tui_rect_make(ex, cy, ew, 1);
    ls_dui_edit_draw(sf, in, ex, cy, ew, &s_compose, s_focus == F_COMPOSE, can ? TUI_GREEN : TUI_WHITE);
    if (!s_compose.len && s_focus != F_COMPOSE)
        tui_put_str(sf, tui_rect_make(ex + 1, cy, ew - 2, 1), ex + 1, cy, "type a message; Enter sends", LS_ATTR_FAINT);
    if (cw) tui_put_str(sf, in, ex + ew + 1, cy, count, s_compose.len >= s_compose.max ? INK(TUI_RED) : LS_ATTR_FAINT);
    ls_dui_key(sf, in, in.x + in.w - sw, cy, "Send", false, !s_compose.len, TUI_GREEN, &s_r_send);
}

/* A signal meter: rising eighth-height traces, -120 dBm to -20. */
static void meter(tui_surface *sf, tui_rect clip, int x, int y, int cells, float dbm, uint8_t hue)
{
    float v = (dbm + 120.0f) / 100.0f;
    if (v < 0) v = 0;
    if (v > 1) v = 1;
    for (int i = 0; i < cells; i++) {
        const float t = (float)(i + 1) / (float)cells;
        const bool lit = v >= t - 0.5f / (float)cells;
        tui_put_char(sf, clip, x + i, y, LS_TUI_TRACE(2 + i * 6 / (cells > 1 ? cells - 1 : 1)),
                     lit ? INK(hue) : LS_ATTR_FAINT);
    }
}

static uint8_t rssi_hue(float dbm) { return dbm >= -90 ? TUI_GREEN : dbm >= -105 ? TUI_YELLOW : TUI_RED; }

static int field(tui_surface *sf, tui_rect in, int y, const char *label, const char *value, uint8_t at)
{
    if (y >= in.y + in.h) return y;
    tui_put_str(sf, in, in.x + 1, y, label, LS_ATTR_DIM);
    tui_put_str(sf, in, in.x + 11, y, value, at);
    return y + 1;
}

static void draw_node_card(tui_surface *sf, tui_rect r, const ls_mesh_peer_t *p, const char *id)
{
    const uint8_t hue = p ? role_hue(p->type) : TUI_WHITE;
    const tui_rect in = ls_dui_pane(sf, r, "NODE", p ? ls_meshd_role(p->type) : "", hue, false);
    char text[64];
    int y = in.y;
    if (!p) {
        tui_put_str(sf, in, in.x + 1, y, id && *id ? id : "No node chosen", WHITE);
        tui_put_str(sf, in, in.x + 1, y + 2, id && *id ? "Not heard since boot." : "Nothing has been heard yet.", LS_ATTR_DIM);
        tui_put_str(sf, in, in.x + 1, y + 3, id && *id ? "It can be addressed after its" : "A node shows when it advertises.", LS_ATTR_DIM);
        if (id && *id) tui_put_str(sf, in, in.x + 1, y + 4, "next advert.", LS_ATTR_DIM);
        return;
    }
    const int k = (int)(p - s_peer);
    tui_put_str(sf, in, in.x + 1, y++, p->name[0] ? p->name : "(no name)", INK(hue));
    tui_put_str(sf, in, in.x + 1, y++, p->id, LS_ATTR_DIM);
    y++;
    if (p->last_heard) {
        ls_age_str(text, sizeof(text), p->last_heard, s_now);
        y = field(sf, in, y, "heard", text, WHITE);
        snprintf(text, sizeof(text), "%.0f dBm", (double)p->rssi);
        if (y < in.y + in.h) meter(sf, in, in.x + 20, y, 8, p->rssi, rssi_hue(p->rssi));
        y = field(sf, in, y, "signal", text, INK(rssi_hue(p->rssi)));
        snprintf(text, sizeof(text), "%+.1f dB", (double)p->snr);
        y = field(sf, in, y, "snr", text, INK(p->snr >= 0 ? TUI_GREEN : TUI_YELLOW));
    } else y = field(sf, in, y, "heard", "saved, not heard", LS_ATTR_DIM);
    snprintf(text, sizeof(text), "%lu", (unsigned long)p->adverts);
    y = field(sf, in, y, "adverts", text, TUI_ATTR(TUI_WHITE, TUI_BLACK));
    if (p->first_heard) {
        ls_age_str(text, sizeof(text), p->first_heard, s_now);
        y = field(sf, in, y, "since", text, TUI_ATTR(TUI_WHITE, TUI_BLACK));
    }
    if (p->has_loc) {
        snprintf(text, sizeof(text), "%.5f", p->lat_e6 / 1e6);
        y = field(sf, in, y, "lat", text, SOFT(TUI_YELLOW));
        snprintf(text, sizeof(text), "%.5f", p->lon_e6 / 1e6);
        y = field(sf, in, y, "lon", text, SOFT(TUI_YELLOW));
        if (s_dist[k] >= 0) {
            char range[16];
            ls_meshd_range_text(s_dist[k], range, sizeof(range));
            snprintf(text, sizeof(text), "%s %s (%03d)", range, ls_geo_compass(s_brg[k]), (int)(s_brg[k] + 0.5f) % 360);
            y = field(sf, in, y, "range", text, INK(TUI_YELLOW));
        } else y = field(sf, in, y, "range", "no fix or home here", LS_ATTR_DIM);
    } else y = field(sf, in, y, "position", "not advertised", LS_ATTR_DIM);
    /* Its signal over the frames in the log, oldest at the left. */
    float seen[LS_MESH_MAX_EVENTS];
    uint32_t when[LS_MESH_MAX_EVENTS];
    int n = 0;
    for (int i = 0; i < s_nev; i++)
        if ((s_ev[i].kind == LS_MESH_EV_RX || s_ev[i].kind == LS_MESH_EV_ADVERT) && !strcasecmp(s_ev[i].id, p->id)) {
            /* Kept in time order whichever way the log runs. */
            int at = n;
            while (at > 0 && when[at - 1] > s_ev[i].t) { when[at] = when[at - 1]; seen[at] = seen[at - 1]; at--; }
            when[at] = s_ev[i].t;
            seen[at] = s_ev[i].rssi;
            n++;
        }
    y++;
    if (y + 2 < in.y + in.h) {
        tui_put_str(sf, in, in.x + 1, y++, "SIGNAL, FRAME BY FRAME", LS_ATTR_FAINT);
        if (n < 2) tui_put_str(sf, in, in.x + 1, y, "no trend yet", LS_ATTR_DIM);
        else {
            const int room = in.w - 2, from = n > room ? n - room : 0;
            for (int i = from; i < n; i++) {
                int h = (int)((seen[i] + 120.0f) / 100.0f * 8.0f + 0.5f);
                tui_put_char(sf, in, in.x + 1 + i - from, y, LS_TUI_TRACE(h < 1 ? 1 : h), INK(rssi_hue(seen[i])));
            }
            snprintf(text, sizeof(text), "%.0f to %.0f dBm, %d frames", (double)seen[from], (double)seen[n - 1], n - from);
            tui_put_str(sf, in, in.x + 1, y + 1, text, LS_ATTR_DIM);
        }
        y += 3;
    }
    if (y + 3 < in.y + in.h) {
        tui_put_str(sf, in, in.x + 1, y++, "Enter  message it", LS_ATTR_DIM);
        tui_put_str(sf, in, in.x + 1, y++, "M map  I inspect  P page", LS_ATTR_DIM);
        tui_put_str(sf, in, in.x + 1, y++, "right button for more", LS_ATTR_FAINT);
    }
}

static void draw_chan_card(tui_surface *sf, tui_rect r)
{
    const ls_meshd_conv_t *c = cur();
    const tui_rect in = ls_dui_pane(sf, r, "ON THIS CHANNEL", "", HUE, false);
    char text[64];
    int y = in.y;
    /* Who spoke here, by how much: only what is in the ring. */
    struct { char name[LS_MESH_PEER_NAME + 4]; int n; uint32_t last; } who[8];
    int nwho = 0;
    for (int i = 0; i < s_nmsg; i++) {
        if (!ls_meshd_belongs(&s_msg[i], c)) continue;
        char name[LS_MESH_PEER_NAME + 4];
        ls_meshd_split(&s_msg[i], name, sizeof(name));
        if (!name[0]) snprintf(name, sizeof(name), "%s", s_msg[i].mine ? ls_mesh_name() : "?");
        int k = 0;
        while (k < nwho && strcmp(who[k].name, name)) k++;
        if (k == nwho) { if (nwho == 8) continue; snprintf(who[k].name, sizeof(who[k].name), "%s", name); who[k].n = 0; nwho++; }
        who[k].n++;
        who[k].last = s_msg[i].t;
    }
    if (!nwho) tui_put_str(sf, in, in.x + 1, y++, "nobody has spoken yet", LS_ATTR_DIM);
    int most = 1;
    for (int k = 0; k < nwho; k++) if (who[k].n > most) most = who[k].n;
    for (int k = 0; k < nwho && y < in.y + in.h; k++, y++) {
        const bool me = !strcmp(who[k].name, ls_mesh_name());
        const uint8_t hue = me ? TUI_YELLOW : name_hue(who[k].name);
        snprintf(text, sizeof(text), "%-12.12s", who[k].name);
        tui_put_str(sf, in, in.x + 1, y, text, INK(hue));
        const int room = in.w - 20, len = room > 0 ? (who[k].n * room + most - 1) / most : 0;
        for (int x = 0; x < len; x++) ls_box_put(sf, in, in.x + 14 + x, y, 0x2500, SOFT(hue));
        snprintf(text, sizeof(text), "%d", who[k].n);
        tui_put_str(sf, in, in.x + in.w - (int)strlen(text) - 1, y, text, WHITE);
    }
    y++;
    if (y + 6 >= in.y + in.h) return;
    tui_put_str(sf, in, in.x + 1, y++, "THE RADIO", LS_ATTR_FAINT);
    y = field(sf, in, y, "state", state_word(), INK(state_hue()));
    const ls_mesh_radio_t cfg = s_cfg;
    snprintf(text, sizeof(text), "%.3f MHz", cfg.freq_hz / 1e6);
    y = field(sf, in, y, "on", text, INK(TUI_YELLOW));
    snprintf(text, sizeof(text), "SF%d  %lu kHz  %d dBm", cfg.sf, (unsigned long)(cfg.bw_hz / 1000u), cfg.power_dbm);
    y = field(sf, in, y, "lora", text, TUI_ATTR(TUI_WHITE, TUI_BLACK));
    snprintf(text, sizeof(text), "%lu in  %lu out  %lu bad", (unsigned long)s_st.rx_packets, (unsigned long)s_st.tx_packets,
             (unsigned long)s_st.rx_bad);
    y = field(sf, in, y, "frames", text, TUI_ATTR(TUI_WHITE, TUI_BLACK));
    snprintf(text, sizeof(text), "%.1f s on the air", s_st.airtime_ms / 1000.0);
    y = field(sf, in, y, "airtime", text, SOFT(TUI_MAGENTA));
    /* The receiver's level, as it has been: nothing is drawn that was not measured. */
    const int n = s_nscope;
    y++;
    if (n > 1 && y + 2 < in.y + in.h) {
        tui_put_str(sf, in, in.x + 1, y++, "LEVEL AT THE RECEIVER", LS_ATTR_FAINT);
        const int room = in.w - 2, from = n > room ? n - room : 0;
        for (int i = from; i < n; i++) {
            if (s_scope[i] == 0) continue;
            const int h = (int)((s_scope[i] + 130.0f) / 100.0f * 8.0f + 0.5f);
            tui_put_char(sf, in, in.x + 1 + i - from, y, LS_TUI_TRACE(h < 1 ? 1 : h), INK(rssi_hue(s_scope[i])));
        }
    }
}

static void draw_nodes(tui_surface *sf)
{
    char note[32];
    snprintf(note, sizeof(note), "%d heard", s_npeer);
    ls_dui_pane(sf, s_r_main, "NODES", note, TUI_MAGENTA, s_focus == F_TABLE);
    ls_dui_table_head(sf, &s_t, NCOLS, LS_MN__COUNT, s_nsort, s_ndesc, TUI_MAGENTA);
    if (!s_npeer) {
        tui_put_str(sf, s_t.body, s_t.body.x + 2, s_t.body.y + 1, s_st.running ? "Listening..." : "The mesh radio is off: Start.", LS_ATTR_DIM);
        tui_put_str(sf, s_t.body, s_t.body.x + 2, s_t.body.y + 3, "A node appears when it advertises; there is nothing to ask for.", LS_ATTR_DIM);
        tui_put_str(sf, s_t.body, s_t.body.x + 2, s_t.body.y + 4, "Still empty? Both ends must match on frequency, SF and bandwidth.", LS_ATTR_DIM);
        return;
    }
    ls_dui_table_follow(&s_t, s_nsel, s_npeer);
    for (int row = 0; row < s_t.rows && s_t.first + row < s_npeer; row++) {
        const int k = s_t.first + row, i = s_order[k], y = s_t.body.y + row;
        const ls_mesh_peer_t *p = &s_peer[i];
        const bool on = k == s_nsel;
        const uint8_t hue = role_hue(p->type);
        char text[48];
        if (on) ls_fill_dither(sf, tui_rect_make(s_t.body.x, y, s_t.body.w, 1), LS_DITHER_MEDIUM, TUI_MAGENTA);
        if (on) ls_box_put(sf, s_t.body, s_t.body.x, y, LS_SHAPE_RIGHT, s_focus == F_TABLE ? WHITE : LS_ATTR_DIM);
        ls_dui_table_cell(sf, &s_t, NCOLS, LS_MN_NAME, row, p->name[0] ? p->name : "(no name)",
                          p->name[0] ? (on ? WHITE : INK(hue)) : LS_ATTR_DIM);
        ls_dui_table_cell(sf, &s_t, NCOLS, LS_MN_ID, row, p->id, LS_ATTR_DIM);
        ls_dui_table_cell(sf, &s_t, NCOLS, LS_MN_ROLE, row, ls_meshd_role(p->type), SOFT(hue));
        if (p->last_heard) ls_age_str(text, sizeof(text), p->last_heard, s_now);
        else snprintf(text, sizeof(text), "saved");
        ls_dui_table_cell(sf, &s_t, NCOLS, LS_MN_HEARD, row, text, p->last_heard ? WHITE : LS_ATTR_DIM);
        if (p->last_heard) {
            snprintf(text, sizeof(text), "%4.0f", (double)p->rssi);
            ls_dui_table_cell(sf, &s_t, NCOLS, LS_MN_RSSI, row, text, INK(rssi_hue(p->rssi)));
            if (s_t.x[LS_MN_RSSI] >= 0) meter(sf, s_t.body, s_t.x[LS_MN_RSSI] + 5, y, 6, p->rssi, rssi_hue(p->rssi));
            snprintf(text, sizeof(text), "%+.1f", (double)p->snr);
            ls_dui_table_cell(sf, &s_t, NCOLS, LS_MN_SNR, row, text, SOFT(p->snr >= 0 ? TUI_GREEN : TUI_YELLOW));
        }
        ls_meshd_range_text(s_dist[i], text, sizeof(text));
        ls_dui_table_cell(sf, &s_t, NCOLS, LS_MN_DIST, row, text, s_dist[i] >= 0 ? INK(TUI_YELLOW) : LS_ATTR_FAINT);
        snprintf(text, sizeof(text), "%lu", (unsigned long)p->adverts);
        ls_dui_table_cell(sf, &s_t, NCOLS, LS_MN_ADVERTS, row, text, TUI_ATTR(TUI_WHITE, TUI_BLACK));
        if (p->first_heard) {
            ls_age_str(text, sizeof(text), p->first_heard, s_now);
            ls_dui_table_cell(sf, &s_t, NCOLS, LS_MN_FIRST, row, text, LS_ATTR_DIM);
        }
    }
    ls_dui_table_strip(sf, &s_t, s_npeer, TUI_MAGENTA);
}

static void draw_status(tui_surface *sf)
{
    const tui_rect r = s_r_status;
    char text[96];
    const int unread = unread_total();
    int k = snprintf(text, sizeof(text), " %d chat%s  %d unread  %d node%s", s_nconv, s_nconv == 1 ? "" : "s", unread,
                     s_npeer, s_npeer == 1 ? "" : "s");
    tui_put_str(sf, r, r.x, r.y, text, unread ? INK(TUI_YELLOW) : INK(HUE));
    const int x = r.x + k + 2;
    const char *said = ls_scr_mesh_said();
    const bool mine = s_say[0] && esp_timer_get_time() - s_said_us < 8000000;
    const char *idle = s_focus == F_FIND ? "Type to filter this chat; Enter keeps it, Esc clears"
                     : s_focus == F_LIST ? "Up and Down, Enter opens; Tab to the message line"
                     : s_tab == TAB_NODES ? "Enter messages the node  M map  I inspect  click a header to sort  right button for more"
                     : !s_st.running ? "The mesh radio is off: Start, or type /start"
                     : !s_st.tx_enabled ? "Disarmed: Arm (or MIC, or /arm) before anything is sent"
                     : "Enter sends  PgUp/PgDn reads back  Ctrl+F finds  Ctrl+U next unread  right button for more";
    tui_put_str(sf, tui_rect_make(x, r.y, r.x + r.w - x, 1), x, r.y, mine ? s_say : said ? said : idle,
                mine || said ? INK(TUI_YELLOW) : LS_ATTR_DIM);
}

static bool context(int col, int row);

void ls_mesh_desk_draw(tui_surface *sf, tui_rect a, const ls_mesh_stats_t *st)
{
    s_area = a;
    s_st = *st;
    s_nhit = 0;
    ls_dui_context_claim(context);
    if (a.w < 44 || a.h < 12) {
        ls_panel_notice(sf, a, "MESH", "Enlarge the window", "the chat keeps its place");
        return;
    }
    fetch();
    layout(a);
    s_r_feed = s_r_compose = s_r_edit = s_r_send = s_r_find = tui_rect_make(0, 0, 0, 0);
    draw_menu(sf);
    draw_tools(sf);
    draw_list(sf);
    if (s_tab == TAB_CHAT) draw_chat(sf);
    else draw_nodes(sf);
    if (s_r_info.w) {
        if (s_tab == TAB_NODES) draw_node_card(sf, s_r_info, node_sel(), NULL);
        else if (cur()->kind == LS_MC_DM) {
            const int p = peer_of(cur()->id);
            draw_node_card(sf, s_r_info, p >= 0 ? &s_peer[p] : NULL, cur()->id);
        } else draw_chan_card(sf, s_r_info);
    }
    draw_status(sf);
    ls_dui_popup_draw(sf, a, &s_pop);
}

/* ----------------------------------------------------------------- input -- */

static void node_move(int k)
{
    if (k >= s_npeer) k = s_npeer - 1;
    if (k < 0) k = 0;
    s_nsel = k;
    s_nsel_id[0] = 0;
    if (node_sel()) snprintf(s_nsel_id, sizeof(s_nsel_id), "%s", node_sel()->id);
}

static void popup_result(int r)
{
    if (r == LS_DUI_PREV) open_menu(s_pop.bar - 1);
    else if (r == LS_DUI_NEXT) open_menu(s_pop.bar + 1);
    else if (r >= 0) run(r);
}

static void wheel(int d)
{
    int c, r;
    const bool known = ls_tui_pointer_cell(&c, &r);
    if (known && tui_rect_contains(s_r_list, c, r)) {
        s_list_cur += d;
        if (s_list_cur < 0) s_list_cur = 0;
        if (s_list_cur >= s_nconv) s_list_cur = s_nconv - 1;
        return;
    }
    if (s_tab == TAB_NODES) { node_move(s_nsel + d); return; }
    s_back -= d * 3;
    if (s_back < 0) s_back = 0;
}

bool ls_mesh_desk_classic_key(ls_tk_t k, char ch)
{
    if (!s_classic) return false;
    if (k == LS_TK_CHAR && (ls_tui_input_mods() & LS_IMOD_CTRL) && (ch == 'l' || ch == 'L' || ch == 12)) {
        s_classic = false;
        ls_scr_mesh_say("Desktop layout");
        return true;
    }
    return false;
}

bool ls_mesh_desk_key(ls_tk_t k, char ch)
{
    const uint8_t mods = ls_tui_input_mods();
    const bool ctrl = mods & LS_IMOD_CTRL, alt = mods & LS_IMOD_ALT;
    if (s_pop.open) { popup_result(ls_dui_popup_key(&s_pop, k, ch)); return true; }
    if (k == LS_TK_WHEEL_UP || k == LS_TK_WHEEL_DOWN) { wheel(k == LS_TK_WHEEL_UP ? -1 : 1); return true; }
    char low = ch;
    if (low >= 'A' && low <= 'Z') low = (char)(low + 32);
    if (k == LS_TK_CHAR && alt) {
        const char *p = low ? strchr("mcvn", low) : NULL;
        if (p) { open_menu((int)(p - "mcvn")); return true; }
        return false;
    }
    if (k == LS_TK_CHAR && ctrl) {
        switch (low) {
        case 'f': run(C_FIND); break;
        case 'u': run(C_NEXT_UNREAD); break;
        case 'e': run(C_EXPORT); break;
        case 'l': run(C_CLASSIC); break;
        case '1': run(C_CHATS); break;
        case '2': run(C_NODES); break;
        case 'v': run(C_PASTE); break;
        case 'c':
            if (s_tab == TAB_NODES || s_focus == F_LIST) node_cmd(C_NODE_COPY);
            else run(C_MSG_COPY);
            break;
        default: return false;
        }
        return true;
    }
    if (k == LS_TK_TAB) {
        const int other = s_tab == TAB_NODES ? F_TABLE : F_COMPOSE;
        if (!s_r_list.w) { s_focus = other; return true; }
        s_focus = s_focus == F_LIST ? other : F_LIST;
        if (s_focus == F_LIST) s_list_cur = s_cur;
        return true;
    }
    if (s_focus == F_FIND) {
        if (k == LS_TK_ESC) { s_find_on = false; ls_dui_edit_set(&s_find, "", 32); s_focus = F_COMPOSE; return true; }
        if (k == LS_TK_ENTER || k == LS_TK_DOWN) { s_focus = F_COMPOSE; return true; }
        if (ls_dui_edit_key(&s_find, k, ch)) { s_back = 0; return true; }
        return true;
    }
    if (s_focus == F_LIST) {
        switch (k) {
        case LS_TK_UP: if (s_list_cur > 0) s_list_cur--; return true;
        case LS_TK_DOWN: if (s_list_cur < s_nconv - 1) s_list_cur++; return true;
        case LS_TK_HOME: s_list_cur = 0; return true;
        case LS_TK_END: s_list_cur = s_nconv - 1; return true;
        case LS_TK_ENTER: case LS_TK_RIGHT: open_conv(s_list_cur); return true;
        case LS_TK_ESC: s_focus = s_tab == TAB_NODES ? F_TABLE : F_COMPOSE; return true;
        default: break;
        }
    }
    if (s_focus == F_LIST || s_focus == F_TABLE) {
        if (s_focus == F_TABLE) {
            const int page = s_t.rows > 1 ? s_t.rows - 1 : 1;
            switch (k) {
            case LS_TK_UP: node_move(s_nsel - 1); return true;
            case LS_TK_DOWN: node_move(s_nsel + 1); return true;
            case LS_TK_PGUP: node_move(s_nsel - page); return true;
            case LS_TK_PGDN: node_move(s_nsel + page); return true;
            case LS_TK_HOME: node_move(0); return true;
            case LS_TK_END: node_move(s_npeer - 1); return true;
            case LS_TK_ENTER: node_cmd(C_NODE_DM); return true;
            case LS_TK_LEFT: if (s_r_list.w) { s_focus = F_LIST; s_list_cur = s_cur; } return true;
            case LS_TK_ESC: run(C_CHATS); return true;
            default: break;
            }
        }
        /* Letters are free here: nothing in these panes is a text field. */
        if (k != LS_TK_CHAR) return false;
        switch (low) {
        case 's': run(C_RUN); return true;
        case 't': run(C_ARM); return true;
        case 'a': run(C_ADVERT); return true;
        case 'r': run(C_RADIO); return true;
        case 'o': run(C_OPTIONS); return true;
        case 'd': node_cmd(C_NODE_DM); return true;
        case 'm': node_cmd(C_NODE_MAP); return true;
        case 'i': node_cmd(C_NODE_INSPECT); return true;
        case 'p': node_cmd(C_NODE_PAGE); return true;
        case '/': run(C_FIND); return true;
        default: return false;
        }
    }
    /* The message line has the keyboard. */
    switch (k) {
    case LS_TK_ENTER: send(); return true;
    case LS_TK_UP: s_back++; return true;
    case LS_TK_DOWN: if (s_back > 0) s_back--; return true;
    case LS_TK_PGUP: s_back += s_r_feed.h > 2 ? s_r_feed.h - 1 : 1; return true;
    case LS_TK_PGDN: s_back -= s_r_feed.h > 2 ? s_r_feed.h - 1 : 1; if (s_back < 0) s_back = 0; return true;
    case LS_TK_END: if (s_back && s_compose.caret == s_compose.len) { s_back = 0; return true; } break;
    case LS_TK_ESC:
        if (s_pick_mark) { s_pick_mark = 0; return true; }
        if (s_compose.len) { ls_dui_edit_set(&s_compose, "", 140); return true; }
        if (s_find_on) { s_find_on = false; ls_dui_edit_set(&s_find, "", 32); return true; }
        if (cur()->kind != LS_MC_PUBLIC) { open_conv(0); say("Back on the public channel"); return true; }
        return false;
    default: break;
    }
    return ls_dui_edit_key(&s_compose, k, ch);
}

/* The message drawn on a row of the feed. */
static const ls_mesh_msg_t *msg_at(int row)
{
    const int k = s_feed_first + row - s_r_feed.y;
    if (row < s_r_feed.y || row >= s_r_feed.y + s_r_feed.h || k < 0 || k >= s_nline) return NULL;
    if (s_line[k].kind == L_DAY || s_line[k].kind == L_NEW) return NULL;
    return &s_msg[s_line[k].msg];
}

static int list_at(int row)
{
    const int k = row - s_list_y;
    return k >= 0 && k < s_list_rows ? s_list_row[k] : -1;
}

bool ls_mesh_desk_touch(int col, int row)
{
    if (s_pop.open) {
        const int bar = s_pop.bar;
        const int r = ls_dui_popup_tap(&s_pop, col, row);
        if (r == LS_DUI_CLOSED && row == s_r_menu.y) {
            const int i = ls_dui_bar_hit(s_bar_x, NMENUS, col);
            if (i >= 0 && i != bar) open_menu(i);
        } else popup_result(r);
        return true;
    }
    if (row == s_r_menu.y) {
        const int i = ls_dui_bar_hit(s_bar_x, NMENUS, col);
        if (i >= 0) open_menu(i);
        return true;
    }
    for (int i = 0; i < s_nhit; i++)
        if (tui_rect_contains(s_hit[i].r, col, row)) { run(s_hit[i].cmd); return true; }
    if (tui_rect_contains(s_r_list, col, row)) {
        const int i = list_at(row);
        if (i >= 0) open_conv(i);
        return true;
    }
    if (s_tab == TAB_CHAT) {
        if (tui_rect_contains(s_r_find, col, row)) {
            s_focus = F_FIND;
            ls_dui_edit_click(&s_find, s_r_find.x, s_r_find.w, col);
            return true;
        }
        if (tui_rect_contains(s_r_send, col, row)) { send(); return true; }
        if (tui_rect_contains(s_r_compose, col, row)) {
            s_focus = F_COMPOSE;
            if (tui_rect_contains(s_r_edit, col, row)) ls_dui_edit_click(&s_compose, s_r_edit.x, s_r_edit.w, col);
            return true;
        }
        if (tui_rect_contains(s_r_feed, col, row)) {
            const ls_mesh_msg_t *m = msg_at(row);
            s_focus = F_COMPOSE;
            s_pick_mark = m ? ls_meshd_mark(m) : 0;
            /* Twice on someone's message: write to them. */
            if (m && !m->mine && ls_tui_input_clicks() >= 2) run(C_MSG_DM);
            return true;
        }
        return true;
    }
    int what = 0;
    switch (ls_dui_table_hit(&s_t, LS_MN__COUNT, col, row, &what)) {
    case LS_DUI_T_HEAD: run(C_SORT + what); break;
    case LS_DUI_T_STRIP: node_move(s_nsel + what * (s_t.rows > 1 ? s_t.rows - 1 : 1)); break;
    case LS_DUI_T_ROW:
        s_focus = F_TABLE;
        if (what < s_npeer) {
            node_move(what);
            if (ls_tui_input_clicks() >= 2) node_cmd(C_NODE_DM);
        }
        break;
    default: break;
    }
    return true;
}

/* The right button: a menu for what is under it. */
static bool context(int col, int row)
{
    s_pop.open = false;
    if (tui_rect_contains(s_r_list, col, row)) {
        const int i = list_at(row);
        if (i < 0) return false;
        s_list_cur = i;
        conv_menu(i, col, row);
        return true;
    }
    if (s_tab == TAB_CHAT) {
        const ls_mesh_msg_t *m = msg_at(row);
        if (!m || !tui_rect_contains(s_r_feed, col, row)) return false;
        s_pick_mark = ls_meshd_mark(m);
        msg_menu(m, col, row);
        return true;
    }
    int what = 0;
    const int part = ls_dui_table_hit(&s_t, LS_MN__COUNT, col, row, &what);
    if (part == LS_DUI_T_ROW && what < s_npeer) {
        s_focus = F_TABLE;
        node_move(what);
        node_menu(node_sel(), col, row);
        return true;
    }
    if (part == LS_DUI_T_HEAD) { head_menu(col, row + 1); return true; }
    return false;
}

void ls_mesh_desk_enter(void)
{
    static EXT_RAM_BSS_ATTR bool once;
    s_pop.open = false;
    s_back = 0;
    s_pick_mark = 0;
    s_slow_us = 0;
    if (s_focus == F_FIND) s_focus = F_COMPOSE;
    if (!once) {
        once = true;
        ls_dui_edit_set(&s_compose, "", 140);
        ls_dui_edit_set(&s_find, "", 32);
        /* Nothing here has an initialiser: all of it is in PSRAM, where
           the Nano has room, and is set the first time the screen opens. */
        s_show_list = s_show_info = true;
        s_nsort = LS_MN_HEARD;
        s_nshown = (1u << LS_MN__COUNT) - 1;
    }
    /* The conversation the handheld layout was in. */
    char id[17];
    int chan = 0;
    ls_scr_mesh_dest_get(id, &chan);
    memset(&s_open, 0, sizeof(s_open));
    if (id[0]) { s_open.kind = LS_MC_DM; snprintf(s_open.id, sizeof(s_open.id), "%s", id); }
    else if (chan > 0) { s_open.kind = LS_MC_CHANNEL; s_open.chan = (uint8_t)chan; }
    else s_open.kind = LS_MC_PUBLIC;
    s_new_mark = 0;
}

#endif /* LS_TUI_HAS_COMPACT */
