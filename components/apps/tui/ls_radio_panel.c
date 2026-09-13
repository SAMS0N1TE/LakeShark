#include "ls_radio_panel.h"
#include "scan_engine.h"
#include "scan_channels.h"
#include <stdio.h>
#include <string.h>

static const uint8_t cyan = TUI_ATTR(TUI_CYAN | TUI_BRIGHT, TUI_BLACK);
static const uint8_t amber = TUI_ATTR(TUI_YELLOW | TUI_BRIGHT, TUI_BLACK);
static const uint8_t white = TUI_ATTR(TUI_WHITE | TUI_BRIGHT, TUI_BLACK);

static void line(tui_surface *sf, tui_rect a, int row, const char *text, uint8_t attr)
{
    tui_put_str(sf, a, a.x + 2, a.y + row, text, attr);
}

static void frequency(tui_surface *sf, tui_rect a, int row, uint32_t hz, uint8_t attr)
{
    static const uint8_t digits[10][5] = {
        {7, 5, 5, 5, 7}, {2, 6, 2, 2, 7}, {7, 1, 7, 4, 7}, {7, 1, 7, 1, 7}, {5, 5, 7, 1, 1},
        {7, 4, 7, 1, 7}, {7, 4, 7, 5, 7}, {7, 1, 1, 1, 1}, {7, 5, 7, 5, 7}, {7, 5, 7, 1, 7}};
    char text[20];
    snprintf(text, sizeof(text), "%lu.%04lu", (unsigned long)(hz / 1000000),
             (unsigned long)((hz % 1000000) / 100));
    int width = (int)strlen(text) * 4 - 2;
    if (a.w < width + 2) {
        line(sf, a, row, text, attr);
        return;
    }
    int x = a.x + (a.w - width) / 2;
    for (const char *p = text; *p; ++p) {
        if (*p == '.') {
            tui_put_char(sf, a, x, a.y + row + 4, LS_TUI_BLOCK_FULL, attr);
            x += 2;
        } else {
            for (int r = 0; r < 5; r++)
                for (int c = 0; c < 3; c++)
                    if (digits[*p - '0'][r] & (4 >> c))
                        tui_put_char(sf, a, x + c, a.y + row + r, LS_TUI_BLOCK_FULL, attr);
            x += 4;
        }
    }
}

static void buttons(ls_radio_panel_t *p, const ls_radio_view_t *v)
{
    bool active = scan_engine_active(), hold = scan_engine_manual_hold();
    ls_btn_t b[] = {{active ? (hold ? "RESUME" : "HOLD") : (v->fm ? "A / B" : "SCAN"), NULL,
                     active  ? 'H'
                     : v->fm ? 'A'
                             : 'S',
                     hold, false},
                    {active ? "NEXT" : "TUNE", NULL, active ? 'N' : 'T', false, false},
                    {active  ? "STOP"
                     : v->fm ? "SCAN"
                             : "SIGNAL",
                     NULL,
                     active  ? 'X'
                     : v->fm ? 'S'
                             : 'W',
                     active, false},
                    {"LISTS", NULL, 'L', false, false},
                    {active  ? "SKIP"
                     : v->fm ? "SQUELCH"
                             : "SETTINGS",
                     NULL,
                     active  ? 'K'
                     : v->fm ? 'Q'
                             : 'P',
                     false, false},
                    {"MORE", NULL, 'M', false, false}};
    if (p->lists) {
        ls_btn_t l[] = {
            {"SAVE FREQ", NULL, 'F', false, false}, {"ENABLE", NULL, 'E', false, p->count == 0},
            {"ZONE", NULL, 'Z', false, false},      {"SCAN TYPE", NULL, 'O', false, false},
            {"RANGE", NULL, 'R', false, false},     {"BACK", NULL, 'M', false, false}};
        memcpy(b, l, sizeof(b));
    }
    if (p->scan_choice) {
        bool band = scan_engine_get_source() == SCAN_SRC_BAND;
        ls_btn_t choices[] = {
            {"CHANNEL LIST", NULL, 'C', !band, false}, {"BAND SCAN", NULL, 'B', band, false},
            {"STEP", NULL, 'I', false, !band},         {"RANGE", NULL, 'R', false, !band},
            {"START", NULL, 'S', false, false},        {"BACK", NULL, 'M', false, false}};
        memcpy(b, choices, sizeof(b));
    }
    memcpy(p->buttons, b, sizeof(b));
}

