#include "menu.h"
#include "menu_art.h"
#include "display.h"
#include "e3d.h"
#include "esp_log.h"
#include "ls_font.h"
#include "ls_theme.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

enum page { LIBRARY, PAUSED, SETTINGS, BINDINGS, CAPTURE_BUTTON, CENTER, RANGE };
enum { BRIGHTNESS, THEME, DEADZONE, SENSITIVITY, INVERT_X, INVERT_Y, CALIBRATE,
       CONTROLS, RESET_CONTROLLER, SAVE_BACK, SETTINGS_ROWS };

typedef struct {
    const char *name;
    const char *genre, *summary, *controls;
    menu_action action;
    menu_art_kind art;
} library_item;

static const library_item library[] = {
    {"TIDAL RELAY", "TIME TRIAL", "Thread all 18 sea gates.", "Stick steers. Boost for speed.", MENU_RELAY, ART_RELAY},
    {"SALVAGE RUN", "REEF RECOVERY", "Collect pods and dodge reefs.", "Stick steers. Boost for speed.", MENU_SALVAGE, ART_SALVAGE},
#ifdef LAKESHARK_GBA
    {"SIGNAL CATCH", "GBA CARTRIDGE", "Catch the incoming pulses.", "Stick up/down moves the paddle.", MENU_GBA_DEMO, ART_SIGNAL},
    {"GBA FROM SD", "GBA / MICROSD", "Load game.gba from microSD.", "Uses the GBA button bindings.", MENU_GBA_SD, ART_CARTRIDGE},
#endif
    {"SETTINGS", "DISPLAY / INPUT", "Tune the display and stick.", "Calibrate or remap buttons.", MENU_NONE, ART_TK21},
};

static const char *const settings_labels[] = {
    "Brightness", "Theme", "Stick dead zone", "Stick sensitivity", "Invert stick X", "Invert stick Y",
    "Calibrate stick", "Button bindings", "Reset controller", "Save & return",
};
static const char *const binding_labels[] = {
    "Flight boost", "GBA A", "GBA B", "GBA Start", "GBA Select", "GBA L", "GBA R",
};
static const char *const page_name[] = {"library", "paused", "settings", "bindings", "bind", "center", "range"};
static const ls_tui_theme_t *const themes[] = {
    &ls_theme_terminal_bay, &ls_theme_amber, &ls_theme_phosphor,
};

static enum page current, settings_return;
static menu_action paused_app;
static int selected;
static int settings_return_row;
static menu_direction last_direction;
static int64_t next_repeat;
static char status[49];
static bool dirty, wait_for_neutral;
static stick_calibration pending;
static int center_sum[2], samples;

static void show(enum page page, int row)
{
    current = page;
    selected = row;
    status[0] = 0;
}

void menu_home(void)
{
    paused_app = MENU_NONE;
    show(LIBRARY, 0);
}

void menu_pause(menu_action running)
{
    paused_app = running;
    show(PAUSED, 0);
    /* A tilt carried from a game must return to center before moving the menu. */
    wait_for_neutral = true;
}

void menu_notice(const char *text)
{
    snprintf(status, sizeof(status), "%s", text);
}

static int limit(int value, int lo, int hi)
{
    return value < lo ? lo : value > hi ? hi : value;
}

static menu_direction direction(tk21_state in, menu_direction serial, int64_t now)
{
    if (serial != NAV_NONE) return serial;
    if (wait_for_neutral) {
        if (fabsf(in.x) > .30f || fabsf(in.y) > .30f) return NAV_NONE;
        wait_for_neutral = false;
        last_direction = NAV_NONE;
    }
    menu_direction next = NAV_NONE;
    if (fabsf(in.y) >= fabsf(in.x)) {
        if (in.y > .55f) next = NAV_UP;
        else if (in.y < -.55f) next = NAV_DOWN;
    } else {
        if (in.x < -.55f) next = NAV_LEFT;
        else if (in.x > .55f) next = NAV_RIGHT;
    }
    bool moved = next != last_direction;
    last_direction = next;
    if (next == NAV_NONE || (!moved && now < next_repeat)) return NAV_NONE;
    next_repeat = now + (moved ? 350000 : 140000);
    return next;
}

