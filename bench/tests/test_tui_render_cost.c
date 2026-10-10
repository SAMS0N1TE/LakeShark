/* LS_TEST_SOURCES: ls_tui.c, ls_waterfall.c and scr_p25.c, panel stubbed */

#include "ls_test.h"
#include "ls_tui.h"
#include "ls_theme.h"
#include "ls_tui_screen.h"
#include "ls_font.h"
#include "ls_panel.h"
#include "p25_state.h"
#include "ls_waterfall.h"
#include "ls_tui_ui.h"
#include "ls_sweep_ui.h"
#include "ls_sweep_app.h"
#include "ls_sweep_scope.h"
#include <math.h>
#include "esp_timer.h"

#include <stdio.h>
#include <string.h>

/* ---------------------------------------------------------- panel stub -- */

#define NATIVE_W 568
#define NATIVE_H 1232

static uint16_t g_fb[NATIVE_W * NATIVE_H];

bool ls_panel_fb(ls_panel_fb_t *out)
{
    if (!out) return false;
    memset(out, 0, sizeof(*out));
    out->pixels = g_fb;
    out->width  = NATIVE_W;
    out->height = NATIVE_H;
    return true;
}
void ls_panel_fb_present(void) { }
void ls_panel_fb_present_rows(int y0, int y1) { (void)y0; (void)y1; }

/* ---------------------------------------------------------------- fakes -- */

p25_state_t P25;
scan_state_t SCAN;
uint32_t s_tune_freq_hz;

void p25_get_receiver_status(ls_iq_control_status_t *out)
{
    if (out) memset(out, 0, sizeof(*out));
}
/* A moving spectrum, so every history row differs from the one above it and
   every visible cell genuinely changes. A static spectrum would let the diff
   renderer skip rows and flatter the measurement. */
static int s_phase;

#define BINS 120

static void feed_row(void)
{
    ls_wf_claim(LS_WF_OWNER_P25, "P25");

    float bins[BINS];
    for (int i = 0; i < BINS; i++)
        bins[i] = (float)((i + s_phase) % 9) / 8.0f;
    s_phase++;

    const ls_wf_feed_t f = {
        .center_hz = 851000000u,
        .span_hz = 240000u,
        .floor_db = -110.0f,
        .top_db = -20.0f,
        .live = true,
        .note = NULL,
    };
    ls_wf_push(LS_WF_OWNER_P25, bins, BINS, &f);
}

/* ------------------------------------------------------------- helpers -- */

extern const ls_tui_screen_t ls_scr_p25;

/* Draw the signal page into a pane of the given height and report how many
   cells the renderer actually pushed on a settled frame. */

#define PANE_W 44

static int cells_for_pane(int pane_h)
{
    tui_surface *sf = ls_tui_surface();
    tui_rect area = { 2, 2, PANE_W, pane_h };

    int last = 0;
    for (int i = 0; i < 60; i++) {
        feed_row();
        tui_frame_begin(sf);
        /* The waterfall directly, not the screen that contains it. */

        ls_wf_draw(sf, area);
        last = ls_tui_present();
    }
    return last;
}

static void to_signal_page(void)
{
    ls_scr_p25.key(LS_TK_CHAR, '2');
}

/* ---------------------------------------------------------------- cases -- */

LS_CASE(a_taller_pane_costs_more_but_only_in_proportion)
{
    LS_CHECK(ls_tui_begin(568, 1232));
    to_signal_page();

    /* The spectrum takes a fixed share of the pane and the history takes the
       rest, so past a point every extra row is split between the two and the
       cost climbs with the pane. Two heights sixteen rows apart should differ
       by about sixteen rows of cells, not by a multiple of that. */
    int small = cells_for_pane(24);
    int large = cells_for_pane(40);

    LS_CHECK_MSG(large > small,
                 "a 40-row pane drew %d cells, a 24-row pane drew %d",
                 large, small);

    int growth = large - small;
    LS_CHECK_MSG(growth <= 16 * PANE_W,
                 "sixteen more rows cost %d cells, more than sixteen rows hold",
                 growth);
    ls_tui_end();
}

LS_CASE(the_cost_is_linear_in_the_rows_drawn)
{

    LS_CHECK(ls_tui_begin(568, 1232));
    to_signal_page();

    /* Kept well below the ring depth on purpose: at the depth the history
       saturates by design and the increments stop, which is correct
       behaviour and not the linearity this case is about. */
    static const int H[] = { 20, 28, 36, 44 };
    int c[4];
    for (int i = 0; i < 4; i++) c[i] = cells_for_pane(H[i]);

    /* Each step adds eight rows. The increments should resemble each other:
       if one is more than three times another, the relationship is not
       linear and something is redrawing more than it shows. */
    int d1 = c[1] - c[0], d2 = c[2] - c[1], d3 = c[3] - c[2];
    LS_CHECK_MSG(d1 > 0 && d2 > 0 && d3 > 0,
                 "increments %d, %d, %d - a taller pane must draw more",
                 d1, d2, d3);

    int lo = d1 < d2 ? (d1 < d3 ? d1 : d3) : (d2 < d3 ? d2 : d3);
    int hi = d1 > d2 ? (d1 > d3 ? d1 : d3) : (d2 > d3 ? d2 : d3);
    LS_CHECK_MSG(hi <= lo * 3,
                 "increments %d, %d, %d are not proportional", d1, d2, d3);
    ls_tui_end();
}

