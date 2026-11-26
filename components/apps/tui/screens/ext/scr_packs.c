/* PACKS, in SYSTEM: the frequency data packs on the card (ls_packs.h).

   The globe at the top shows them: each US state filled by what is on the
   card for it (green installed, yellow an update waits, cyan marked), and it
   turns to the region under the cursor. Under it what the card holds and
   what the service is doing (the bar moves with bytes on the wire), one big
   INSTALL MY AREA with its radius, then the catalog as a tree: country, and
   for the US its states; a row's column of kind letters says what is in it
   (P25 DMR NXDN FM AIR WX MAR). Tap rows (or states on the globe) to mark
   them, then INSTALL or REMOVE; UPDATE ALL refreshes what has a newer date.
   Installing is the user's choice; the position only picks among what is
   installed. LANGUAGES, above the countries, holds the translator languages
   (MESHTASTIC's), which belong to no place; APP DATA, after it, the raw
   files apps read (ble-ids, ble-flows, ...), one row each. P25, FM and FREQS open this from
   their HERE lists, and BACK returns there. */
#include "../../ls_app.h"
#include "../../ls_here.h"
#include "../../ls_globe.h"
#include "../../ls_drag_scroll.h"
#include "../../ls_tui_screen.h"
#include "../../ls_tui_ui.h"
#include "../../ls_deskui.h"
#include "ls_location.h"
#include "ls_textpack.h"
#include "esp_attr.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

#define HUE   TUI_CYAN
#define UMAX  400
#define RMAX  48
#define A(f, b) TUI_ATTR(f, b)
#define BR(c) ((uint8_t)((c) | TUI_BRIGHT))
#ifdef ESP_PLATFORM
#define OUTLINES NULL
#else
#include <stdlib.h>
#define OUTLINES getenv("LSSIM_OUTLINES")
#endif

/* One installable unit: a US state, or a country, or the global table. */
typedef struct {
    char cc[3], sub[4];
    uint16_t first, count, kinds, inst, upd;
    uint32_t bytes, ibytes;
    uint8_t sel;
} unit_t;
typedef struct { int16_t u; uint8_t head; } vis_t;

static EXT_RAM_BSS_ATTR unit_t s_u[UMAX];
static EXT_RAM_BSS_ATTR vis_t s_vis[UMAX + 64];
static EXT_RAM_BSS_ATTR ls_pack_region_t s_row[RMAX];
static EXT_RAM_BSS_ATTR char s_ids[64][32];
static EXT_RAM_BSS_ATTR char s_say[80];
static int s_nu, s_nvis, s_cur, s_first, s_per = 2, s_rows, s_ncat = -1;
static uint16_t s_inst = 0xFFFF, s_upd = 0xFFFF;
static uint32_t s_used;
static char s_open[3] = "US";
static bool s_find_here;
static int s_km_i = 1;
static tui_rect s_list, s_globe;
static const float KM[] = { 25, 50, 100, 200 };
/* Fingers: the list scrolls under a drag (s_lfree: the cursor may be off
   screen until a key moves it); the globe turns under one, and zooms by its
   [+] [-] keys. A drag's lift is not a tap. */
static ls_drag_scroll_t s_ldrag;
static bool s_lfree;
static struct { bool held, moved; float lat0, lon0; int64_t lift_us; } s_gd;
static tui_rect s_zin, s_zout;

/* ------------------------------------------------------------- the model -- */

static void build(void)
{
    const int n = ls_packs_catalog_regions(NULL, 0, 0);
    const int old = s_nu;
    int nu = 0, ndata = 0;
    s_used = 0;
    for (int skip = 0; skip < n;) {
        const int got = ls_packs_catalog_regions(s_row, skip, RMAX);
        if (got <= 0) break;
        for (int i = 0; i < got; i++) {
            const ls_pack_region_t *r = &s_row[i];
            /* each app data pack is a unit of its own under APP DATA: number it */
            char sub[4];
            if (!strcmp(r->cc, LS_PACK_DATA_CC)) snprintf(sub, sizeof(sub), "%u", (unsigned)(++ndata % 1000));
            else snprintf(sub, sizeof(sub), "%s", r->sub);
            unit_t *u = nu ? &s_u[nu - 1] : NULL;
            if (!u || strcmp(u->cc, r->cc) || strcmp(u->sub, sub)) {
                if (nu >= UMAX) break;
                u = &s_u[nu++];
                const uint8_t keep = nu <= old && !strcmp(u->cc, r->cc) && !strcmp(u->sub, sub) ? u->sel : 0;
                memset(u, 0, sizeof(*u));
                strcpy(u->cc, r->cc); strcpy(u->sub, sub);
                u->first = (uint16_t)(skip + i);
                u->sel = keep;
            }
            u->count++;
            u->kinds |= r->kinds;
            u->bytes += r->bytes;
            if (r->installed) { u->inst++; u->ibytes += r->bytes; s_used += r->bytes; }
            u->upd += r->updatable;
        }
        skip += got;
    }
    s_nu = nu;
    s_ncat = n;
    ls_globe_refill();
}

