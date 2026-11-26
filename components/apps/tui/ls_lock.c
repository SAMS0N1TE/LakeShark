/* The lock page and its PIN. See ls_lock.h. */

#include "ls_lock.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "esp_attr.h"
#include "esp_timer.h"

#include "ls_tui_screen.h"
#include "ls_tui_ui.h"
#include "ls_app.h"
#include "ls_value.h"
#include "ls_notify.h"
#include "ls_gauge.h"
#include "ls_i18n.h"
#include "ls_textpack.h"
#include "../../lakeshark/core/ls_tz.h"

#ifdef ESP_PLATFORM
#include "core/settings.h"
#include "core/display_ctl.h"
#include "esp_random.h"
#include "mbedtls/sha256.h"
#endif

/* lk_cfg: the PIN's length, and when the unit locks by itself. */
#define LK_LEN  0x0Fu
#define LK_DARK 0x10u
#define LK_BOOT 0x20u

static uint32_t s_cfg, s_salt;
static uint64_t s_hash;          /* 0: no PIN */

enum { M_UNLOCK, M_OLD, M_NEW, M_CONFIRM };
enum { A_NONE, A_CHANGE, A_REMOVE };
static uint8_t s_mode, s_after, s_fails;   /* s_fails: kept in NVS (lk_fail) over a reboot */
static char    s_in[9], s_new[9];
static uint8_t s_n, s_new_n;
static int64_t s_wait_until, s_arm_us, s_seen_us;
static const char *s_msg;

/* ------------------------------------------------------------------ store */

#ifdef ESP_PLATFORM
static void load_raw(void) { (void)settings_get_lock(&s_cfg, &s_salt, &s_hash); }
static bool save(void) { return settings_set_lock(s_cfg, s_salt, s_hash); }
static uint32_t load_fails(void) { return settings_get_lock_fails(); }
static void save_fails(void) { (void)settings_set_lock_fails(s_fails); }
static uint32_t new_salt(void) { return esp_random() | 1u; }
/* SHA-256 over salt and PIN, stretched: the NVS copy is not a short cut to
   the PIN. Kept to 64 bits, which is far past a 10^8 keyspace. */
static uint64_t pin_hash(uint32_t salt, const char *pin, int n)
{
    uint8_t in[32], out[32];
    memset(in, 0, sizeof(in));
    memcpy(in, &salt, 4);
    memcpy(in + 4, pin, (size_t)n);
    mbedtls_sha256(in, 4 + (size_t)n, out, 0);
    for (int i = 0; i < 1000; i++) {
        memcpy(in, out, 32);
        mbedtls_sha256(in, 32, out, 0);
    }
    uint64_t h;
    memcpy(&h, out, 8);
    return h ? h : 1;
}
static int  off_after(void) { return display_ctl_off_after(); }
static void set_off_after(int s) { display_ctl_set_off_after(s); }
#else
/* The bench: RAM only, and a plain hash - nothing real is ever stored. */
static void load_raw(void) {}
static bool save(void) { return true; }
static uint32_t load_fails(void) { return 0; }
static void save_fails(void) {}
static uint32_t new_salt(void) { return (uint32_t)rand() | 1u; }
static uint64_t pin_hash(uint32_t salt, const char *pin, int n)
{
    uint64_t h = 1469598103934665603ull ^ salt;
    for (int i = 0; i < n; i++) h = (h ^ (uint8_t)pin[i]) * 1099511628211ull;
    return h ? h : 1;
}
static int s_off_host = 60;
static int  off_after(void) { return s_off_host; }
static void set_off_after(int s) { s_off_host = s; }
#endif

bool ls_lock_has_pin(void) { return s_hash != 0; }
static int pin_len(void) { return (int)(s_cfg & LK_LEN); }

/* Three tries, then a wait that doubles from 30 s to four minutes. */
static void arm_wait(void)
{
    if (s_fails < 3) { s_wait_until = 0; return; }
    const int sh = s_fails - 3 > 3 ? 3 : s_fails - 3;
    s_wait_until = esp_timer_get_time() + (int64_t)(30 << sh) * 1000000;
}