static void channel_list(ls_radio_panel_t *p, const ls_radio_view_t *v, tui_surface *sf, tui_rect a)
{
    p->count = 0;
    for (int i = 0; i < scan_channels_count(); i++) {
        const scan_channel_t *c = scan_channel_get(i);
        if (c && c->mode == (v->fm ? SCAN_MODE_NFM : SCAN_MODE_P25))
            p->visible[p->count++] = i;
    }
    if (!p->lists && scan_engine_get_source() == SCAN_SRC_CHANNELS) {
        for (int i = 0; i < p->count; i++)
            if (p->visible[i] == scan_engine_candidate()) {
                p->selected = i;
                break;
            }
    }
    if (p->selected >= p->count)
        p->selected = p->count ? p->count - 1 : 0;
    int rows = (a.h - 7) / 3;
    if (rows < 1)
        rows = 1;
    p->first = (p->selected / rows) * rows;
    p->list_area = a;
    p->previous = tui_rect_make(a.x + 1, a.y + a.h - 4, (a.w - 2) / 2, 3);
    p->next =
        tui_rect_make(p->previous.x + p->previous.w, p->previous.y, a.w - 2 - p->previous.w, 3);
    tui_box(sf, a, p->lists ? "SAVED CHANNELS / UP-DOWN SELECT" : "SCAN LIST", cyan);
    if (p->lists && p->count > rows) {
        tui_box(sf, p->previous, "< PREV", cyan);
        tui_box(sf, p->next, "NEXT >", cyan);
    }
    if (!p->count) {
        line(sf, a, 2, "No channels saved for this mode", white);
        line(sf, a, 4, "LISTS > SAVE FREQ, or choose BAND", LS_ATTR_DIM);
    }
    for (int n = 0; n < rows && p->first + n < p->count; n++) {
        int pos = p->first + n, idx = p->visible[pos];
        const scan_channel_t *c = scan_channel_get(idx);
        if (!c)
            continue;
        char text[64];
        bool current =
            scan_engine_get_source() == SCAN_SRC_CHANNELS && scan_engine_candidate() == idx;
        snprintf(text, sizeof(text), "%c %s %lu.%04lu  Z%d %s",
                 p->lists ? (pos == p->selected ? '>' : ' ') : (current ? '>' : ' '),
                 c->flags & SCAN_FLAG_ENABLED ? "[x]" : "[ ]",
                 (unsigned long)(c->freq_hz / 1000000),
                 (unsigned long)((c->freq_hz % 1000000) / 100), c->zone,
                 c->flags & SCAN_FLAG_LOCKOUT ? "LOCK" : "");
        line(sf, a, 2 + n * 3, text, (p->lists ? pos == p->selected : current) ? amber : white);
        line(sf, a, 3 + n * 3, c->name, LS_ATTR_DIM);
    }
}

static void scan_choices(tui_surface *sf, tui_rect a, bool wide)
{
    bool band = scan_engine_get_source() == SCAN_SRC_BAND;
    tui_rect list = a, range = a;
    if (wide) {
        list.w = (a.w - 2) / 2;
        range.x = list.x + list.w + 2;
        range.w = a.w - list.w - 2;
    } else {
        list.h = 10;
        range.y = list.y + 11;
        range.h = a.h - 11;
    }
    tui_box(sf, list, band ? "CHANNEL LIST" : "CHANNEL LIST / SELECTED", band ? cyan : amber);
    line(sf, list, 2, "Visit saved frequencies only", white);
    line(sf, list, 4, "Uses your enabled channels and zone", LS_ATTR_DIM);
    line(sf, list, 6, "Manage frequencies with LISTS", LS_ATTR_DIM);
    tui_box(sf, range, band ? "BAND SCAN / SELECTED" : "BAND SCAN", band ? amber : cyan);
    uint32_t lo, hi, step;
    scan_engine_get_band(&lo, &hi, &step);
    char text[64];
    line(sf, range, 2, "Sweep every frequency step", white);
    snprintf(text, sizeof(text), "FROM %.4f TO %.4f MHz", lo / 1e6, hi / 1e6);
    line(sf, range, 4, text, white);
    snprintf(text, sizeof(text), "STEP %.2f kHz / no list needed", step / 1000.0);
    line(sf, range, 6, text, amber);
    line(sf, range, 8, "RANGE chooses band / STEP changes spacing", LS_ATTR_DIM);
}