/* A country whose units carry states is a header with its states under it;
   so is LANGUAGES, with a language a unit, and APP DATA, a pack a unit. */
static bool has_subs(int u) { return s_u[u].sub[0] != 0; }
static bool is_lang(const unit_t *u) { return !strcmp(u->cc, LS_PACK_LANG_CC); }
static bool is_data(const unit_t *u) { return !strcmp(u->cc, LS_PACK_DATA_CC); }

/* What an app data pack is for; an id missing here shows "app data". */
static const char *data_for(const char *id)
{
    if (!strcmp(id, "ble-ids")) return LS_TEXT(t_packs_data_bleids, "Bluetooth names for every BLE app");
    if (!strcmp(id, "ble-flows")) return LS_TEXT(t_packs_data_bleflows, "BLE EXPLORER flows");
    if (!strcmp(id, "bughunt-ids")) return LS_TEXT(t_packs_data_bughunt, "BUGHUNT names");
    return LS_TEXT(t_packs_data_any, "app data");
}

/* "PT" -> "Portuguese". LANGUAGES lists whatever LANG rows the catalog has;
   a code missing here shows as the code alone. */
static const char *lang_name(const char *code)
{
    static const char *const L[][2] = { { "PT", "Portuguese" }, { "FR", "French" }, { "DE", "German" },
                                        { "IT", "Italian" }, { "NL", "Dutch" }, { "UK", "Ukrainian" },
                                        { "RU", "Russian" }, { "PL", "Polish" } };
    for (unsigned i = 0; i < sizeof(L) / sizeof(L[0]); i++) if (!strcmp(code, L[i][0])) return L[i][1];
    return "";
}

static void visible(void)
{
    s_nvis = 0;
    for (int u = 0; u < s_nu && s_nvis < (int)(sizeof(s_vis) / sizeof(s_vis[0])) - 1; u++) {
        if (has_subs(u) && (!u || strcmp(s_u[u - 1].cc, s_u[u].cc))) s_vis[s_nvis++] = (vis_t){ (int16_t)u, 1 };
        if (!has_subs(u) || !strcmp(s_u[u].cc, s_open)) s_vis[s_nvis++] = (vis_t){ (int16_t)u, 0 };
    }
    if (s_cur >= s_nvis) s_cur = s_nvis ? s_nvis - 1 : 0;
}

static void size_text(uint32_t b, char *out, size_t n)
{
    if (b >= 1000000) snprintf(out, n, "%.1fM", (double)b / 1e6);
    else snprintf(out, n, "%luK", (unsigned long)((b + 999) / 1000));
}

static void code_of(const unit_t *u, char code[8])
{
    if (u->sub[0]) snprintf(code, 8, "%s-%s", u->cc, u->sub);
    else snprintf(code, 8, "%s", u->cc);
}

/* Turn the globe to the unit under the cursor. */
static void aim(void)
{
    if (s_cur < 0 || s_cur >= s_nvis) return;
    char code[8];
    float la, lo;
    code_of(&s_u[s_vis[s_cur].u], code);
    if (s_vis[s_cur].head) code[2] = 0;
    if (ls_globe_region(code, NULL, 0, &la, &lo)) ls_globe_target(la, lo, code[2] ? 4.0f : 2.0f);
}

static uint8_t fill(const char *code, void *ctx)
{
    (void)ctx;
    if (strncmp(code, "US-", 3)) return 0;
    for (int u = 0; u < s_nu; u++) {
        const unit_t *x = &s_u[u];
        if (strcmp(x->cc, "US") || strcmp(x->sub, code + 3)) continue;
        return x->sel ? BR(TUI_CYAN) : x->upd ? BR(TUI_YELLOW) : x->inst == x->count ? BR(TUI_GREEN)
             : x->inst ? TUI_GREEN : 0;
    }
    return 0;
}

static int marked(bool installed)
{
    int n = 0;
    for (int u = 0; u < s_nu; u++)
        if (s_u[u].sel) n += installed ? s_u[u].inst : s_u[u].count - s_u[u].inst;
    return n;
}

static int total_upd(void)
{
    int n = 0;
    for (int u = 0; u < s_nu; u++) n += s_u[u].upd;
    return n;
}