/* A record no PIN can match (a length outside 4..8, or no salt) would lock
   the unit for good: it counts as no PIN. The wrong-PIN count survives a
   reboot, and so does its wait, which starts over from the boot. */
static void load(void)
{
    load_raw();
    const int n = pin_len();
    if (s_hash && (n < 4 || n > 8 || !s_salt)) {
        printf("lock: the PIN record is damaged; no PIN until a new one is set\n");
        s_hash = 0; s_salt = 0; s_cfg &= ~LK_LEN;
    }
    const uint32_t f = s_hash ? load_fails() : 0;
    s_fails = (uint8_t)(f > 200 ? 200 : f);
    arm_wait();
}

/* ------------------------------------------------------------------ logic */

static void begin(int mode, int after)
{
    s_mode = (uint8_t)mode; s_after = (uint8_t)after;
    memset(s_in, 0, sizeof(s_in)); s_n = 0;
    s_msg = NULL; s_arm_us = 0;
    s_seen_us = esp_timer_get_time();
}

static void finish(void)
{
    begin(M_UNLOCK, A_NONE);
    ls_tui_set_locked(false);
}

static bool waiting(void) { return s_wait_until && esp_timer_get_time() < s_wait_until; }
static bool setup(void) { return s_mode != M_UNLOCK; }

static void submit(void)
{
    const int mode = s_mode, after = s_after;
    const uint32_t old_cfg = s_cfg, old_salt = s_salt;
    const uint64_t old_hash = s_hash;
    if (mode == M_UNLOCK || mode == M_OLD) {
        if (!s_hash) { finish(); return; }
        if (waiting()) return;                  /* OK during the wait is not another try */
        if (s_n != pin_len() || pin_hash(s_salt, s_in, s_n) != s_hash) {
            begin(mode, after);
            s_msg = "WRONG PIN";
            if (s_fails < 200) s_fails++;
            save_fails();
            arm_wait();
            return;
        }
        if (s_fails) { s_fails = 0; save_fails(); }
        s_wait_until = 0;
        if (mode == M_UNLOCK) { finish(); return; }
        if (after == A_REMOVE) {
            s_hash = 0; s_salt = 0; s_cfg &= ~LK_LEN;
            if (!save()) {                      /* the PIN stays, as flash has it */
                s_cfg = old_cfg; s_salt = old_salt; s_hash = old_hash;
                begin(M_OLD, after); s_msg = "NOT SAVED, TRY AGAIN"; return;
            }
            finish(); return;
        }
        begin(M_NEW, after);
    } else if (mode == M_NEW) {
        if (s_n < 4) { s_msg = "4 TO 8 DIGITS"; return; }
        memcpy(s_new, s_in, sizeof(s_new)); s_new_n = s_n;
        begin(M_CONFIRM, after);
    } else if (s_n != s_new_n || memcmp(s_in, s_new, s_n)) {
        begin(M_NEW, after); s_msg = "DID NOT MATCH";
    } else {
        /* A first PIN locks on dark and at boot until told otherwise. */
        if (!s_hash) s_cfg |= LK_DARK | LK_BOOT;
        s_salt = new_salt();
        s_hash = pin_hash(s_salt, s_in, s_n);
        s_cfg = (s_cfg & ~LK_LEN) | s_n;
        memset(s_new, 0, sizeof(s_new));
        if (!save()) {                          /* the old PIN (or none) stays, as flash has it */
            s_cfg = old_cfg; s_salt = old_salt; s_hash = old_hash;
            begin(M_NEW, after); s_msg = "NOT SAVED, TRY AGAIN"; return;
        }
        finish();
    }
}