LS_CASE(the_waterfall_never_draws_more_cells_than_its_pane_holds)
{
    /* The hard bound. Whatever the ring depth, a frame cannot legitimately
       push more cells than the pane contains, and a frame that does is
       writing somewhere it should not. */
    LS_CHECK(ls_tui_begin(568, 1232));
    to_signal_page();

    static const int H[] = { 20, 30, 40, 50, 60 };
    for (unsigned i = 0; i < sizeof(H) / sizeof(H[0]); i++) {
        int drawn = cells_for_pane(H[i]);
        LS_CHECK_MSG(drawn <= PANE_W * H[i],
                     "a %dx%d pane pushed %d cells", PANE_W, H[i], drawn);
    }
    ls_tui_end();
}

LS_CASE(a_page_with_nothing_moving_costs_almost_nothing)
{
    /* The diff renderer's whole reason for existing. The signal page is the
       wrong place to measure it: its history ring advances every frame by
       design, so every visible row genuinely changes and a low number there
       would mean the waterfall had stopped. The decode page is static text,
       which is what a settled screen actually looks like. */
    LS_CHECK(ls_tui_begin(568, 1232));
    ls_scr_p25.key(LS_TK_LEFT, 0);
    ls_scr_p25.key(LS_TK_LEFT, 0);

    tui_surface *sf = ls_tui_surface();
    tui_rect area = { 2, 2, PANE_W, 40 };

    tui_frame_begin(sf);
    ls_scr_p25.draw(sf, area);
    int first = ls_tui_present();

    tui_frame_begin(sf);
    ls_scr_p25.draw(sf, area);
    int second = ls_tui_present();

    LS_CHECK_MSG(first > 0, "the first frame drew nothing");
    LS_CHECK_MSG(second * 4 < first,
                 "an unchanged frame pushed %d cells against the first %d",
                 second, first);
    ls_tui_end();
}

LS_CASE(settled_radio_and_settings_do_not_repaint_in_either_orientation)
{
    for (int wide = 0; wide < 2; wide++) {
        LS_CHECK(ls_tui_begin(wide ? 1232 : 568, wide ? 568 : 1232));
        int cols, rows;
        ls_tui_geometry(&cols, &rows, NULL, NULL);
        tui_rect area = {1, 5, cols - 2, rows - 6};
        tui_surface *sf = ls_tui_surface();
        for (int settings = 0; settings < 2; settings++) {
            ls_scr_p25.key(LS_TK_CHAR, settings ? '3' : '0');
            tui_frame_begin(sf);
            ls_scr_p25.draw(sf, area);
            ls_tui_present();
            tui_frame_begin(sf);
            ls_scr_p25.draw(sf, area);
            int cells = ls_tui_present();
            LS_CHECK_MSG(cells == 0, "wide=%d settings=%d repainted %d cells",
                         wide, settings, cells);
            if (settings) ls_scr_p25.key(LS_TK_ESC, 0);
        }
        ls_tui_end();
    }
}

LS_CASE(the_landscape_signal_page_stays_within_its_measured_cost)
{

    LS_CHECK(ls_tui_begin(1232, 568));
    to_signal_page();

    tui_surface *sf = ls_tui_surface();
    int cols, rows;
    ls_tui_geometry(&cols, &rows, NULL, NULL);
    tui_rect area = { 1, 2, cols - 2, rows - 3 };

    int last = 0;
    for (int i = 0; i < 60; i++) {
        feed_row();
        tui_frame_begin(sf);
        ls_scr_p25.draw(sf, area);
        last = ls_tui_present();
    }

    LS_CHECK_MSG(last <= 1900,
                 "the signal page now pushes %d cells a frame, was 1701", last);
    LS_CHECK_MSG(last > 900,
                 "the signal page pushes only %d cells - has it stopped?", last);
    ls_tui_end();
}

