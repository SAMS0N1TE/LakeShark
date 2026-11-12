/* PINS: where the add-on parts are wired (GPS, LoRa / LR2021, nine-axis IMU,
   Flipper link). Edits a staged copy of the pin table; SAVE writes it to NVS
   and it applies at the next boot. Every value shown is the real table or
   the real part state: nothing here animates on its own. */
#include "../../ls_tui_screen.h"
#include "../../ls_tui_ui.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ls_pins.h"
#include "ls_board.h"
#if defined(LS_BOARD_PINS_RUNTIME) && LS_BOARD_PINS_RUNTIME
#include "ls_gps.h"
#include "ls_lora.h"
#include "ls_lora_port.h"
#include "ls_imu.h"
#ifdef ESP_PLATFORM
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#endif

#define ROWS (LS_PIN_COUNT + 1)     /* every pin, then the IMU address */
#define ROW_IMU_ADDR LS_PIN_COUNT
#define GPIO_TOP 54

static int     s_stage[LS_PIN_COUNT];
static uint8_t s_imu;
static int     s_sel;
static int     s_top;
static char    s_typed[4];
static char    s_msg[72];
static uint8_t s_msg_attr;
static tui_rect s_list;             /* rows of the list, for taps */
static int     s_list_x_val;        /* column where the value field starts */

static const ls_btn_t BUTTONS[] = {
    {.label = "S SAVE",     .value = "NEXT BOOT", .key = 's'},
    {.label = "U UNDO",     .value = "AS SAVED",  .key = 'u'},
    {.label = "D DEFAULTS", .value = "BOARD",     .key = 'd'},
    {.label = "B REBOOT",   .value = "APPLY",     .key = 'b'},
};
#define N_BUTTONS ((int)(sizeof(BUTTONS) / sizeof(BUTTONS[0])))

static uint8_t group_hue(const char *g)
{
    if (!strcmp(g, "GPS"))  return TUI_YELLOW | TUI_BRIGHT;
    if (!strcmp(g, "LORA")) return TUI_GREEN | TUI_BRIGHT;
    if (!strcmp(g, "IMU"))  return TUI_MAGENTA | TUI_BRIGHT;
    return TUI_CYAN | TUI_BRIGHT;
}

static void say(const char *text, uint8_t hue)
{
    snprintf(s_msg, sizeof(s_msg), "%s", text);
    s_msg_attr = TUI_ATTR(hue, TUI_BLACK);
}

static void load_saved(void)
{
    for (int i = 0; i < LS_PIN_COUNT; i++) s_stage[i] = ls_pin_next((ls_pin_id_t)i);
    s_imu = ls_pins_imu_addr_next();
}

static void enter(void)
{
    load_saved();
    s_typed[0] = 0;
    if (!ls_pins_runtime()) say("This board's pins are fixed: read only.", TUI_YELLOW);
    else if (ls_pins_pending()) say("Saved changes wait for a reboot (B).", TUI_YELLOW | TUI_BRIGHT);
    else say("Arrows pick and step, digits type a GPIO, N = none.", LS_DIM_FG);
}

static bool staged_dirty(void)
{
    for (int i = 0; i < LS_PIN_COUNT; i++) if (s_stage[i] != ls_pin_next((ls_pin_id_t)i)) return true;
    return s_imu != ls_pins_imu_addr_next();
}

static bool lora_changed(void)
{
    for (int i = LS_PIN_SPI_SCLK; i <= LS_PIN_LORA_IRQ; i++)
        if (s_stage[i] != ls_pin_next((ls_pin_id_t)i)) return true;
    return false;
}

/* The owner of a GPIO in the staged table, -1 for none. */
static int owner_of(int g)
{
    for (int i = 0; i < LS_PIN_COUNT; i++) if (s_stage[i] == g) return i;
    return -1;
}