static void digit(char c)
{
    if (waiting() || s_n >= 8) return;
    s_in[s_n++] = c;
    s_msg = NULL;
    /* A known length submits itself, the way a phone does. */
    if (((s_mode == M_UNLOCK || s_mode == M_OLD) && s_hash && s_n == pin_len()) ||
        (s_mode == M_CONFIRM && s_n == s_new_n)) submit();
}

/* No PIN: one key, pressed twice within three seconds. */
static void tap_unlock(void)
{
    const int64_t now = esp_timer_get_time();
    if (s_arm_us && now - s_arm_us < 3000000) { finish(); return; }
    s_arm_us = now;
}

/* ------------------------------------------------------------------- draw */

#define K_N 13          /* 12 keys and CANCEL / UNLOCK */
static tui_rect s_hit[K_N];
static const char KEYS[12][4] = { "1","2","3","4","5","6","7","8","9","DEL","0","OK" };

static void centre(tui_surface *sf, tui_rect clip, int y, const char *s, uint8_t at)
{
    tui_put_str(sf, clip, clip.x + (clip.w - (int)strlen(s)) / 2, y, s, at);
}

/* Seven segments, five by five: a b c d e f g. */
static const uint8_t SEG[10] = { 0x3F, 0x06, 0x5B, 0x4F, 0x66, 0x6D, 0x7D, 0x07, 0x7F, 0x6F };

static void big_digit(tui_surface *sf, tui_rect clip, int x, int y, int d, uint8_t at)
{
    const uint8_t m = d >= 0 ? SEG[d] : 0x40;
    for (int r = 0; r < 5; r += 2)          /* a, g, d: the rules */
        if (m & (r == 0 ? 0x01 : r == 2 ? 0x40 : 0x08)) tui_hline(sf, clip, x + 1, y + r, 3, '_', at);
    for (int r = 1; r <= 4; r++) {          /* f b above, e c below */
        if (m & (r <= 2 ? 0x20 : 0x10)) tui_put_char(sf, clip, x, y + r, '|', at);
        if (m & (r <= 2 ? 0x02 : 0x04)) tui_put_char(sf, clip, x + 4, y + r, '|', at);
    }
}