/* op 0 install the marked, 1 remove the marked, 2 every update. */
static void act(int op)
{
    const char *ptr[64];
    int n = 0;
    for (int u = 0; u < s_nu && n < 64; u++) {
        if (op < 2 ? !s_u[u].sel : !s_u[u].upd) continue;
        const int got = ls_packs_catalog_regions(s_row, s_u[u].first, s_u[u].count < RMAX ? s_u[u].count : RMAX);
        for (int i = 0; i < got && n < 64; i++) {
            const bool want = op == 0 ? !s_row[i].installed : op == 1 ? s_row[i].installed : s_row[i].updatable;
            if (want) { snprintf(s_ids[n], sizeof(s_ids[n]), "%s", s_row[i].id); ptr[n] = s_ids[n]; n++; }
        }
        if (op < 2) s_u[u].sel = 0;
    }
    const int q = !n ? 0 : op == 1 ? ls_packs_remove(ptr, n) : ls_packs_install(ptr, n);
    if (!n) snprintf(s_say, sizeof(s_say), "%s", op == 2 ? LS_TEXT(t_packs_no_upd, "Every pack is up to date.")
                                                         : LS_TEXT(t_packs_mark_first, "Tap regions to mark them first."));
    else snprintf(s_say, sizeof(s_say), "%s %d pack%s", op == 1 ? "removing" : "getting", q, q == 1 ? "" : "s");
    ls_globe_refill();
}

static void area(void)
{
    double la, lo;
    if (ls_location_here(&la, &lo) == LS_LOC_SRC_NONE) {
        snprintf(s_say, sizeof(s_say), "%s", LS_TEXT(t_packs_nofix, "No position yet: wait for a GPS fix or set HOME."));
        ls_location_want();
        return;
    }
    ls_packs_install_around(la, lo, KM[s_km_i], 0);
    snprintf(s_say, sizeof(s_say), "getting every pack within %.0f km", (double)KM[s_km_i]);
}

static void back(void)
{
    const ls_app_t *home = ls_app_by_id("home");
    const int to = ls_packs_back >= 0 ? ls_packs_back : home ? ls_tui_screen_index_of(home->screen) : 0;
    ls_packs_back = -1;
    ls_tui_screen_show(to);
}

/* -------------------------------------------------------------- drawing -- */

/* The fixed column of kind letters, so rows read down as a matrix. */
static void kinds_col(tui_surface *sf, tui_rect c, int x, int y, uint16_t kinds)
{
    for (int i = 0; i < 7; i++)
        tui_put_char(sf, c, x + i, y, kinds & (1u << i) ? LS_HERE_GLYPH[i] : '.',
                     kinds & (1u << i) ? A(LS_HERE_HUE[i], TUI_BLACK) : LS_ATTR_FAINT);
}

/* The card without the globe's outlines: a wire globe of its own. */
static void wire_globe(tui_surface *sf, tui_rect r)
{
    static const char *const G[] = {
        "      _.-~~~~~-._      ",
        "   .-'   |   |   '-.   ",
        " .'  .---+---+---.  '. ",
        "/   /    |   |    \\   \\",
        "|--+-----+---+-----+--|",
        "\\   \\    |   |    /   /",
        " '.  '---+---+---'  .' ",
        "   '-.   |   |   .-'   ",
        "      '-._____.-'      ",
    };
    const int h = (int)(sizeof(G) / sizeof(G[0])), x = r.x + (r.w - 23) / 2, y = r.y + (r.h - h) / 2;
    for (int i = 0; i < h; i++) tui_put_str(sf, r, x, y + i, G[i], A(i == 4 ? TUI_CYAN : TUI_BLUE, TUI_BLACK));
    if (r.h > h + 1)
        tui_put_str(sf, r, r.x + 1, r.y + r.h - 1, LS_TEXT(t_packs_noglobe, "globe outlines come with MAP"), LS_ATTR_FAINT);
}

