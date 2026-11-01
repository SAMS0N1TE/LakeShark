#include "ble_hid_input.h"
#include <limits.h>
#include <string.h>
static bool bit(const uint8_t *b, unsigned u)
{
    return (b[u / 8] >> (u % 8)) & 1;
}
static uint8_t mods(const uint8_t *k)
{
    return (bit(k, 0xe1) || bit(k, 0xe5) ? LS_IMOD_SHIFT : 0) |
           (bit(k, 0xe0) || bit(k, 0xe4) ? LS_IMOD_CTRL : 0) |
           (bit(k, 0xe2) || bit(k, 0xe6) ? LS_IMOD_ALT : 0) |
           (bit(k, 0xe3) || bit(k, 0xe7) ? LS_IMOD_META : 0);
}
static ls_input_ev_t translate(unsigned u, uint8_t m, bool caps)
{
    ls_input_ev_t e = {.mods = m, .clicks = 1, .key = LS_TK_HID_USAGE_BASE + u};
    bool shift = m & LS_IMOD_SHIFT;
    if (u >= 4 && u <= 29) {
        e.key = LS_TK_CHAR;
        e.ch = 'a' + u - 4;
        if (shift != caps)
            e.ch -= 'a' - 'A';
    } else if (u >= 30 && u <= 39) {
        e.key = LS_TK_CHAR;
        e.ch = (shift ? "!@#$%^&*()" : "1234567890")[u - 30];
    } else if (u >= 0x2d && u <= 0x38) {
        e.key = LS_TK_CHAR;
        e.ch = (shift ? "_+{}|~:\"~<>?" : "-=[]\\~;'`,./")[u - 0x2d];
        if (u == 0x32) {
            e.key = LS_TK_HID_USAGE_BASE + u;
            e.ch = 0;
        }
    } else if (u >= 0x3a && u <= 0x44)
        e.key = LS_TK_F1 + u - 0x3a;
    else
        switch (u) {
        case 0x28:
            e.key = LS_TK_ENTER;
            break;
        case 0x29:
            e.key = LS_TK_ESC;
            break;
        case 0x2a:
            e.key = LS_TK_BACKSPACE;
            break;
        case 0x2b:
            e.key = LS_TK_TAB;
            break;
        case 0x2c:
            e.key = LS_TK_CHAR;
            e.ch = ' ';
            break;
        case 0x39:
            e.key = LS_TK_CAPS_LOCK;
            break;
        case 0x45:
            e.key = LS_TK_F12;
            break;
        case 0x49:
            e.key = LS_TK_INSERT;
            break;
        case 0x4a:
            e.key = LS_TK_HOME;
            break;
        case 0x4b:
            e.key = LS_TK_PGUP;
            break;
        case 0x4c:
            e.key = LS_TK_DELETE;
            break;
        case 0x4d:
            e.key = LS_TK_END;
            break;
        case 0x4e:
            e.key = LS_TK_PGDN;
            break;
        case 0x4f:
            e.key = LS_TK_RIGHT;
            break;
        case 0x50:
            e.key = LS_TK_LEFT;
            break;
        case 0x51:
            e.key = LS_TK_DOWN;
            break;
        case 0x52:
            e.key = LS_TK_UP;
            break;
        case 0x53:
            e.key = LS_TK_NUM_LOCK;
            break;
        case 0xe0:
        case 0xe4:
            e.key = LS_TK_CTRL;
            break;
        case 0xe1:
        case 0xe5:
            e.key = LS_TK_SHIFT;
            break;
        case 0xe2:
        case 0xe6:
            e.key = LS_TK_ALT;
            break;
        case 0xe3:
        case 0xe7:
            e.key = LS_TK_META;
            break;
        default:
            break;
        }
    return e;
}
static void emit(hid_input_t *s, ls_input_ev_t e, hid_input_sink_t sink, void *ctx)
{
    e.source = LS_ISRC_BLE;
    e.seq = ++s->seq;
    if (!sink(&e, ctx)) {
        s->dropped++;
        s->resync = true;
    }
}
static bool repeatable(unsigned u)
{
    return (u >= 4 && u <= 0x38) || (u >= 0x49 && u <= 0x52);
}
static uint16_t media(unsigned u)
{
    switch (u) {
    case 0xcd:
        return LS_TK_MEDIA_PLAY;
    case 0xb7:
        return LS_TK_MEDIA_STOP;
    case 0xb5:
        return LS_TK_MEDIA_NEXT;
    case 0xb6:
        return LS_TK_MEDIA_PREV;
    case 0xe9:
        return LS_TK_VOLUME_UP;
    case 0xea:
        return LS_TK_VOLUME_DOWN;
    case 0xe2:
        return LS_TK_MUTE;
    default:
        return 0;
    }
}
static void merge(hid_input_t *s, uint32_t now, hid_input_sink_t sink, void *ctx)
{
    uint8_t next[32] = {0};
    for (unsigned p = 0; p < 2; p++)
        for (unsigned r = 0; r < HID_REPORTS_MAX; r++)
            for (unsigned i = 0; i < 32; i++)
                next[i] |= s->keys[p][r][i];
    uint8_t m = mods(next);
    for (unsigned u = 1; u < 256; u++)
        if (bit(next, u) != bit(s->held, u)) {
            bool down = bit(next, u);
            if (down && u == 0x39)
                s->caps = !s->caps;
            ls_input_ev_t e = down ? translate(u, m, s->caps) : s->pressed[u];
            e.mods = m;
            e.kind = down ? LS_IEV_KEY_DOWN : LS_IEV_KEY_UP;
            e.clicks = 1;
            if (down)
                s->pressed[u] = e;
            s->next_repeat[u] = down && repeatable(u) ? now + 400 : 0;
            if (e.key)
                emit(s, e, sink, ctx);
        }
    memcpy(s->held, next, 32);
}
static int32_t clamp(int64_t v, int32_t max)
{
    return v < 0 ? 0 : v > max ? max : (int32_t)v;
}
void hid_input_report(hid_input_t *s, unsigned p, unsigned r, const hid_sample_t *v, uint32_t now,
                      int cols, int rows, int cw, int ch, hid_input_sink_t sink, void *ctx)
{
    if (p >= 2 || r >= HID_REPORTS_MAX || v->rollover)
        return;
    memcpy(s->keys[p][r], v->keys, 32);
    merge(s, now, sink, ctx);
    /* Consumer transitions are merged across all reports and peers. */
    for (unsigned pass = 0; pass < 2; pass++)
        for (unsigned k = 0; k < 8; k++) {
            uint16_t u = pass ? v->consumer[k] : s->consumer[p][r][k];
            if (!u || !media(u))
                continue;
            bool duplicate = false;
            for (unsigned prior = 0; prior < k; prior++)
                if ((pass ? v->consumer[prior] : s->consumer[p][r][prior]) == u)
                    duplicate = true;
            if (duplicate)
                continue;
            bool old = false, newer = false;
            for (unsigned a = 0; a < 2; a++)
                for (unsigned b = 0; b < HID_REPORTS_MAX; b++)
                    for (unsigned c = 0; c < 8; c++) {
                        if (s->consumer[a][b][c] == u)
                            old = true;
                        if (a != p || b != r) {
                            if (s->consumer[a][b][c] == u)
                                newer = true;
                        }
                    }
            for (unsigned c = 0; c < 8; c++)
                if (v->consumer[c] == u)
                    newer = true;
            if (old != newer && (pass ? newer : old))
                emit(s,
                     (ls_input_ev_t){.kind = newer ? LS_IEV_KEY_DOWN : LS_IEV_KEY_UP,
                                     .key = media(u),
                                     .mods = mods(s->held),
                                     .clicks = 1},
                     sink, ctx);
        }
    memcpy(s->consumer[p][r], v->consumer, sizeof(v->consumer));
    if (!v->pointer)
        return;
    s->buttons[p][r] = v->buttons;
    uint8_t buttons = 0;
    for (unsigned a = 0; a < 2; a++)
        for (unsigned b = 0; b < HID_REPORTS_MAX; b++)
            buttons |= s->buttons[a][b];
    /* HID is left,right,middle; contract is left,middle,right. */
    buttons = (buttons & ~6) | ((buttons & 2) << 1) | ((buttons & 4) >> 1);
    if (cols < 1 || rows < 1 || cw < 1 || ch < 1) {
        cols = s->cols > 0 ? s->cols : 1;
        rows = s->rows > 0 ? s->rows : 1;
        cw = s->cw > 0 ? s->cw : 1;
        ch = s->ch > 0 ? s->ch : 1;
    }
    s->cols = cols;
    s->rows = rows;
    s->cw = cw;
    s->ch = ch;
    /* Another device cannot move the pointer during an active drag. */
    bool other = false;
    for (unsigned b = 0; b < HID_REPORTS_MAX; b++)
        other |= s->buttons[1 - p][b] != 0;
    if (!other) {
        s->x = clamp((int64_t)s->x + v->x, (int64_t)cols * cw - 1);
        s->y = clamp((int64_t)s->y + v->y, (int64_t)rows * ch - 1);
    }
    ls_input_ev_t e = {.px = s->x,
                       .py = s->y,
                       .col = s->x / cw,
                       .row = s->y / ch,
                       .buttons = buttons,
                       .mods = mods(s->held),
                       .clicks = 1};
    if ((v->x || v->y) && !other) {
        e.kind = LS_IEV_PTR_MOVE;
        emit(s, e, sink, ctx);
    }
    for (unsigned b = 0; b < 8; b++)
        if ((buttons ^ s->held_buttons) & (1 << b)) {
            e.kind = buttons & (1 << b) ? LS_IEV_PTR_DOWN : LS_IEV_PTR_UP;
            e.button = b;
            emit(s, e, sink, ctx);
        }
    s->held_buttons = buttons;
    if (v->wheel) {
        e.kind = LS_IEV_WHEEL;
        int64_t wheel = -(int64_t)v->wheel;
        e.wheel = wheel > INT16_MAX ? INT16_MAX : wheel < INT16_MIN ? INT16_MIN : wheel;
        emit(s, e, sink, ctx);
    }
}
void hid_input_release(hid_input_t *s, unsigned p, uint32_t now, hid_input_sink_t sink, void *ctx)
{
    if (p >= 2)
        return;
    hid_sample_t empty = {.pointer = true};
    for (unsigned r = 0; r < HID_REPORTS_MAX; r++)
        hid_input_report(s, p, r, &empty, now, s->cols, s->rows, s->cw, s->ch, sink, ctx);
}
void hid_input_repeat(hid_input_t *s, uint32_t now, hid_input_sink_t sink, void *ctx)
{
    if (s->resync) {
        ls_input_ev_t e = {.kind = LS_IEV_RELEASE_ALL, .source = LS_ISRC_BLE, .seq = ++s->seq};
        if (!sink(&e, ctx))
            return;
        s->resync = false;
        memset(s->held, 0, 32);
        memset(s->next_repeat, 0, sizeof(s->next_repeat));
        merge(s, now, sink, ctx);
        if (s->cw > 0 && s->ch > 0)
            for (unsigned b = 0; b < 8; b++)
                if (s->held_buttons & (1 << b)) {
                    e = (ls_input_ev_t){.kind = LS_IEV_PTR_DOWN,
                                        .button = b,
                                        .buttons = s->held_buttons,
                                        .px = s->x,
                                        .py = s->y,
                                        .col = s->x / s->cw,
                                        .row = s->y / s->ch,
                                        .clicks = 1};
                    emit(s, e, sink, ctx);
                }
    }
    for (unsigned u = 0; u < 256; u++)
        if (s->next_repeat[u] && (int32_t)(now - s->next_repeat[u]) >= 0) {
            ls_input_ev_t e = translate(u, mods(s->held), s->caps);
            e.kind = LS_IEV_KEY_DOWN;
            e.clicks = 0;
            emit(s, e, sink, ctx);
            s->next_repeat[u] = now + 45;
        }
}