LS_CASE(bottom_controls_do_not_accept_taps_in_rounded_corners)
{
    ls_tui_set_corner_radius(72);
    LS_CHECK(ls_tui_begin(568, 1232));
    int cols, rows;
    ls_tui_geometry(&cols, &rows, NULL, NULL);
    tui_surface *sf = ls_tui_surface();
    tui_rect bar = {0, rows - 12, cols, 12};
    const ls_btn_t buttons[] = {
        {"ONE", NULL, 'a'}, {"TWO", NULL, 'b'}, {"THREE", NULL, 'c'},
        {"FOUR", NULL, 'd'}, {"FIVE", NULL, 'e'}, {"SIX", NULL, 'f'},
        {"SEVEN", NULL, 'g'}, {"EIGHT", NULL, 'h'}, {"NINE", NULL, 'i'},
        {"TEN", NULL, 'j'}, {"TUNE MARK", NULL, 'y'}
    };
    tui_frame_begin(sf);
    ls_btn_bar_raised(sf, bar, buttons, 11, -1);
    for (int y = bar.y; y < rows; ++y) {
        int pad = ls_tui_corner_pad(y);
        for (int x = 0; x < pad; ++x) {
            LS_EQ_INT(ls_btn_hit(x, y), -1);
            LS_EQ_INT(ls_btn_hit(cols - x - 1, y), -1);
        }
    }
    LS_EQ_INT(ls_btn_shortcut('y', LS_BTN_SLOT_SCREEN), 10);
    ls_tui_end();
    ls_tui_set_corner_radius(40);
}

/* The radio panel shows system volume; this screen test does not run audio. */
int audio_volume_get(void) { return 60; }

/* scr_p25.c gained a profile picker, which reaches the PROGRAM session.  That
   session lives in p25_program_sd.c, which the bench deliberately does not
   link - a host test that really loaded a profile off a card would be lying
   about what it exercised.  NULL here is the same first-run empty state a
   board shows before any profile has been read. */
#include "p25_program.h"
const p25_program_t *p25_program_session(void) { return NULL; }
bool p25_program_request_reload_path(const char *path)
{ (void)path; return false; }

LS_CASE(sweep_real_contacts_have_zero_settled_frame_cost) {
    ls_sweep_init();ls_sweep_clear();ls_sweep_defaults();ls_sweep_start(true);
    int64_t now=esp_timer_get_time();
    for(unsigned i=0;i<15;i++) {
        uint8_t mac[6]={1,2,3,4,5,(uint8_t)i};
        ls_sweep_match_t m={.category=i%4,.label="Receiver contact",.vendor="Unverified"};
        ls_sweep_observe(mac,0,0,-45-(i*11)%45,&m,now);
    }
    uint8_t target[6]={1,2,3,4,5,2};ls_sweep_hunt(target,0,0);
    for(int wide=0;wide<2;wide++) {
        LS_CHECK(ls_tui_begin(wide?1232:568,wide?568:1232));
        ls_sweep_match_t before={.category=SW_DRONE,.label="Receiver contact",.vendor="Unverified"};
        ls_sweep_observe(target,0,0,-67,&before,now);
        int cols,rows;ls_tui_geometry(&cols,&rows,NULL,NULL);
        tui_rect area={1,5,cols-2,rows-6};tui_surface *sf=ls_tui_surface();
        tui_frame_begin(sf);ls_scr_sweep.draw(sf,area);ls_tui_present();
        for(int frame=0;frame<5;frame++) {
            tui_frame_begin(sf);ls_scr_sweep.draw(sf,area);
            int cells=ls_tui_present();
            LS_CHECK_MSG(cells==0,"SWEEP wide=%d unchanged frame pushed %d cells",wide,cells);
        }
        /* A genuine new reading should affect only a small part of the frame. */
        ls_sweep_match_t m={.category=SW_DRONE,.label="Receiver contact",.vendor="Unverified"};
        ls_sweep_observe(target,0,0,-49,&m,now);
        tui_frame_begin(sf);ls_scr_sweep.draw(sf,area);int cells=ls_tui_present();
        LS_CHECK_MSG(cells>0 && cells<cols*rows/4,"SWEEP advert pushed %d cells",cells);
        ls_tui_end();
    }
    ls_sweep_clear();
}

