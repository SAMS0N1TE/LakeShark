/* The desk (components/apps/tui/desk): the router drawn into a window's own
   surface, taps that follow the window when it moves, and the window
   manager - open, raise, move, resize, minimize, maximize and close - with
   every PSRAM block it took given back. Menus open, cascade and close. */

#include "ls_test.h"
#include "tui_core.h"
#include "ls_tui_screen.h"
#include "ls_tui_ui.h"
#include "ls_tui_touch.h"
#include "ls_theme.h"
#include "ls_app.h"
#include "ls_icons.h"
#include "desk/ls_desk.h"
#include "desk/ls_desk_priv.h"

#include <string.h>

/* ---------------------------------------------------------------- stubs -- */

static int s_cols = 160, s_rows = 45;
void ls_tui_image(tui_rect cells, const uint16_t *src, int w, int h, uint32_t serial) { (void)cells; (void)src; (void)w; (void)h; (void)serial; }
bool ls_tui_image_now(tui_rect *cells, const uint16_t **src, int *w, int *h, uint32_t *serial) { (void)cells; (void)src; (void)w; (void)h; (void)serial; return false; }
void ls_tui_image_ring(tui_rect cells, const uint16_t *src, int w, int h, int x0, int y0, int dx, int dy, uint32_t serial) { (void)cells; (void)src; (void)w; (void)h; (void)x0; (void)y0; (void)dx; (void)dy; (void)serial; }
void ls_tui_geometry(int *cols, int *rows, int *cw, int *ch)
{
    if (cols) *cols = s_cols;
    if (rows) *rows = s_rows;
    if (cw) *cw = 10;
    if (ch) *ch = 17;
}
void ls_tui_invalidate(void) { }
int ls_tui_corner_pad(int row) { (void)row; return 0; }
bool ls_tui_cutout(tui_rect *cells) { if (cells) *cells = tui_rect_make(0, 0, 0, 0); return false; }
static const ls_tui_theme_t *s_active;
const ls_tui_theme_t *ls_tui_get_theme(void) { return s_active ? s_active : ls_tui_theme_at(0); }
void ls_tui_set_theme(const ls_tui_theme_t *t) { s_active = t; }
bool ls_tui_daylight(void) { return false; }
void ls_tui_last_cost(uint32_t *us, int *cells) { if (us) *us = 0; if (cells) *cells = 0; }
const char *dk_live_line(const ls_app_t *a, char *buf, size_t n) { (void)a; (void)buf; (void)n; return NULL; }
/* The board around the desk: a fixed sound state, nothing saved, no card. */
static int s_vol = 30; static bool s_mute; static int s_saved_theme = -1; static uint32_t s_saved_wp;
void ls_tui_glass(int col, int row) { (void)col; (void)row; }
bool ls_touch_read(uint16_t *x,uint16_t *y,bool *pressed)
{ (void)x;(void)y;*pressed=false;return true; }
bool ls_tui_pixel_to_cell(int x,int y,int *col,int *row)
{ if(col)*col=x/10;if(row)*row=y/17;return x>=0 && y>=0; }
bool ls_tui_pixel_to_grid(int x,int y,int *gx,int *gy)
{ if(gx)*gx=x;if(gy)*gy=y;return true; }
const char *ls_desk_version(void) { return "test"; }
int  dk_sys_volume(void) { return s_vol; }
void dk_sys_set_volume(int p) { s_vol = p < 0 ? 0 : p > 100 ? 100 : p; }
bool dk_sys_muted(void) { return s_mute; }
void dk_sys_set_mute(bool on) { s_mute = on; }
int  dk_sys_bt(int *c) { if (c) *c = 0; return -1; }
int  dk_sys_time_source(void) { return 0; }
int  dk_sys_saved_theme(void) { return s_saved_theme; }
void dk_sys_save_theme(int t) { s_saved_theme = t; }
uint32_t dk_sys_saved_wallpaper(void) { return s_saved_wp; }
void dk_sys_save_wallpaper(uint32_t id) { s_saved_wp = id; }
static uint32_t s_words[24];
uint32_t dk_sys_saved_word(int s) { return s_words[s]; }
void dk_sys_save_word(int s, uint32_t v) { s_words[s] = v; }
int  dk_sys_wallpaper_files(char names[][40], int cap) { (void)names; (void)cap; return 0; }
bool dk_sys_load_wallpaper(const char *n, int mw, int mh, int fit, int fw, uint8_t g, uint16_t **px, int *w, int *h)
{ (void)n; (void)mw; (void)mh; (void)fit; (void)fw; (void)g; (void)px; (void)w; (void)h; return false; }
/* Where the desk said the window's surface sits. */
static const tui_surface *s_org_sf;
static int s_org_x = -1, s_org_y = -1, s_org_calls;
void ls_tui_set_origin(const tui_surface *sf, int dx, int dy)
{
    if (sf) { s_org_sf = sf; s_org_x = dx; s_org_y = dy; s_org_calls++; }
}