static void status(tui_surface *sf, tui_rect r, const ls_packs_status_t *st)
{
    char t[64], a[12];
    size_text(s_used, a, sizeof(a));
    snprintf(t, sizeof(t), "ON CARD %u pack%s  %sB", st->installed, st->installed == 1 ? "" : "s", a);
    tui_put_str(sf, r, r.x, r.y, t, ls_ink(LS_INK_STRONG, A(BR(TUI_WHITE), TUI_BLACK)));
    if (st->updatable) {
        snprintf(t, sizeof(t), "%u update%s", st->updatable, st->updatable == 1 ? "" : "s");
        tui_put_str(sf, r, r.x + r.w - (int)strlen(t), r.y, t, A(BR(TUI_YELLOW), TUI_BLACK));
    }
    if (r.h < 2) return;
    uint8_t at = LS_ATTR_DIM;
    if (st->state == LS_PACKS_FETCHING) {
        ls_packs_status_text(t, sizeof(t));
        tui_put_str(sf, r, r.x, r.y + 1, t, A(BR(TUI_YELLOW), TUI_BLACK));
        if (st->total) {
            const int x = r.x + (int)strlen(t) + 1;
            char b[24], c[12];
            size_text(st->got, b, sizeof(b)); size_text(st->total, c, sizeof(c));
            snprintf(b + strlen(b), sizeof(b) - strlen(b), "/%s", c);
            ls_meter_ascii(sf, r, x, r.y + 1, r.x + r.w - x - (int)strlen(b) - 1, (float)st->got / (float)st->total, TUI_YELLOW);
            tui_put_str(sf, r, r.x + r.w - (int)strlen(b), r.y + 1, b, A(TUI_YELLOW, TUI_BLACK));
        }
        return;
    }
    const char *s = s_say;
    if (st->state == LS_PACKS_NO_WIFI) { s = LS_TEXT(t_packs_nowifi, "No Wi-Fi: connect Wi-Fi in Settings or copy packs from the desktop"); at = A(BR(TUI_RED), TUI_BLACK); }
    else if (st->state == LS_PACKS_FAILED) { s = LS_TEXT(t_packs_failed, "A download failed; it tries again in 10 min"); at = A(BR(TUI_RED), TUI_BLACK); }
    else if (!s[0]) { ls_packs_status_text(t, sizeof(t)); s = t; }
    tui_put_str(sf, r, r.x, r.y + 1, s, at);
}

static void row(tui_surface *sf, tui_rect r, int y, int v, bool cur)
{
    const unit_t *u = &s_u[s_vis[v].u];
    const bool head = s_vis[v].head;
    char t[80], sz[12];
    uint8_t at = cur ? ls_ink(LS_INK_SELECT, A(TUI_BLACK, HUE)) : ls_ink(LS_INK_STRONG, A(BR(TUI_WHITE), TUI_BLACK));
    if (cur) tui_fill(sf, tui_rect_make(r.x, y, r.w, 1), ' ', at);
    char code[8], name[24] = "";
    const bool lang = is_lang(u), data = is_data(u);
    ls_pack_region_t one;
    memset(&one, 0, sizeof(one));
    if (data && !head) (void)ls_packs_catalog_regions(&one, u->first, 1);
    code_of(u, code);
    if (head) code[2] = 0;
    if (lang) snprintf(name, sizeof(name), "%s", head ? "" : lang_name(u->sub));
    else if (!data) ls_globe_region(code, name, sizeof(name), NULL, NULL);
    if (head) {
        int states = 0, on = 0, k = 0;
        for (int i = s_vis[v].u; i < s_nu && !strcmp(s_u[i].cc, u->cc); i++) { states++; on += s_u[i].inst > 0; k |= s_u[i].kinds; }
        if (lang || data) snprintf(t, sizeof(t), "%s %-17s", strcmp(s_open, u->cc) ? "+" : "-", lang ? "LANGUAGES" : "APP DATA");
        else snprintf(t, sizeof(t), "%s %s %-14.14s", strcmp(s_open, u->cc) ? "+" : "-", u->cc, name[0] ? name : "");
        tui_put_str(sf, r, r.x, y, t, at);
        snprintf(t, sizeof(t), "%d %s, %d on card", states, lang ? "languages" : data ? "packs" : "regions", on);
        tui_put_str(sf, r, r.x + 21, y, t, cur ? at : A(HUE, TUI_BLACK));
        return;
    }
    const int ind = has_subs(s_vis[v].u) ? 2 : 0;
    if (data) snprintf(t, sizeof(t), "%*s[%c] %-18.18s", ind, "", u->sel ? 'x' : ' ', one.id);
    else snprintf(t, sizeof(t), "%*s[%c] %s %-15.15s", ind, "", u->sel ? 'x' : ' ', u->sub[0] ? u->sub : strcmp(u->cc, "--") ? u->cc : "**",
                  name[0] ? name : !strcmp(u->cc, "--") ? "everywhere" : "");
    tui_put_str(sf, r, r.x, y, t, u->sel && !cur ? A(BR(TUI_CYAN), TUI_BLACK) : at);
    const int kx = r.x + r.w - 23;
    if (kx > r.x + 20 && lang) tui_put_str(sf, r, kx, y, "LANG", cur ? at : A(BR(TUI_MAGENTA), TUI_BLACK));
    else if (kx > r.x + 20 && data) tui_put_str(sf, r, kx, y, "DATA", cur ? at : A(BR(TUI_BLUE), TUI_BLACK));
    else if (kx > r.x + 20) kinds_col(sf, r, kx, y, u->kinds);
    size_text(u->bytes, sz, sizeof(sz));
    snprintf(t, sizeof(t), "%5s", sz);
    tui_put_str(sf, r, r.x + r.w - 15, y, t, cur ? at : LS_ATTR_DIM);
    const char *mark = u->upd ? "UPDATE" : u->inst == u->count ? "ON CARD" : u->inst ? "PART" : "";
    tui_put_str(sf, r, r.x + r.w - 8, y, mark, cur ? at : A(u->upd ? BR(TUI_YELLOW) : BR(TUI_GREEN), TUI_BLACK));
    if (s_per < 2) return;
    if (data) { tui_put_str(sf, r, r.x + ind + 4, y + 1, data_for(one.id), LS_ATTR_FAINT); return; }
    const int got = ls_packs_catalog_regions(s_row, u->first, u->count < 4 ? u->count : 4);
    t[0] = 0;
    for (int i = 0; i < got; i++) snprintf(t + strlen(t), sizeof(t) - strlen(t), "%s%s", s_row[i].id, s_row[i].installed ? "* " : "  ");
    if (u->count > 4) snprintf(t + strlen(t), sizeof(t) - strlen(t), "+%d", u->count - 4);
    if (lang) snprintf(t + strlen(t), sizeof(t) - strlen(t), "%s", LS_TEXT(t_packs_lang_for, "MESHTASTIC translator"));
    tui_put_str(sf, r, r.x + ind + 4, y + 1, t, LS_ATTR_FAINT);
}