/* The clock, the date and what the unit is doing: 13 rows. */
static void draw_info(tui_surface *sf, tui_rect a)
{
    const uint8_t good = ls_ink(LS_INK_GOOD, TUI_ATTR(TUI_GREEN | TUI_BRIGHT, TUI_BLACK));
    const uint8_t warn = ls_ink(LS_INK_WARN, TUI_ATTR(TUI_YELLOW | TUI_BRIGHT, TUI_BLACK));
    const uint8_t rule = ls_ink(LS_INK_RULE, TUI_ATTR(TUI_CYAN, TUI_BLACK));
    ls_rframe(sf, a, a, '-', rule);

    /* The router's clock text says whether the time is real. */
    const char *clk = ls_tui_status_clock();
    const bool synced = clk && clk[0] >= '0' && clk[0] <= '9';
    const time_t now = time(NULL);
    struct tm local;
    const struct tm *tm = synced && ls_tz_local(now, &local) ? &local : NULL;   /* the status bar's zone */
    const bool h12 = !ls_tz_24h();
    const int cx = a.x + (a.w - 25) / 2, cy = a.y + 1;
    const int hm[2] = { tm ? (h12 ? (tm->tm_hour % 12 ? tm->tm_hour % 12 : 12) : tm->tm_hour) : -1,
                        tm ? tm->tm_min : -1 };
    for (int i = 0; i < 4; i++) {
        const int v = hm[i / 2];
        if (i == 0 && h12 && v >= 0 && v < 10) continue;   /* " 4:34", as a clock reads */
        big_digit(sf, a, cx + i * 6 + (i / 2) * 2, cy, v < 0 ? -1 : i & 1 ? v % 10 : v / 10,
                  ls_ink(LS_INK_STRONG, TUI_ATTR(TUI_WHITE | TUI_BRIGHT, TUI_BLACK)));
    }
    tui_put_str(sf, a, cx + 12, cy + 2, "o", ls_ink(LS_INK_ACCENT, TUI_ATTR(TUI_CYAN | TUI_BRIGHT, TUI_BLACK)));
    tui_put_str(sf, a, cx + 12, cy + 4, "o", ls_ink(LS_INK_ACCENT, TUI_ATTR(TUI_CYAN | TUI_BRIGHT, TUI_BLACK)));

    char line[48];
    char zone[12];
    if (tm) ls_tz_abbr(tm, zone, sizeof(zone));
    if (!tm || !strftime(line, sizeof(line), h12 ? "%a %d %b %Y  %p" : "%a %d %b %Y", tm))
        snprintf(line, sizeof(line), "%s", ls_tr(LS_TEXT(t_ls_lock_no_time, "time not set yet")));
    else snprintf(line + strlen(line), sizeof(line) - strlen(line), " %s", zone);
    centre(sf, a, cy + 6, line, LS_ATTR_DIM);
    tui_hline(sf, a, a.x + 2, cy + 7, a.w - 4, '-', rule);

    /* Battery, GPS, messages: each in the colour of its state. */
    char t[3][20];
    uint8_t at[3] = { LS_ATTR_DIM, LS_ATTR_DIM, LS_ATTR_DIM };
    ls_gauge_t gg;
    if (ls_gauge_present() && ls_gauge_get(&gg)) {
        if (gg.full) snprintf(t[0], sizeof(t[0]), "BAT FULL");
        else snprintf(t[0], sizeof(t[0]), "BAT %u%%%s", (unsigned)gg.percent,
                      gg.charging ? " CHG" : gg.plugged ? " USB" : "");
        at[0] = gg.plugged || gg.percent > 40 ? good : gg.percent > 15 ? warn
              : ls_ink(LS_INK_BAD, TUI_ATTR(TUI_RED | TUI_BRIGHT, TUI_BLACK));
    } else snprintf(t[0], sizeof(t[0]), "%s", ls_tr("USB POWER"));
    ls_val_t v;
    const bool gps_on = ls_value_read("gps.on", &v, NULL) && v.i;
    if (gps_on && ls_value_read("gps.fix", &v, NULL) && v.i) {
        snprintf(t[1], sizeof(t[1]), "GPS FIX %d SAT", ls_value_read("gps.sats", &v, NULL) ? (int)v.i : 0);
        at[1] = good;
    } else {
        snprintf(t[1], sizeof(t[1]), "%s", ls_tr(gps_on ? "GPS SEARCHING" : "GPS OFF"));
        if (gps_on) at[1] = warn;
    }
    const int unread = ls_notify_unread();
    if (unread > 0) { snprintf(t[2], sizeof(t[2]), "%d MSG", unread > 99 ? 99 : unread); at[2] = warn; }
    else snprintf(t[2], sizeof(t[2]), "%s", ls_tr("NO MSG"));
    int x = a.x + (a.w - (int)(strlen(t[0]) + strlen(t[1]) + strlen(t[2])) - 6) / 2;
    for (int i = 0; i < 3; i++) {
        tui_put_str(sf, a, x, cy + 8, t[i], at[i]);
        x += (int)strlen(t[i]) + 3;
    }

    /* What keeps running under the lock, from the apps' own live lamps. */
    int n = snprintf(line, sizeof(line), "*");
    for (int i = 0; i < ls_app_count() && n < (int)sizeof(line) - 12; i++) {
        const ls_app_t *ap = ls_app_at(i);
        if (ap && ap->live && ap->live() && ap->name)
            n += snprintf(line + n, sizeof(line) - n, "  %.10s", ap->name);
    }
    if (n > 1) centre(sf, a, cy + 10, line, good);
    else centre(sf, a, cy + 10, ls_tr("radios idle"), LS_ATTR_DIM);
}

