/* T-Display: the TUI shell, the panel it owns, and its console commands. */
#include "compact_ui.h"
#include "ls_board.h"
#if LS_HAS_COMPACT_UI
#include "ls_panel.h"
#include "ls_keypad.h"
#include "tui/ls_map.h"
#include "ls_keymap.h"
#include "ls_spi.h"
#include "ls_lora.h"
#include "ls_gps.h"
#include "ls_rtc.h"
#include "ls_gauge.h"
/* The nine-axis sensor, for auto-rotate. */
#include "ls_imu.h"
#include "ls_safe_mode.h"
#include "ls_vitals.h"
/* Whether the wall clock is real yet, for the status row's clock. */
#include "ls_time.h"
#include "ls_haptic.h"
#include "ls_mesh.h"
#include "tui/ls_tui.h"
#include "tui/ls_theme.h"
#include "tui/ls_tui_screen.h"
/* Notices from apps that are not the one on screen. */
#include "tui/ls_notify.h"

extern "C" bool ls_scr_mesh_notice(ls_notice_t *out);
extern "C" void ls_scr_fm_show_page(int page);
#include "tui/ls_tui_touch.h"
#include "tui/ls_keyboard.h"
#include "tui/ls_tui_png.h"
#include "tui/ls_app.h"
#include "tui/ls_icons.h"
#include "tui/ls_wordmark.h"
/* For the waterfall's own numbers in 'tui cost'. */
#include "tui/ls_waterfall.h"
#include "tui/ls_wf_source.h"
#include "tui/ls_value.h"
#include "tui/ls_action.h"
#include "tui/ls_userapp.h"
#include "ls_touch.h"
#include "esp_timer.h"
#include "esp_cpu.h"
#include "esp_log.h"
#include "esp_console.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_heap_caps.h"
#include "esp_attr.h"
#include "ls_flash_task.h"
#include "nvs.h"
#include "ls_wifi.h"
#include "tui/ls_wireless.h"
/* No LVGL shell headers: see compact_ui_start. */
extern "C" {
#include "ls_sdcard.h"
#include "settings.h"
#include "display_ctl.h"
#include "ls_nvs_safe.h"
#include "app_registry.h"
/* The audio diagnostic and the test tone: the console is where the
   speaker chain gets tested, because it is the only place that can report
   what every link is set to. */
#include "ls_audio_hw.h"
#include "tone.h"
}
#include <dirent.h>
#include <sys/stat.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>

