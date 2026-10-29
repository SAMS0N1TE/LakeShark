/* LS_TEST_SOURCES: ls_waterfall.c alone */
/* The LR wide span's channel mark: the LO in the middle, the channel 144 kHz
   up. The marker lands on the channel when it first appears and when it
   moves, and reads back as the channel itself, not its column's middle. */
#include "ls_test.h"
#include "ls_waterfall.h"
#include "ls_tui_screen.h"
#include "esp_timer.h"
#include <string.h>

bool ls_tui_is_wide(void) { return true; }
bool ls_tui_keyboard_mode(void) { return false; }
bool ls_tui_daylight(void) { return false; }
static uint32_t stored;
uint32_t settings_get_waterfall(uint32_t fallback) { return stored ? stored : fallback; }
void settings_set_waterfall(uint32_t value) { stored = value; }

static tui_cell g_back[96 * 30], g_front[96 * 30];
static void draw(void)
{
    tui_surface sf;
    tui_surface_setup(&sf, g_back, g_front, 96, 30);
    tui_frame_begin(&sf);
    ls_wf_draw(&sf, tui_rect_make(0, 0, 96, 30));
}
static void push(uint32_t lo_hz, uint32_t mark_hz)
{
    float bins[LS_WF_BINS_MAX];
    for (int i = 0; i < LS_WF_BINS_MAX; ++i) bins[i] = 0.2f;
    ls_wf_feed_t f = {
        .center_hz = lo_hz, .span_hz = 576000, .floor_db = -100, .top_db = 0,
        .live = true, .note = "wide span", .period_ms = 100, .iq = true, .mark_hz = mark_hz,
    };
    ls_wf_push(LS_WF_OWNER_LORA, bins, LS_WF_BINS_MAX, &f);
    ls_shim_time_advance(100000);
}

LS_CASE(mark_places_the_marker_on_the_channel_and_reads_it_exactly)
{
    ls_wf_claim(LS_WF_OWNER_NONE, NULL);
    ls_wf_claim(LS_WF_OWNER_LORA, "LR IQ");
    push(152456000, 152600000);
    draw();
    LS_EQ_UINT(152600000, ls_wf_marker_hz());
    /* A hop: the span recentres and the marker follows the new channel. */
    for (int i = 0; i < 3; ++i) push(160856000, 161000000);
    draw();
    LS_EQ_UINT(161000000, ls_wf_marker_hz());
}

LS_CASE(no_mark_leaves_an_lr_marker_off)
{
    ls_wf_claim(LS_WF_OWNER_NONE, NULL);
    ls_wf_claim(LS_WF_OWNER_LORA, "LR IQ");
    push(152456000, 0);
    draw();
    LS_EQ_UINT(0, ls_wf_marker_hz());
}