static void step(int dir)
{
    if (!ls_pins_runtime()) return;
    if (s_sel == ROW_IMU_ADDR) {
        int a = s_imu + dir;
        if (a < 0x08) a = 0x77;
        if (a > 0x77) a = 0x08;
        s_imu = (uint8_t)a;
        return;
    }
    const ls_pin_info_t *in = ls_pin_info((ls_pin_id_t)s_sel);
    int g = s_stage[s_sel];
    /* Next GPIO that the board does not own; NONE sits below 0 when optional. */
    for (int tries = 0; tries < GPIO_TOP + 3; tries++) {
        g += dir;
        if (g > GPIO_TOP) g = in->optional ? LS_PIN_NONE : 0;
        if (g < LS_PIN_NONE || (g == LS_PIN_NONE && !in->optional)) g = GPIO_TOP;
        if (g == LS_PIN_NONE || !ls_pin_board_own(g) ||
            ((s_sel == LS_PIN_IMU_SDA || s_sel == LS_PIN_IMU_SCL) && g == ls_pin_default((ls_pin_id_t)s_sel)))
            break;
    }
    s_stage[s_sel] = g;
}

static void commit_typed(void)
{
    if (!s_typed[0] || s_sel == ROW_IMU_ADDR) { s_typed[0] = 0; return; }
    const int g = (int)strtol(s_typed, NULL, 10);
    s_typed[0] = 0;
    if (g > GPIO_TOP) { say("GPIO 0-54.", TUI_RED | TUI_BRIGHT); return; }
    s_stage[s_sel] = g;
}

static void save(void)
{
    char why[64];
    if (!ls_pins_runtime()) { say("This board's pins are fixed.", TUI_YELLOW); return; }
    if (!staged_dirty()) { say("Nothing changed.", LS_DIM_FG); return; }
    const bool lora = lora_changed();
    if (!ls_pins_apply(s_stage, s_imu, why, sizeof(why))) {
        char t[72];
        snprintf(t, sizeof(t), "Not saved: %s", why);
        say(t, TUI_RED | TUI_BRIGHT);
        return;
    }
    if (lora) {
        /* A module set up with `lora port header` would keep overriding the
           table: point the radio back at the table's socket. */
        ls_lora_port_t p;
        if (ls_lora_port_saved(&p) && p.header) {
            ls_lora_port_board(&p);
            p.header = false;
            ls_lora_port_save(&p, why, sizeof(why));
        }
    }
    say("Saved. B reboots to apply.", TUI_GREEN | TUI_BRIGHT);
}

static void reboot(void)
{
#ifdef ESP_PLATFORM
    if (staged_dirty()) { say("Unsaved edits: S first, or U to drop them.", TUI_YELLOW | TUI_BRIGHT); return; }
    printf("pins: reboot to apply the pin table\n");
    vTaskDelay(pdMS_TO_TICKS(150));
    esp_restart();
#endif
}

static bool activate(int i)
{
    switch (i) {
    case 0: commit_typed(); save(); return true;
    case 1: load_saved(); say("Back to the saved table.", LS_DIM_FG); return true;
    case 2:
        for (int k = 0; k < LS_PIN_COUNT; k++) s_stage[k] = ls_pin_default((ls_pin_id_t)k);
        s_imu = 0x68;
        say("Board defaults staged: S to save.", TUI_YELLOW | TUI_BRIGHT);
        return true;
    case 3: reboot(); return true;
    default: return false;
    }
}

/* ------------------------------------------------------------- drawing -- */

static void pin_text(int g, char *out, size_t n)
{
    if (g < 0) snprintf(out, n, "--");
    else snprintf(out, n, "%2d", g);
}