/* ---------------------------------------------------------- test apps -- */

static tui_rect s_win_area;
static bool s_win_wide;
static int s_win_hit = -2, s_keys;
static ls_tk_t s_last_key;
static int s_wheel_col,s_wheel_row;
static void win_draw(tui_surface *sf, tui_rect a)
{
    s_win_area = a;
    s_win_wide = ls_tui_is_wide();
    static const ls_btn_t B[3] = { {"ONE", NULL, 0, false, false, false},
                                   {"TWO", NULL, 0, false, false, false},
                                   {"THREE", NULL, 0, false, false, false} };
    ls_btn_bar(sf, tui_rect_make(a.x, a.y + a.h - 3, a.w, 3), B, 3, -1);
}
static bool win_touch(int col, int row) { s_win_hit = ls_btn_hit(col, row); return s_win_hit >= 0; }
static bool win_key(ls_tk_t k, char ch) { (void)ch; s_keys++; s_last_key = k;
    ls_tui_pointer_cell(&s_wheel_col,&s_wheel_row); return k != LS_TK_ESC; }
static void plain_draw(tui_surface *sf, tui_rect a) { tui_put_str(sf, a, a.x + 1, a.y + 1, "PLAIN", TUI_DEFAULT_ATTR); }

static const ls_tui_screen_t SCR_HOME = { .name = "HOME", .hint = "home", .draw = plain_draw };
static const ls_tui_screen_t SCR_WIN = { .name = "WIN", .hint = "W hint", .draw = win_draw,
                                         .touch = win_touch, .key = win_key, .wheel = true };
static const ls_tui_screen_t SCR_PLAIN = { .name = "PLAIN", .hint = "P hint", .draw = plain_draw, .radio = "FM" };
static const ls_app_doc_t DOC = { .purpose = "A test window.", .records = LS_APP_RECORDS_NOTHING, .gps = LS_APP_GPS_UNUSED };
static const ls_app_t APP_HOME = { "home", "HOME", NULL, LS_ICON_SHARK, TUI_CYAN, LS_APP_MAIN, &SCR_HOME, NULL, &DOC, NULL };
static const ls_app_t APP_WIN = { "p25", "P25", NULL, LS_ICON_TOWER, TUI_GREEN, LS_APP_MAIN, &SCR_WIN, NULL, &DOC, NULL };
static const ls_app_t APP_PLAIN = { "fm", "FM", NULL, LS_ICON_WAVE, TUI_YELLOW, LS_APP_MAIN, &SCR_PLAIN, NULL, &DOC, NULL };

static tui_cell g_back[384 * 112], g_front[384 * 112];
static tui_surface g_sf;

static void frame(int cols, int rows)
{
    s_cols = cols; s_rows = rows;
    if (g_sf.w != cols || g_sf.h != rows) tui_surface_setup(&g_sf, g_back, g_front, cols, rows);
    ls_desk_draw(&g_sf);
}

static void setup(void)
{
    if (!ls_app_by_id("home")) {
        ls_app_register(&APP_HOME);
        ls_app_register(&APP_WIN);
        ls_app_register(&APP_PLAIN);
    }
    ls_desk_set_enabled(false);      /* closes any menu a case left open */
    ls_desk_set_enabled(true);
    ls_desk_close_all();
    frame(160, 45);
}