static void tree(tui_surface *sf, tui_rect r, const ls_packs_status_t *st)
{
    s_list = r;
    if (!s_nu) {
        int y = r.y + 1;
        tui_put_str(sf, r, r.x, y++, LS_TEXT(t_packs_nolist, "No list of packs on the card yet."), A(BR(HUE), TUI_BLACK));
        tui_put_str(sf, r, r.x, y++, LS_TEXT(t_packs_nolist2, "REFRESH fetches it over Wi-Fi (about 60 KB)."), LS_ATTR_DIM);
        tui_put_str(sf, r, r.x, y++, LS_TEXT(t_packs_nolist3, "Or copy catalog.txt and packs from the desktop"), LS_ATTR_DIM);
        tui_put_str(sf, r, r.x, y++, LS_TEXT(t_packs_nolist4, "to /lakeshark/packs on the card."), LS_ATTR_DIM);
        if (st->state == LS_PACKS_NO_WIFI)
            tui_put_str(sf, r, r.x, y + 1, LS_TEXT(t_packs_nowifi_s, "No Wi-Fi now: connect Wi-Fi in Settings."), A(BR(TUI_RED), TUI_BLACK));
        return;
    }
    s_per = ls_tui_compact() || r.h < 10 ? 1 : 2;
    s_rows = r.h / s_per;
    if (s_rows > 0 && ls_drag_scroll(&s_ldrag, r, s_per, &s_first, s_nvis, s_rows)) s_lfree = true;
    if (!s_lfree) {
        if (s_cur < s_first) s_first = s_cur;
        if (s_cur >= s_first + s_rows) s_first = s_cur - s_rows + 1;
    }
    if (s_first > s_nvis - s_rows) s_first = s_nvis - s_rows;
    if (s_first < 0) s_first = 0;
    for (int k = 0; k < s_rows && s_first + k < s_nvis; k++) row(sf, r, r.y + k * s_per, s_first + k, s_first + k == s_cur);
    if (s_nvis > s_rows) {
        char t[16];
        snprintf(t, sizeof(t), "%d/%d", s_cur + 1, s_nvis);
        tui_put_str(sf, r, r.x + r.w - (int)strlen(t), r.y + r.h - 1, t, LS_ATTR_FAINT);
    }
}

/* The words status() puts on its second line, for the window's status bar. */
static const char *say(const ls_packs_status_t *st, char *t, size_t n)
{
    if (st->state == LS_PACKS_FETCHING || (!s_say[0] && st->state != LS_PACKS_NO_WIFI && st->state != LS_PACKS_FAILED)) {
        ls_packs_status_text(t, n);
        return t;
    }
    if (st->state == LS_PACKS_NO_WIFI) return LS_TEXT(t_packs_nowifi, "No Wi-Fi: connect Wi-Fi in Settings or copy packs from the desktop");
    if (st->state == LS_PACKS_FAILED) return LS_TEXT(t_packs_failed, "A download failed; it tries again in 10 min");
    return s_say;
}

static void globe_in(tui_surface *sf, tui_rect r)
{
    s_globe = r;
    if (ls_globe_load(OUTLINES) == LS_GLOBE_READY || ls_globe_has_shapes()) {
        char code[8] = "";
        if (s_cur < s_nvis && !s_vis[s_cur].head) code_of(&s_u[s_vis[s_cur].u], code);
        ls_globe_draw(sf, s_globe, fill, NULL, code[0] ? code : NULL);
    } else wire_globe(sf, s_globe);
}

/* A desk window: both rows of keys on the one toolbar (BACK is the title
   bar's close and Esc), the globe on black glass, what is on the card in a
   group box, the packs in a white well, the news in the status bar. */