static void draw_list(tui_surface *sf, tui_rect r)
{
    ls_panel_box(sf, r, "01 / WIRING", TUI_CYAN | TUI_BRIGHT);
    const int x = r.x + 2;
    const uint8_t head = TUI_ATTR(TUI_WHITE, TUI_BLACK);
    tui_put_str(sf, r, x, r.y + 1, "PART  LINE              NOW  NEXT", head);
    s_list = tui_rect_make(r.x + 1, r.y + 2, r.w - 2, r.h - 3);
    s_list_x_val = x + 24;
    const int visible = s_list.h;
    if (s_sel < s_top) s_top = s_sel;
    if (s_sel >= s_top + visible) s_top = s_sel - visible + 1;
    const char *last_group = "";
    for (int row = 0; row < visible && s_top + row < ROWS; row++) {
        const int i = s_top + row;
        const int y = s_list.y + row;
        const bool sel = i == s_sel;
        char now[8], next[16], line[64];
        uint8_t hue;
        const char *group, *label;
        bool bad = false, changed;
        char why[64] = "";
        if (i == ROW_IMU_ADDR) {
            group = "IMU"; label = "I2C ADDRESS";
            snprintf(now, sizeof(now), "%02X", ls_pins_imu_addr());
            snprintf(next, sizeof(next), "%02X", s_imu);
            changed = s_imu != ls_pins_imu_addr();
        } else {
            const ls_pin_info_t *in = ls_pin_info((ls_pin_id_t)i);
            group = in->group; label = in->label;
            pin_text(ls_pin((ls_pin_id_t)i), now, sizeof(now));
            if (sel && s_typed[0]) snprintf(next, sizeof(next), "%s_", s_typed);
            else pin_text(s_stage[i], next, sizeof(next));
            changed = s_stage[i] != ls_pin((ls_pin_id_t)i);
            bad = ls_pins_runtime() && !ls_pins_check_in(s_stage, (ls_pin_id_t)i, why, sizeof(why));
        }
        hue = group_hue(group);
        const uint8_t bg = sel ? TUI_BLUE : TUI_BLACK;
        if (sel) tui_fill(sf, tui_rect_make(s_list.x, y, s_list.w, 1), ' ', TUI_ATTR(TUI_WHITE, bg));
        const bool first = strcmp(group, last_group) != 0;
        last_group = group;
        tui_put_str(sf, s_list, x, y, first ? group : "  .", TUI_ATTR(first ? hue : LS_FAINT_FG, bg));
        snprintf(line, sizeof(line), "%-17s", label);
        tui_put_str(sf, s_list, x + 6, y, line, TUI_ATTR(sel ? TUI_WHITE | TUI_BRIGHT : TUI_WHITE, bg));
        tui_put_str(sf, s_list, x + 24, y, now, TUI_ATTR(LS_DIM_FG, bg));
        const uint8_t nattr = bad ? TUI_ATTR(TUI_RED | TUI_BRIGHT, bg)
                            : changed ? TUI_ATTR(TUI_YELLOW | TUI_BRIGHT, bg)
                                      : TUI_ATTR(hue, bg);
        tui_put_str(sf, s_list, x + 28, y, sel && ls_pins_runtime() ? "<" : " ", TUI_ATTR(TUI_CYAN, bg));
        tui_put_str(sf, s_list, x + 29, y, next, nattr);
        tui_put_str(sf, s_list, x + 29 + (int)strlen(next), y, sel && ls_pins_runtime() ? ">" : " ",
                    TUI_ATTR(TUI_CYAN, bg));
        if (changed && !bad) tui_put_str(sf, s_list, x + 34, y, "*", TUI_ATTR(TUI_YELLOW | TUI_BRIGHT, bg));
        if (sel && bad && why[0]) {
            char t[72];
            snprintf(t, sizeof(t), "! %s", why);
            say(t, TUI_RED | TUI_BRIGHT);
        }
    }
}

/* Every GPIO, coloured by who has it in the staged table: the header as
   the parts see it. The selected line's pin is bracketed. */