static int row_count(void)
{
    switch (current) {
    case LIBRARY: return sizeof(library) / sizeof(*library);
    case PAUSED: return 4;
    case SETTINGS: return SETTINGS_ROWS;
    case BINDINGS: return BIND_COUNT + 1;
    default: return 1;
    }
}

static const library_item *library_at(int row)
{
    return &library[row];
}

static void leave_settings(game_settings *s)
{
    esp_err_t result = dirty ? settings_save(s) : ESP_OK;
    show(settings_return, settings_return_row);
    if (result != ESP_OK) {
        menu_notice("Save failed. Current settings are active.");
        ESP_LOGW("game_menu", "Settings save failed: %s", esp_err_to_name(result));
    } else if (dirty) {
        menu_notice("Settings saved.");
        dirty = false;
    }
}

static void change_setting(game_settings *s, int step)
{
    switch (selected) {
    case BRIGHTNESS: s->brightness = limit(s->brightness + step * 5, 20, 100); break;
    case THEME: s->theme = (s->theme + step + THEME_COUNT) % THEME_COUNT; break;
    case DEADZONE: s->deadzone = limit(s->deadzone + step, 0, 35); break;
    case SENSITIVITY: s->sensitivity = limit(s->sensitivity + step * 5, 50, 150); break;
    case INVERT_X: s->invert ^= 1; break;
    case INVERT_Y: s->invert ^= 2; break;
    default: return;
    }
    dirty = true;
}

static void enter_settings(enum page return_to)
{
    settings_return = return_to;
    settings_return_row = selected;
    show(SETTINGS, 0);
}

static void calibration_step(game_settings *s, tk21_state in, tk21_raw_state raw)
{
    if (in.pressed & PAD_B) {
        show(SETTINGS, CALIBRATE);
        return;
    }
    if (current == CENTER) {
        for (int i = 0; i < 2; ++i) center_sum[i] += raw.axis[i];
        if (++samples == 45) {
            for (int i = 0; i < 2; ++i)
                pending.low[i] = pending.high[i] = pending.center[i] = center_sum[i] / samples;
            show(RANGE, 0);
        }
        return;
    }
    for (int i = 0; i < 2; ++i) {
        if (raw.axis[i] < pending.low[i]) pending.low[i] = raw.axis[i];
        if (raw.axis[i] > pending.high[i]) pending.high[i] = raw.axis[i];
    }
    if (in.pressed & PAD_A) {
        if (settings_calibration_valid(&pending)) {
            s->calibration = pending;
            dirty = true;
            show(SETTINGS, CALIBRATE);
            menu_notice("Stick calibrated.");
        } else {
            menu_notice("Move both axes farther in both directions.");
        }
    }
}