void ls_radio_panel_draw(ls_radio_panel_t *p, const ls_radio_view_t *v, tui_surface *sf, tui_rect a)
{
    bool wide = a.w > 72, active = scan_engine_active();
    if (a.w < 38 || a.h < (wide ? 20 : 34)) {
        ls_panel_notice(sf, a, "RADIO", "Open a larger radio view", "M: detailed controls");
        ls_btn_clear_hits();
        return;
    }
    a.x++;
    a.w -= 2;
    char text[96];
    if (p->scan_choice)
        snprintf(text, sizeof(text), "CHOOSE SCAN TYPE / THEN START");
    else if (active) {
        char state[64];
        scan_engine_status(state, sizeof(state));
        snprintf(text, sizeof(text), "%s / %s",
                 scan_engine_get_source() == SCAN_SRC_BAND ? "BAND SCAN" : "CHANNEL LIST", state);
    } else
        snprintf(text, sizeof(text), "%s / %s", v->mode,
                 v->receiver.receiver_streaming ? "MANUAL" : "NO RECEIVER / CONNECT USB SDR");
    tui_rect status = tui_rect_make(a.x, a.y + 1, a.w, 3);
    tui_box(sf, status, NULL, cyan);
    line(sf, status, 1,
         scan_engine_manual_hold()
             ? (scan_engine_get_source() == SCAN_SRC_BAND ? "BAND SCAN / HOLD"
                                                          : "CHANNEL LIST / HOLD")
             : text,
         amber);
    int bh = wide ? 5 : 10;
    tui_rect controls = tui_rect_make(a.x, a.y + a.h - bh - 3, a.w, bh);
    tui_rect body = tui_rect_make(a.x, a.y + 5, a.w, controls.y - a.y - 6);
    if (p->scan_choice) {
        scan_choices(sf, body, wide);
    } else if (p->lists) {
        uint32_t lo, hi, step;
        scan_engine_get_band(&lo, &hi, &step);
        snprintf(text, sizeof(text), "%s / ZONE %d / %.1fs hang",
                 scan_engine_get_source() == SCAN_SRC_BAND ? "BAND" : "CHANNELS",
                 scan_engine_get_zone(), scan_engine_get_hang_ms() / 1000.0);
        line(sf, body, 0, text, amber);
        snprintf(text, sizeof(text), "%.4f - %.4f MHz / %.1fk", lo / 1e6, hi / 1e6, step / 1000.0);
        line(sf, body, wide ? 1 : 2, text, white);
        line(sf, body, wide ? 2 : 4,
             p->notice[0] ? p->notice : "Zone -1 = all / changes stop scanning", LS_ATTR_DIM);
        body.y += wide ? 3 : 6;
        body.h -= wide ? 3 : 6;
        channel_list(p, v, sf, body);
    } else {
        tui_rect primary = body, details = body;
        if (wide) {
            primary.w = (body.w - 2) / 2;
            details.x = primary.x + primary.w + 2;
            details.w = body.w - primary.w - 2;
        } else {
            primary.h = 12;
            details.y = primary.y + 13;
            details.h = body.h - 13;
        }
        tui_box(sf, primary, v->fm ? "A / ACTIVE RECEIVER" : "P25 / PHASE 1", v->fm ? amber : cyan);
        uint32_t hz = v->receiver.effective_center_known ? (uint32_t)v->receiver.effective_center_hz
                                                         : v->frequency;
        frequency(sf, primary, 2, hz, v->fm ? amber : white);
        snprintf(text, sizeof(text), "MHz  %s", v->mode);
        line(sf, primary, 8, text, cyan);
        line(sf, primary, 10,
             !v->receiver.receiver_streaming                  ? "RECEIVER OFFLINE"
             : v->receiver.tune_state == LS_IQ_RESULT_PENDING ? "TUNE PENDING"
             : v->receiver.tune_state == LS_IQ_RESULT_FAILED  ? "TUNE FAILED"
             : v->receiver.effective_center_known             ? "EFFECTIVE RECEIVER FREQUENCY"
                                                              : "REQUESTED / NOT YET CONFIRMED",
             LS_ATTR_DIM);
        if (active && scan_engine_get_source() == SCAN_SRC_CHANNELS)
            channel_list(p, v, sf, details);
        else if(active) {
            uint32_t lo,hi,step;scan_engine_get_band(&lo,&hi,&step);
            tui_box(sf,details,"BAND SCAN / FREQUENCY RANGE",amber);
            snprintf(text,sizeof(text),"START %.4f MHz",lo/1e6);line(sf,details,2,text,white);
            snprintf(text,sizeof(text),"END   %.4f MHz",hi/1e6);line(sf,details,4,text,white);
            snprintf(text,sizeof(text),"STEP  %.2f kHz",step/1000.0);line(sf,details,6,text,amber);
            line(sf,details,8,"Every step / no saved list",LS_ATTR_DIM);
        } else {
            if (v->fm && !wide && details.h > 25) {
                tui_rect standby = details;
                standby.h = 12;
                tui_box(sf, standby, "B / STANDBY - ONE RECEIVER", cyan);
                frequency(sf, standby, 2, v->standby, cyan);
                line(sf, standby, 8, "A / B SWAPS FREQUENCY", LS_ATTR_DIM);
                line(sf, standby, 10, "NFM / WFM / AM / DATA: MORE", white);
                details.y += 13;
                details.h -= 13;
            }
            tui_box(sf, details, v->fm ? "RECEIVER" : "CHANNEL / DECODE", cyan);
            int row = 2;
            if (v->fm && wide) {
                snprintf(text, sizeof(text), "B  %.4f MHz / STANDBY", v->standby / 1e6);
                line(sf, details, row, text, cyan);
                row += 2;
            }
            for (int i = 0; i < 4; i++, row += 2)
                line(sf, details, row, v->detail[i], white);
            if (details.h > row + 4) {
                int level = v->receiver.receiver_streaming ? (int)(v->power * 100) : 0;
                if (level < 0)
                    level = 0;
                if (level > 100)
                    level = 100;
                snprintf(text, sizeof(text), "IQ LEVEL %d%%", level);
                line(sf, details, row + 1, text, LS_ATTR_DIM);
                for (int c = 0; c < details.w - 4; c++)
                    tui_put_char(sf, details, details.x + 2 + c, details.y + row + 3,
                                 c * 100 < (details.w - 4) * level ? '|' : '.', cyan);
            }
        }
    }
    buttons(p, v);
    ls_btn_bar_raised(sf, controls, p->buttons, 6, p->focus);
}

