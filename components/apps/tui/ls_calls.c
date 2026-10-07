#include "ls_calls.h"
#include "call_archive.h"
#include "ls_picker.h"
#include "ls_motion.h"
#include "ls_music_backend.h"
#include <stdio.h>
#include <string.h>
#include <time.h>

static bool opened, replay;
static int selected, top, visible;
static const char *return_receiver;
static tui_rect list_area, controls;
/* Paths are pinned to the rendered rows; an SD refresh cannot redirect a
   tap or a pending delete confirmation to a different call. */
#ifdef ESP_PLATFORM
#include "esp_attr.h"
static EXT_RAM_BSS_ATTR call_entry_t chosen;
static EXT_RAM_BSS_ATTR call_entry_t rows[CALL_RECENT_MAX];
#else
static call_entry_t chosen;
static call_entry_t rows[CALL_RECENT_MAX];
#endif
static int row_count;
static char note[80];

static int opt_get(const ls_opt_t *o) { return call_archive_option(o->arg); }
static void opt_set(const ls_opt_t *o, int v) { call_archive_set_option(o->arg, v); }
static double opt_num(const ls_opt_t *o) { return call_archive_option(o->arg); }
static void opt_set_num(const ls_opt_t *o, double v) { call_archive_set_option(o->arg, (int)v); }
static const char *opt_text(const ls_opt_t *o) { (void)o; return call_archive_talkgroups(); }
static void opt_set_text(const ls_opt_t *o, const char *s)
{
    (void)o;
    if (!call_archive_set_talkgroups(s)) snprintf(note, sizeof(note), "Use up to 32 talkgroups, 1..65535, separated by commas");
}
static void browse(const ls_opt_t *o)
{
    (void)o;
    if (opened) return;
    const char *radio = ls_tui_radio_claimed();
    ls_calls_open(radio && !strcmp(radio, "FM") ? "FM" : "P25");
}
static const ls_opt_t OPT_CALLS[] = {
    {.label = "Recent calls", .kind = LS_OPT_ACTION, .act = browse, .leaves = true},
    {.label = "Record P25", .kind = LS_OPT_TOGGLE, .arg = CALL_OPT_P25, .get = opt_get, .set = opt_set},
    {.label = "Record FM", .kind = LS_OPT_TOGGLE, .arg = CALL_OPT_FM, .get = opt_get, .set = opt_set},
    {.label = "Minimum call (ms)", .kind = LS_OPT_LEVEL, .arg = CALL_OPT_MIN_MS,
     .num = opt_num, .set_num = opt_set_num, .lo = 0, .hi = 10000, .step = 100, .unit = "milliseconds"},
    {.label = "Keep newest days", .kind = LS_OPT_LEVEL, .arg = CALL_OPT_DAYS,
     .num = opt_num, .set_num = opt_set_num, .lo = 0, .hi = 365, .step = 1, .unit = "UTC days; 0 keeps all"},
    {.label = "Only listed talkgroups", .kind = LS_OPT_TOGGLE, .arg = CALL_OPT_FILTER, .get = opt_get, .set = opt_set},
    {.label = "Talkgroup list", .kind = LS_OPT_TEXT, .text = opt_text, .set_text = opt_set_text, .max = 191},
};
const ls_opt_ctx_t ls_calls_options = {.name = "CALLS", .job = -1, .radio = LS_RSEL_NONE, LS_OPT_ROWS(OPT_CALLS)};