static void ptr(int kind, int button, int col, int row)
{
    ls_input_ev_t e = {.kind = (uint8_t)kind, .source = LS_ISRC_LOCAL, .button = (uint8_t)button,
                       .buttons = kind == LS_IEV_PTR_UP ? 0 : (uint8_t)(1u << button), .clicks = 1,
                       .col = (int16_t)col, .row = (int16_t)row};
    LS_CHECK(ls_desk_input(&e));
}
static void click(int col, int row) { ptr(LS_IEV_PTR_DOWN, LS_IBTN_LEFT, col, row); ptr(LS_IEV_PTR_UP, LS_IBTN_LEFT, col, row); }
static void key(ls_tk_t k)
{
    ls_input_ev_t e = {.kind = LS_IEV_KEY_DOWN, .source = LS_ISRC_LOCAL, .clicks = 1, .key = (uint16_t)k};
    LS_CHECK(ls_desk_input(&e));
}
static bool info(int id, ls_desk_win_info_t *o)
{
    for (int z = 0; ls_desk_window_at(z, o); z++) if (o->id == id) return true;
    return false;
}

static const char *lbl(int level, int row)
{
    const char *l = ls_desk_menu_label(level, row);
    return l ? l : "(none)";
}
#define LABEL_IS(level, row, want) LS_CHECK_MSG(!strcmp(lbl(level, row), want), "menu %d row %d is '%s', want '%s'", level, row, lbl(level, row), want)

/* -------------------------------------------------------------- cases -- */

LS_CASE(the_router_lays_out_in_a_window_and_its_hit_rects_move_with_it)
{
    setup();
    const int id = ls_desk_open_app("p25");
    LS_CHECK(id > 0);
    frame(160, 45);
    ls_desk_win_info_t w;
    LS_CHECK(info(id, &w));
    LS_EQ_INT(115, w.cw); LS_EQ_INT(27, w.ch);
    /* The app was laid out in its 115x27 surface, not the 160x45 grid, and
       told so by is_wide; its pixels are offset to the client's place. */
    LS_EQ_INT(0, s_win_area.x); LS_EQ_INT(0, s_win_area.y);
    LS_EQ_INT(115, s_win_area.w); LS_EQ_INT(27, s_win_area.h);
    LS_CHECK(s_win_wide);
    LS_EQ_INT(w.cx, s_org_x); LS_EQ_INT(w.cy, s_org_y);
    int bx, by, bw, bh;
    LS_CHECK(ls_btn_rect_slot(LS_BTN_SLOT_SCREEN, 1, &bx, &by, &bw, &bh));
    LS_CHECK_MSG(by >= 24 && by + bh <= 27, "button rows %d..%d are not the window's bottom", by, by + bh);
    /* The same button at two places: a tap in grid cells reaches it. */
    const int old_x = w.cx + bx + bw / 2, old_y = w.cy + by + bh / 2;
    s_win_hit = -2;
    click(old_x, old_y);
    LS_EQ_INT(1, s_win_hit);
    ls_desk_move(id, w.x + 11, w.y + 7);
    frame(160, 45);
    LS_CHECK(info(id, &w));
    LS_EQ_INT(w.cx, s_org_x); LS_EQ_INT(w.cy, s_org_y);
    s_win_hit = -2;
    click(w.cx + bx + bw / 2, w.cy + by + bh / 2);
    LS_EQ_INT(1, s_win_hit);
    s_win_hit = -2;
    click(old_x, old_y);              /* where it was: now another part of the window */
    LS_CHECK(s_win_hit != 1);
    /* Keys reach the front app; the router's global ones do not act. */
    s_keys = 0;
    key(LS_TK_F2);
    LS_EQ_INT(1, s_keys);
    LS_EQ_INT(ls_tui_screen_index_of(&SCR_WIN), ls_tui_screen_current());
    /* An Esc the app does not take opens its window's menu. */
    key(LS_TK_ESC);
    LS_EQ_INT(1, ls_desk_menu_depth());
    LABEL_IS(0, 0, "Back");
    key(LS_TK_ESC);
    LS_EQ_INT(0, ls_desk_menu_depth());
    ls_desk_close_all();
    frame(160, 45);
    LS_EQ_INT(0, ls_desk_alloc_blocks());
}