static char action(ls_radio_panel_t *p, const ls_radio_view_t *v, char c)
{
    if (c >= 'a' && c <= 'z')
        c -= 32;
    if (p->scan_choice) {
        if (c == 'M') {
            p->scan_choice = false;
            return 0;
        }
        if (c == 'C' || c == 'B') {
            scan_engine_stop();
            scan_engine_set_source(c == 'C' ? SCAN_SRC_CHANNELS : SCAN_SRC_BAND);
        } else if (c == 'S') {
            p->scan_choice = p->lists = false;
            scan_engine_start();
        } else if (c == 'I' && scan_engine_get_source() == SCAN_SRC_BAND) {
            static const uint32_t steps[] = {5000, 6250, 10000, 12500, 15000, 20000, 25000, 50000};
            uint32_t lo, hi, step;
            scan_engine_get_band(&lo, &hi, &step);
            int next = 0;
            for (int i = 0; i < 8; i++)
                if (steps[i] == step) {
                    next = (i + 1) % 8;
                    break;
                }
            scan_engine_set_band(lo, hi, steps[next]);
        } else if (c == 'R' && scan_engine_get_source() == SCAN_SRC_BAND) {
            static const uint32_t bands[][2] = {
                {150000000, 162000000}, {144000000, 148000000}, {420000000, 450000000}};
            uint32_t step;
            scan_engine_get_band(NULL, NULL, &step);
            int n = p->preset++ % 3;
            scan_engine_set_band(bands[n][0], bands[n][1], step);
        }
        return 0;
    }
    if (p->lists) {
        if (c == 'M') {
            p->lists = false;
            return 0;
        }
        if (c == 'F') {
            scan_engine_stop();
            int mode = v->fm ? SCAN_MODE_NFM : SCAN_MODE_P25;
            int zone = scan_engine_get_zone();
            if (zone < 0)
                zone = 0;
            if (v->fm && strcmp(v->mode, "NFM")) {
                snprintf(p->notice, sizeof(p->notice), "Select NFM before saving a scan channel");
                return 0;
            }
            char name[16];
            snprintf(name, sizeof(name), "%.4f MHz", v->frequency / 1e6);
            int idx = -1;
            for (int i = 0; i < scan_channels_count(); i++) {
                const scan_channel_t *saved = scan_channel_get(i);
                if (saved && saved->freq_hz == v->frequency && saved->zone == zone &&
                    saved->mode == mode) {
                    idx = i;
                    break;
                }
            }
            if (idx >= 0)
                snprintf(p->notice, sizeof(p->notice), "Already saved in zone %d", zone);
            else {
                idx = scan_channel_add(name, v->frequency, mode, zone);
                snprintf(p->notice, sizeof(p->notice),
                         idx >= 0 ? "Channel saved" : "Could not save channel / list full");
            }
        } else if (c == 'E' && p->count) {
            scan_engine_stop();
            int idx = p->visible[p->selected];
            const scan_channel_t *ch = scan_channel_get(idx);
            if (ch)
                scan_channel_set_enabled(idx, !(ch->flags & SCAN_FLAG_ENABLED));
        } else if (c == 'Z') {
            scan_engine_stop();
            int z = scan_engine_get_zone() + 1;
            scan_engine_set_zone(z >= SCAN_MAX_ZONES ? -1 : z);
        } else if (c == 'O') {
            scan_engine_stop();
            p->scan_choice = true;
            p->focus = -1;
        } else if (c == 'R') {
            static const uint32_t bands[][3] = {{150000000, 162000000, 12500},
                                                {144000000, 148000000, 12500},
                                                {420000000, 450000000, 12500}};
            scan_engine_stop();
            int n = p->preset++ % 3;
            scan_engine_set_band(bands[n][0], bands[n][1], bands[n][2]);
            scan_engine_set_source(SCAN_SRC_BAND);
            snprintf(p->notice, sizeof(p->notice), "RANGE cycles VHF / 2m / 70cm");
        }
        return 0;
    }
    if (c == 'L') {
        p->lists = true;
        p->notice[0] = 0;
    } else if (c == 'S') {
        if (!scan_engine_active()) {
            p->scan_choice = true;
            p->focus = -1;
        }
    } else if (c == 'X')
        scan_engine_stop();
    else if (c == 'H') {
        if (scan_engine_manual_hold())
            scan_engine_next();
        else
            scan_engine_hold(true);
    } else if (c == 'N')
        scan_engine_next();
    else if (c == 'K')
        scan_engine_skip();
    else
        return c;
    return 0;
}