static void draw_map(tui_surface *sf, tui_rect r)
{
    ls_panel_box(sf, r, "02 / GPIO 0-54", TUI_BLUE | TUI_BRIGHT);
    const int cols = (r.w - 4) / 5 >= 11 ? 11 : ((r.w - 4) / 5 > 0 ? (r.w - 4) / 5 : 1);
    const int sel_pin = s_sel < LS_PIN_COUNT ? s_stage[s_sel] : -1;
    for (int g = 0; g <= GPIO_TOP; g++) {
        const int cx = r.x + 2 + (g % cols) * 5;
        const int cy = r.y + 2 + (g / cols) * 2;
        if (cy >= r.y + r.h - 6) break;
        const int who = owner_of(g);
        uint8_t a;
        char cell[8];
        if (who >= 0) {
            a = TUI_ATTR(group_hue(ls_pin_info((ls_pin_id_t)who)->group), TUI_BLACK);
            snprintf(cell, sizeof(cell), "%02d", g);
        } else if (ls_pin_board_own(g)) {
            a = TUI_ATTR(LS_DIM_FG, TUI_BLACK);
            snprintf(cell, sizeof(cell), "%02d", g);
        } else {
            a = LS_ATTR_FAINT;
            snprintf(cell, sizeof(cell), "%02d", g);
        }
        if (g == sel_pin) {
            tui_put_str(sf, r, cx - 1, cy, "[", TUI_ATTR(TUI_WHITE | TUI_BRIGHT, TUI_BLACK));
            tui_put_str(sf, r, cx + 2, cy, "]", TUI_ATTR(TUI_WHITE | TUI_BRIGHT, TUI_BLACK));
        }
        tui_put_str(sf, r, cx, cy, cell, a);
        /* A tick under each pin a part holds, in its colour; '=' under the
           board's own lines; '.' free. */
        tui_put_str(sf, r, cx, cy + 1, who >= 0 ? "^^" : ls_pin_board_own(g) ? "==" : "..",
                    who >= 0 ? a : LS_ATTR_FAINT);
    }
    /* Key, then what the parts say right now. */
    int y = r.y + r.h - 6;
    int x = r.x + 2;
    static const char *const G[] = { "GPS", "LORA", "IMU", "LINK" };
    for (int k = 0; k < 4; k++) {
        tui_put_str(sf, r, x, y, "^^", TUI_ATTR(group_hue(G[k]), TUI_BLACK));
        tui_put_str(sf, r, x + 3, y, G[k], TUI_ATTR(group_hue(G[k]), TUI_BLACK));
        x += 9;
    }
    tui_put_str(sf, r, x, y, "== BOARD", TUI_ATTR(LS_DIM_FG, TUI_BLACK));

    char t[64];
    ls_gps_state_t gs;
    ls_gps_get(&gs);
    snprintf(t, sizeof(t), "%s", !ls_gps_running() ? "off" : gs.fix ? "fix" : gs.alive ? "NMEA, no fix" : "silent");
    ls_kv(sf, r, r.h - 5, "GPS", t,
          TUI_ATTR(gs.fix ? TUI_GREEN | TUI_BRIGHT : gs.alive ? TUI_YELLOW : LS_DIM_FG, TUI_BLACK));
    const ls_lora_chip_t chip = ls_lora_fitted();
    ls_kv(sf, r, r.h - 4, "LORA", chip == LS_LORA_CHIP_NONE ? "not found" : ls_lora_fitted_name(),
          TUI_ATTR(chip == LS_LORA_CHIP_NONE ? LS_DIM_FG : TUI_GREEN | TUI_BRIGHT, TUI_BLACK));
    ls_kv(sf, r, r.h - 3, "IMU", ls_imu_present() ? "ICM-20948" : "not found",
          TUI_ATTR(ls_imu_present() ? TUI_GREEN | TUI_BRIGHT : LS_DIM_FG, TUI_BLACK));
    ls_kv(sf, r, r.h - 2, "TABLE", !ls_pins_runtime() ? "fixed" : ls_pins_pending() ? "reboot pending" : "in use",
          TUI_ATTR(ls_pins_pending() ? TUI_YELLOW | TUI_BRIGHT : TUI_CYAN, TUI_BLACK));
}