static void draw_desk(tui_surface *sf, tui_rect a, const ls_packs_status_t *st, const ls_btn_t *bar, const char *km, bool wide)
{
    tui_fill(sf, a, ' ', A(TUI_WHITE, TUI_BLACK));
    const ls_btn_t mine[2] = {
        { "INSTALL MY AREA", LS_TEXT(t_packs_around, "by GPS"), 'a', false, st->state == LS_PACKS_FETCHING, false },
        { "RADIUS", km, 'k', false, false, false },
    };
    const int w1 = ls_btn_bar_width(bar, 4) + 2;
    ls_btn_bar_raised_slot(sf, tui_rect_make(a.x, a.y, w1 < a.w ? w1 : a.w, 1), bar, 4, -1, LS_BTN_SLOT_SCREEN);
    /* MY AREA and RADIUS on the toolbar when they fit, else in the card's group */
    const bool quick_top = w1 + ls_btn_bar_width(mine, 2) + 2 <= a.w;
    if (quick_top) ls_btn_bar_raised_slot(sf, tui_rect_make(a.x + w1, a.y, a.w - w1, 1), mine, 2, -1, LS_BTN_SLOT_QUICK);
    const tui_rect body = tui_rect_make(a.x + 1, a.y + 1, a.w - 2, a.h - 1);
    char t[80];
    ls_tui_status_set(say(st, t, sizeof(t)), NULL);
    tui_rect list = body;
    if (wide) {
        const int lw = body.w * 2 / 5;
        const int gh = quick_top ? 4 : 5;
        const tui_rect g = ls_dui_glass(sf, tui_rect_make(body.x, body.y, lw, body.h - gh));
        globe_in(sf, g);
        const tui_rect c = ls_dui_group(sf, tui_rect_make(body.x, body.y + body.h - gh, lw, gh), "On card");
        status(sf, tui_rect_make(c.x + 1, c.y, c.w - 2, 2), st);
        if (!quick_top) ls_btn_bar_raised_slot(sf, tui_rect_make(c.x + 1, c.y + 2, c.w - 2, 1), mine, 2, -1, LS_BTN_SLOT_QUICK);
        list = tui_rect_make(body.x + lw + 1, body.y, body.w - lw - 1, body.h);
    } else {
        s_globe = tui_rect_make(body.x, body.y, 0, 0);
        const int gh = quick_top ? 4 : 5;
        const tui_rect c = ls_dui_group(sf, tui_rect_make(body.x, body.y, body.w, gh), "On card");
        status(sf, tui_rect_make(c.x + 1, c.y, c.w - 2, 2), st);
        if (!quick_top) ls_btn_bar_raised_slot(sf, tui_rect_make(c.x + 1, c.y + 2, c.w - 2, 1), mine, 2, -1, LS_BTN_SLOT_QUICK);
        list = tui_rect_make(body.x, body.y + gh, body.w, body.h - gh);
    }
    const tui_rect in = ls_dui_well(sf, list, NULL);
    tree(sf, tui_rect_make(in.x + 1, in.y, in.w - 2, in.h), st);
}