static void key_box(tui_surface *sf, tui_rect clip, tui_rect r, const char *label, uint8_t hue)
{
    ls_rframe(sf, clip, r, '-', TUI_ATTR(hue, TUI_BLACK));
    tui_put_str(sf, clip, r.x + (r.w - (int)strlen(label)) / 2, r.y + (r.h - 1) / 2, label,
                TUI_ATTR(TUI_WHITE | TUI_BRIGHT, TUI_BLACK));
}

static void draw_pad(tui_surface *sf, tui_rect a)
{
    memset(s_hit, 0, sizeof(s_hit));
    const int64_t now = esp_timer_get_time();

    if (!s_hash && !setup()) {
        /* No PIN: a pocket guard, not a lock. */
        const bool armed = s_arm_us && now - s_arm_us < 3000000;
        const int h = a.h > 11 ? 7 : 3;
        const tui_rect k = tui_rect_make(a.x + 2, a.y + a.h - h - 2, a.w - 4, h);
        centre(sf, a, k.y - 2, armed ? ls_tr(LS_TEXT(t_ls_lock_tap_again, "tap again to unlock"))
                                     : ls_tr(LS_TEXT(t_ls_lock_tap_twice, "tap twice to unlock")), LS_ATTR_DIM);
        key_box(sf, a, k, ls_tr(armed ? "TAP AGAIN" : "UNLOCK"), TUI_GREEN);
        s_hit[12] = k;
        return;
    }

    /* The keys sit at the bottom, under the thumb; the entry just above. */
    const int cancel_h = setup() ? 3 : 0;
    int kh = (a.h - 6 - cancel_h) / 4;
    if (kh > 7) kh = 7;
    if (kh < 3) kh = 3;
    const int top = a.y + a.h - cancel_h - 4 * kh;
    const int py = top - 6 > a.y ? top - 6 : a.y;

    const char *prompt = s_mode == M_OLD ? LS_TEXT(t_ls_lock_current, "CURRENT PIN")
                       : s_mode == M_NEW ? LS_TEXT(t_ls_lock_new, "NEW PIN, 4 TO 8 DIGITS")
                       : s_mode == M_CONFIRM ? LS_TEXT(t_ls_lock_again, "THE NEW PIN AGAIN")
                       : LS_TEXT(t_ls_lock_enter, "ENTER PIN");
    centre(sf, a, py, ls_tr(prompt), TUI_ATTR(TUI_WHITE | TUI_BRIGHT, TUI_BLACK));

    /* The entry: one cell per digit, the PIN's own length when known. */
    const int slots = setup() ? (s_mode == M_CONFIRM ? s_new_n : s_mode == M_NEW ? 8 : pin_len()) : pin_len();
    char dots[32];
    int dn = 0;
    for (int i = 0; i < slots && i < 8; i++) {
        dots[dn++] = i < s_n ? '*' : '_';
        dots[dn++] = ' '; dots[dn++] = ' ';
    }
    dots[dn ? dn - 2 : 0] = 0;
    centre(sf, a, py + 2, dots, ls_ink(LS_INK_ACCENT, TUI_ATTR(TUI_CYAN | TUI_BRIGHT, TUI_BLACK)));

    if (waiting()) {
        char msg[32];
        snprintf(msg, sizeof(msg), ls_tr("TRY AGAIN IN %d s"), (int)((s_wait_until - now + 999999) / 1000000));
        centre(sf, a, py + 4, msg, ls_ink(LS_INK_WARN, TUI_ATTR(TUI_YELLOW | TUI_BRIGHT, TUI_BLACK)));
    } else if (s_msg)
        centre(sf, a, py + 4, ls_tr(s_msg), ls_ink(LS_INK_BAD, TUI_ATTR(TUI_RED | TUI_BRIGHT, TUI_BLACK)));

    /* Three across, four down. */
    int kw = (a.w - 4) / 3;
    if (kw > 18) kw = 18;
    const int x0 = a.x + (a.w - kw * 3 - 2) / 2;
    for (int i = 0; i < 12; i++) {
        s_hit[i] = tui_rect_make(x0 + (i % 3) * (kw + 1), top + (i / 3) * kh, kw, kh);
        key_box(sf, a, s_hit[i], ls_tr(KEYS[i]), i == 9 ? TUI_YELLOW
                                                : waiting() ? (TUI_BLACK | TUI_BRIGHT) : i == 11 ? TUI_GREEN : TUI_CYAN);
    }
    if (cancel_h) {
        s_hit[12] = tui_rect_make(x0, top + 4 * kh, kw * 3 + 2, cancel_h);
        key_box(sf, a, s_hit[12], ls_tr("CANCEL"), TUI_MAGENTA);
    }
}