menu_action menu_update(game_settings *s, tk21_state in, tk21_raw_state raw,
                        menu_direction serial, int64_t now)
{
    if (current == CENTER || current == RANGE) {
        calibration_step(s, in, raw);
        return MENU_NONE;
    }
    if (current == CAPTURE_BUTTON) {
        uint8_t button = in.pressed & (PAD_ALT | PAD_A | PAD_B | PAD_X | PAD_Y | TK21_KEY);
        if (in.pressed & PAD_MENU) show(BINDINGS, selected);
        else if (button) {
            settings_bind(s, selected, button & -button);
            dirty = true;
            show(BINDINGS, selected);
            menu_notice("Button assigned.");
        }
        return MENU_NONE;
    }
    bool select = in.pressed & PAD_A;
    bool back = in.pressed & PAD_B;
    menu_direction nav = direction(in, serial, now);
    if (nav == NAV_UP || nav == NAV_DOWN) {
        selected = limit(selected + (nav == NAV_UP ? -1 : 1), 0, row_count() - 1);
        status[0] = 0;
    }
    if (current == SETTINGS) {
        if (back || (select && selected == SAVE_BACK)) leave_settings(s);
        else if (select && selected == CONTROLS) show(BINDINGS, 0);
        else if (select && selected == CALIBRATE) {
            center_sum[0] = center_sum[1] = samples = 0;
            show(CENTER, 0);
        } else if (select && selected == RESET_CONTROLLER) {
            unsigned brightness = s->brightness, theme = s->theme;
            settings_defaults(s);
            s->brightness = brightness;
            s->theme = theme;
            dirty = true;
            menu_notice("Controller defaults restored.");
        } else if (selected == INVERT_X || selected == INVERT_Y) {
            if (select) {
                change_setting(s, 1);
                wait_for_neutral = true;
            }
        } else if (select || nav == NAV_LEFT || nav == NAV_RIGHT) {
            change_setting(s, nav == NAV_LEFT ? -1 : 1);
        }
        return MENU_NONE;
    }
    if (current == BINDINGS) {
        if (back) show(SETTINGS, CONTROLS);
        else if (select && selected < BIND_COUNT) show(CAPTURE_BUTTON, selected);
        else if (select) {
            game_settings factory;
            settings_defaults(&factory);
            memcpy(s->bindings, factory.bindings, sizeof(s->bindings));
            dirty = true;
            menu_notice("Default buttons restored.");
        }
        return MENU_NONE;
    }
    if (current == PAUSED) {
        if (back || (in.pressed & PAD_MENU) || (select && selected == 0)) return MENU_RESUME;
        if (select && selected == 1) return MENU_RESTART;
        if (select && selected == 2) enter_settings(PAUSED);
        if (select && selected == 3) return MENU_HOME;
        return MENU_NONE;
    }
    if (in.pressed & PAD_ALT) {
        enter_settings(LIBRARY);
        return MENU_NONE;
    }
    if (!select) return MENU_NONE;
    menu_action action = library_at(selected)->action;
    if (action == MENU_NONE) enter_settings(LIBRARY);
    return action;
}

static void rect(uint16_t *frame, int x, int y, int w, int h, uint16_t color)
{
    for (int row = y; row < y + h; ++row)
        for (int col = x; col < x + w; ++col) frame[row * GAME_WIDTH + col] = color;
}

static void draw_text(uint16_t *frame, const ls_font_t *font, int x, int y, const char *text, uint16_t color)
{
    for (; *text && x < GAME_WIDTH; ++text, x += font->cell_w) {
        const ls_font_glyph_t *g = ls_font_glyph(font, *text);
        if (!g) continue;
        const uint8_t *bitmap = font->bitmap + g->bitmap_index;
        int gx = x + g->ofs_x, gy = y + font->cell_h - font->base_line - g->box_h - g->ofs_y;
        for (int row = 0; row < g->box_h; ++row)
            for (int col = 0; col < g->box_w; ++col) {
                int k = row * g->box_w + col;
                unsigned coverage = (k & 1) ? bitmap[k / 2] & 15 : bitmap[k / 2] >> 4;
                if (coverage >= 6 && (unsigned)(gx + col) < GAME_WIDTH &&
                    (unsigned)(gy + row) < GAME_HEIGHT)
                    frame[(gy + row) * GAME_WIDTH + gx + col] = color;
            }
    }
}

void menu_text(uint16_t *frame, int x, int y, const char *text, uint16_t color)
{
    draw_text(frame, &ls_font_mono_13, x, y, text, color);
}

static void small_text(uint16_t *frame, int x, int y, const char *text, uint16_t color)
{
    draw_text(frame, &ls_font_mono_11, x, y, text, color);
}

static void list_row(uint16_t *frame, int index, const char *label, const char *value,
                     const uint16_t *p, uint16_t accent)
{
    int y = 49 + 16 * index;
    if (index == selected) small_text(frame, 14, y, ">", p[11]);
    small_text(frame, 27, y, label, index == selected ? accent : p[15]);
    if (value) small_text(frame, 157, y, value, index == selected ? p[15] : p[7]);
}