void ls_calls_open(const char *receiver)
{
    call_archive_init(); call_archive_refresh();
    return_receiver = receiver; opened = true; selected = top = row_count = 0; note[0] = 0;
}
bool ls_calls_active(void) { return opened; }
static bool stop(bool resume)
{
#ifdef ESP_PLATFORM
    if (replay) {
        ls_music_close();
        if (ls_music_is_open()) { snprintf(note, sizeof(note), "%s", ls_music_error()); return false; }
        if (resume) ls_tui_radio_want(return_receiver);
    }
#else
    (void)resume;
#endif
    replay = false; return true;
}
void ls_calls_leave(void) { stop(false); opened = false; }
static void confirm_delete(int index)
{
    if (index == 1) {
        if (replay && !stop(true)) return;
        snprintf(note, sizeof(note), "%s", call_archive_delete(chosen.path)
            ? "Delete queued; refreshing card" : "Card busy; try again");
    }
}
static void action(int index)
{
    if (index == 4) { if (stop(true)) opened = false; return; }
    if (index == 3) { ls_opt_open(&ls_calls_options); return; }
    if (index == 1) { stop(true); return; }
    if (selected < 0 || selected >= row_count) return;
    chosen = rows[selected];
    if (index == 2) {
        ls_picker_open("DELETE CALL?", confirm_delete);
        ls_picker_add("CANCEL", "Keep this recording");
        ls_picker_add("DELETE", "Remove WAV and metadata");
        ls_picker_select(0); return;
    }
    if (index == 0) {
#ifdef ESP_PLATFORM
        /* MUSIC parks RF before acquiring the decoder, and retains the
           user's codec volume and mute state. Clear the router's claim too. */
        ls_tui_radio_want(NULL);
        replay = ls_music_open();
        if (!replay || !ls_music_play_path(chosen.path)) {
            snprintf(note, sizeof(note), "%s", ls_music_error());
            if (replay) stop(true); else ls_tui_radio_want(return_receiver);
        } else snprintf(note, sizeof(note), "Playing call; STOP returns to receive");
#else
        snprintf(note, sizeof(note), "Playback requires the device audio decoder");
#endif
    }
}
void ls_calls_draw(tui_surface *sf, tui_rect area)
{
    bool busy = call_archive_busy();
    /* Full tiles where there is height for them: two rows of five in
       portrait, a thumb-height row in landscape without a keyboard. */
    int bar_h = ls_tui_is_wide() ? (ls_tui_keyboard_mode() ? 3 : 4)
                                 : 2 * (area.h >= 40 ? 5 : 4);
    if (area.h < bar_h + 9) bar_h = 3;
    controls = tui_rect_make(area.x, area.y + area.h - bar_h, area.w, bar_h);
    tui_rect activity = tui_rect_make(area.x, area.y, area.w, 6);
    tui_box(sf, activity, "CALLS / RECEIVE ARCHIVE", TUI_ATTR(TUI_CYAN, TUI_BLACK));
    ls_motion_busy(sf, activity, busy);
    char text[96];
    snprintf(text, sizeof(text), "%c %s  DROP %u  ERR %u", ls_motion_pip(busy),
        busy ? "RECORDING / WRITING" : "LISTENING", call_archive_drops(), call_archive_errors());
    ls_safe_line(sf, activity, activity.y + 1, text, TUI_ATTR(TUI_CYAN | TUI_BRIGHT, TUI_BLACK));
    unsigned peak = call_archive_peak();
    int width = activity.w - 4;
    if (width > 0) {
        tui_rect meter = tui_rect_make(activity.x + 2, activity.y + 2, width, 1);
        tui_fill(sf, meter, '.', LS_ATTR_FAINT);
        int level = (int)((uint64_t)peak * width / 32768);
        tui_fill(sf, tui_rect_make(meter.x, meter.y, level, 1), '#', TUI_ATTR(TUI_GREEN | TUI_BRIGHT, TUI_BLACK));
        if (busy) {
            int at = ls_motion_phase(width, 1200);
            tui_put_str(sf, meter, meter.x + at, meter.y, ">", TUI_ATTR(TUI_CYAN | TUI_BRIGHT, TUI_BLACK));
        }
    }
    ls_safe_line(sf, activity, activity.y + 3, note[0] ? note : "Clear P25 / squelched FM; newest first", LS_ATTR_DIM);
    snprintf(text, sizeof(text), "P25 %s  FM %s  KEEP %d days (0=all)",
        call_archive_option(CALL_OPT_P25) ? "ON" : "OFF", call_archive_option(CALL_OPT_FM) ? "ON" : "OFF",
        call_archive_option(CALL_OPT_DAYS));
    ls_safe_line(sf, activity, activity.y + 4, text, LS_ATTR_DIM);
    list_area = tui_rect_make(area.x, area.y + 6, area.w, area.h - bar_h - 6);
    tui_box(sf, list_area, "RECENT CALLS", TUI_ATTR(TUI_CYAN, TUI_BLACK));
    /* Restore selection by path when a new call moves the list. */
    char previous[CALL_PATH_MAX] = "";
    if (selected >= 0 && selected < row_count) strcpy(previous, rows[selected].path);
    row_count = call_archive_count();
    for (int i = 0; i < row_count; ++i) {
        if (!call_archive_entry(i, &rows[i])) { row_count = i; break; }
        if (previous[0] && !strcmp(previous, rows[i].path)) selected = i;
    }
    if (selected >= row_count) selected = row_count ? row_count - 1 : 0;
    visible = (list_area.h - 2) / 2; if (visible < 1) visible = 1;
    if (selected < top) top = selected;
    if (selected >= top + visible) top = selected - visible + 1;
    if (!row_count) ls_safe_line(sf, list_area, list_area.y + 1, "No saved calls / card refreshing", LS_ATTR_DIM);
    for (int i = top; i < row_count && i < top + visible; ++i) {
        const call_entry_t *e = &rows[i];
        char stamp[24] = "TIME UNKNOWN";
        time_t t = (time_t)e->meta.time;
        if (t > 0) { struct tm tm; if (call_utc_tm(t, &tm)) strftime(stamp, sizeof(stamp), "%m/%d %H:%M:%S", &tm); }
        char id[24];
        if (e->meta.talkgroup) snprintf(id, sizeof(id), "TG %lu", (unsigned long)e->meta.talkgroup);
        else snprintf(id, sizeof(id), "%.4f MHz", e->meta.hz / 1e6);
        snprintf(text, sizeof(text), "%c %s  %s  %.1fs", i == selected ? '>' : ' ', stamp, id, e->duration_ms / 1000.0);
        int y = list_area.y + 1 + (i - top) * 2;
        uint8_t ink = TUI_ATTR(i == selected ? TUI_CYAN | TUI_BRIGHT : TUI_WHITE, TUI_BLACK);
        ls_safe_line(sf, list_area, y, text, ink);
        snprintf(text, sizeof(text), "  %.4f MHz  SRC %lu%s", e->meta.hz / 1e6, (unsigned long)e->meta.source, e->meta.gps ? " / GPS" : "");
        ls_safe_line(sf, list_area, y + 1, text, LS_ATTR_DIM);
    }
    const ls_btn_t buttons[] = {
        {.label = "PLAY", .key = 'p', .on = replay, .dim = !row_count}, {.label = "STOP", .key = 's', .dim = !replay},
        {.label = "DELETE", .key = 'd', .dim = !row_count}, ls_opt_button(&ls_calls_options),
        {.label = "BACK", .key = 'b'},
    };
    ls_btn_bar_raised(sf, controls, buttons, 5, -1);
}
bool ls_calls_key(ls_tk_t key, char ch)
{
    if (key >= LS_TK_F1) return false;
    if (key == LS_TK_ESC) { action(4); return true; }
    if (key == LS_TK_UP) { if (selected > 0) --selected; return true; }
    if (key == LS_TK_DOWN) { if (selected + 1 < row_count) ++selected; return true; }
    if (key == LS_TK_ENTER) { action(0); return true; }
    if (key == LS_TK_CHAR) {
        if (ch == 'p' || ch == 'P') action(0);
        else if (ch == 's' || ch == 'S') action(1);
        else if (ch == 'd' || ch == 'D') action(2);
        else if (ch == 'o' || ch == 'O') action(3);
        else if (ch == 'b' || ch == 'B') action(4);
        else if (ch == 'r' || ch == 'R') call_archive_refresh();
    }
    return true;
}
bool ls_calls_touch(int col, int row)
{
    if (row >= controls.y && row < controls.y + controls.h) {
        int i = ls_btn_hit(col, row); if (i >= 0 && i < 5) action(i); return true;
    }
    if (col >= list_area.x && col < list_area.x + list_area.w &&
        row > list_area.y && row < list_area.y + list_area.h - 1) {
        int i = top + (row - list_area.y - 1) / 2;
        if (i < row_count) selected = i;
    }
    return true;
}