static void draw(tui_surface *sf, tui_rect area)
{
    const int bar_h = ls_btn_raised_height(area, N_BUTTONS);
    tui_rect bar = tui_rect_make(area.x, area.y + area.h - bar_h, area.w, bar_h);
    tui_rect msg = tui_rect_make(area.x, bar.y - 1, area.w, 1);
    tui_rect body = tui_rect_make(area.x, area.y, area.w, area.h - bar_h - 1);
    const bool wide = body.w >= 80;
    tui_rect list = body, map = body;
    if (wide) {
        list.w = 40;
        map.x += list.w; map.w -= list.w;
    } else {
        list.h = ROWS + 4 < body.h / 2 ? ROWS + 4 : body.h / 2;
        map.y += list.h; map.h -= list.h;
    }
    draw_list(sf, list);
    draw_map(sf, map);
    tui_fill(sf, msg, ' ', LS_ATTR_DIM);
    ls_safe_line(sf, msg, msg.y, s_msg, s_msg_attr ? s_msg_attr : LS_ATTR_DIM);
    ls_btn_bar_raised_row(sf, bar, BUTTONS, N_BUTTONS, -1);
}

/* --------------------------------------------------------------- input -- */

static bool key(ls_tk_t k, char ch)
{
    switch (k) {
    case LS_TK_UP:    commit_typed(); s_sel = (s_sel + ROWS - 1) % ROWS; return true;
    case LS_TK_DOWN:  commit_typed(); s_sel = (s_sel + 1) % ROWS; return true;
    case LS_TK_LEFT:  s_typed[0] = 0; step(-1); return true;
    case LS_TK_RIGHT: s_typed[0] = 0; step(+1); return true;
    case LS_TK_ENTER: commit_typed(); return true;
    case LS_TK_BACKSPACE: {
        const size_t n = strlen(s_typed);
        if (n) s_typed[n - 1] = 0;
        return true;
    }
    case LS_TK_CHAR:
        if (ch >= '0' && ch <= '9' && ls_pins_runtime() && s_sel != ROW_IMU_ADDR) {
            const size_t n = strlen(s_typed);
            if (n < 2) { s_typed[n] = ch; s_typed[n + 1] = 0; }
            if (strlen(s_typed) == 2) commit_typed();
            return true;
        }
        if ((ch == 'n' || ch == 'N') && s_sel < LS_PIN_COUNT && ls_pins_runtime()) {
            s_typed[0] = 0;
            if (ls_pin_info((ls_pin_id_t)s_sel)->optional) s_stage[s_sel] = LS_PIN_NONE;
            else say("That line is needed.", TUI_YELLOW);
            return true;
        }
        if (ch == '-' ) { step(-1); return true; }
        if (ch == '+' || ch == '=') { step(+1); return true; }
        return activate(ls_btn_key(ch, BUTTONS, N_BUTTONS));
    default:
        return false;
    }
}

static bool touch(int col, int row)
{
    const int b = ls_btn_hit(col, row);
    if (b >= 0) return activate(b);
    if (row >= s_list.y && row < s_list.y + s_list.h && col >= s_list.x && col < s_list.x + s_list.w) {
        const int i = s_top + (row - s_list.y);
        if (i < ROWS) {
            commit_typed();
            if (i == s_sel && col == s_list_x_val + 4) step(-1);
            else if (i == s_sel && col >= s_list_x_val + 7 && col <= s_list_x_val + 8) step(+1);
            s_sel = i;
        }
        return true;
    }
    return false;
}

const ls_tui_screen_t ls_scr_pins = {
    .name = "PINS", .hint = "arrows pick/step  0-9 GPIO  n none  s save  b reboot",
    .enter = enter, .draw = draw, .key = key, .touch = touch,
    .diff_switch = true,
};
#endif /* LS_BOARD_PINS_RUNTIME */