static void rule(uint16_t *frame, int y, const uint16_t *p)
{
    menu_text(frame, 9, y, "+", p[6]);
    menu_text(frame, 383, y, "+", p[6]);
    for (int x = 20; x < 382; x += 4) rect(frame, x, y + 6, 2, 1, p[8]);
}

static void heading(uint16_t *frame, const uint16_t *p, uint16_t accent, const char *title)
{
    const int x = 13, y = 9;
    e3d_text(frame, GAME_WIDTH, GAME_WIDTH, GAME_HEIGHT, x, y, "LAKESHARK", accent, 3);
    // The boot splash uses the same bright middle stripe and a dim lower edge.
    for (int row = y + 11; row < y + 21; ++row) {
        uint16_t *line = frame + row * GAME_WIDTH + x;
        for (int col = 0; col < 162; ++col) {
            if (line[col] == accent) line[col] = row < y + 15 ? p[15] : p[6];
        }
    }
    menu_text(frame, 181, 12, "[ GAME ]", p[11]);
    small_text(frame, GAME_WIDTH - 14 - (int)strlen(title) * 7, 13, title, p[7]);
    rule(frame, 29, p);
}

static void meter(uint16_t *frame, int y, int value, uint16_t ink)
{
    rect(frame, 66, y, 264, 3, 0x294b);
    int x = 66 + 263 * limit(value, 0, 4095) / 4095;
    rect(frame, x, y - 2, 2, 7, ink);
}

static void live_stick(uint16_t *frame, tk21_raw_state raw, tk21_state input, uint16_t ink)
{
    char tag[46];
    snprintf(tag, sizeof(tag), "X %4d   Y %4d   KEY %d   BTN %4dmV",
             raw.axis[0], raw.axis[1], (input.held & TK21_KEY) != 0, raw.ladder_mv);
    menu_text(frame, 22, 174, tag, ink);
    menu_text(frame, 32, 189, "X", ink);
    menu_text(frame, 32, 201, "Y", ink);
    meter(frame, 195, raw.axis[0], ink);
    meter(frame, 207, raw.axis[1], ink);
}