static void draw(tui_surface *sf, tui_rect a)
{
    ls_packs_status_t st;
    ls_packs_status(&st);
    if (s_ncat != ls_packs_catalog_regions(NULL, 0, 0) || s_inst != st.installed || s_upd != st.updatable) {
        build();
        s_inst = st.installed; s_upd = st.updatable;
    }
    visible();
    /* Open on the region the board is in, its country unfolded. */
    if (s_find_here && s_nu) {
        ls_location_t l;
        ls_location_get(&l);
        s_find_here = false;
        if (l.region[0]) {
            snprintf(s_open, sizeof(s_open), "%.2s", l.region);
            visible();
            for (int v = 0; v < s_nvis; v++) {
                const unit_t *u = &s_u[s_vis[v].u];
                if (!s_vis[v].head && !strncmp(u->cc, l.region, 2) && (!u->sub[0] || !strcmp(u->sub, l.region + 3))) { s_cur = v; break; }
            }
            s_first = s_cur - 2;
        }
    }
    const bool compact = ls_tui_compact(), wide = a.w > a.h * 2 && a.w >= 90;
    const int nm = marked(false), nr = marked(true), nu = total_upd();
    char vi[8], vr[8], vu[8], km[12];
    snprintf(vi, sizeof(vi), "%d", nm); snprintf(vr, sizeof(vr), "%d", nr); snprintf(vu, sizeof(vu), "%d", nu);
    snprintf(km, sizeof(km), "%.0f km", (double)KM[s_km_i]);
    const ls_btn_t bar[5] = {
        { "INSTALL", vi, 'i', nm > 0, !nm, false },
        { "REMOVE", vr, 'x', false, !nr, false },
        { "UPDATE", vu, 'u', nu > 0, !nu, nu > 0 },
        { "REFRESH", NULL, 'r', false, false, false },
        { "BACK", NULL, 'b', false, false, false },
    };
    if (compact) { draw_desk(sf, a, &st, bar, km, wide); return; }
    const int bh = compact ? 1 : ls_btn_raised_height(a, 5);
    ls_btn_bar_raised_slot(sf, tui_rect_make(a.x, a.y + a.h - bh, a.w, bh), bar, 5, -1, LS_BTN_SLOT_SCREEN);
    const tui_rect body = tui_rect_make(a.x, a.y, a.w, a.h - bh - 1);
    tui_rect left = body, right = body;
    if (wide) {
        left.w = body.w * 2 / 5; right.x = left.x + left.w + 1; right.w = body.w - left.w - 1;
        tui_rect hole;                   /* a landscape camera hole at the edge: the tree stops short of it */
        if (ls_tui_cutout(&hole) && hole.x > right.x && hole.x < right.x + right.w) right.w = hole.x - 1 - right.x;
    }
    /* The globe in the wide column a window has too; stacked windows keep the rows for the list. */
    const int gh = wide ? left.h - (compact ? 4 : 8) : compact ? 0 : body.h >= 56 ? 18 : body.h >= 40 ? 12 : 0;
    s_globe = tui_rect_make(left.x, left.y, left.w, gh > 0 ? gh : 0);
    /* zoom keys on the globe's top edge, right: a finger's width each, the
       row under them takes the touch too */
    s_zin = s_zout = tui_rect_make(0, 0, 0, 0);
    const int zx = s_globe.x + s_globe.w - 12;
    if (gh > 0 && s_globe.w >= 16) {
        s_zin = tui_rect_make(zx, s_globe.y, 5, 2);
        s_zout = tui_rect_make(zx + 6, s_globe.y, 5, 2);
    }
    /* A finger that went down on the globe (not on its zoom keys) turns it. */
    int sc, sr, fc, fr;
    if (gh > 0 && ls_tui_touch_held(&sc, &sr, &fc, &fr)) {
        if (!s_gd.held && tui_rect_contains(s_globe, sc, sr) && !tui_rect_contains(s_zin, sc, sr) &&
            !tui_rect_contains(s_zout, sc, sr)) {
            s_gd.held = true; s_gd.moved = false;
            ls_globe_view(&s_gd.lat0, &s_gd.lon0, NULL);
        }
        if (s_gd.held && (fc != sc || fr != sr)) {
            s_gd.moved = true;
            ls_globe_drag(s_gd.lat0, s_gd.lon0, fc - sc, fr - sr);
        }
    } else {
        if (s_gd.held && s_gd.moved) s_gd.lift_us = esp_timer_get_time();
        s_gd.held = s_gd.moved = false;
    }
    if (gh > 0) {
        if (ls_globe_load(OUTLINES) == LS_GLOBE_READY || ls_globe_has_shapes()) {
            char code[8] = "";
            if (s_cur < s_nvis && !s_vis[s_cur].head) code_of(&s_u[s_vis[s_cur].u], code);
            ls_globe_draw(sf, s_globe, fill, NULL, code[0] ? code : NULL);
            if (s_zin.w) {
                const uint8_t kc = ls_ink(LS_INK_KEY, A(TUI_BLACK, TUI_WHITE));
                tui_put_str(sf, s_globe, zx, s_globe.y, "  +  ", kc);
                tui_put_str(sf, s_globe, zx + 6, s_globe.y, "  -  ", kc);
            }
        } else wire_globe(sf, s_globe);
    }
    int y = left.y + (gh > 0 ? gh : 0);
    status(sf, tui_rect_make(left.x, y, left.w, 2), &st);
    y += 2;
    const ls_btn_t mine[2] = {
        { "INSTALL MY AREA", LS_TEXT(t_packs_around, "by GPS"), 'a', false, st.state == LS_PACKS_FETCHING, false },
        { "RADIUS", km, 'k', false, false, false },
    };
    const int mh = compact ? 1 : 3;
    tui_rect mr = tui_rect_make(left.x, y, left.w, mh);
    ls_btn_bar_raised_slot(sf, mr, mine, 2, -1, LS_BTN_SLOT_QUICK);
    y += mh + 1;
    if (!wide) right = tui_rect_make(body.x, y, body.w, body.y + body.h - y);
    tui_hline(sf, a, right.x, right.y - 1, right.w, '-', ls_ink(LS_INK_RULE, LS_ATTR_FAINT));
    tree(sf, right, &st);
}

/* ---------------------------------------------------------------- input -- */