char ls_radio_panel_key(ls_radio_panel_t *p, const ls_radio_view_t *v, ls_tk_t k, char c)
{
    if (k == LS_TK_ESC) {
        if (p->scan_choice) {
            p->scan_choice = false;
            return 0;
        }
        if (p->lists) {
            p->lists = false;
            return 0;
        }
        return 'M';
    }
    if (p->lists && !p->scan_choice && (k == LS_TK_UP || k == LS_TK_DOWN)) {
        if (p->count)
            p->selected = (p->selected + (k == LS_TK_UP ? p->count - 1 : 1)) % p->count;
        return 0;
    }
    if (k == LS_TK_CHAR)
        return action(p, v, c);
    if (k == LS_TK_ENTER && p->focus >= 0 && p->focus < 6)
        return action(p, v, p->buttons[p->focus].key);
    ls_btn_navigate(k, &p->slot, &p->focus, false);
    return 0;
}
char ls_radio_panel_touch(ls_radio_panel_t *p, const ls_radio_view_t *v, int x, int y)
{
    if (p->lists && !p->scan_choice &&
        (tui_rect_contains(p->previous, x, y) || tui_rect_contains(p->next, x, y))) {
        int rows = (p->list_area.h - 7) / 3;
        if (rows < 1)
            rows = 1;
        p->selected += tui_rect_contains(p->previous, x, y) ? -rows : rows;
        if (p->selected < 0)
            p->selected = 0;
        if (p->selected >= p->count)
            p->selected = p->count ? p->count - 1 : 0;
        return 0;
    }
    if (p->lists && !p->scan_choice && tui_rect_contains(p->list_area, x, y)) {
        int rows = (p->list_area.h - 7) / 3;
        if (rows < 1) rows = 1;
        int pos = p->first + (y - p->list_area.y - 2) / 3;
        if (y >= p->list_area.y + 2 && pos < p->first + rows && pos < p->count)
            p->selected = pos;
        return 0;
    }
    int hit = ls_btn_hit(x, y);
    return hit >= 0 && hit < 6 ? action(p, v, p->buttons[hit].key) : 0;
}