static void draw(tui_surface *sf, tui_rect area)
{
    const int64_t now = esp_timer_get_time();
    /* Frames stopped for a while: this is a new lock, not the old one. */
    if (now - s_seen_us > 3000000) begin(M_UNLOCK, A_NONE);
    s_seen_us = now;
    if (s_wait_until && now >= s_wait_until) s_wait_until = 0;

    tui_rect a = tui_rect_make(area.x + 1, area.y + 1, area.w - 2, area.h - 2);
    if (a.w < 30 || a.h < 16) {
        memset(s_hit, 0, sizeof(s_hit));
        centre(sf, area, area.y + area.h / 2, ls_tr("LOCKED"), TUI_ATTR(TUI_CYAN | TUI_BRIGHT, TUI_BLACK));
        return;
    }
    if (ls_tui_is_wide() && a.w >= 70) {
        /* Side by side: the clock left, the keys right. */
        const int lw = a.w / 2 - 1, ih = a.h > 15 ? 13 : a.h;
        tui_rect info = tui_rect_make(a.x, a.y + (a.h - ih) / 2, lw, ih);
        tui_rect pad = tui_rect_make(a.x + lw + 2, a.y, a.w - lw - 2, a.h);
        /* A camera hole at the glass edge: the side it sits on stops short of it. */
        tui_rect hole;
        if (ls_tui_cutout(&hole)) {
            if (hole.x > pad.x && hole.x < pad.x + pad.w) pad.w = hole.x - 1 - pad.x;
            else if (hole.x + hole.w > info.x && hole.x < info.x + info.w) {
                const int d = hole.x + hole.w + 1 - info.x;
                info.x += d; info.w -= d;
            }
        }
        draw_info(sf, info);
        draw_pad(sf, pad);
        return;
    }
    /* A camera hole over the top of the page: start under it. */
    tui_rect hole;
    if (ls_tui_cutout(&hole) && hole.y + hole.h > a.y && hole.y < a.y + 4) {
        const int d = hole.y + hole.h - a.y;
        a.y += d; a.h -= d;
    }
    /* A third of the spare room above the clock, the rest over the keys. */
    const int ih = 13;
    int sp = (a.h - ih - 2 - 36) / 3;
    if (sp < 0) sp = 0;
    a.y += sp; a.h -= sp;
    draw_info(sf, tui_rect_make(a.x, a.y, a.w, ih));
    /* The padlock in the room between, in the colour of how it stands. */
    static const char PADLOCK[6][9] = {
        "  .--.  ", " / .. \\ ", "_|____|_", "|  ()  |", "|  ||  |", "'------'" };
    const int rem = a.h - ih - 2 - 37;
    if (rem >= 8) {
        const uint8_t at = ls_ink_hue(TUI_ATTR(waiting() ? TUI_YELLOW | TUI_BRIGHT
                                             : s_msg ? TUI_RED | TUI_BRIGHT : TUI_CYAN, TUI_BLACK));
        for (int r = 0; r < 6; r++)
            tui_put_str(sf, a, a.x + (a.w - 8) / 2, a.y + ih + 1 + (rem - 6) / 2 + r, PADLOCK[r], at);
    }
    draw_pad(sf, tui_rect_make(a.x, a.y + ih + 2, a.w, a.h - ih - 2));
}