void menu_draw(uint16_t *frame, const game_settings *s, tk21_raw_state raw, tk21_state in, int64_t now)
{
    const uint16_t *p = themes[s->theme]->palette;
    const uint16_t accent = p[s->theme == THEME_AMBER ? 11 : s->theme == THEME_GREEN ? 10 : 14];
    const uint16_t text = p[15], subdued = p[7];
    memset(frame, 0, GAME_WIDTH * GAME_HEIGHT * sizeof(*frame));
    const char *headings[] = {"GAME LIBRARY", "PAUSED", "SETTINGS", "BUTTON BINDINGS",
                              "ASSIGN BUTTON", "CALIBRATE STICK", "CALIBRATE STICK"};
    heading(frame, p, accent, headings[current]);
    rule(frame, 214, p);
    const char *footer = "[A] Select   [B] Back";
    if (current == LIBRARY) {
        const library_item *item = library_at(selected);
        enum { VISIBLE_LIBRARY_ROWS = 6 };
        int first = limit(selected - VISIBLE_LIBRARY_ROWS + 1, 0, row_count() - 1);
        for (int i = first; i < row_count() && i < first + VISIBLE_LIBRARY_ROWS; ++i) {
            int y = 47 + 25 * (i - first);
            bool active = selected == i;
            const library_item *entry = library_at(i);
            if (active) {
                small_text(frame, 14, y, now / 415000 % 2 ? ">>" : " >", p[11]);
                small_text(frame, 143, y, "]", p[11]);
                rect(frame, 27, y + 24, 120, 1, p[8]);
            }
            menu_text(frame, 28, y, entry->name, active ? accent : text);
            small_text(frame, 29, y + 12, entry->genre, active ? p[7] : p[6]);
        }
        for (int y = 43; y < 208; y += 5) rect(frame, 166, y, 1, 2, p[8]);
        menu_art_draw(frame, item->art == ART_TK21 ? 211 : 172, item->art, p, s->theme, now, in.held);
        rect(frame, 172, 188, 228, 26, p[0]);
        small_text(frame, 179, 188, item->summary, text);
        small_text(frame, 179, 201, item->controls, subdued);
        footer = "[STICK] Browse   [A] Open   [ALT] Settings";
    } else if (current == PAUSED) {
        const char *names[] = {"Resume", "Restart", "Settings", "Game library"};
        for (int i = 0; i < row_count(); ++i) list_row(frame, i, names[i], NULL, p, accent);
        for (int i = 0; i < (int)(sizeof(library) / sizeof(*library)); ++i) {
            if (library[i].action == paused_app) {
                menu_art_draw(frame, 172, library[i].art, p, s->theme, now, in.held);
                small_text(frame, 179, 193, library[i].name, accent);
                break;
            }
        }
    } else if (current == SETTINGS) {
        char brightness[12], zone[12], sensitivity[12];
        snprintf(brightness, sizeof(brightness), "%u%%", s->brightness);
        snprintf(zone, sizeof(zone), "%u%%", s->deadzone);
        snprintf(sensitivity, sizeof(sensitivity), "%u%%", s->sensitivity);
        const char *values[] = {brightness, themes[s->theme]->name, zone, sensitivity,
                                s->invert & 1 ? "On" : "Off", s->invert & 2 ? "On" : "Off",
                                settings_calibration_valid(&s->calibration) ? "Custom" : "Auto",
                                NULL, NULL, NULL};
        for (int i = 0; i < SETTINGS_ROWS; ++i)
            list_row(frame, i, settings_labels[i], values[i], p, accent);
        menu_art_draw(frame, 251, ART_TK21, p, s->theme, now, in.held);
        footer = (selected == INVERT_X || selected == INVERT_Y) ?
                 "[A] Toggle   [B] Back" : "Left/Right: change   [A] Select";
    } else if (current == BINDINGS) {
        for (int i = 0; i < BIND_COUNT; ++i)
            list_row(frame, i, binding_labels[i], settings_button_name(s->bindings[i]), p, accent);
        list_row(frame, BIND_COUNT, "Restore default buttons", NULL, p, accent);
        menu_art_draw(frame, 251, ART_TK21, p, s->theme, now, in.held);
        footer = "[A] Bind   [B] Back";
    } else if (current == CAPTURE_BUTTON) {
        menu_text(frame, 22, 60, binding_labels[selected], accent);
        menu_text(frame, 22, 88, "Press a button.", text);
        menu_art_draw(frame, 251, ART_TK21, p, s->theme, now, in.held);
        live_stick(frame, raw, in, subdued);
        footer = "[MENU] Cancel";
    } else {
        menu_text(frame, 22, 60, current == CENTER ? "Release the stick." :
                  "Sweep to all four limits.", text);
        menu_art_draw(frame, 251, ART_TK21, p, s->theme, now, in.held);
        if (current == CENTER) {
            rect(frame, 24, 92, 200, 4, p[8]);
            rect(frame, 24, 92, samples * 200 / 45, 4, accent);
        } else {
            char limits[46];
            snprintf(limits, sizeof(limits), "X %4u-%4u   Y %4u-%4u",
                     pending.low[0], pending.high[0], pending.low[1], pending.high[1]);
            menu_text(frame, 22, 93, limits, subdued);
        }
        live_stick(frame, raw, in, accent);
        footer = current == CENTER ? "[B] Cancel" : "[A] Save calibration   [B] Cancel";
    }
    menu_text(frame, 16, 224, status[0] ? status : footer, status[0] ? accent : subdued);
}

void menu_log(const game_settings *s)
{
    ESP_LOGI("game_menu", "page=%s row=%d app=%d brightness=%u theme=%u deadzone=%u sensitivity=%u invert=%u boost=%s gba_a=%s gba_b=%s",
             page_name[current], selected, paused_app, s->brightness, s->theme, s->deadzone,
             s->sensitivity, s->invert, settings_button_name(s->bindings[BIND_BOOST]),
             settings_button_name(s->bindings[BIND_A]), settings_button_name(s->bindings[BIND_B]));
}