LS_CASE(open_raise_move_minimize_maximize_close_and_nothing_leaks)
{
    setup();
    for (int round = 0; round < 3; round++) {
        const int a = ls_desk_open_app("p25");
        frame(160, 45);
        LS_EQ_INT(2, ls_desk_alloc_blocks());              /* the live surface, two grids */
        const int b = ls_desk_open_app("fm");
        frame(160, 45);
        LS_EQ_INT(2, ls_desk_window_count());
        LS_EQ_INT(b, ls_desk_front());
        ls_desk_win_info_t wa, wb;
        LS_CHECK(info(a, &wa) && info(b, &wb));
        LS_CHECK(wa.has_snapshot && !wa.front);            /* the back one keeps its frame */
        LS_EQ_INT(3, ls_desk_alloc_blocks());
        /* The same app again raises its window, it does not open another. */
        LS_EQ_INT(a, ls_desk_open_app("p25"));
        frame(160, 45);
        LS_EQ_INT(a, ls_desk_front());
        LS_CHECK(info(b, &wb) && wb.has_snapshot);
        LS_EQ_INT(4, ls_desk_alloc_blocks());
        /* Drag the title: the window follows every cell (live move). */
        LS_CHECK(info(a, &wa));
        ptr(LS_IEV_PTR_DOWN, LS_IBTN_LEFT, wa.x + 30, wa.y);
        ptr(LS_IEV_PTR_MOVE, LS_IBTN_LEFT, wa.x + 35, wa.y + 2);
        frame(160, 45);
        ls_desk_win_info_t still;
        LS_CHECK(info(a, &still));
        LS_EQ_INT(wa.x + 5, still.x); LS_EQ_INT(wa.y + 2, still.y);
        ptr(LS_IEV_PTR_MOVE, LS_IBTN_LEFT, wa.x + 40, wa.y + 3);
        ptr(LS_IEV_PTR_UP, LS_IBTN_LEFT, wa.x + 40, wa.y + 3);
        frame(160, 45);
        LS_CHECK(info(a, &still));
        LS_EQ_INT(wa.x + 10, still.x); LS_EQ_INT(wa.y + 3, still.y);
        /* Resize at the grip, held to the minimum. */
        ptr(LS_IEV_PTR_DOWN, LS_IBTN_LEFT, still.x + still.w - 1, still.y + still.h - 1);
        ptr(LS_IEV_PTR_MOVE, LS_IBTN_LEFT, still.x + 20, still.y + 5);
        ptr(LS_IEV_PTR_UP, LS_IBTN_LEFT, still.x + 20, still.y + 5);
        frame(160, 45);
        LS_CHECK(info(a, &wa));
        LS_EQ_INT(62, wa.w); LS_EQ_INT(23, wa.h);
        LS_EQ_INT(60, wa.cw); LS_EQ_INT(20, wa.ch);
        /* Minimize with its title key: the other comes to the front. */
        click(wa.x + wa.w - 8, wa.y);
        frame(160, 45);
        LS_CHECK(info(a, &wa));
        LS_EQ_INT(LS_DESK_WIN_MIN, wa.state);
        LS_EQ_INT(b, ls_desk_front());
        /* Its taskbar button brings it back. */
        click(13, 44);
        frame(160, 45);
        LS_EQ_INT(a, ls_desk_front());
        /* Maximize with a double click on the title: the whole work area. */
        LS_CHECK(info(a, &wa));
        ls_input_ev_t e = {.kind = LS_IEV_PTR_DOWN, .source = LS_ISRC_LOCAL, .button = LS_IBTN_LEFT,
                           .buttons = 1, .clicks = 2, .col = (int16_t)(wa.x + 20), .row = (int16_t)wa.y};
        ls_desk_input(&e);
        e.kind = LS_IEV_PTR_UP; e.buttons = 0; ls_desk_input(&e);
        frame(160, 45);
        LS_CHECK(info(a, &wa));
        LS_EQ_INT(LS_DESK_WIN_MAX, wa.state);
        LS_EQ_INT(0, wa.x); LS_EQ_INT(0, wa.y); LS_EQ_INT(160, wa.w); LS_EQ_INT(43, wa.h);
        /* Close with its cross, then the other through the window menu. */
        click(wa.x + wa.w - 2, wa.y);
        frame(160, 45);
        LS_EQ_INT(1, ls_desk_window_count());
        LS_EQ_INT(b, ls_desk_front());
        ls_desk_close(b);
        frame(160, 45);
        LS_EQ_INT(0, ls_desk_window_count());
        LS_EQ_INT(0, ls_desk_alloc_blocks());
        LS_EQ_INT(-1, ls_desk_front());
    }
}