LS_CASE(sweep_category_rows_history_and_hunt_heat_in_both_orientations) {
    int64_t saved=esp_timer_get_time();ls_shim_time_set(120000000);
    const uint8_t hues[]={TUI_YELLOW,TUI_MAGENTA,TUI_CYAN,TUI_GREEN,TUI_RED};
    for(int wide=0;wide<2;wide++) {
        ls_sweep_init();ls_sweep_clear();ls_sweep_defaults();ls_sweep_start(true);
        ls_sweep_settings_t settings;ls_sweep_settings_get(&settings);settings.enabled=31;ls_sweep_settings_set(&settings);
        for(int c=0;c<5;c++) {
            uint8_t mac[6]={1,2,3,4,5,c};ls_sweep_match_t m={.category=c};
            snprintf(m.label,sizeof(m.label),"CATEGORY-%d",c);
            for(int j=0;j<24;j++) ls_sweep_observe(mac,0,0,-95+c*12,&m,120000000-(23-j)*2500000LL);
        }
        LS_CHECK(ls_tui_begin(wide?1232:568,wide?568:1232));
        int cols,rows;ls_tui_geometry(&cols,&rows,NULL,NULL);
        tui_rect a={1,5,cols-2,rows-6};tui_surface *sf=ls_tui_surface();
        for(int daylight=0;daylight<2;daylight++) {
            ls_tui_set_daylight(daylight);tui_frame_begin(sf);ls_scr_sweep.draw(sf,a);ls_tui_present();
            for(int c=0;c<5;c++) {
                bool found=false,history=false;
                for(int y=a.y;y<a.y+a.h;y++) for(int x=a.x;x<a.x+a.w-10;x++) {
                    int at=y*sf->w+x;
                    if(sf->back[at].ch!='C' || sf->back[at+8].ch!='-' || sf->back[at+9].ch!='0'+c) continue;
                    found=true;
                    for(int k=0;k<10;k++) LS_EQ_INT(sf->back[at+k].attr, TUI_ATTR(hues[c]|TUI_BRIGHT,TUI_BLACK));
                    /* Category spine in the same hue at the row's left edge. */
                    bool spine=false;
                    for(int k=a.x;k<x;k++) {
                        tui_cell s=sf->back[y*sf->w+k];
                        spine|=(s.ch==LS_TUI_BLOCK_LEFT || s.ch==LS_TUI_SEXT(0x05)) && s.attr==TUI_ATTR(hues[c]|TUI_BRIGHT,TUI_BLACK);
                    }
                    LS_CHECK_MSG(spine,"wide=%d category=%d spine missing",wide,c);
                    /* History rides beside the row when wide, under its meter otherwise. */
                    for(int hy=y;hy<=y+1 && !history;hy++) for(int k=0;k<a.w;k++) {
                        tui_cell cell=sf->back[hy*sf->w+a.x+k];
                        if(cell.ch>=0x2800 && cell.ch<=0x28ff) {history=true;LS_EQ_INT(cell.attr,TUI_ATTR(hues[c]|TUI_BRIGHT,TUI_BLACK));}
                    }
                }
                LS_CHECK_MSG(found && history,"wide=%d category=%d label/history missing",wide,c);
            }
            tui_frame_begin(sf);ls_scr_sweep.draw(sf,a);LS_EQ_INT(ls_tui_present(),0);
        }
        ls_tui_set_daylight(false);
        ls_scr_sweep.key(LS_TK_CHAR,'h');ls_scr_sweep.key(LS_TK_CHAR,'v');
        tui_frame_begin(sf);ls_scr_sweep.draw(sf,a);ls_tui_present();
        /* HUNT gauge on the strongest contact (-47 dBm): VFD segments with the
           cold end lit bright blue, the top still unlit and faint, the scale
           in all four heat hues, and the house cyan frame around it. */
        int lit_cold=0,ghost=0;bool scale[4]={0},frame=false;
        const uint8_t ramp[4]={TUI_BLUE,TUI_CYAN,TUI_YELLOW,TUI_RED};
        for(int y=a.y;y<a.y+a.h;y++) for(int x=a.x;x<a.x+a.w;x++) {
            tui_cell cell=sf->back[y*sf->w+x];
            if(cell.ch==LS_TUI_BLOCK_FULL || cell.ch==LS_TUI_BLOCK_LEFT) {
                lit_cold+=cell.attr==TUI_ATTR(TUI_BLUE|TUI_BRIGHT,TUI_BLACK);
                ghost+=cell.attr==TUI_ATTR(TUI_BLACK|TUI_BRIGHT,TUI_BLACK);
            }
            if(cell.ch=='-' && x+1<a.x+a.w && sf->back[y*sf->w+x+1].ch>='0' && sf->back[y*sf->w+x+1].ch<='9')
                for(int z=0;z<4;z++) scale[z]|=cell.attr==TUI_ATTR(ramp[z],TUI_BLACK);
            frame|=cell.ch=='+' && cell.attr==TUI_ATTR(TUI_CYAN,TUI_BLACK);
        }
        LS_CHECK_MSG(lit_cold>0 && ghost>0,"wide=%d gauge lit=%d ghost=%d",wide,lit_cold,ghost);
        LS_CHECK(scale[0] && scale[1] && scale[2] && scale[3] && frame);
        tui_frame_begin(sf);ls_scr_sweep.draw(sf,a);LS_EQ_INT(ls_tui_present(),0);
        ls_scr_sweep.key(LS_TK_CHAR,'v');ls_tui_end();
    }
    ls_sweep_clear();ls_shim_time_set(saved);
}