static void toggle(int v)
{
    if (v < 0 || v >= s_nvis) return;
    if (s_vis[v].head) {
        const char *cc = s_u[s_vis[v].u].cc;
        if (strcmp(s_open, cc)) snprintf(s_open, sizeof(s_open), "%s", cc);
        else s_open[0] = 0;
    } else s_u[s_vis[v].u].sel ^= 1;
    s_say[0] = 0;
    ls_globe_refill();
}

static void zoom(bool in)
{
    float la, lo, z;
    ls_globe_view(&la, &lo, &z);
    ls_globe_target(la, lo, in ? z * 1.6f : z / 1.6f);
}

static void move(int d)
{
    s_lfree = false;
    s_cur += d;
    if (s_cur < 0) s_cur = 0;
    if (s_cur >= s_nvis) s_cur = s_nvis ? s_nvis - 1 : 0;
    aim();
}

static void button(int b)
{
    if (b == 0) act(0);
    else if (b == 1) act(1);
    else if (b == 2) act(2);
    else if (b == 3) { ls_packs_refresh_catalog(); snprintf(s_say, sizeof(s_say), "%s", LS_TEXT(t_packs_asked, "asked for the list")); }
    else if (b == 4) back();
}

static bool key(ls_tk_t k, char ch)
{
    if (k == LS_TK_WHEEL_UP || k == LS_TK_UP) { move(-1); return true; }
    if (k == LS_TK_WHEEL_DOWN || k == LS_TK_DOWN) { move(1); return true; }
    if (k == LS_TK_PGUP || k == LS_TK_PGDN) { move(k == LS_TK_PGUP ? -s_rows : s_rows); return true; }
    if (k == LS_TK_ENTER || (k == LS_TK_CHAR && ch == ' ')) { toggle(s_cur); return true; }
    if (k == LS_TK_ESC || k == LS_TK_BACKSPACE) { back(); return true; }
    if (k != LS_TK_CHAR) return false;
    if (ch >= 'A' && ch <= 'Z') ch = (char)(ch + 32);
    const char *p = strchr("ixurb", ch);
    if (p) { button((int)(p - "ixurb")); return true; }
    if (ch == 'a') { area(); return true; }
    if (ch == 'k') { s_km_i = (s_km_i + 1) % 4; return true; }
    if (ch == '+' || ch == '=' || ch == '-') { zoom(ch != '-'); return true; }
    return false;
}

static bool touch(int col, int row)
{
    int b = ls_btn_hit_slot(col, row, LS_BTN_SLOT_SCREEN);
    if (b >= 0) { if (ls_btn_enabled(LS_BTN_SLOT_SCREEN, b)) button(b); return true; }
    b = ls_btn_hit_slot(col, row, LS_BTN_SLOT_QUICK);
    if (b == 0) { area(); return true; }
    if (b == 1) { s_km_i = (s_km_i + 1) % 4; return true; }
    if (tui_rect_contains(s_zin, col, row) || tui_rect_contains(s_zout, col, row)) {
        zoom(tui_rect_contains(s_zin, col, row));
        return true;
    }
    if (tui_rect_contains(s_list, col, row) && s_nvis) {
        if (ls_drag_scroll_tap_is_lift(&s_ldrag)) return true;     /* the end of a scroll, not a pick */
        const int v = s_first + (row - s_list.y) / (s_per ? s_per : 1);
        if (v < s_nvis) { s_cur = v; toggle(v); aim(); }
        return true;
    }
    char code[8];
    /* the lift of a finger that turned the globe */
    if (s_gd.moved || (s_gd.lift_us && esp_timer_get_time() - s_gd.lift_us < 250000)) return true;
    if (s_globe.h > 0 && tui_rect_contains(s_globe, col, row) && ls_globe_hit(col, row, code, NULL, NULL) >= 0) {
        /* A state marks itself; a country without states too; a country
           with states unfolds them. */
        const bool st = code[2] == '-';
        snprintf(s_open, sizeof(s_open), "%.2s", code);
        visible();
        for (int v = 0; v < s_nvis; v++) {
            const unit_t *u = &s_u[s_vis[v].u];
            if (strncmp(u->cc, code, 2) || (st && (s_vis[v].head || strcmp(u->sub, code + 3)))) continue;
            s_cur = v;
            if (!s_vis[v].head) toggle(v);
            break;
        }
    }
    return true;
}

static void enter(void)
{
    double la, lo;
    s_say[0] = 0;
    s_ncat = -1;
    s_find_here = true;
    s_lfree = false;
    if (ls_location_here(&la, &lo) != LS_LOC_SRC_NONE) ls_globe_target((float)la, (float)lo, 3.0f);
}

const ls_tui_screen_t ls_scr_packs = {
    .name  = "PACKS",
    .hint  = "UP/DOWN move  SPACE mark  I install  X remove  U update  A my area  K radius  R refresh  +/- zoom",
    .enter = enter,
    .draw  = draw,
    .key   = key,
    .touch = touch,
    .wheel = true,
};