LS_CASE(start_cascades_and_the_right_button_opens_the_right_menu)
{
    setup();
    click(3, 44);
    LS_EQ_INT(1, ls_desk_menu_depth());
    LABEL_IS(0, -1, "Programs");
    key(LS_TK_RIGHT);
    LS_EQ_INT(2, ls_desk_menu_depth());
    LABEL_IS(1, -1, "Radio");
    key(LS_TK_RIGHT);
    LS_EQ_INT(3, ls_desk_menu_depth());
    LABEL_IS(2, -1, "P25");
    /* Enter on it opens P25 in a window and closes every menu. */
    key(LS_TK_ENTER);
    frame(160, 45);
    LS_EQ_INT(0, ls_desk_menu_depth());
    LS_EQ_INT(1, ls_desk_window_count());
    /* The desktop's own menu, and a window's from its title. */
    ptr(LS_IEV_PTR_DOWN, LS_IBTN_RIGHT, 150, 30);
    LS_EQ_INT(1, ls_desk_menu_depth());
    LABEL_IS(0, 0, "Arrange icons");
    click(100, 20);                     /* outside: closes, and does nothing else */
    LS_EQ_INT(0, ls_desk_menu_depth());
    ls_desk_win_info_t w;
    LS_CHECK(ls_desk_window_at(0, &w));
    ptr(LS_IEV_PTR_DOWN, LS_IBTN_RIGHT, w.x + 20, w.y);
    LABEL_IS(0, 0, "Restore");
    /* Hover moves the highlight, Esc closes one level at a time. */
    ls_input_ev_t h = {.kind = LS_IEV_PTR_MOVE, .source = LS_ISRC_LOCAL, .col = (int16_t)(w.x + 24), .row = (int16_t)(w.y + 4)};
    ls_desk_input(&h);
    LABEL_IS(0, -1, "Minimize");
    key(LS_TK_ESC);
    LS_EQ_INT(0, ls_desk_menu_depth());
    ls_desk_close_all();
    frame(160, 45);
}

LS_CASE(a_smaller_grid_keeps_every_title_on_screen)
{
    setup();
    const int id = ls_desk_open_app("p25");
    ls_desk_move(id, 40, 10);
    frame(214, 60);
    frame(120, 40);
    ls_desk_win_info_t w;
    LS_CHECK(info(id, &w));
    LS_CHECK(w.w <= 120 && w.x + 8 <= 120 && w.y < 38);
    LS_CHECK(!w.has_snapshot);
    frame(214, 60);
    ls_desk_close_all();
    frame(160, 45);
    LS_EQ_INT(0, ls_desk_alloc_blocks());
    ls_desk_set_enabled(false);
}

LS_CASE(dense_desktop_maximizes_and_hits_beyond_column_255)
{
    setup();
    const int sizes[][2] = {{360, 105}, {384, 112}};
    for (unsigned i = 0; i < sizeof(sizes) / sizeof(sizes[0]); i++) {
        const int cols = sizes[i][0], rows = sizes[i][1];
        frame(cols, rows);
        const int id = ls_desk_open_app("p25");
        ls_desk_maximize(id);
        frame(cols, rows);
        ls_desk_win_info_t w;
        LS_CHECK(info(id, &w));
        LS_EQ_INT(LS_DESK_WIN_MAX, w.state);
        LS_EQ_INT(cols, w.w); LS_EQ_INT(rows - 2, w.h);
        LS_EQ_INT(w.cw, s_win_area.w); LS_EQ_INT(w.ch, s_win_area.h);
        int bx, by, bw, bh;
        LS_CHECK(ls_btn_rect_slot(LS_BTN_SLOT_SCREEN, 2, &bx, &by, &bw, &bh));
        s_win_hit = -2;
        const int x = w.cx + bx + bw / 2;
        click(x, w.cy + by + bh / 2);
        LS_EQ_INT(2, s_win_hit);
        LS_CHECK(w.x + w.w - 2 > 255);
        click(w.x + w.w - 2, w.y); /* the cross near column 384 */
        frame(cols, rows);
        LS_EQ_INT(0, ls_desk_window_count());
        LS_EQ_INT(0, ls_desk_alloc_blocks());
    }
    frame(160, 45);
    ls_desk_set_enabled(false);
}