/* ------------------------------------------------------------------ input */

static void press(int i)
{
    if (i == 12) { if (setup()) finish(); else tap_unlock(); }
    else if (i == 9) { if (s_n) s_in[--s_n] = 0; }
    else if (i == 11) submit();
    else digit(KEYS[i][0]);
}

static bool touch(int col, int row)
{
    for (int i = 0; i < K_N; i++)
        if (s_hit[i].w && tui_rect_contains(s_hit[i], col, row)) { press(i); break; }
    return true;
}

static bool key(ls_tk_t k, char ch)
{
    if (!s_hash && !setup()) { if (k == LS_TK_ENTER || k == LS_TK_CHAR) tap_unlock(); }
    else if (k == LS_TK_CHAR && ch >= '0' && ch <= '9') digit(ch);
    else if (k == LS_TK_BACKSPACE || k == LS_TK_DELETE) press(9);
    else if (k == LS_TK_ENTER) submit();
    else if (k == LS_TK_ESC) { if (setup()) finish(); else begin(M_UNLOCK, A_NONE); }
    return true;
}

static const ls_tui_lock_page_t PAGE = { draw, touch, key };

/* ------------------------------------------------------------ the public */

static void lock_in(int mode, int after)
{
    begin(mode, after);
    ls_tui_set_locked(true);
}

void ls_lock_init(void)
{
    load();
    ls_tui_set_lock_page(&PAGE);
    if (s_hash && (s_cfg & LK_BOOT)) lock_in(M_UNLOCK, A_NONE);
}

/* Dark in the middle of changing the PIN cancels the change. */
void ls_lock_dark(void) { if (s_hash && (s_cfg & LK_DARK)) lock_in(M_UNLOCK, A_NONE); }

void ls_lock_pin_show(char *b, size_t n)
{
    if (s_hash) snprintf(b, n, ls_tr("%d digits, change"), pin_len());
    else snprintf(b, n, "%s", ls_tr("none, set one"));
}

void ls_lock_pin_edit(void) { lock_in(s_hash ? M_OLD : M_NEW, A_CHANGE); }
void ls_lock_pin_remove(void) { if (s_hash) lock_in(M_OLD, A_REMOVE); }

static const uint8_t WHEN[4] = { LK_DARK | LK_BOOT, LK_DARK, LK_BOOT, 0 };

static int when_index(void)
{
    int i = 0;
    while (i < 3 && (s_cfg & (LK_DARK | LK_BOOT)) != WHEN[i]) i++;
    return i;
}

void ls_lock_when_show(char *b, size_t n)
{
    static const char *const NAME[4] = { "screen off + boot", "screen off", "at boot", "manual only" };
    snprintf(b, n, "%s", ls_tr(s_hash ? NAME[when_index()] : "set a PIN first"));
}

void ls_lock_when_step(int dir)
{
    if (!s_hash) return;
    const uint32_t was = s_cfg;
    s_cfg = (s_cfg & ~(LK_DARK | LK_BOOT)) | WHEN[(when_index() + (dir < 0 ? 3 : 1)) % 4];
    if (!save()) s_cfg = was;                   /* the row keeps showing what flash has */
}

void ls_lock_when_next(void) { ls_lock_when_step(+1); }

static const uint16_t OFFS[7] = { 0, 15, 30, 60, 120, 300, 600 };

void ls_lock_off_show(char *b, size_t n)
{
    const int s = off_after();
    if (!s) snprintf(b, n, "%s", ls_tr("never"));
    else snprintf(b, n, ls_tr(s < 60 ? "%d s after dim" : "%d min after dim"), s < 60 ? s : s / 60);
}

void ls_lock_off_step(int dir)
{
    int i = 0;
    while (i < 6 && OFFS[i] < off_after()) i++;
    set_off_after(OFFS[(i + (dir < 0 ? 6 : 1)) % 7]);
}

void ls_lock_off_next(void) { ls_lock_off_step(+1); }