static int s_rotation;
static bool s_touch_ok;
struct TouchSample { uint16_t x,y; bool pressed; int64_t received; };
static TouchSample s_touch={};
static portMUX_TYPE s_touch_lock=portMUX_INITIALIZER_UNLOCKED;
static TouchSample touch_snapshot()
{
    portENTER_CRITICAL(&s_touch_lock);
    TouchSample sample=s_touch;
    portEXIT_CRITICAL(&s_touch_lock);
    return sample;
}
static void touch_task(void *)
{
    TouchSample sample={};
    for(;;) {
        if(ls_touch_read(&sample.x,&sample.y,&sample.pressed)) {
            sample.received=esp_timer_get_time();
            portENTER_CRITICAL(&s_touch_lock); s_touch=sample; portEXIT_CRITICAL(&s_touch_lock);
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}
/* The TUI takes its touch from the task that already owns the controller, instead of reading it a second time. */

static bool tui_touch_source(int *x, int *y, bool *pressed)
{
    TouchSample sample = touch_snapshot();
    if (esp_timer_get_time() - sample.received > 250000) sample.pressed = false;
    if (sample.pressed) {
        *x = sample.x; *y = sample.y;
        /* A finger on the glass is the auto-dim clock's input. */
        display_ctl_activity();
    }
    *pressed = sample.pressed;
    return true;
}

static esp_err_t save_font(void *ctx)
{
    nvs_handle_t h;
    esp_err_t e=nvs_open("compact_ui",NVS_READWRITE,&h);
    if (e!=ESP_OK) return e;
    e=nvs_set_u8(h,"font",*(uint8_t *)ctx);
    if (e==ESP_OK) e=nvs_commit(h);
    nvs_close(h); return e;
}

static uint8_t load_font_index(void)
{
    nvs_handle_t h;
    uint8_t v = 0;
    if (nvs_open("compact_ui",NVS_READONLY,&h)==ESP_OK) {
        if (nvs_get_u8(h,"font",&v)!=ESP_OK) v = 0;
        nvs_close(h);
    }
    return v;
}

static esp_err_t save_rotation(void *ctx)
{
    nvs_handle_t h;
    esp_err_t e=nvs_open("compact_ui",NVS_READWRITE,&h);
    if (e!=ESP_OK) return e;
    e=nvs_set_u8(h,"rotation",*(int *)ctx);
    if (e==ESP_OK) e=nvs_commit(h);
    nvs_close(h); return e;
}
/* Reading the matrix by hand is the only way to know a key works
   before anything is bound to it, so the probe and the watch live here rather
   than inside whatever consumes the events later. */
/* The TUI proof. It takes the LVGL port lock for its whole run so the
   two never paint at once, draws a screen shaped like a real LakeShark page,
   then reports two numbers that settle the argument: the cost of painting
   every cell, and the cost of a typical edit. LVGL's own figures for the same
   panel are 23 ms of rasterising in portrait and 41 ms in landscape. */
/* Matrix position to router key.

   The keymap gives a key identity; the router wants its own small vocabulary
   so screens never see hardware. Modifier state lives here because it is a
   property of the keyboard, not of any screen, and because a screen that
   tracked shift itself would get it wrong the moment focus moved. */
static bool s_shift, s_caps;

static ls_tk_t tui_key_from_matrix(const ls_keymap_entry_t *k, char *out_ch)
{
    *out_ch = 0;
    if (!k) return LS_TK_NONE;
    switch (k->key) {
    case LS_KEY_UP:        return LS_TK_UP;
    case LS_KEY_DOWN:      return LS_TK_DOWN;
    case LS_KEY_LEFT:      return LS_TK_LEFT;
    case LS_KEY_RIGHT:     return LS_TK_RIGHT;
    case LS_KEY_ENTER:     return LS_TK_ENTER;
    case LS_KEY_ESC:       return LS_TK_ESC;
    case LS_KEY_TAB:       return LS_TK_TAB;
    case LS_KEY_BACKSPACE: return LS_TK_BACKSPACE;

    case LS_KEY_ALT:       return LS_TK_ALT;
    case LS_KEY_CTRL:      return LS_TK_CTRL;
    case LS_KEY_FN:        return LS_TK_FN;
    case LS_KEY_META:      return LS_TK_META;
    case LS_KEY_RECORD:    return LS_TK_MIC;
    case LS_KEY_F1:  return LS_TK_F1;   case LS_KEY_F2:  return LS_TK_F2;
    case LS_KEY_F3:  return LS_TK_F3;   case LS_KEY_F4:  return LS_TK_F4;
    case LS_KEY_F5:  return LS_TK_F5;   case LS_KEY_F6:  return LS_TK_F6;
    case LS_KEY_F7:  return LS_TK_F7;   case LS_KEY_F8:  return LS_TK_F8;
    case LS_KEY_F9:  return LS_TK_F9;   case LS_KEY_F10: return LS_TK_F10;
    case LS_KEY_F11: return LS_TK_F11;
    case LS_KEY_CHAR: {
        char c = ls_keymap_char(k, s_shift, s_caps);
        if (!c) return LS_TK_NONE;
        *out_ch = c;
        return LS_TK_CHAR;
    }
    default: return LS_TK_NONE;
    }
}

#define SPLASH_MS       1500
#define SPLASH_STEP_MS    16
#define SPLASH_HOLD_MS   320
#define SPLASH_FRAMES   (SPLASH_MS / SPLASH_STEP_MS)

/* The wordmark fades up instead of appearing.

   Sixteen colours have no alpha, but they do have a brightness ladder, and
   walking it IS a fade: everything starts deep blue and each row climbs
   until it reaches its own final colour - so the mark blooms from the bottom
   up over the first half of the run and settles into the ramp. Costs
   nothing, because the attribute byte is already in every cell. */
static const uint8_t SPLASH_LADDER[5] = {
    TUI_BLUE, TUI_BLUE | TUI_BRIGHT, TUI_CYAN,
    TUI_CYAN | TUI_BRIGHT, TUI_WHITE | TUI_BRIGHT,
};
#define SPLASH_FADE_PCT 55

/* THE REBUILD GETS A WIPE, NOT THE WHOLE POWER-ON SEQUENCE. */

/* FRAMES, NOT MILLISECONDS, because this animation pays for itself. */

#define WIPE_FRAMES    6
#define WIPE_STEP_MS   2
#define WIPE_EDGE      4

static const uint8_t WIPE_SHARK[5] = {
    0x0C,  /* 0 0 0 0 1 1 0 0 */
    0x1E,  /* 0 0 0 1 1 1 1 0 */
    0x7F,  /* 0 1 1 1 1 1 1 1 */
    0x1E,
    0x0C,
};

static void tui_wipe(tui_surface *sf, int cols, int rows, int frame,
                     int frames)
{
    tui_rect all = tui_surface_rect(sf);
    tui_frame_begin(sf);

    if (frames < 1) frames = 1;

    /* The wave travels along x+y, so its span is the sum of the two sides
       and every cell has a place on it. */
    const int span = cols + rows;
    const int head = span * frame / frames;

    for (int y = 0; y < rows; y++) {
        for (int x = 0; x < cols; x++) {
            const int d = x + y;
            const int behind = head - d;
            if (behind < 0) continue;            /* the wave has not arrived */
            if (behind >= WIPE_EDGE) {           /* and it has gone past */
                tui_put_char(sf, all, x, y, ' ', TUI_ATTR(TUI_WHITE, TUI_BLACK));
                continue;
            }
            /* On the face of it. Densest at the crest, thinning behind, so
               the band has a direction. */
            static const char RAMP[WIPE_EDGE] = {
                LS_TUI_SHADE_FULL, LS_TUI_SHADE_75,
                LS_TUI_SHADE_50,   LS_TUI_SHADE_25,
            };
            static const uint8_t HUE[WIPE_EDGE] = {
                TUI_WHITE | TUI_BRIGHT, TUI_CYAN | TUI_BRIGHT,
                TUI_CYAN, TUI_BLUE | TUI_BRIGHT,
            };
            tui_put_char(sf, all, x, y, RAMP[behind],
                         TUI_ATTR(HUE[behind], TUI_BLACK));
        }
    }

    const int sx = head - rows / 2 - 4;
    const int sy = rows / 2 - 2;
    if (frame < frames - 2) {
        for (int r = 0; r < 5; r++) {
            for (int b = 0; b < 7; b++) {
                if (!(WIPE_SHARK[r] & (0x40 >> b))) continue;
                const int x = sx + b, y = sy + r;
                if (x < 0 || x >= cols || y < 0 || y >= rows) continue;
                tui_put_char(sf, all, x, y, LS_TUI_BLOCK_FULL,
                             TUI_ATTR(TUI_WHITE | TUI_BRIGHT, TUI_BLACK));
            }
        }
    }
}

static void tui_splash(tui_surface *sf, int cols, int rows, int frame,
                       int frames)
{
    tui_rect all = tui_surface_rect(sf);
    tui_frame_begin(sf);

    if (frames < 1) frames = 1;
    const int pct = frame * 100 / frames;

    /* Split before anything is positioned: the assembly's height depends on
       whether the mark needed two lines, and its top on the height. */
    char line_a[32], line_b[32];
    const bool two = ls_wordmark_split("TERMINAL BAY", cols - 2,
                                       line_a, sizeof(line_a),
                                       line_b, sizeof(line_b));
    const int mark_rows = two ? (LS_WORDMARK_ROWS * 2 + 1) : LS_WORDMARK_ROWS;
    const int top = rows / 2 - (mark_rows + 7) / 2;

    for (int r = 0; r < LS_WORDMARK_ROWS; r++) {
        /* Row 0 is the brightest, so its target is the top of the ladder. */
        const int target = LS_WORDMARK_ROWS - 1 - r;
        int lit = (pct >= SPLASH_FADE_PCT)
                ? LS_WORDMARK_ROWS - 1
                : pct * LS_WORDMARK_ROWS / SPLASH_FADE_PCT;
        if (lit > target) lit = target;
        const uint8_t at = TUI_ATTR(SPLASH_LADDER[lit], TUI_BLACK);

        ls_wordmark_row(sf, all, cols / 2 - ls_wordmark_width(line_a) / 2,
                        top, r, line_a, at);
        if (two)
            ls_wordmark_row(sf, all,
                            cols / 2 - ls_wordmark_width(line_b) / 2,
                            top + LS_WORDMARK_ROWS + 1, r, line_b, at);
    }

    const char *sub = "L A K E S H A R K";
    tui_put_str(sf, all, cols / 2 - (int)strlen(sub) / 2, top + mark_rows + 1,
                sub, TUI_ATTR(TUI_WHITE, TUI_BLACK));

    const int rw = cols / 2, rx = cols / 2 - rw / 2;
    int rule = pct >= 20 ? rw : rw * pct / 20;
    if (rule & 1) rule++;
    for (int i = (rw - rule) / 2; i < (rw + rule) / 2; i++) {
        if (i < 0 || i >= rw) continue;
        tui_put_char(sf, all, rx + i, top - 2, LS_TUI_BLOCK_LOWER,
                     TUI_ATTR(TUI_BLUE, TUI_BLACK));
        tui_put_char(sf, all, rx + i, top + mark_rows + 3, LS_TUI_BLOCK_UPPER,
                     TUI_ATTR(TUI_BLUE, TUI_BLACK));
    }

    const int filled = rw * frame / frames;
    for (int i = 0; i < filled; i++) {
        char glyph = (i >= filled - 2 && filled < rw) ? LS_TUI_SHADE_50
                                                      : LS_TUI_SHADE_FULL;
        uint8_t c = i * 3 < rw ? TUI_BLUE | TUI_BRIGHT
                  : i * 3 < rw * 2 ? TUI_CYAN : TUI_CYAN | TUI_BRIGHT;
        tui_put_char(sf, all, rx + i, top + mark_rows + 5, glyph,
                     TUI_ATTR(c, TUI_BLACK));
    }

    static const char *const STAGE[] = { "PANEL", "RADIO", "USB HOST",
                                         "KEYBOARD", "READY" };
    int stage = filled * 5 / (rw ? rw : 1);
    if (stage > 4) stage = 4;
    const char *label = STAGE[stage];
    tui_put_str(sf, all, cols / 2 - (int)strlen(label) / 2,
                top + mark_rows + 7, label,
                TUI_ATTR(stage == 4 ? (TUI_GREEN | TUI_BRIGHT) : TUI_WHITE,
                         TUI_BLACK));
}

static bool tui_live_from(const char *path)
{
    ls_val_t v;
    const char *unit = nullptr;
    if (!ls_value_read(path, &v, &unit)) return false;
    switch (v.kind) {
    case LS_VAL_BOOL:
    case LS_VAL_INT:   return v.i != 0;
    case LS_VAL_FLOAT: return v.f > 0.0f;
    default:           return false;
    }
}
/* A receiver app is live when it HOLDS the receiver, not when it
   happens to be hearing something.

   p25.sync, fm.squelch and adsb.aircraft are all false on a receiver that is
   running and hearing nothing, so the lamp went out the moment the thing it
   was watching went quiet - which is the moment it was worth having. The
   claim survives leaving the screen, so this is also the only one of the two
   that answers "is it still going while I am over here". */
static bool tui_live_claim(const char *mode)
{
    const char *now = ls_tui_radio_claimed();
    return now && mode && strcmp(now, mode) == 0;
}
static bool tui_live_p25(void)  { return tui_live_claim("P25"); }
static bool tui_live_fm(void)   { return tui_live_claim("FM"); }
static bool tui_live_adsb(void) { return tui_live_claim("ADS-B"); }
static bool tui_live_rec(void)  { return tui_live_claim("REC"); }
/* The directory tile's live lamp. mesh.up is a bool published by
   ls_value, so this is the same one-line reader as the others and the tile
   lights whenever the stack is running - which is exactly what "live" means
   for a node that spends most of its time listening. */
static bool tui_live_mesh(void)  { return tui_live_from("mesh.up"); }
/* Lit when there is a fix, not when the receiver is merely talking: the tile is a glance and a fix is the thing worth glancing at. */

static bool tui_live_gps(void)   { return tui_live_from("gps.on"); }

/* The TUI holds the screen until it is told to stop. */

static TaskHandle_t     s_tui_task;
static volatile bool    s_tui_stop;
/* A rotation the running session has been asked for. The session cannot turn
   the screen itself - it holds the LVGL port lock and takes its grid size at
   begin - so it retires and hands the request to its own exit path. */
static volatile int     s_tui_rotate_req = -1;

static bool             s_imu_ok;

/* A ring, not one slot. s_tui_inject was a single int, so two keys
   sent inside one 40 ms frame silently overwrote each other - and a console
   preset sends four in a row. Eight is enough for any preset and costs 8
   bytes. Written by the console task, read by the TUI task; single producer,
   single consumer, so the indices need no lock. */
static int64_t s_mic_down_us;   /* when MIC went down */

#define TUI_INJECT_N 8
/* The ring carries the CHARACTER too, not just the key code.
   Screens bind their controls to letters - the mesh screen is S/T/A, the
   waterfall is V/H - and `tui key` could only ever send F-keys and arrows.
   So the console, which is the only way to drive this board when the
   detachable keyboard is not attached, could reach none of them. */
typedef struct { ls_tk_t k; char ch; } tui_inject_t;
static volatile tui_inject_t s_tui_inject[TUI_INJECT_N];
static volatile uint8_t s_tui_inject_w = 0;
static volatile uint8_t s_tui_inject_r = 0;

/* A tap, from the console, at a named cell. */

/* A screenshot taken from the console raced the task drawing it. */

static volatile bool s_tui_shot_req = false;

/*'tui png': the pixels, copied between frames for the same reason
   the text dump is taken there. The console task allocates the snapshot and
   waits; this task fills it and says so by clearing the request. */
static volatile bool      s_tui_png_req = false;
static uint16_t *volatile s_tui_png_buf = nullptr;

/* The encode runs on a task of its own with a 16 kB stack in PSRAM. */

struct TuiPngJob {
    const uint16_t *snap;
    int             w, h;
    bool            wide;
    size_t          n;
    uint32_t        crc;
    TaskHandle_t    waiter;
};

static void tui_png_task(void *arg)
{
    TuiPngJob *j = (TuiPngJob *)arg;
    j->n = ls_tui_png_emit(j->snap, j->w, j->h, j->wide,
        [](const char *line, void *) { printf("~%s\n", line); },
        nullptr, &j->crc);
    xTaskNotifyGive(j->waiter);

    vTaskSuspend(nullptr);
}

typedef struct { int16_t col, row; } tui_tap_t;
static volatile tui_tap_t s_tui_tap[TUI_INJECT_N];
static volatile uint8_t   s_tui_tap_w = 0;
static volatile uint8_t   s_tui_tap_r = 0;

/* A font change is a grid change, and the grid is settled at
   ls_tui_begin. So it is the rotation dance without the rotation: stop the
   session, and the exit path starts a fresh one that reads the new size. */
static volatile bool s_tui_regrid_req = false;

static void tui_request_regrid(void)
{
    s_tui_regrid_req = true;
    s_tui_stop = true;
}

/* A manual rotate costs TWO session rebuilds and then loses. */

static ls_imu_pose_t s_manual_pose = LS_IMU_FLAT;
static bool          s_manual_held = false;

/* What a rotation actually costs, kept so the next report is a measurement. */

static int64_t s_rebuild_started;

/* Is this session a REBUILD of the last one, or a cold start? */

static volatile bool s_tui_rebuilding;

static void tui_manual_rotation_taken(void)
{
    s_manual_pose = s_imu_ok ? ls_imu_pose() : LS_IMU_FLAT;
    s_manual_held = true;
}

/* Console calls queue screen changes; only the UI runs lifecycle hooks. */
static int s_tui_screen_req = -1;
/* Flipper mode selection changed the receiver but left HOME on
 * the P4. Resolve the requested app on the UI task, after registration.
 * Keep this separate from radio reconciliation: FALLS may request FM
 * without asking to leave the spectrum screen. */
static int s_tui_mode_req = -1;
static const struct { const char *mode; const char *app; int page; } TUI_MODES[] = {
    { "P25", "p25", -1 }, { "ADS-B", "adsb", -1 },
    { "FM", "fm", 0 }, { "REC", "rec", -1 }, { "POCSAG", "fm", 1 },
};
static bool tui_defer_screen(int index)
{
    if (xTaskGetCurrentTaskHandle() == s_tui_task) return false;
    __atomic_store_n(&s_tui_screen_req, index, __ATOMIC_RELEASE);
    return true;
}

static void tui_request_rotate(void)
{
    tui_manual_rotation_taken();
    s_tui_rotate_req = (s_rotation + 1) % 4;
    s_tui_stop = true;
}

/* A rotation is a number now. It was an LVGL display rotation and a
   resize of the old shell as well, under the port lock, and those two calls
   are what repainted the retired interface on every turn. */
static bool tui_apply_rotation(int quarter)
{
    s_rotation = quarter & 3;
    ls_nvs_call(save_rotation, &s_rotation, 0);
    return true;
}

/* The logical size the grid is built against: the panel's own
   native size, turned by the rotation - which is what lv_disp_get_hor_res
   answered for a software-rotated display, asked of the thing that knows. */
static bool tui_logical_size(int *w, int *h)
{
    ls_panel_fb_t fb;
    if (!ls_panel_fb(&fb)) return false;
    const bool wide = (s_rotation & 1) != 0;
    *w = wide ? fb.height : fb.width;
    *h = wide ? fb.width : fb.height;
    return true;
}

static bool tui_start_session(void);

static int64_t  s_loop_last_us;
static uint32_t s_loop_avg_us;

/* Where a frame's time goes, one smoothed number per phase. */

static uint32_t s_ph_input_us, s_ph_imu_us, s_ph_draw_us, s_ph_present_us,
                s_ph_sleep_us;
static uint32_t s_ph_core_frames[2];

static inline uint32_t ph_avg(uint32_t avg, int64_t dt)
{
    const uint32_t d = dt > 0 ? (uint32_t)dt : 0u;
    return avg ? (avg * 7u + d) / 8u : d;
}

static bool tui_session(void)
{

    /* Before begin, because begin is where the cell size becomes the
       grid and nothing may change it afterwards. */
    ls_tui_set_font_index(load_font_index());
    /* The TUI knows nothing about LVGL now, so hand it the logical size. */
    /* The glass's corner radius, before the grid is laid out against
       it. Board fact in, layout arithmetic inside. */
#ifdef LS_BOARD_LCD_CORNER_R
    ls_tui_set_corner_radius(LS_BOARD_LCD_CORNER_R);
#endif

    static bool s_look_restored;
    if (!s_look_restored) {
        s_look_restored = true;
        ls_tui_set_theme(ls_tui_theme_at(settings_get_theme()));
        ls_tui_set_daylight(settings_get_daylight());
    }
    int lw = 0, lh = 0;
    if (!tui_logical_size(&lw, &lh) || !ls_tui_begin(lw, lh)) {
        printf("tui: could not start\n");
        return false;
    }

    int cols, rows, cw, ch;
    ls_tui_geometry(&cols, &rows, &cw, &ch);
    tui_surface *sf = ls_tui_surface();
    uint32_t us = 0; int drawn = 0;

    /* Registration order is the F-key order and the tab order, and
       HOME is first because boot lands on it. The directory reads the same
       table, so an app cannot be on the tab strip and missing from the
       launcher, or the other way round. */

    if (s_rebuild_started) {
        const uint32_t ms =
            (uint32_t)((esp_timer_get_time() - s_rebuild_started) / 1000);
        s_rebuild_started = 0;
        ls_tui_rebuild_note(ms);
        printf("tui: rebuild %u took %u ms\n",
               (unsigned)ls_tui_rebuild_count(), (unsigned)ms);
    }

    ls_tui_screen_set_dispatch_cb(tui_defer_screen);
    ls_tui_screen_set_rotate_cb(tui_request_rotate);
    ls_tui_screen_set_regrid_cb(tui_request_regrid);
    /* What a consumed tap feels like. TICK is deliberately at the
       edge of perceptible - it fires on every control in the interface, and
       a tick you notice becomes a tick you resent. */
    ls_tui_set_tap_cb([] { ls_haptic_play(LS_HAPTIC_TICK); });
    ls_tui_touch_set_source(s_touch_ok ? tui_touch_source : nullptr);

    extern const ls_tui_screen_t ls_scr_home, ls_scr_p25, ls_scr_fm,
                                 ls_scr_adsb, ls_scr_falls, ls_scr_mesh,
                                 ls_scr_rec, ls_scr_diag, ls_scr_settings,
                                 ls_scr_map, ls_scr_gps, ls_scr_radios, ls_scr_wireless;
    if (ls_app_count() == 0) {
        ls_wireless_set_active(false);
        /* Publish the named values before anything can read them: a user app
           off the card is registered below and draws on the next frame. */
        ls_value_publish_builtin();
        /* ...and the actions beside them: ls_value is what the firmware
           knows, ls_action is what it can be asked to do. A user app binds
           to both by name and to neither by symbol. */
        ls_action_register_builtin();

        static const ls_app_t APPS[] = {
            { "home", "HOME", "directory", LS_ICON_SHARK, TUI_CYAN,
              LS_APP_MAIN, &ls_scr_home, nullptr },
            { "p25",  "P25",  "trunking",  LS_ICON_TOWER, TUI_GREEN,
              LS_APP_MAIN, &ls_scr_p25, tui_live_p25 },
            { "fm",   "FM",   "analogue",  LS_ICON_WAVE,  TUI_YELLOW,
              LS_APP_MAIN, &ls_scr_fm, tui_live_fm },
            { "adsb", "ADSB", "aircraft",  LS_ICON_PLANE, TUI_MAGENTA,
              LS_APP_MAIN, &ls_scr_adsb, tui_live_adsb },
            { "falls","FALLS","spectrum",  LS_ICON_FALLS, TUI_BLUE,
              LS_APP_EXTRA, &ls_scr_falls, nullptr },
            { "mesh", "MESH", "meshcore",  LS_ICON_MESH,  TUI_CYAN,
              LS_APP_EXTRA, &ls_scr_mesh, tui_live_mesh },

            { "rec",  "REC",  "capture",   LS_ICON_RECORD, TUI_RED,
              LS_APP_EXTRA, &ls_scr_rec, tui_live_rec },
            { "diag", "DIAG", "health",    LS_ICON_CHIP,  TUI_WHITE,
              LS_APP_EXTRA, &ls_scr_diag, nullptr },
            { "set",  "SET",  "display",   LS_ICON_GEAR,  TUI_BLUE,
              LS_APP_EXTRA, &ls_scr_settings, nullptr },

            { "map",  "MAP",  "vector tiles", LS_ICON_MAP, TUI_GREEN,
              LS_APP_EXTRA, &ls_scr_map, nullptr },
            { "gps",  "GPS",  "position",  LS_ICON_SAT,   TUI_YELLOW,
              LS_APP_EXTRA, &ls_scr_gps, tui_live_gps },

            { "radios", "RADIOS", "power",  LS_ICON_POWER, TUI_RED,
              LS_APP_EXTRA, &ls_scr_radios, nullptr },
            { "link", "LINK", "Wi-Fi + Bluetooth", LS_ICON_WIRELESS, TUI_CYAN,
              LS_APP_EXTRA, &ls_scr_wireless, nullptr },
        };
        /* LINK was the thirteenth app, beyond the router's old
           twelve-screen limit, and vanished without a startup error. */
        static_assert(sizeof(APPS) / sizeof(APPS[0]) <= LS_TUI_MAX_SCREENS,
                      "Built-in apps exceed the screen registry capacity");
        for (unsigned i = 0; i < sizeof(APPS) / sizeof(APPS[0]); i++) {
            if (ls_app_register(&APPS[i]) < 0)
                ESP_LOGE("tdp_ui", "could not register app %s", APPS[i].id);
        }

        {
            static const char *const TABS[] = { "home", "mesh", "radios", "set" };
            int idx[4], n = 0;
            for (unsigned i = 0; i < sizeof(TABS) / sizeof(TABS[0]); i++) {
                const ls_app_t *a = ls_app_by_id(TABS[i]);
                if (!a || !a->screen) continue;
                const int si = ls_tui_screen_index_of(a->screen);
                if (si >= 0) idx[n++] = si;
            }
            if (n) ls_tui_screen_set_tabs(idx, n);
        }

        /* What each app tells the rest of the unit while it is not
           the one on screen. Installed here because this is the one place
           that already knows which apps this build has - see ls_notify.h. */
        ls_notify_add_probe(ls_scr_mesh_notice);

        /* And what a notice is allowed to DO when one arrives.

           ls_notify keeps ring/vibe in its own statics because the hook reads
           them on the draw path and must not touch NVS there. Nothing loaded
           them until now, so the stored preference was read by nobody and the
           firmware alerted however it was compiled. */
        ls_notify_set_alerts(settings_get_alert_ring(),
                             settings_get_alert_vibe());

        /* User apps last, so they land after the built-ins in the directory
           and can never displace one. A missing card is not an error. */
        const int user = ls_userapp_load_dir("/sdcard/apps");
        if (user) printf("tui: %d user app(s) from /sdcard/apps\n", user);
        if (ls_userapp_last_error())
            printf("tui: user app rejected - %s\n", ls_userapp_last_error());
    }

    ls_tui_invalidate();

    /* A rebuilt session goes straight to the screen.

       Read and cleared here so exactly one session skips it: whatever put
       this task back is done being special, and the next cold start gets its
       splash. The panel still has to be lit - that was the first frame's
       job and it is not part of the animation - and it is lit from the same
       stored brightness either way. */
    const bool rebuilt = s_tui_rebuilding;
    s_tui_rebuilding = false;
    if (rebuilt) {
        display_ctl_reapply();

        for (int frame = 0; frame <= WIPE_FRAMES; frame++) {
            tui_wipe(sf, cols, rows, frame, WIPE_FRAMES);
            ls_tui_present();
            vTaskDelay(pdMS_TO_TICKS(WIPE_STEP_MS));
        }
    }

    for (int frame = 0; !rebuilt && frame <= SPLASH_FRAMES; frame++) {
        tui_splash(sf, cols, rows, frame, SPLASH_FRAMES);
        ls_tui_present();
        if (frame == 0) {
            /* First frame is on the glass; safe to light it now. */
            display_ctl_reapply();
            /* And the chime and the buzz go with it, not before it. */

            /* NOT AFTER A BOOT THAT DID NOT FINISH. */

            /* WHAT ACTUALLY WENT DOWN, found by capturing the panic. */

            const ls_safe_boot_t *sb = ls_safe_boot_result();
            if (sb && sb->faults > 0) {
                printf("tui: last boot failed at '%s' - starting silent\n",
                       ls_safe_stage_name((ls_safe_stage_t)sb->prev_stage));
            } else {
                snd_boot_start(settings_get_boot_sound());
            }
            /* Vibration follows the vibrate preference, not the sound one -
               somebody who silences a device in a pocket has not asked for
               it to stop buzzing, and the reverse is just as true. */
            if (settings_get_alert_vibe()) ls_haptic_play(LS_HAPTIC_CONFIRM);
        }
        vTaskDelay(pdMS_TO_TICKS(SPLASH_STEP_MS));
    }
    vTaskDelay(pdMS_TO_TICKS(SPLASH_HOLD_MS));

    ls_tui_invalidate();
    ls_tui_router_draw(sf);
    int full = ls_tui_present();
    ls_tui_last_cost(&us, &drawn);
    printf("tui: %dx%d cells at %dx%d px\n", cols, rows, cw, ch);
    printf("tui: first paint  %d cells  %lu us\n", full, (unsigned long)us);

    s_imu_ok = (ls_imu_start() == ESP_OK) && ls_imu_present();
    printf("tui: holding the screen - %s to rotate, 'tui off' to exit\n",
           s_imu_ok ? "turn it over or press F11" : "press F11");

    ls_keypad_backlight(true);
    bool kbd_was = ls_keypad_present();
    /* A new session starts its own count: the gap since the last one
       is a rebuild, not a frame. */
    s_loop_last_us = 0;
    s_loop_avg_us = 0;
    s_ph_input_us = s_ph_imu_us = s_ph_draw_us = s_ph_present_us = 0;
    s_ph_sleep_us = 0;
    s_ph_core_frames[0] = s_ph_core_frames[1] = 0;
    while (!s_tui_stop) {
        /* The frame's start, for the phase split. */
        const int64_t ph_start = esp_timer_get_time();
        {
            const int64_t now = ph_start;
            if (s_loop_last_us) {
                const uint32_t dt = (uint32_t)(now - s_loop_last_us);
                s_loop_avg_us = s_loop_avg_us ? (s_loop_avg_us * 7u + dt) / 8u
                                              : dt;
            }
            s_loop_last_us = now;
        }

        ls_vitals_tick(esp_timer_get_time());

        /* The auto-dim clock; it gates itself to five ticks a second. */
        display_ctl_tick();

        /* The keyboard decides the orientation, because attaching it is a
           statement about how the thing is being held. A manual rotate stands
           until the keyboard is attached or removed again.

           ls_keypad_tick is what makes that sentence true. Without
           it presence is whatever it was at boot and this compares a
           constant against itself forever. */
        ls_keypad_tick();
        bool kbd_now = ls_keypad_present();
        if (kbd_now != kbd_was) {
            kbd_was = kbd_now;
            int want = kbd_now ? 1 : 0;
            if (want != s_rotation) { s_tui_rotate_req = want; s_tui_stop = true; }
        }

        /* TURN THE SCREEN TO MATCH THE HAND HOLDING IT. */

        if (!kbd_now && s_imu_ok && settings_get_auto_rotate()) {
            const int64_t ph_imu = esp_timer_get_time();
            const ls_imu_pose_t pose = ls_imu_pose();
            s_ph_imu_us = ph_avg(s_ph_imu_us, esp_timer_get_time() - ph_imu);

            /* A manual rotate stands until the board is moved.

               Without this the block below undoes it on the next pass and
               charges a second teardown for the privilege. The latch clears
               the moment the pose actually changes, so picking the unit up
               and turning it still works immediately - what it will not do
               is argue with a deliberate press. */
            if (s_manual_held && pose != s_manual_pose && pose != LS_IMU_FLAT)
                s_manual_held = false;

            int want = -1;
            switch (pose) {
            case LS_IMU_UP:    want = 0; break;
            case LS_IMU_LEFT:  want = 1; break;
            case LS_IMU_DOWN:  want = 2; break;
            case LS_IMU_RIGHT: want = 3; break;
            default: break;             /* FLAT: leave it where it is */
            }
            if (!s_manual_held && want >= 0 && want != s_rotation) {
                /* Turning the board over is using it. */
                display_ctl_activity();
                s_tui_rotate_req = want;
                s_tui_stop = true;
            }
        }

        ls_keypad_event_t ev;
        while (ls_keypad_read(&ev)) {
            display_ctl_activity();
            const ls_keymap_entry_t *k = ls_keymap_lookup(ev.row, ev.col);
            if (!k) continue;
            if (k->key == LS_KEY_SHIFT) { s_shift = ev.pressed; continue; }

            if (k->key == LS_KEY_RECORD) {
                if (ev.pressed) {
                    s_mic_down_us = esp_timer_get_time();
                } else if (s_mic_down_us) {
                    const int64_t held = esp_timer_get_time() - s_mic_down_us;
                    s_mic_down_us = 0;
                    ls_tui_router_key(held > 500000 ? LS_TK_MIC_HOLD : LS_TK_MIC, 0);
                }
                continue;
            }

            if (!ev.pressed) continue;
            if (k->key == LS_KEY_CAPS) { s_caps = !s_caps; continue; }
            char ch = 0;
            ls_tk_t tk = tui_key_from_matrix(k, &ch);
            if (tk != LS_TK_NONE) ls_tui_router_key(tk, ch);
        }
        /* Touch is polled beside the keyboard and lands in the same router,
           so a screen cannot tell which drove it. */
        ls_tui_touch_t tap;
        if (ls_tui_touch_poll(&tap)) ls_tui_router_touch(tap.col, tap.row);

        /* Between frames, where the grid is one whole frame. */
        if (s_tui_shot_req) {
            ls_tui_dump();
            s_tui_shot_req = false;
        }
        /* The last presented frame is on the glass whole here. */
        if (s_tui_png_req) {
            ls_panel_fb_t fb;
            if (s_tui_png_buf && ls_panel_fb(&fb))
                memcpy(s_tui_png_buf, fb.pixels,
                       (size_t)fb.width * fb.height * sizeof(uint16_t));
            s_tui_png_req = false;
        }

        const int screen = __atomic_exchange_n(&s_tui_screen_req, -1, __ATOMIC_ACQ_REL);
        if (screen >= 0) ls_tui_screen_show(screen);
        const int mode = __atomic_exchange_n(&s_tui_mode_req, -1, __ATOMIC_ACQ_REL);
        if (mode >= 0) {
            const ls_app_t *app = ls_app_by_id(TUI_MODES[mode].app);
            if (app && app->screen) {
                display_ctl_activity();
                ls_tui_screen_show(ls_tui_screen_index_of(app->screen));
                if (TUI_MODES[mode].page >= 0)
                    ls_scr_fm_show_page(TUI_MODES[mode].page);
            }
        }

        /* Injected taps go through the same router entry as real ones, so a
           console tap and a thumb cannot disagree about what a cell does. */
        while (s_tui_tap_r != s_tui_tap_w) {
            display_ctl_activity();
            const int c = s_tui_tap[s_tui_tap_r].col;
            const int r = s_tui_tap[s_tui_tap_r].row;
            s_tui_tap_r = (uint8_t)((s_tui_tap_r + 1) % TUI_INJECT_N);
            ls_tui_router_touch(c, r);
        }

        while (s_tui_inject_r != s_tui_inject_w) {
            display_ctl_activity();
            ls_tk_t k  = s_tui_inject[s_tui_inject_r].k;
            char    ch = s_tui_inject[s_tui_inject_r].ch;
            s_tui_inject_r = (uint8_t)((s_tui_inject_r + 1) % TUI_INJECT_N);
            ls_tui_router_key(k, ch);
        }

        /* One status update per frame, from where the truth lives - the router does not go looking for it. */

        char right[48];
        /* Under Daylight the field says so and names the theme it
           will hand back to. F9 steps that theme without changing a pixel of
           the white screen, and this is where the step shows. Portrait keeps
           its single field, for the reason above. */
        if (ls_tui_is_wide() && ls_tui_daylight())
            snprintf(right, sizeof(right), "%s Daylight (%s)",
                     ls_keypad_present() ? "KBD" : "---",
                     ls_tui_get_theme()->name);
        else if (ls_tui_is_wide())
            snprintf(right, sizeof(right), "%s %s",
                     ls_keypad_present() ? "KBD" : "---",
                     ls_tui_get_theme()->name);
        else
            snprintf(right, sizeof(right), "%s",
                     ls_keypad_present() ? "KBD" : "---");
        ls_tui_status_set(NULL, right);

        {
            char clk[12] = "--:--";
            if (ls_time_is_synced()) {
                const time_t now = time(nullptr);
                struct tm tmv;
                gmtime_r(&now, &tmv);
                snprintf(clk, sizeof(clk), "%02d:%02dZ", tmv.tm_hour, tmv.tm_min);
            }
            ls_tui_status_set_clock(clk);
        }

        const int64_t ph_draw = esp_timer_get_time();
        s_ph_input_us = ph_avg(s_ph_input_us, ph_draw - ph_start);
        ls_tui_router_draw(sf);
        const int64_t ph_present = esp_timer_get_time();
        s_ph_draw_us = ph_avg(s_ph_draw_us, ph_present - ph_draw);
        ls_tui_present();
        const int64_t ph_sleep = esp_timer_get_time();
        s_ph_present_us = ph_avg(s_ph_present_us, ph_sleep - ph_present);
        s_ph_core_frames[esp_cpu_get_core_id() ? 1 : 0]++;

        /* A shorter sleep while a picture is still arriving. */

        /* short keyboard taps must not wait behind the normal
           40 ms idle cadence. The touch sampler updates every 10 ms. */
        vTaskDelay(pdMS_TO_TICKS(ls_keyboard_active() ? 10 :
                                 ls_map_render_busy() ? 5 : 40));
        s_ph_sleep_us = ph_avg(s_ph_sleep_us, esp_timer_get_time() - ph_sleep);
    }

    ls_keypad_backlight(false);
    ls_tui_end();
    display_ctl_reapply();

    /* The old LVGL screen showed through on rotations. took
       one repaint away and the rotation kept causing another; removed the shell it came from, so nothing else draws this panel
       between two sessions. */
    if (s_tui_regrid_req) {
        s_tui_regrid_req = false;
        /* A rebuild, not a power-on. See s_tui_rebuilding. */
        s_tui_rebuilding = true;
        uint8_t idx = (uint8_t)ls_tui_font_index();
        esp_err_t e = ls_nvs_call(save_font, &idx, 0);
        printf("tui: font %s, saved=%s\n", ls_tui_font_label(idx),
               esp_err_to_name(e));
        s_tui_stop = false;
        return true;
    } else if (s_tui_rotate_req >= 0) {
        int quarter = s_tui_rotate_req;
        s_tui_rotate_req = -1;
        s_rebuild_started = esp_timer_get_time();
        /* Likewise. A quarter turn is not a power-on. */
        s_tui_rebuilding = true;
        tui_apply_rotation(quarter);
        printf("tui: %d degrees, %s\n", quarter * 90,
               (quarter % 2) ? "landscape" : "portrait");
        /* Rebuild the grid on the same reserved stack. */
        s_tui_stop = false;
        return true;
    } else {
        printf("tui: stopped - nothing else draws the panel; 'tui' starts it again\n");
    }
    return false;
}

/* enabling C6 moved this stack to RTC RAM (0x5010952c). HOME then
 * spent 39 ms comparing an unchanged grid. Reserve DRAM and reuse one task
 * across rotation/regrid, so neither memory pressure nor overlap with the
 * previous session changes stack placement. A stopped UI sleeps here. */
static DRAM_ATTR StackType_t s_tui_stack[6144 / sizeof(StackType_t)]
    __attribute__((aligned(16)));
static DRAM_ATTR StaticTask_t s_tui_tcb;
static TaskHandle_t s_tui_worker;

static void tui_task(void *arg)
{
    (void)arg;
    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        while (tui_session()) {}
        s_tui_task = nullptr;
    }
}

static bool tui_start_session(void)
{
    if (s_tui_task) return true;

    /* Taking the screen ends the shell's transition fence. */

    app_ui_park_release();

    s_tui_stop = false;
    if (!s_tui_worker) {
        s_tui_worker = ls_flash_task_create_static(tui_task, "ls_tui",
            sizeof(s_tui_stack), nullptr, 4, s_tui_stack, &s_tui_tcb,
            tskNO_AFFINITY);
        if (!s_tui_worker) return false;
    }
    s_tui_task = s_tui_worker;
    xTaskNotifyGive(s_tui_worker);
    return true;
}

static bool tui_rotate_to(int quarter)
{
    bool was_running = (s_tui_task != nullptr);

    if (was_running) {
        s_tui_stop = true;
        /* The session releases the port lock on its way out; waiting for the
           handle to clear is what makes the lock below safe to take. */
        for (int i = 0; i < 100 && s_tui_task; i++) vTaskDelay(pdMS_TO_TICKS(20));
        if (s_tui_task) { printf("tui: session did not stop\n"); return false; }
    }

    s_rebuild_started = esp_timer_get_time();
    if (!tui_apply_rotation(quarter)) { s_rebuild_started = 0; return false; }
    printf("tui: %d degrees, %s\n", s_rotation * 90,
           (s_rotation % 2) ? "landscape" : "portrait");

    if (was_running && !tui_start_session()) {
        printf("tui: could not restart\n");
        return false;
    }
    return true;
}

/* The card, and what is on it.

   A mount that fails has several causes and they need different things done:
   no card in the slot, a card with no readable filesystem, or a board that
   declares no pins. `sd` says which. `sd ls` lists a directory, because the
   first question after "is it mounted" is always "is my file where I think
   it is" - and answering that over a serial link beats pulling the card. */
static int sd_cmd(int argc, char **argv)
{
    if (argc >= 2 && !strcmp(argv[1], "ls")) {
        const char *dir = (argc >= 3) ? argv[2] : LS_SDCARD_MOUNT;
        if (!ls_sdcard_mounted()) { printf("sd: not mounted\n"); return 1; }

        DIR *d = opendir(dir);
        if (!d) { printf("sd: cannot open %s\n", dir); return 1; }

        int n = 0;
        struct dirent *e;
        while ((e = readdir(d)) != NULL) {
            char path[256];
            snprintf(path, sizeof(path), "%s/%s", dir, e->d_name);
            struct stat st;
            if (stat(path, &st) == 0 && S_ISDIR(st.st_mode))
                printf("  %-28s <dir>\n", e->d_name);
            else if (stat(path, &st) == 0)
                printf("  %-28s %lu\n", e->d_name, (unsigned long)st.st_size);
            else
                printf("  %-28s ?\n", e->d_name);
            n++;
        }
        closedir(d);
        printf("sd: %d entries in %s\n", n, dir);
        return 0;
    }

    if (argc >= 2 && !strcmp(argv[1], "mount")) {
        const esp_err_t e = ls_sdcard_mount();
        printf("sd: %s\n", esp_err_to_name(e));
        return e == ESP_OK ? 0 : 1;
    }

    ls_sdcard_diagnostics();
    return 0;
}

/* The audio chain, and the two links that could be wrong.

   No sound, with every layer reporting success. The console cannot hear the
   speaker any more than I can, so this is the shape the answer has to take:
   report what each link is set to, and make each candidate switchable
   without a rebuild. */
static int audio_cmd(int argc, char **argv)
{
#if !LS_HAS_AUDIO
    (void)argc; (void)argv;
    printf("audio: this board has no codec driver\n");
    return 0;
#else
    if (argc == 1) { ls_audio_diag_report(); return 0; }

    if (!strcmp(argv[1], "swap")) {
        const esp_err_t e = ls_audio_diag_swap_pins();
        printf("audio: %s\n", e == ESP_OK ? "re-initialised - try a beep"
                                          : esp_err_to_name(e));
        return 0;
    }
    if (!strcmp(argv[1], "reinit")) {
        const esp_err_t e = ls_audio_diag_reinit();
        printf("audio: %s\n", e == ESP_OK ? "re-initialised" : esp_err_to_name(e));
        return 0;
    }
    if (argc >= 3 && !strcmp(argv[1], "pa")) {
        ls_audio_diag_pa(!strcmp(argv[2], "on"));
        return 0;
    }
    if (!strcmp(argv[1], "tone")) {

        if (!snd_test_start(snd_test_from_name("sweep")))
            printf("audio: the test tone is already playing\n");
        else
            printf("audio: tone started\n");
        return 0;
    }
    printf("audio: 'audio' reports the chain, then swap | reinit | "
           "pa on|off | tone\n");
    return 0;
#endif
}

static int tui_cmd(int argc, char **argv)
{
    if (argc >= 2 && !strcmp(argv[1], "off")) {
        if (!s_tui_task) { printf("tui: not running\n"); return 0; }

        ls_tui_radio_want(nullptr);
        s_tui_stop = true;
        return 0;
    }
    /* Sweep the corner radius against the actual glass.

       The number the grid stands off by was inherited from the LVGL chrome
       and never measured, and measuring it needs an eye on the panel rather
       than a test. A regrid re-runs ls_tui_begin, so this settles it in
       seconds instead of one reflash per trial. */
    if (argc >= 2 && !strcmp(argv[1], "corner")) {
        if (argc >= 3) {
            ls_tui_set_corner_radius(atoi(argv[2]));
            if (s_tui_task) tui_request_regrid();
        }
        int cols = 0, rows = 0, cw = 0, ch = 0;
        ls_tui_geometry(&cols, &rows, &cw, &ch);
        printf("tui: corner radius %d px, grid %dx%d cells of %dx%d\n",
               ls_tui_corner_radius(), cols, rows, cw, ch);
        return 0;
    }
    /* Above the already-running guard on purpose: a theme is a live
       setting, and the common case is changing it while looking at it. */
    if (argc >= 2 && !strcmp(argv[1], "theme")) {

        if (argc >= 3) {
            char want[48] = { 0 };
            for (int i = 2; i < argc; i++) {
                if (want[0])
                    strncat(want, " ", sizeof(want) - strlen(want) - 1);
                strncat(want, argv[i], sizeof(want) - strlen(want) - 1);
            }
            const ls_tui_theme_t *t = ls_tui_theme_by_name(want);
            if (t) {
                ls_tui_set_theme(t);
                printf("tui: theme %s - %s\n", t->name, t->desc);
                return 0;
            }
            printf("tui: no theme matches '%s'\n", want);
        }
        printf("tui: themes are\n");
        for (int i = 0; i < ls_tui_theme_count(); i++) {
            const ls_tui_theme_t *t = ls_tui_theme_at(i);
            if (t) printf("  %-14s %s\n", t->name, t->desc);
        }
        /* Daylight is not in that list, on purpose; say where it is. */
        printf("  daylight is laid over any of these: 'tui daylight on|off'\n");
        return 0;
    }
    /* Daylight from the bench, above the running guard like the
       theme and for the same reason. It makes the same two calls the SET row
       makes - apply, then store - so the console and the box cannot disagree
       about what the switch does, and a reboot keeps what was set here.

       It reports everything whether or not it changed anything: what the
       glass is drawing, which theme is underneath and comes back, and
       whether there is a session for any of it to show on. */
    if (argc >= 2 && !strcmp(argv[1], "daylight")) {
        int rc = 0;
        if (argc >= 3 && !strcmp(argv[2], "on")) {
            ls_tui_set_daylight(true);
            settings_set_daylight(true);
        } else if (argc >= 3 && !strcmp(argv[2], "off")) {
            ls_tui_set_daylight(false);
            settings_set_daylight(false);
        } else if (argc >= 3) {
            printf("tui: daylight takes on or off, not '%s'\n", argv[2]);
            rc = 1;
        }
        const ls_tui_theme_t *chosen = ls_tui_get_theme();
        const ls_tui_theme_t *drawn = ls_tui_active_theme();
        printf("tui: daylight %s - drawing %s, %s underneath%s\n",
               ls_tui_daylight() ? "on" : "off",
               drawn ? drawn->name : "?", chosen ? chosen->name : "?",
               s_tui_task ? "" : " - no session running, it shows at the next");
        return rc;
    }
    if (argc >= 4 && !strcmp(argv[1], "tap")) {
        if (!s_tui_task) { printf("tui: not running\n"); return 0; }
        int cols = 0, rows = 0;
        ls_tui_geometry(&cols, &rows, nullptr, nullptr);
        const int c = atoi(argv[2]), r = atoi(argv[3]);
        if (c < 0 || r < 0 || c >= cols || r >= rows) {
            printf("tui: (%d,%d) is outside the %dx%d grid\n", c, r, cols, rows);
            return 1;
        }
        const uint8_t next = (uint8_t)((s_tui_tap_w + 1) % TUI_INJECT_N);
        if (next == s_tui_tap_r) { printf("tui: tap queue full\n"); return 1; }
        s_tui_tap[s_tui_tap_w].col = (int16_t)c;
        s_tui_tap[s_tui_tap_w].row = (int16_t)r;
        s_tui_tap_w = next;
        printf("tui: tapped (%d,%d)\n", c, r);
        return 0;
    }

    if (argc >= 3 && !strcmp(argv[1], "key")) {
        if (!s_tui_task) { printf("tui: not running\n"); return 0; }
        ls_tk_t k = LS_TK_NONE;
        if (argv[2][0] == 'f' && argv[2][1]) {
            int n = atoi(argv[2] + 1);
            if (n >= 1 && n <= 11) k = (ls_tk_t)(LS_TK_F1 + n - 1);
        } else if (!strcmp(argv[2], "esc"))   k = LS_TK_ESC;
        else if (!strcmp(argv[2], "tab"))     k = LS_TK_TAB;
        else if (!strcmp(argv[2], "enter"))   k = LS_TK_ENTER;
        else if (!strcmp(argv[2], "up"))      k = LS_TK_UP;
        else if (!strcmp(argv[2], "down"))    k = LS_TK_DOWN;
        else if (!strcmp(argv[2], "left"))    k = LS_TK_LEFT;
        else if (!strcmp(argv[2], "right"))   k = LS_TK_RIGHT;
        /* Named because the shell eats a bare space and a bare
           backspace cannot be typed into a command line at all. The
           on-screen keyboard in lsconsole sends both by name. */
        else if (!strcmp(argv[2], "backspace")) k = LS_TK_BACKSPACE;
        /* The five that were only ever on the hardware. Naming them
           here is what lets the GUI keyboard drive them too - and the GUI is
           the only keyboard available while the USB cable is attached. */
        else if (!strcmp(argv[2], "alt"))     k = LS_TK_ALT;
        else if (!strcmp(argv[2], "ctrl"))    k = LS_TK_CTRL;
        else if (!strcmp(argv[2], "fn"))      k = LS_TK_FN;
        else if (!strcmp(argv[2], "meta"))    k = LS_TK_META;
        else if (!strcmp(argv[2], "lilygo"))  k = LS_TK_META;
        else if (!strcmp(argv[2], "mic"))     k = LS_TK_MIC;
        else if (!strcmp(argv[2], "michold")) k = LS_TK_MIC_HOLD;
        else if (!strcmp(argv[2], "space"))   { k = LS_TK_CHAR; }
        /* Anything that is a single printable character is sent as
           one - that is how a screen's own shortcuts are reached. Checked
           last so a named key never loses to a one-letter spelling of it. */
        char ch = 0;
        if (k == LS_TK_CHAR && !strcmp(argv[2], "space")) ch = ' ';
        if (k == LS_TK_NONE && argv[2][0] && !argv[2][1] &&
            argv[2][0] > 0x20 && argv[2][0] < 0x7F) {
            k  = LS_TK_CHAR;
            ch = argv[2][0];
        }
        if (k == LS_TK_NONE) {
            printf("tui: key is f1..f11, esc, tab, enter, up, down, left, "
                   "right, or one character for a screen's own shortcut\n");
            return 0;
        }
        uint8_t next = (uint8_t)((s_tui_inject_w + 1) % TUI_INJECT_N);
        if (next == s_tui_inject_r) { printf("tui: key queue full\n"); return 1; }
        s_tui_inject[s_tui_inject_w].k  = k;
        s_tui_inject[s_tui_inject_w].ch = ch;
        s_tui_inject_w = next;

        vTaskDelay(pdMS_TO_TICKS(60));
        const char *scr = ls_tui_screen_name(ls_tui_screen_current());
        printf("tui: key %s -> screen %d %s\n", argv[2],
               ls_tui_screen_current(), scr ? scr : "?");
        return 0;
    }
    if (argc >= 2 && !strcmp(argv[1], "cost")) {
        /* What the last frame cost, on whatever screen is up.

           The boot line reports one full paint of the directory and nothing
           after it, so every later question - what does the waterfall cost,
           what does a settled screen cost - needed a screen that happened to
           print it. DIAG did; the others did not, which meant measuring the
           waterfall meant leaving the waterfall.

           Cells are the transferable half: the renderer is linear in them and
           the number is the same on any machine. The microseconds are this
           board's, which is the half that needed the board. */
        uint32_t us = 0;
        int cells = 0;
        int cols = 0, rows = 0;
        ls_tui_last_cost(&us, &cells);
        ls_tui_geometry(&cols, &rows, nullptr, nullptr);
        const char *scr = ls_tui_screen_name(ls_tui_screen_current());
        printf("tui: %s  %d cells of %d  %lu us\n", scr ? scr : "?",
               cells, cols * rows, (unsigned long)us);
        /* And how often the loop actually comes round, which is what
           "slow" means to somebody watching it. */
        printf("tui: a frame every %lu.%lu ms\n",
               (unsigned long)(s_loop_avg_us / 1000u),
               (unsigned long)((s_loop_avg_us % 1000u) / 100u));
        /* And where that time goes. */
        printf("tui: frame split  input %lu us (imu %lu)  draw %lu  present %lu  "
               "sleep %lu\n",
               (unsigned long)s_ph_input_us, (unsigned long)s_ph_imu_us,
               (unsigned long)s_ph_draw_us, (unsigned long)s_ph_present_us,
               (unsigned long)s_ph_sleep_us);
        printf("tui: frames on core 0 %lu, core 1 %lu since the last look\n",
               (unsigned long)s_ph_core_frames[0],
               (unsigned long)s_ph_core_frames[1]);
        s_ph_core_frames[0] = s_ph_core_frames[1] = 0;
        if (ls_lora_scanning()) {
            ls_lora_scan_prof_t p;
            ls_lora_scan_profile(&p);
            printf("tui: lora sweep, a bin  standby %lu  tune %lu  rx %lu  "
                   "settle %lu  rssi %lu us\n",
                   (unsigned long)p.standby_us, (unsigned long)p.tune_us,
                   (unsigned long)p.rx_us, (unsigned long)p.settle_us,
                   (unsigned long)p.rssi_us);
        }
        if (ls_wf_owner() != LS_WF_OWNER_NONE) {
            ls_wf_stats_t w;
            ls_wf_stats(&w);
            printf("tui: waterfall %s  %dx%d  a row every %lu ms  draw %lu us  "
                   "%lu dropped\n", ls_wf_source_name(), (int)w.bins,
                   (int)w.rows, (unsigned long)w.row_ms,
                   (unsigned long)w.draw_us, (unsigned long)w.dropped);
        }
        return 0;
    }
    if (argc >= 2 && !strcmp(argv[1], "shot")) {

        if (!s_tui_task) { ls_tui_dump(); return 0; }

        s_tui_shot_req = true;
        for (int i = 0; i < 100 && s_tui_shot_req; i++)
            vTaskDelay(pdMS_TO_TICKS(20));
        if (s_tui_shot_req) {
            s_tui_shot_req = false;
            printf("tui: the screen task did not answer - is it wedged?\n");
            return 1;
        }
        return 0;
    }
    /* The glass, as a PNG. See ls_tui_png.h. Framed by a begin and an
       end line, every data line starts with '~' so a log line from another
       task landing in the middle cannot be mistaken for part of the image,
       and the end line carries the size and CRC-32 for the receiver. */
    if (argc >= 2 && !strcmp(argv[1], "png")) {
        if (!s_tui_task) { printf("tui: not running - nothing on the glass\n"); return 1; }
        ls_panel_fb_t fb;
        if (!ls_panel_fb(&fb)) { printf("tui: no framebuffer\n"); return 1; }
        const size_t bytes = (size_t)fb.width * fb.height * sizeof(uint16_t);
        uint16_t *snap = (uint16_t *)heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM);
        if (!snap) { printf("tui: no PSRAM for a %u byte snapshot\n", (unsigned)bytes); return 1; }
        s_tui_png_buf = snap;
        s_tui_png_req = true;
        for (int i = 0; i < 100 && s_tui_png_req; i++) vTaskDelay(pdMS_TO_TICKS(20));
        if (s_tui_png_req) {
            /* The task may yet be copying into it, so it is not freed. */
            s_tui_png_req = false;
            printf("tui: the screen task did not answer - is it wedged?\n");
            return 1;
        }
        s_tui_png_buf = nullptr;
        const bool wide = (s_rotation & 1) != 0;
        const char *name = argc >= 3 ? argv[2] : "shot";

        /* The job lives on the heap, not this stack: if the wait below ever
           gives up, the encoder must not be left writing into a dead frame. */
        const size_t stack_bytes = 16 * 1024;
        TuiPngJob *job = (TuiPngJob *)heap_caps_calloc(1, sizeof(TuiPngJob),
                                                       MALLOC_CAP_SPIRAM);
        StackType_t *stack = (StackType_t *)heap_caps_malloc(
            stack_bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        StaticTask_t *tcb = (StaticTask_t *)heap_caps_malloc(
            sizeof(StaticTask_t), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        if (!job || !stack || !tcb) {
            heap_caps_free(job); heap_caps_free(stack); heap_caps_free(tcb);
            heap_caps_free(snap);
            printf("tui: no memory for the encoder\n");
            return 1;
        }
        job->snap = snap; job->w = fb.width; job->h = fb.height;
        job->wide = wide; job->waiter = xTaskGetCurrentTaskHandle();

        printf("tui: png begin %s %dx%d\n", name,
               wide ? fb.height : fb.width, wide ? fb.width : fb.height);
        (void)ulTaskNotifyTake(pdTRUE, 0);
        TaskHandle_t enc = xTaskCreateStaticPinnedToCore(
            tui_png_task, "tui_png", stack_bytes / sizeof(StackType_t), job, 3,
            stack, tcb, tskNO_AFFINITY);
        if (!enc) {
            heap_caps_free(job); heap_caps_free(stack); heap_caps_free(tcb);
            heap_caps_free(snap);
            printf("tui: could not start the encoder\n");
            return 1;
        }
        if (!ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(180000))) {
            /* Still running on them, so none of its memory is freed. */
            printf("tui: png end %s 0 bytes crc 00000000 - the encoder did not finish\n", name);
            return 1;
        }
        for (int i = 0; i < 100 && eTaskGetState(enc) != eSuspended; i++)
            vTaskDelay(pdMS_TO_TICKS(2));
        vTaskDelete(enc);
        const size_t n = job->n;
        const uint32_t crc = job->crc;
        heap_caps_free(stack); heap_caps_free(tcb); heap_caps_free(job);
        heap_caps_free(snap);
        printf("tui: png end %s %u bytes crc %08lx\n", name, (unsigned)n,
               (unsigned long)crc);
        return n ? 0 : 1;
    }
    if (argc >= 2 && !strcmp(argv[1], "rotate")) {
        tui_manual_rotation_taken();
        tui_rotate_to((s_rotation + 1) % 4);
        return 0;
    }
    if (argc >= 2) {
        char *end;
        long deg = strtol(argv[1], &end, 10);
        if (*argv[1] && !*end && deg >= 0 && deg <= 270 && deg % 90 == 0) {
            tui_manual_rotation_taken();
            tui_rotate_to((int)(deg / 90));
            return 0;
        }
    }
    if (s_tui_task) { printf("tui: already running\n"); return 0; }
    if (argc >= 2 && !strcmp(argv[1], "flip")) {
        printf("tui: flip is not available - the blitter writes the clockwise "
               "transform only, and inverting just the touch mapping made "
               "taps land mirrored. Use 'tui rotate' to turn the screen.\n");
        return 0;
    }
    if (!tui_start_session()) { printf("tui: could not create task\n"); return 1; }
    return 0;
}
/* Bring the SPI bus up and say what happened. Until a radio driver
   exists this is the only way to know the bus works, and proving the wires
   before writing a driver against them is the cheaper order. */

static int call_cmd(int argc, char **argv)
{
    if (argc < 2) {
        printf("actions (%d):\n", ls_action_count());
        for (int i = 0; i < ls_action_count(); i++)
            printf("  %-16s (%-4s) %-12s %s\n",
                   ls_action_name(i),
                   ls_action_sig(i)[0] ? ls_action_sig(i) : "-",
                   ls_cap_str(ls_action_needs(i)),
                   ls_action_help(i) ? ls_action_help(i) : "");
        return 0;
    }
    int idx = -1;
    for (int i = 0; i < ls_action_count(); i++)
        if (!strcmp(ls_action_name(i), argv[1])) { idx = i; break; }
    if (idx < 0) { printf("call: %s\n", ls_act_status_str(LS_ACT_UNKNOWN)); return 1; }

    ls_args_t args = {};
    const char *sig = ls_action_sig(idx);
    int want = (int)strlen(sig);
    if (argc - 2 != want) {
        printf("call: %s takes %d argument%s (%s)\n",
               argv[1], want, want == 1 ? "" : "s", want ? sig : "none");
        return 1;
    }
    for (int i = 0; i < want; i++) {
        if (!ls_action_parse_arg(idx, i, argv[2 + i], &args.v[i])) {
            printf("call: argument %d is not a '%c'\n", i + 1, sig[i]);
            return 1;
        }
    }
    args.n = want;

    ls_val_t out;
    ls_act_status_t st = ls_action_call(argv[1], &args, &out,
        (ls_cap_t)(LS_CAP_READ|LS_CAP_TUNE|LS_CAP_UI|LS_CAP_STORE|LS_CAP_POWER|LS_CAP_TX));
    if (st != LS_ACT_OK) { printf("call: %s\n", ls_act_status_str(st)); return 1; }

    switch (out.kind) {
        case LS_VAL_INT:   printf("%s = %ld\n",  argv[1], out.i); break;
        case LS_VAL_FLOAT: printf("%s = %.4f\n", argv[1], out.f); break;
        case LS_VAL_BOOL:  printf("%s = %s\n",   argv[1], out.i ? "true" : "false"); break;
        case LS_VAL_TEXT:  printf("%s = %s\n",   argv[1], out.s ? out.s : "-"); break;
        default:           printf("%s ok\n",     argv[1]); break;
    }
    return 0;
}
static int spi_cmd(int argc, char **argv)
{
    if (argc >= 2 && !strcmp(argv[1], "up")) {
        esp_err_t err = ls_spi_bus(LS_SPI_RADIO);
        printf("spi: bus up: %s\n", esp_err_to_name(err));
    }
    ls_spi_diagnostics();
    return 0;
}
/* The console face of the packet path. `lora` on its own still just
   reports what answered - that is the liveness check and it stays. The rest
   is deliberately explicit about which half of the radio it touched: a
   receiver that is configured but not listening looks identical to one that
   is listening and hearing nothing, and telling those apart from the far end
   of a serial cable is most of the work. */
static int lora_cmd(int argc, char **argv)
{
    /* One owner at a time. */

    if (argc >= 2 && ls_mesh_running() &&
        (!strcmp(argv[1], "rx") || !strcmp(argv[1], "tx") || !strcmp(argv[1], "config"))) {
        printf("lora: MeshCore owns the radio - 'mesh stop' first, "
               "or use 'mesh' to see what it is hearing\n");
        /*'lora scan' is deliberately NOT in that list. It borrows
           the radio through ls_mesh_radio_hold and puts it back, which is
           the whole point of there being a lease - a survey of the band you
           are meshing in is most useful while you are meshing in it. */
        return 1;
    }

    if (argc >= 2 && !strcmp(argv[1], "scan")) {
        uint32_t lo = 902000000u, hi = 928000000u;
        if (argc >= 4) {
            lo = (uint32_t)(atof(argv[2]) * 1e6 + 0.5);
            hi = (uint32_t)(atof(argv[3]) * 1e6 + 0.5);
        }
        int bins = argc >= 5 ? atoi(argv[4]) : 32;
        if (bins < 2) bins = 2;
        if (bins > 64) bins = 64;

        /* The mesh first, and loudly if it will not let go: a sweep that
           silently retuned the radio under a running node would look like
           the node had died. */
        if (!ls_mesh_radio_hold(true)) {
            ls_mesh_radio_hold(false);
            printf("lora: the mesh would not release the radio - a transmit "
                   "may be in flight; try again\n");
            return 1;
        }
        if (!ls_lora_cfg()) {
            ls_lora_cfg_t cfg; ls_lora_cfg_default(&cfg);
            ls_lora_configure(&cfg);
        }
        esp_err_t err = ls_lora_scan_begin(lo, hi);
        if (err != ESP_OK) {
            ls_mesh_radio_hold(false);
            printf("lora: scan %s\n", esp_err_to_name(err));
            return 1;
        }

        static float dbm[64];
        const int64_t t0 = esp_timer_get_time();
        const int got = ls_lora_scan_sweep(dbm, bins);
        const int64_t us = esp_timer_get_time() - t0;

        printf("lora: %.3f-%.3f MHz, %d bins, one pass in %lld us "
               "(%lld us a bin)\n",
               lo / 1e6, hi / 1e6, got, (long long)us,
               (long long)(got ? us / got : 0));
        float lo_db = 0, hi_db = -200;
        for (int i = 0; i < got; i++) {
            const uint32_t f = ls_lora_scan_bin_hz(lo, hi, bins, i);
            if (i == 0 || dbm[i] < lo_db) lo_db = dbm[i];
            if (dbm[i] > hi_db) hi_db = dbm[i];
            printf("  %9.4f MHz  %6.1f dBm\n", f / 1e6, (double)dbm[i]);
        }
        if (got) printf("lora: floor %.1f dBm, peak %.1f dBm\n",
                        (double)lo_db, (double)hi_db);
        ls_lora_scan_prof_t pr;
        ls_lora_scan_profile(&pr);
        printf("lora: per bin - standby %lu, tune %lu, rx %lu, settle %lu, "
               "rssi %lu us\n",
               (unsigned long)pr.standby_us, (unsigned long)pr.tune_us,
               (unsigned long)pr.rx_us, (unsigned long)pr.settle_us,
               (unsigned long)pr.rssi_us);

        ls_lora_scan_end();
        ls_mesh_radio_hold(false);
        return 0;
    }
    if (argc >= 2 && !strcmp(argv[1], "config")) {
        ls_lora_cfg_t cfg;
        const ls_lora_cfg_t *cur = ls_lora_cfg();
        if (cur) cfg = *cur; else ls_lora_cfg_default(&cfg);
        /* lora config [freq_mhz [sf [bw_khz [cr [dbm]]]]] */
        if (argc >= 3) cfg.freq_hz   = (uint32_t)(atof(argv[2]) * 1e6 + 0.5);
        if (argc >= 4) {
            cfg.sf = (uint8_t)atoi(argv[3]);
            /* The preamble follows the spreading factor, because
               MeshCore's does. Changing sf and leaving the preamble behind
               produces a link that transmits and is never heard. */
            cfg.preamble = (cfg.sf <= 8) ? 32 : 16;
        }
        if (argc >= 5) cfg.bw_hz     = (uint32_t)(atof(argv[4]) * 1000.0 + 0.5);
        if (argc >= 6) cfg.cr        = (uint8_t)atoi(argv[5]);
        if (argc >= 7) cfg.power_dbm = (int8_t)atoi(argv[6]);
        esp_err_t err = ls_lora_configure(&cfg);
        printf("lora: configure %s\n", esp_err_to_name(err));
        if (err != ESP_OK) return 1;
        const ls_lora_cfg_t *c = ls_lora_cfg();
        printf("lora: %.4f MHz SF%u BW%lu Hz CR4/%u %+d dBm sync 0x%02X "
               "airtime(16B)=%lu ms\n",
               c->freq_hz / 1e6, (unsigned)c->sf, (unsigned long)c->bw_hz,
               (unsigned)c->cr, (int)c->power_dbm, (unsigned)c->sync_word,
               (unsigned long)ls_lora_airtime_ms(16));
        return 0;
    }
    if (argc >= 2 && !strcmp(argv[1], "rx")) {
        if (!ls_lora_cfg()) {
            ls_lora_cfg_t cfg; ls_lora_cfg_default(&cfg);
            esp_err_t e = ls_lora_configure(&cfg);
            if (e != ESP_OK) { printf("lora: configure %s\n", esp_err_to_name(e)); return 1; }
        }
        esp_err_t err = ls_lora_receive();
        printf("lora: receive %s\n", esp_err_to_name(err));
        if (err != ESP_OK) return 1;

        int secs = argc >= 3 ? atoi(argv[2]) : 10;
        if (secs < 1) secs = 1;
        if (secs > 120) secs = 120;
        printf("lora: listening %d s...\n", secs);
        int64_t end = esp_timer_get_time() + (int64_t)secs * 1000000;
        unsigned got = 0, bad = 0;
        uint8_t buf[256];
        while (esp_timer_get_time() < end) {
            float rssi = 0, snr = 0;
            int n = ls_lora_poll(buf, sizeof(buf), &rssi, &snr);
            if (n > 0) {
                got++;
                printf("lora: rx %d bytes rssi=%.1f dBm snr=%.1f dB\n", n, rssi, snr);
                int show = n > 32 ? 32 : n;
                printf("      ");
                for (int i = 0; i < show; i++) printf("%02X ", buf[i]);
                printf("%s\n", n > show ? "..." : "");
            } else if (n < 0) {
                bad++;
            }
            vTaskDelay(pdMS_TO_TICKS(5));
        }
        float floor_dbm = 0;
        ls_lora_rssi_inst(&floor_dbm);
        printf("lora: %u packet%s, %u crc/header error%s, floor %.1f dBm\n",
               got, got == 1 ? "" : "s", bad, bad == 1 ? "" : "s", floor_dbm);
        return 0;
    }
    if (argc >= 2 && !strcmp(argv[1], "tx")) {
        /* Deliberately NOT wired to the transmit broker yet, and deliberately
           not silent about that: this emits. It is here so the packet path
           can be proved against a second radio, and it must be behind the
           broker before anything ships that uses it. */
        if (!ls_lora_cfg()) {
            ls_lora_cfg_t cfg; ls_lora_cfg_default(&cfg);
            esp_err_t e = ls_lora_configure(&cfg);
            if (e != ESP_OK) { printf("lora: configure %s\n", esp_err_to_name(e)); return 1; }
        }
        const char *text = argc >= 3 ? argv[2] : "LakeShark";
        size_t len = strlen(text);
        esp_err_t err = ls_lora_send((const uint8_t *)text, len);
        printf("lora: send %s (%u bytes, %lu ms air)\n", esp_err_to_name(err),
               (unsigned)len, (unsigned long)ls_lora_airtime_ms((int)len));
        if (err != ESP_OK) return 1;
        int64_t end = esp_timer_get_time() + 10LL * 1000 * 1000;
        while (!ls_lora_send_done() && esp_timer_get_time() < end)
            vTaskDelay(pdMS_TO_TICKS(5));
        printf("lora: transmit %s\n", ls_lora_send_done() ? "complete" : "still busy");
        ls_lora_receive();
        return 0;
    }
    ls_lora_diagnostics();
    if (argc < 2) printf("lora: also 'lora config [MHz [sf [bw_kHz [cr [dBm]]]]]', "
                         "'lora rx [secs]', 'lora tx [text]'\n");
    return 0;
}
/* The RTC's console face. `rtc set` takes UNIX epoch seconds rather
   than a friendly date on purpose: parsing a human date needs a timezone to
   be meaningful, this board has no timezone, and the one thing worse than no
   clock is a clock that is confidently wrong by some number of hours. */
static int rtc_cmd(int argc, char **argv)
{
    if (argc >= 3 && !strcmp(argv[1], "set")) {
        char *end;
        long long v = strtoll(argv[2], &end, 10);
        if (*argv[2] && !*end && v > 1735689600LL) {
            esp_err_t err = ls_rtc_set((time_t)v);
            printf("rtc: set %s\n", esp_err_to_name(err));
            if (err == ESP_OK) ls_rtc_seed_system_time();
        } else {
            printf("rtc: set takes UNIX epoch seconds after 2025-01-01\n");
            return 1;
        }
    } else if (argc >= 2 && !strcmp(argv[1], "sync")) {

        time_t now = time(NULL);
        if (now < 1735689600) { printf("rtc: system clock is not set yet\n"); return 1; }
        esp_err_t err = ls_rtc_set(now);
        printf("rtc: stored system time, %s\n", esp_err_to_name(err));
        if (err != ESP_OK) return 1;
    }
    ls_rtc_diagnostics();
    return 0;
}

static int mesh_cmd(int argc, char **argv)
{
    if (argc >= 2 && !strcmp(argv[1], "start")) {
        esp_err_t err = ls_mesh_start();
        printf("mesh: start %s\n", esp_err_to_name(err));
        if (err != ESP_OK) return 1;
    } else if (argc >= 2 && !strcmp(argv[1], "stop")) {
        ls_mesh_stop();
        printf("mesh: stopped\n");
        return 0;
    } else if (argc >= 3 && !strcmp(argv[1], "tx")) {
        bool on = !strcmp(argv[2], "on") || !strcmp(argv[2], "1");
        if (!on && strcmp(argv[2], "off") && strcmp(argv[2], "0")) {
            printf("mesh: tx on|off\n");
            return 1;
        }
        if (!ls_mesh_running()) { printf("mesh: not running\n"); return 1; }
        ls_mesh_set_tx(on);
        printf("mesh: transmit %s\n", on ? "ARMED - this radio can now emit"
                                          : "disarmed");
        return 0;
    } else if (argc >= 3 && !strcmp(argv[1], "auto")) {
        /*`mesh auto listen|tx on|off`. Manual start/stop and
           tx on/off are unchanged - this only decides what happens at boot. */
        const bool on = argc >= 4 && (!strcmp(argv[3], "on") || !strcmp(argv[3], "1"));
        if (!strcmp(argv[2], "listen"))
            ls_mesh_set_auto(on, ls_mesh_auto_tx());
        else if (!strcmp(argv[2], "tx"))
            ls_mesh_set_auto(ls_mesh_auto_listen(), on);
        else { printf("mesh: auto listen|tx on|off\n"); return 1; }
        printf("mesh: auto listen %s, auto transmit %s\n",
               ls_mesh_auto_listen() ? "on" : "off",
               ls_mesh_auto_tx() ? "ON - can emit from boot" : "off");
        return 0;
    } else if (argc >= 4 && !strcmp(argv[1], "set")) {
        /* The radio parameters from the console. They were only
           reachable from the SETUP page, which is a lot of keystrokes over a
           serial link - and matching a phone app's settings is exactly the
           moment you want one command. Everything goes through
           ls_mesh_set_radio, so the preamble still follows the spreading
           factor () and the image calibration still follows the
           frequency (). */
        ls_mesh_radio_t r; ls_mesh_get_radio(&r);
        const char *k = argv[2];
        const double v = atof(argv[3]);
        if      (!strcmp(k, "freq"))  r.freq_hz   = (uint32_t)(v * 1e6 + 0.5);
        else if (!strcmp(k, "sf"))    r.sf        = (uint8_t)v;
        else if (!strcmp(k, "bw"))    r.bw_hz     = (uint32_t)(v * 1000.0 + 0.5);
        else if (!strcmp(k, "cr"))    r.cr        = (uint8_t)v;
        else if (!strcmp(k, "power")) r.power_dbm = (int8_t)v;
        else { printf("mesh: set freq|sf|bw|cr|power <value>\n"); return 1; }
        esp_err_t e = ls_mesh_set_radio(&r);
        if (e != ESP_OK) { printf("mesh: set %s\n", esp_err_to_name(e)); return 1; }
        ls_mesh_get_radio(&r);
        printf("mesh: %.4f MHz SF%u BW%.1f kHz CR4/%u %+d dBm\n",
               r.freq_hz / 1e6, (unsigned)r.sf, r.bw_hz / 1000.0,
               (unsigned)r.cr, (int)r.power_dbm);
        return 0;
    } else if (argc >= 3 && !strcmp(argv[1], "role")) {
        /* What this node calls itself in its adverts. Not cosmetic:
           a repeater's neighbour table filters on ADV_TYPE_REPEATER, so a
           node advertising as chat is invisible to it however well the link
           works. Claiming repeater also turns forwarding on - saying it
           without doing it is a lie other nodes route around. */
        ls_mesh_radio_t r; ls_mesh_get_radio(&r);
        if      (!strcmp(argv[2], "chat"))     r.role = LS_MESH_ROLE_CHAT;
        else if (!strcmp(argv[2], "repeater")) { r.role = LS_MESH_ROLE_REPEATER; r.repeat = 1; }
        else if (!strcmp(argv[2], "room"))     r.role = LS_MESH_ROLE_ROOM;
        else if (!strcmp(argv[2], "sensor"))   r.role = LS_MESH_ROLE_SENSOR;
        else { printf("mesh: role chat|repeater|room|sensor\n"); return 1; }
        esp_err_t e = ls_mesh_set_radio(&r);
        printf("mesh: role %s%s (%s)\n", argv[2],
               r.repeat ? ", relaying" : "", esp_err_to_name(e));
        return e == ESP_OK ? 0 : 1;
    } else if (argc >= 3 && !strcmp(argv[1], "repeat")) {
        ls_mesh_radio_t r; ls_mesh_get_radio(&r);
        r.repeat = (!strcmp(argv[2], "on") || !strcmp(argv[2], "1")) ? 1 : 0;
        esp_err_t e = ls_mesh_set_radio(&r);
        printf("mesh: relaying %s%s\n", r.repeat ? "ON" : "off",
               r.repeat ? " - needs transmit armed to do anything" : "");
        return e == ESP_OK ? 0 : 1;
    } else if (argc >= 3 && !strcmp(argv[1], "loc")) {
        ls_mesh_radio_t r; ls_mesh_get_radio(&r);
        if      (!strcmp(argv[2], "off"))    r.share_loc = 0;
        else if (!strcmp(argv[2], "gps"))    r.share_loc = 1;
        else if (!strcmp(argv[2], "manual")) r.share_loc = 2;
        else { printf("mesh: loc off|gps|manual\n"); return 1; }
        ls_mesh_set_radio(&r);
        printf("mesh: location %s\n", argv[2]);
        return 0;
    } else if (argc >= 4 && !strcmp(argv[1], "dm")) {
        /*`mesh dm <peer#> <text...>` - addressed, encrypted, and
           acknowledged by the recipient itself. The index is the row number
           `mesh peers` prints. */
        char text[96]; text[0] = 0;
        for (int i = 3; i < argc; i++) {
            if (i > 3) strncat(text, " ", sizeof(text) - strlen(text) - 1);
            strncat(text, argv[i], sizeof(text) - strlen(text) - 1);
        }
        esp_err_t e = ls_mesh_send_dm(atoi(argv[2]), text);
        if (e == ESP_ERR_NOT_ALLOWED) { printf("mesh: transmit is disarmed\n"); return 1; }
        if (e == ESP_ERR_NOT_FOUND)   { printf("mesh: no such peer - see 'mesh peers'\n"); return 1; }
        printf("mesh: dm %s\n", esp_err_to_name(e));
        return e == ESP_OK ? 0 : 1;
    } else if (argc >= 2 && !strcmp(argv[1], "forget")) {
        /* The other half of remembering. Peers persist now, so a
           node that has moved on, or a contact added from a mistyped key,
           holds one of twelve slots until something takes it out. */
        const char *who = (argc >= 3) ? argv[2] : "all";
        const int n = ls_mesh_forget_peer(who);
        if (!n && strcasecmp(who, "all"))
            printf("mesh: no node here with id %s - 'mesh peers' lists them\n",
                   who);
        else
            printf("mesh: forgot %d, on the card as well as in memory\n", n);
        return 0;
    } else if (argc >= 3 && !strcmp(argv[1], "contact")) {

        esp_err_t e = ls_mesh_add_contact(argv[2], argc >= 4 ? argv[3] : NULL);
        if (e == ESP_ERR_INVALID_SIZE) {
            printf("mesh: the public key is 64 hex characters\n");
            return 1;
        }
        if (e != ESP_OK) { printf("mesh: contact %s\n", esp_err_to_name(e)); return 1; }
        printf("mesh: contact added - it will show as never heard until it "
               "actually transmits\n");
        return 0;
    } else if (argc >= 2 && !strcmp(argv[1], "peers")) {
        /* One peer at a time. The whole table is about 1.1 KB and
           this runs on console_repl's 3,960-byte stack - the same overflow
           and both were. */
        static const char *ROLE[] = { "-", "chat", "repeater", "room", "sensor" };
        ls_mesh_peer_t one;
        int shown = 0;

        const uint32_t now = ls_mesh_now();
        for (int i = 0; ls_mesh_peer_at(i, &one); i++) {
            /* A peer with no last_heard has NOT been heard, and the interval since never is not a number. */

            char when[24];
            if (one.last_heard == 0) {
                snprintf(when, sizeof(when), "never heard");
            } else {
                const uint32_t age = (now > one.last_heard)
                                   ? now - one.last_heard : 0;
                snprintf(when, sizeof(when), "heard %lus ago",
                         (unsigned long)age);
            }
            printf("  %d %s %-20s %-8s %4.0f dBm  x%lu  %s\n",
                   i, one.id,
                   one.name[0] ? one.name : "(no name)",
                   one.type < 5 ? ROLE[one.type] : "?",
                   (double)one.rssi, (unsigned long)one.adverts, when);
            if (one.has_loc)
                printf("      at %.5f %.5f\n",
                       one.lat_e6 / 1e6, one.lon_e6 / 1e6);
            shown++;
        }
        /* The same two causes the screen names. A bare "nothing
           heard yet" is true and sends nobody anywhere. */
        if (!shown) {
            printf("mesh: nothing heard yet\n");
            printf("mesh: a node enters this list when IT advertises - an "
                   "advert from here announces us and asks for nothing, so a "
                   "silent peer stays invisible\n");
            printf("mesh: if it stays empty, check the far end matches the "
                   "radio line above. Mismatched SF or bandwidth is "
                   "orthogonal: zero packets AND zero bad, identical to an "
                   "empty room\n");
        }
        return 0;
    } else if (argc >= 2 && !strcmp(argv[1], "advert")) {
        esp_err_t err = ls_mesh_advertise();
        if (err == ESP_ERR_NOT_ALLOWED) {
            printf("mesh: transmit is disarmed - 'mesh tx on' first\n");
            return 1;
        }
        printf("mesh: advert %s\n", esp_err_to_name(err));
        if (err != ESP_OK) return 1;
        return 0;
    } else if (argc >= 2 && !strcmp(argv[1], "radio")) {
        /* The way out of a stored radio setting that disagrees with
           the build. Without this the only route was the SETUP page, and a
           board whose stored spreading factor cannot hear the mesh is a
           board you are trying to fix through a screen you cannot reach
           anything else from. */
        if (argc >= 3 && !strcmp(argv[2], "default")) {
            const esp_err_t err = ls_mesh_radio_default();
            printf("mesh: stored radio settings cleared: %s\n",
                   esp_err_to_name(err));
            if (err == ESP_OK)
                printf("mesh: running the built-in preset now - "
                       "'mesh' to see it\n");
            return err == ESP_OK ? 0 : 1;
        }
        printf("mesh: radio settings are %s\n",
               ls_mesh_radio_stored() ? "STORED and override the build"
                                      : "the built-in preset");
        printf("mesh: 'mesh radio default' clears a stored override\n");
        return 0;
    }
    ls_mesh_diagnostics();
    return 0;
}
/* The gauge, with the voltage beside the percentage - the voltage is
   the measurement and the percentage is the gauge's estimate. */
static int gauge_cmd(int argc, char **argv)
{
    (void)argc; (void)argv;
    ls_gauge_diagnostics();
    return 0;
}
static int gps_cmd(int argc, char **argv)
{
    if (argc >= 2 && !strcmp(argv[1], "baud")) { ls_gps_scan_baud(); return 0; }
    ls_gps_diagnostics();
    return 0;
}
/* The backlight's switching rate, from the console, while it runs. */

static int keys_bl_sweep(void)
{
    struct { const char *what; int pct; uint32_t hz; int hold_ms; } step[] = {
        { "OFF - if this is quiet it IS the backlight",  0,      0, 4000 },
        { "ON at the current setting, 90% 20 kHz",      90,  20000, 4000 },
        { "100% - held high, converter free-running",  100,  20000, 4000 },
        { "90% at 30 kHz",                              90,  30000, 3500 },
        { "90% at 40 kHz",                              90,  40000, 3500 },
        { "90% at 60 kHz",                              90,  60000, 3500 },
        { "90% at 80 kHz",                              90,  80000, 3500 },
        { "50% at 40 kHz",                              50,  40000, 3500 },
        { "25% at 40 kHz",                              25,  40000, 3500 },
        { "10% at 20 kHz",                              10,  20000, 3500 },
    };
    const int n = (int)(sizeof(step) / sizeof(step[0]));

    printf("keys: sweeping the backlight. Listen, and note which line is\n");
    printf("      followed by silence. About %d seconds.\n\n",
           (4000 + 4000 + 4000 + 3500 * 7) / 1000);

    for (int i = 0; i < n; i++) {
        if (step[i].pct <= 0) {
            ls_keypad_backlight(false);
            printf("  %2d/%d  %s\n", i + 1, n, step[i].what);
        } else {
            ls_keypad_backlight_tune(step[i].hz ? step[i].hz : 20000,
                                     step[i].pct * 1023 / 100);
            ls_keypad_backlight(true);
            /* What it ACHIEVED, not what it was asked for. The first
               version of this printed the request, and three of its steps
               could not be delivered at the resolution in use - so it
               announced 40, 60 and 80 kHz while playing 30 kHz three times.
               A sweep that misreports what it is doing is worse than one
               that does not run. */
            printf("  %2d/%d  %s  -> %lu Hz, %d%%\n", i + 1, n, step[i].what,
                   (unsigned long)ls_keypad_backlight_freq(),
                   ls_keypad_backlight_duty());
        }
        fflush(stdout);
        vTaskDelay(pdMS_TO_TICKS(step[i].hold_ms));
    }

    /* Back to where it started, so a sweep that found nothing has changed
       nothing. */
    ls_keypad_backlight_tune(20000, 920);
    ls_keypad_backlight(true);
    printf("\nkeys: back to 90%% at 20 kHz. 'keys bl <pct> <hz>' sets one.\n");
    return 0;
}

static int keys_bl_cmd(int argc, char **argv)
{
    if (argc >= 3 && !strcmp(argv[2], "sweep")) return keys_bl_sweep();

    if (argc < 3) {
        printf("keys bl sweep | off | on | <duty 0..100> [freq_hz]\n");
        printf("  now: %lu Hz achieved, duty %d%%\n",
               (unsigned long)ls_keypad_backlight_freq(),
               ls_keypad_backlight_duty());
        return 0;
    }
    if (!strcmp(argv[2], "off")) {
        ls_keypad_backlight(false);
        printf("keys: backlight off\n");
        return 0;
    }
    if (!strcmp(argv[2], "on")) {
        ls_keypad_backlight(true);
        printf("keys: backlight on, %lu Hz, duty %d%%\n",
               (unsigned long)ls_keypad_backlight_freq(),
               ls_keypad_backlight_duty());
        return 0;
    }

    const int pct = atoi(argv[2]);
    const uint32_t freq = (argc >= 4) ? (uint32_t)strtoul(argv[3], nullptr, 0)
                                      : ls_keypad_backlight_freq();
    const esp_err_t e = ls_keypad_backlight_tune(freq, pct * 1023 / 100);
    printf("keys: %d%% at %lu Hz asked -> %lu Hz achieved: %s\n",
           pct, (unsigned long)freq,
           (unsigned long)ls_keypad_backlight_freq(), esp_err_to_name(e));
    /* Achieved, not asked, is the number that matters: LEDC rounds to what
       its divider can reach at the current resolution, and a request that
       landed a quarter low would be back in the audible band while the
       source still said twenty thousand. */
    return 0;
}

/* What the last map render actually cost, on this board. */

static int map_cmd(int argc, char **argv)
{
    /* Ask the archive about a point without going there.

       The mesh node MAP button asks this before it navigates, so the same
       question has to be answerable by hand - otherwise the only way to find
       out why a node would not map is to press the button and read a blank
       screen, which is the fault this was written to remove. */
    /* Pan from the console, because the tile store could not be
       measured without one.

       The store reads 0 hits after a zoom cycle, which is correct and
       uninteresting - zooming turns the whole working set over. The number
       that matters is the hit rate across a PAN, where most of the view is
       the same tiles, and the only way to pan was a finger on the glass. A
       cache cannot be tuned against a workload that cannot be reproduced. */
    if (argc >= 4 && !strcmp(argv[1], "pan")) {
        ls_map_pan(atoi(argv[2]), atoi(argv[3]));
        double la = 0, lo = 0;
        ls_map_get_center(&la, &lo);
        printf("map: panned to %.5f %.5f\n", la, lo);
        return 0;
    }

    if (argc >= 4 && !strcmp(argv[1], "covers")) {
        const double lat = atof(argv[2]), lon = atof(argv[3]);
        const int z = ls_map_zoom_covering(lat, lon);
        if (z < 0)
            printf("map: %.5f %.5f is outside this archive at every zoom\n",
                   lat, lon);
        else
            printf("map: %.5f %.5f has a tile at z%d (currently z%d)\n",
                   lat, lon, z, ls_map_zoom());
        return 0;
    }
    (void)argc; (void)argv;

    /* It printed the complaint and stopped.

       "map: no tiles here at this zoom" and nothing else - no centre, no
       zoom, no archive range, no tile counts - which is the one moment every
       one of those numbers is worth having. A diagnostic that goes quiet
       exactly when there is a fault has it backwards: the fault is when it
       is needed. The reason is printed, and then everything else is printed
       anyway. */
    const char *why = ls_map_status();
    if (why) printf("map: %s\n", why);

    double lat = 0, lon = 0;
    ls_map_get_center(&lat, &lon);

    ls_map_stats_t st;
    ls_map_stats(&st);

    int zlo = 0, zhi = 0;
    ls_map_zoom_range(&zlo, &zhi);
    printf("map: z%d  %.5f %.5f  tile %d px\n",
           ls_map_zoom(), lat, lon, ls_map_tile_px());
    printf("map: archive declares z%d..z%d, last render took tiles from z%d\n",
           zlo, zhi, ls_map_source_zoom());
    printf("map: last render  %d of %d tiles  %lu us\n",
           st.tiles_drawn, st.tiles_wanted, (unsigned long)st.render_us);
    printf("map:   frame in %s\n",
           ls_map_frame_is_internal() ? "internal RAM" : "PSRAM");
    printf("map:   scratch peak %d points of 65536 (%d KB of the arena)\n",
           st.scratch_peak, (int)(st.scratch_peak * 8 / 1024));
    printf("map:   fetch  %lu us   raster %lu us   arena %lu bytes\n",
           (unsigned long)st.fetch_us, (unsigned long)st.raster_us,
           (unsigned long)st.arena_peak);
    if (st.tiles_drawn > 0)
        printf("map:   per tile  fetch %lu us  raster %lu us\n",
               (unsigned long)(st.fetch_us / (unsigned)st.tiles_drawn),
               (unsigned long)(st.raster_us / (unsigned)st.tiles_drawn));

    int slots = 0; uint32_t hits = 0, misses = 0, absent = 0;
    ls_map_tile_cache_stats(&slots, &hits, &misses, &absent);
    /* The bytes as well as the count, because for a year the count
       WAS the budget divided by the worst tile in the archive and told you
       nothing about what was held. The two together say whether the store
       ran out of memory or ran out of tiles worth keeping. */
    uint32_t held = 0, budget = 0;
    ls_map_tile_cache_bytes(&held, &budget);
    printf("map: tile store %d tiles, %lu of %lu KB, %lu hits, %lu misses",
           slots, (unsigned long)(held / 1024u),
           (unsigned long)(budget / 1024u),
           (unsigned long)hits, (unsigned long)misses);
    if (hits + misses)
        printf("  (%lu%% hit)", (unsigned long)(hits * 100 / (hits + misses)));
    /* Outside the hit rate, because a tile the archive does not have
       is not a tile the store failed to keep. */
    if (absent) printf(", %lu not in the archive", (unsigned long)absent);
    printf("\n");

    const carto_label *L = NULL;
    printf("map: %d place names in view\n", ls_map_labels(&L));
    return 0;
}

static int keys_cmd(int argc, char **argv)
{
    if (argc >= 2 && !strcmp(argv[1], "bl")) return keys_bl_cmd(argc, argv);

    if (argc >= 2 && !strcmp(argv[1], "watch")) {
        ls_keypad_backlight(true);
        printf("watching for 10s, press keys...\n");
        int64_t end = esp_timer_get_time() + 10LL * 1000 * 1000;
        unsigned seen = 0;
        while (esp_timer_get_time() < end) {
            ls_keypad_event_t ev;
            while (ls_keypad_read(&ev)) {
                printf("key r%u c%u %s\n", ev.row, ev.col,
                       ev.pressed ? "down" : "up");
                seen++;
            }
            vTaskDelay(pdMS_TO_TICKS(10));
        }
        printf("keys: %u events\n", seen);
        return 0;
    }
    ls_keypad_diagnostics();
    return 0;
}
/*`display brightness 100` answered "display busy: UI lock timed
   out" whenever the TUI was up, because this command took the LVGL port lock
   that the TUI held for its whole session. removed the lock with the
   shell, so everything here is a panel or a setting and nothing waits. The
   LVGL-only subcommands - key, nav, layout, page, app - went with the shell,
   and so did vsync, fb and bench, which tuned the LVGL flush path. */
static int display_cmd(int argc,char **argv)
{
    if (argc == 3 && !strcmp(argv[1], "brightness")) {
        char *end; long value = strtol(argv[2], &end, 10);
        if (*argv[2] && !*end && value >= 5 && value <= 100) display_ctl_set_user((int)value);
        else printf("brightness must be 5..100\n");
    } else if (argc == 3 && !strcmp(argv[1], "timeout")) {
        char *end; long value = strtol(argv[2], &end, 10);
        if (*argv[2] && !*end && value >= 5 && value <= 240) display_ctl_set_autodim_timeout((int)value);
        else printf("timeout must be 5..240 seconds\n");
    } else if (argc == 3 && !strcmp(argv[1], "autodim")) {
        display_ctl_set_autodim(!strcmp(argv[2], "on"));
    } else if (argc == 2 && !strcmp(argv[1], "rotate")) {
        tui_manual_rotation_taken();
        tui_rotate_to((s_rotation + 1) % 4);
    } else if (argc == 2) {
        char *end; long deg = strtol(argv[1], &end, 10);
        if (*argv[1] && !*end && deg >= 0 && deg <= 270 && deg % 90 == 0) {
            tui_manual_rotation_taken();
            tui_rotate_to((int)(deg / 90));
        } else {
            printf("display: rotate | 0 | 90 | 180 | 270 | brightness 5..100 | timeout 5..240 | autodim on|off\n");
        }
    }
    printf("display=%d degrees touch=%s brightness=%d%% timeout=%ds touch_age_ms=%lld\n",
           s_rotation * 90, s_touch_ok ? "ready" : "unavailable",
           display_ctl_get_user(), display_ctl_autodim_timeout(),
           (long long)((esp_timer_get_time() - touch_snapshot().received) / 1000));
    printf("autodim=%s%s\n", display_ctl_autodim_enabled() ? "on" : "off",
           display_ctl_dimmed() ? ", dimmed now" : "");
    ls_panel_diagnostics();
    return 0;
}
/* Two defects, one root cause, both fatal to `mode fm` while the TUI is up - which is the normal state of this board. */

/* And now there is no LVGL shell at all, so there is never an app to
   launch: the answer is always "not handled here", and select_mode does the
   real radio switch. */
bool compact_ui_select_mode(const char *mode)
{
    (void)mode;
    return false;
}
void compact_ui_show_mode(const char *mode)
{
    if (!mode) return;
    for (unsigned i = 0; i < sizeof(TUI_MODES) / sizeof(TUI_MODES[0]); ++i) {
        if (!strcmp(mode, TUI_MODES[i].mode)) {
            __atomic_store_n(&s_tui_mode_req, (int)i, __ATOMIC_RELEASE);
            return;
        }
    }
}
/* THE LVGL SHELL IS NOT BUILT ON THIS BOARD ANY MORE. */

esp_err_t compact_ui_start(void (*mode_changed)(const char *))
{
    static bool s_started;
    if (s_started) return ESP_OK;

    (void)mode_changed;
    esp_err_t e = ls_panel_start();
    if (e != ESP_OK) return e;
    s_started = true;
    e=ls_touch_init();
    s_touch_ok=e==ESP_OK;
    /* Poll on a small internal-RAM stack, and let input processing copy one
     * snapshot. */
    if(s_touch_ok && xTaskCreatePinnedToCoreWithCaps(touch_task,"tdp_touch",3072,
            nullptr,2,nullptr,1,MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT)!=pdPASS) {
        s_touch_ok=false; e=ESP_ERR_NO_MEM;
    }
    ESP_LOGI("tdp_ui","touch: %s",esp_err_to_name(e));
    nvs_handle_t h;
    if (nvs_open("compact_ui",NVS_READONLY,&h)==ESP_OK) {
        uint8_t r=0; nvs_get_u8(h,"rotation",&r); nvs_close(h); if(r<4)s_rotation=r;
    }
    display_ctl_init();
    /* Dark until the TUI has a frame up, and lit on its first present,
       so the boot reads as deliberate. there is no longer an old
       interface behind it to hide - only an empty buffer. */
    ls_panel_set_brightness(0);
    const esp_console_cmd_t mesh_c={.command="mesh",
        .help="MeshCore in the background: 'mesh start', 'mesh' for status, "
              "'mesh tx on|off' (off at boot), 'mesh advert', 'mesh stop'.",
        .hint="[start|stop|tx on|off|advert|peers|dm <n> <text>|role|repeat|loc|auto|radio [default]]",.func=mesh_cmd,.argtable=nullptr};
    esp_console_cmd_register(&mesh_c);
    const esp_console_cmd_t gauge_c={.command="gauge",
        .help="Battery fuel gauge: voltage, current, learned charge, temperature.",
        .hint=nullptr,.func=gauge_cmd,.argtable=nullptr};
    esp_console_cmd_register(&gauge_c);
    const esp_console_cmd_t rtc_c={.command="rtc",
        .help="Real-time clock: bare 'rtc' reads it, 'rtc set <unix>' writes "
              "it, 'rtc sync' stores the system clock into it.",
        .hint="[set <unix>|sync]",.func=rtc_cmd,.argtable=nullptr};
    esp_console_cmd_register(&rtc_c);
    const esp_console_cmd_t call_c={.command="call",
        .help="Invoke a named action. Bare 'call' lists them.",
        .hint="<action> [args...]",.func=call_cmd,.argtable=nullptr};
    esp_console_cmd_register(&call_c);
    const esp_console_cmd_t cmd={.command="display",.help="Display: rotate | 0 | 90 | 180 | 270 | brightness 5..100 | timeout 5..240 | autodim on|off",.hint=nullptr,.func=display_cmd,.argtable=nullptr};
    esp_console_cmd_register(&cmd);

    ls_keypad_start();
    const esp_console_cmd_t tui={.command="tui",
        .help="TUI: 'tui' starts it, 'tui off' stops it, 'tui rotate' turns it, 'tui shot' prints the screen as text, 'tui png [name]' sends the real pixels as a PNG, 'tui tap C R' taps a cell, 'tui cost' the last frame, 'tui corner [px]' the corner standoff, 'tui theme [name]' the colours, 'tui daylight on|off' black on white for the sun",
        .hint=nullptr,.func=tui_cmd,.argtable=nullptr};
    esp_console_cmd_register(&tui);
    const esp_console_cmd_t sd={.command="sd",
        .help="SD card: 'sd' says whether it mounted and why not, 'sd mount' retries, 'sd ls [dir]' lists",
        .hint=nullptr,.func=sd_cmd,.argtable=nullptr};
    esp_console_cmd_register(&sd);
    const esp_console_cmd_t gps={.command="gps",
        .help="GPS: report fix and sentence health. 'gps baud' listens at each rate and dumps the wire",
        .hint=nullptr,.func=gps_cmd,.argtable=nullptr};
    esp_console_cmd_register(&gps);
    const esp_console_cmd_t lora={.command="lora",
        .help="LoRa: bring the SX1262 up and report what answered. 'lora scan [loMHz hiMHz [bins]]' sweeps the band as a spectrum",
        .hint=nullptr,.func=lora_cmd,.argtable=nullptr};
    esp_console_cmd_register(&lora);
    const esp_console_cmd_t spi={.command="spi",
        .help="SPI: 'spi' reports the bus, 'spi up' creates it",
        .hint=nullptr,.func=spi_cmd,.argtable=nullptr};
    esp_console_cmd_register(&spi);
    const esp_console_cmd_t audio={.command="audio",
        .help="Audio: report the codec, I2S pins and amplifier, and try each "
              "one - 'audio swap', 'audio pa on|off', 'audio tone'",
        .hint=nullptr,.func=audio_cmd,.argtable=nullptr};
    esp_console_cmd_register(&audio);
    const esp_console_cmd_t mapc={.command="map",
        .help="Map: the last render's cost, split into fetching tiles and "
              "rasterising them",
        .hint=nullptr,.func=map_cmd,.argtable=nullptr};
    esp_console_cmd_register(&mapc);
    const esp_console_cmd_t keys={.command="keys",
        .help="Keypad: probe, 'keys watch' to print events for 10 seconds, "
              "'keys bl sweep' to chase the backlight whine",
        .hint=nullptr,.func=keys_cmd,.argtable=nullptr};
    esp_console_cmd_register(&keys);

    app_park();

    /* Boot into the TUI. it is the only interface. */
    tui_start_session();
    ESP_LOGI("tdp_ui","LakeShark TUI ready, %d degrees",s_rotation*90);
    return ESP_OK;
}
#else
esp_err_t compact_ui_start(void (*mode_changed)(const char *)) { (void)mode_changed; return ESP_ERR_NOT_SUPPORTED; }
bool compact_ui_select_mode(const char *mode) { (void)mode; return false; }
void compact_ui_show_mode(const char *mode) { (void)mode; }
#endif