LS_CASE(app_finger_is_never_polled_back_as_desktop_input)
{
    setup();int id=ls_desk_open_app("p25");frame(160,45);
    dk_win_t *w=dk_win(id);tui_rect c;dk_win_rects(w,NULL,&c);
    LS_CHECK(c.x>0 && c.y>0);
    ls_input_ev_t e={.kind=LS_IEV_PTR_DOWN,.source=LS_ISRC_LOCAL,.button=LS_IBTN_LEFT,
        .buttons=1,.col=c.x+25,.row=c.y+12,.px=(c.x+25)*10+3,.py=(c.y+12)*17+2};
    LS_CHECK(ls_desk_input(&e));
    e.kind=LS_IEV_PTR_MOVE;e.col-=20;e.row-=6;e.px-=200;e.py-=102;
    LS_CHECK(ls_desk_input(&e));
    for(int i=0;i<8;i++) {
        frame(160,45);
        int sc,sr,col,row,x,y;
        LS_CHECK(ls_tui_touch_held(&sc,&sr,&col,&row));
        LS_EQ_INT(25,sc);LS_EQ_INT(12,sr);LS_EQ_INT(5,col);LS_EQ_INT(6,row);
        LS_CHECK(ls_tui_touch_px(&x,&y,NULL,NULL,NULL));
        LS_EQ_INT(53,x);LS_EQ_INT(104,y);
        LS_CHECK(!ls_tui_touch_input_held(NULL,NULL,NULL,NULL));
    }
    e.kind=LS_IEV_PTR_UP;e.buttons=0;LS_CHECK(ls_desk_input(&e));
    LS_CHECK(!ls_tui_touch_held(NULL,NULL,NULL,NULL));
}
LS_CASE(wheel_preserves_direction_steps_and_window_relative_pointer)
{
    setup();int id=ls_desk_open_app("p25");frame(160,45);
    dk_win_t *w=dk_win(id);tui_rect c;dk_win_rects(w,NULL,&c);
    ls_input_ev_t e={.kind=LS_IEV_WHEEL,.col=c.x+10,.row=c.y+7,.wheel=-2};
    int before=s_keys;LS_CHECK(ls_desk_input(&e));
    LS_EQ_INT(before+2,s_keys);LS_EQ_INT(LS_TK_WHEEL_UP,s_last_key);
    LS_EQ_INT(10,s_wheel_col);LS_EQ_INT(7,s_wheel_row);
    LS_CHECK(!ls_tui_pointer_cell(NULL,NULL));
    e.wheel=1;LS_CHECK(ls_desk_input(&e));LS_EQ_INT(LS_TK_WHEEL_DOWN,s_last_key);
    ls_desk_fullscreen(true);e.col=34;e.row=18;e.wheel=-1;
    LS_CHECK(ls_desk_input(&e));LS_EQ_INT(LS_TK_WHEEL_UP,s_last_key);
    LS_EQ_INT(34,s_wheel_col);LS_EQ_INT(18,s_wheel_row);
}

LS_CASE(raw_terminal_finger_is_translated_once_and_retains_grid_pixels)
{
    setup();int id=ls_desk_open_app("p25");frame(160,45);
    dk_win_t *w=dk_win(id);tui_rect c;dk_win_rects(w,NULL,&c);
    int x=(c.x+25)*10+3,y=(c.y+12)*17+2;
    ls_tui_touch_remote(c.x+25,c.y+12,x,y,true);frame(160,45);
    ls_tui_touch_remote(c.x+5,c.y+6,x-200,y-102,true);
    for(int i=0;i<8;i++) {
        frame(160,45);int sc,sr,col,row,px,py;
        LS_CHECK(ls_tui_touch_held(&sc,&sr,&col,&row));
        LS_EQ_INT(25,sc);LS_EQ_INT(12,sr);LS_EQ_INT(5,col);LS_EQ_INT(6,row);
        LS_CHECK(ls_tui_touch_px(&px,&py,NULL,NULL,NULL));
        LS_EQ_INT(53,px);LS_EQ_INT(104,py);
        LS_CHECK(ls_tui_touch_input_held(&sc,&sr,&col,&row));
        LS_EQ_INT(c.x+25,sc);LS_EQ_INT(c.y+12,sr);
        LS_EQ_INT(c.x+5,col);LS_EQ_INT(c.y+6,row);
    }
    /* Pixels can change without crossing a cell edge. */
    ls_tui_touch_remote(c.x+5,c.y+6,x-199,y-101,true);frame(160,45);
    int px,py;LS_CHECK(ls_tui_touch_px(&px,&py,NULL,NULL,NULL));
    LS_EQ_INT(54,px);LS_EQ_INT(105,py);
    ls_tui_touch_remote(c.x+5,c.y+6,x-199,y-101,false);frame(160,45);
    LS_CHECK(!ls_tui_touch_held(NULL,NULL,NULL,NULL));
}
