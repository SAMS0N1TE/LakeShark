/* lssim --term: the whole UI, live, in the terminal lssim runs in.

   The screens are the firmware's own, fed by the simulator's fake state;
   the cells go out through ls_vt_vt.c, the same renderer the board uses
   for its serial and network terminal. Keys and mouse clicks drive the
   router as the keyboard and the glass do. Ctrl-] quits, Ctrl-L repaints.

   Works in Windows Terminal (and over SSH into Windows, which is a console
   too) and in any POSIX terminal. The sextants need a font that has them:
   Cascadia Mono as bundled with Windows Terminal does; LSSIM_QUADRANTS=1
   draws them as quadrants for one that does not. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#include <fcntl.h>
#include <io.h>
#else
#include <sys/select.h>
#include <termios.h>
#include <unistd.h>
#endif

#include "ls_vt.h"
#include "ls_input_ev.h"
#include "desk/ls_desk.h"
#include "ls_theme.h"
#include "ls_tui.h"
#include "ls_tui_screen.h"
#include "ls_lap_sys.h"

static void out_stdout(void *ctx, const char *b, size_t n)
{
    (void)ctx;
    fwrite(b, 1, n, stdout);
}

#ifdef _WIN32
static HANDLE s_in;
static DWORD s_in_mode, s_out_mode;
static UINT s_cp;
static bool s_console;

static void tty_raw(void)
{
    s_in = GetStdHandle(STD_INPUT_HANDLE);
    HANDLE out = GetStdHandle(STD_OUTPUT_HANDLE);
    s_console = GetConsoleMode(s_in, &s_in_mode);
    if (s_console)   /* VT input; no line editing, echo or Ctrl-C handling; no QuickEdit selection */
        SetConsoleMode(s_in, ENABLE_VIRTUAL_TERMINAL_INPUT | ENABLE_EXTENDED_FLAGS | ENABLE_WINDOW_INPUT);
    if (GetConsoleMode(out, &s_out_mode))
        SetConsoleMode(out, s_out_mode | ENABLE_PROCESSED_OUTPUT | ENABLE_VIRTUAL_TERMINAL_PROCESSING |
                            DISABLE_NEWLINE_AUTO_RETURN);
    s_cp = GetConsoleOutputCP();
    SetConsoleOutputCP(CP_UTF8);
}

static void tty_restore(void)
{
    if (s_console) SetConsoleMode(s_in, s_in_mode);
    SetConsoleMode(GetStdHandle(STD_OUTPUT_HANDLE), s_out_mode);
    SetConsoleOutputCP(s_cp);
}

/* What has arrived, without waiting. */
static size_t tty_read(uint8_t *buf, size_t cap)
{
    size_t n = 0;
    if (s_console) {
        DWORD waiting = 0;
        while (n + 4 < cap && GetNumberOfConsoleInputEvents(s_in, &waiting) && waiting) {
            INPUT_RECORD rec[32];
            DWORD got = 0;
            if (!ReadConsoleInputW(s_in, rec, 32, &got)) break;
            for (DWORD i = 0; i < got && n + 4 < cap; i++) {
                if (rec[i].EventType != KEY_EVENT || !rec[i].Event.KeyEvent.bKeyDown) continue;
                const WCHAR w = rec[i].Event.KeyEvent.uChar.UnicodeChar;
                if (!w) continue;
                for (WORD r = 0; r < rec[i].Event.KeyEvent.wRepeatCount && n + 4 < cap; r++)
                    n += (size_t)WideCharToMultiByte(CP_UTF8, 0, &w, 1, (char *)buf + n, 4, NULL, NULL);
            }
        }
    } else {
        DWORD avail = 0, got = 0;
        if (PeekNamedPipe(s_in, NULL, 0, NULL, &avail, NULL) && avail &&
            ReadFile(s_in, buf, (DWORD)(avail < cap ? avail : cap), &got, NULL))
            n = got;
    }
    return n;
}

static void nap(int ms) { Sleep((DWORD)ms); }
#else
static struct termios s_tio;
static bool s_tty;

static void tty_raw(void)
{
    s_tty = tcgetattr(0, &s_tio) == 0;
    if (s_tty) {
        struct termios raw = s_tio;
        cfmakeraw(&raw);
        tcsetattr(0, TCSANOW, &raw);
    }
}

static void tty_restore(void)
{
    if (s_tty) tcsetattr(0, TCSANOW, &s_tio);
}

static size_t tty_read(uint8_t *buf, size_t cap)
{
    fd_set fds;
    FD_ZERO(&fds);
    FD_SET(0, &fds);
    struct timeval tv = {0, 0};
    if (select(1, &fds, NULL, NULL, &tv) <= 0) return 0;
    const ssize_t n = read(0, buf, cap);
    return n > 0 ? (size_t)n : 0;
}

static void nap(int ms) { usleep((useconds_t)ms * 1000); }
#endif

/* The desk takes the whole mouse: SGR reports become desktop input events
   (every button, drags, wheel, hover with ?1003), and are taken out of the
   bytes before ls_vt sees them, which knows only the left button. A
   sequence split across reads waits in `hold`. */
static uint8_t s_hold[32];
static size_t s_hold_n;
static uint8_t s_held_buttons;

static size_t desk_mouse(uint8_t *b, size_t n)
{
    static uint8_t buf[256 + sizeof(s_hold)];
    size_t m = 0;
    memcpy(buf, s_hold, s_hold_n);
    memcpy(buf + s_hold_n, b, n);
    m = s_hold_n + n;
    s_hold_n = 0;
    size_t o = 0;
    for (size_t i = 0; i < m;) {
        if (buf[i] == 0x1B && (i + 1 >= m || buf[i + 1] == '[') && (i + 2 >= m || buf[i + 2] == '<')) {
            size_t j = i + 3;
            while (j < m && buf[j] != 'M' && buf[j] != 'm' && j - i < 24) j++;
            if (j >= m) {                              /* not all here yet */
                if (m - i < sizeof(s_hold)) { memcpy(s_hold, buf + i, m - i); s_hold_n = m - i; }
                break;
            }
            int btn = 0, x = 0, y = 0;
            char tmp[32];
            const size_t len = j - (i + 3) < sizeof(tmp) - 1 ? j - (i + 3) : sizeof(tmp) - 1;
            memcpy(tmp, buf + i + 3, len);
            tmp[len] = 0;
            if (sscanf(tmp, "%d;%d;%d", &btn, &x, &y) == 3) {
                ls_input_ev_t e = {.source = LS_ISRC_TERM, .col = (int16_t)(x - 1), .row = (int16_t)(y - 1), .clicks = 1};
                e.mods = (uint8_t)(((btn & 4) ? LS_IMOD_SHIFT : 0) | ((btn & 8) ? LS_IMOD_ALT : 0) |
                                   ((btn & 16) ? LS_IMOD_CTRL : 0));
                const int which = btn & 3;
                if (btn & 64) {
                    e.kind = LS_IEV_WHEEL;
                    e.wheel = (btn & 1) ? 1 : -1;
                } else if (btn & 32) {
                    e.kind = LS_IEV_PTR_MOVE;
                } else if (buf[j] == 'M' && which != 3) {
                    e.kind = LS_IEV_PTR_DOWN;
                    e.button = (uint8_t)(which == 0 ? LS_IBTN_LEFT : which == 1 ? LS_IBTN_MIDDLE : LS_IBTN_RIGHT);
                    s_held_buttons |= (uint8_t)(1u << e.button);
                } else {
                    e.kind = LS_IEV_PTR_UP;
                    e.button = (uint8_t)(which == 1 ? LS_IBTN_MIDDLE : which == 2 ? LS_IBTN_RIGHT : LS_IBTN_LEFT);
                    s_held_buttons &= (uint8_t)~(1u << e.button);
                }
                e.buttons = s_held_buttons;
                ls_input_post(&e);
            }
            i = j + 1;
            continue;
        }
        b[o++] = buf[i++];
    }
    return o;
}

int lssim_term_run(void (*frame)(int))
{
    int cols = 0, rows = 0;
    ls_tui_geometry(&cols, &rows, NULL, NULL);
    const size_t cells = (size_t)cols * rows;
    ls_vt_cell *now = calloc(cells, sizeof(*now)), *shown = calloc(cells, sizeof(*shown));
    uint32_t *top = calloc(cells, sizeof(*top)), *bot = calloc(cells, sizeof(*bot));
    if (!now || !shown || !top || !bot) { fprintf(stderr, "lssim: out of memory\n"); return 1; }
    const bool quads = getenv("LSSIM_QUADRANTS") != NULL;

    ls_vt_screen scr;
    ls_vt_screen_init(&scr, cols, rows, shown);
    ls_vt_input in = {0};
    tty_raw();
    ls_vt_enter(out_stdout, NULL, cols, rows);
    if (ls_desk_enabled()) fputs("[?1003h", stdout);        /* hover too */
    int quiet = 0;
    bool run = true;
    /* The mouse as a finger: a press holds it down (a lap app's stick
       follows it), a release that never left the cell is the tap. */
    int cw = 10, ch = 17;
    ls_tui_geometry(NULL, NULL, &cw, &ch);
    bool m_down = false, m_moved = false;
    int m_col = 0, m_row = 0;
    while (run) {
        uint8_t bytes[256];
        ls_vt_event ev[64];
        size_t got = tty_read(bytes, sizeof(bytes));
        if (got && ls_desk_enabled()) got = desk_mouse(bytes, got);
        int n = got ? ls_vt_input_feed(&in, bytes, got, ev, 64) : 0;
        quiet = got ? 0 : quiet + 1;
        if (!got && quiet == 2 && in.n) n = ls_vt_input_idle(&in, ev, 64);
        for (int i = 0; i < n; i++) {
            if (ev[i].type == LS_VT_EV_QUIT) run = false;
            else if (ev[i].type == LS_VT_EV_REDRAW) scr.full = true;
            else if (ev[i].type == LS_VT_EV_TAP) {
                m_down = true;
                m_moved = false;
                m_col = ev[i].col;
                m_row = ev[i].row;
                ls_lap_sys_host_finger(m_col * cw + cw / 2, m_row * ch + ch / 2, cw, ch, true);
            } else if (ev[i].type == LS_VT_EV_DRAG && m_down) {
                m_moved |= ev[i].col != m_col || ev[i].row != m_row;
                ls_lap_sys_host_finger(ev[i].col * cw + cw / 2, ev[i].row * ch + ch / 2, cw, ch, true);
            } else if (ev[i].type == LS_VT_EV_UP && m_down) {
                m_down = false;
                ls_lap_sys_host_finger(ev[i].col * cw + cw / 2, ev[i].row * ch + ch / 2, cw, ch, false);
                if (!m_moved) ls_tui_router_touch(m_col, m_row);
            }
            else if (ev[i].type == LS_VT_EV_KEY) ls_tui_router_key((ls_tk_t)ev[i].key, ev[i].ch);
        }
        frame(1);

        tui_surface *sf = ls_tui_surface();
        const ls_tui_theme_t *th = ls_tui_theme_effective(ls_tui_get_theme(), ls_tui_daylight());
        ls_vt_frame f = {.cols = cols, .rows = rows, .cells = sf->front, .palette = th->palette};
        size_t gn = 0;
        f.glass = ls_tui_glass_now(&gn);
        const uint16_t *src = NULL;
        int w = 0, h = 0, x0 = 0, y0 = 0;
        uint32_t serial = 0;
        if (ls_tui_image_now(&f.image, &src, &w, &h, &serial)) {
            ls_tui_image_ring_offset(&x0, &y0);
            ls_vt_sample_image(f.image, cols, src, w, h, x0, y0, top, bot);
            f.img_top = top;
            f.img_bot = bot;
        } else f.image.w = 0;
        ls_vt_resolve(&f, quads, now);
        ls_vt_render(&scr, now, out_stdout, NULL);
        fflush(stdout);
        nap(33);
    }
    fputs("[?1003l", stdout);
    ls_vt_leave(out_stdout, NULL);
    fflush(stdout);
    tty_restore();
    free(now); free(shown); free(top); free(bot);
    return 0;
}

/* ------------------------------------------------------------- --feed --

   lssim --feed: LakeShark Studio's run panel. The board's cell feed (the
   records ls_rterm.c's cells_render sends, built by lssim's feed_records) in
   the board's frames on stdout: 0x1E, 4 hex length, up to 4000 bytes, 0x1F,
   4 hex CRC-16, and an empty frame to end each update, so web/screen.js
   draws it as it draws a board. Between the frames, plain lines: first
   "term: web begin cols C rows R cells", then whatever the app logs.

   Lines on stdin, one word each:
     in HEX      VT input bytes as screen.js sends them ('term in HEX')
     step SPEC   one LAPSIM_SCRIPT step now (tilt=0.3,-0.1  swipe=L  rx=1 ...)
     redraw      send the whole screen again
     quit        leave (so does the end of stdin)
   An update goes out when the screen changed, and at least every second. */

static void put_frame(const uint8_t *b, size_t n)
{
    unsigned crc = 0xFFFF;
    for (size_t i = 0; i < n; i++) {
        crc ^= (unsigned)b[i] << 8;
        for (int k = 0; k < 8; k++) crc = crc & 0x8000 ? ((crc << 1) ^ 0x1021) & 0xFFFF : (crc << 1) & 0xFFFF;
    }
    char head[6], tail[6];
    snprintf(head, sizeof(head), "\x1e%04x", (unsigned)n);
    snprintf(tail, sizeof(tail), "\x1f%04x", crc);
    fwrite(head, 1, 5, stdout);
    if (n) fwrite(b, 1, n, stdout);
    fwrite(tail, 1, 5, stdout);
}

static int hexv(char c)
{
    return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
}

int lssim_feed_run(void (*frame)(int), size_t (*records)(const uint8_t **))
{
    void ls_lap_sim_step(const char *spec);
#ifdef _WIN32
    _setmode(_fileno(stdout), _O_BINARY);
    _setmode(_fileno(stdin), _O_BINARY);
    s_in = GetStdHandle(STD_INPUT_HANDLE);
    s_console = false;
#endif
    int cols = 0, rows = 0, cw = 10, ch = 17;
    ls_tui_geometry(&cols, &rows, &cw, &ch);
    printf("term: web begin cols %d rows %d cells\n", cols, rows);
    fflush(stdout);
    ls_vt_input in = {0};
    uint8_t *shown = NULL;
    size_t shown_n = 0;
    char line[1024];
    size_t ln = 0;
    bool run = true, force = true, m_down = false, m_moved = false;
    int m_col = 0, m_row = 0, quiet = 0, since = 0;
    while (run) {
        uint8_t bytes[4096];
#ifdef _WIN32
        DWORD avail = 0, got = 0;
        size_t n = 0;
        if (!PeekNamedPipe(s_in, NULL, 0, NULL, &avail, NULL)) break;      /* Studio went away */
        if (avail && ReadFile(s_in, bytes, (DWORD)(avail < sizeof(bytes) ? avail : sizeof(bytes)), &got, NULL)) n = got;
#else
        size_t n = 0;
        {
            fd_set fds;
            FD_ZERO(&fds);
            FD_SET(0, &fds);
            struct timeval tv = {0, 0};
            if (select(1, &fds, NULL, NULL, &tv) > 0) {
                const ssize_t r = read(0, bytes, sizeof(bytes));
                if (r <= 0) break;
                n = (size_t)r;
            }
        }
#endif
        uint8_t vt[512];
        size_t vn = 0;
        for (size_t i = 0; i < n && run; i++) {
            if (bytes[i] != '\n') {
                if (ln < sizeof(line) - 1) line[ln++] = (char)bytes[i];
                continue;
            }
            line[ln] = 0;
            if (ln && line[ln - 1] == '\r') line[--ln] = 0;
            ln = 0;
            if (!strncmp(line, "in ", 3)) {
                for (const char *p = line + 3; p[0] && p[1] && vn < sizeof(vt); p += 2) {
                    const int a = hexv(p[0]), b = hexv(p[1]);
                    if (a < 0 || b < 0) break;
                    vt[vn++] = (uint8_t)(a << 4 | b);
                }
            } else if (!strncmp(line, "step ", 5)) {
                ls_lap_sim_step(line + 5);
            } else if (!strcmp(line, "redraw")) {
                force = true;
            } else if (!strcmp(line, "quit")) {
                run = false;
            }
        }
        ls_vt_event ev[64];
        int ne = vn ? ls_vt_input_feed(&in, vt, vn, ev, 64) : 0;
        quiet = vn ? 0 : quiet + 1;
        if (!vn && quiet == 2 && in.n) ne = ls_vt_input_idle(&in, ev, 64);
        for (int i = 0; i < ne; i++) {
            if (ev[i].type == LS_VT_EV_QUIT) continue;           /* Ctrl-] is the page's, not a quit */
            if (ev[i].type == LS_VT_EV_REDRAW) force = true;
            else if (ev[i].type == LS_VT_EV_TAP) {
                m_down = true;
                m_moved = false;
                m_col = ev[i].col;
                m_row = ev[i].row;
                ls_lap_sys_host_finger(m_col * cw + cw / 2, m_row * ch + ch / 2, cw, ch, true);
            } else if (ev[i].type == LS_VT_EV_DRAG && m_down) {
                m_moved |= ev[i].col != m_col || ev[i].row != m_row;
                ls_lap_sys_host_finger(ev[i].col * cw + cw / 2, ev[i].row * ch + ch / 2, cw, ch, true);
            } else if (ev[i].type == LS_VT_EV_UP && m_down) {
                m_down = false;
                ls_lap_sys_host_finger(ev[i].col * cw + cw / 2, ev[i].row * ch + ch / 2, cw, ch, false);
                if (!m_moved) ls_tui_router_touch(m_col, m_row);
            } else if (ev[i].type == LS_VT_EV_KEY) ls_tui_router_key((ls_tk_t)ev[i].key, ev[i].ch);
        }
        frame(1);
        const uint8_t *rec = NULL;
        const size_t rn = records(&rec);
        if (rn && (force || rn != shown_n || memcmp(rec, shown, rn) || ++since >= 25)) {
            for (size_t o = 0; o < rn; o += 4000) put_frame(rec + o, rn - o < 4000 ? rn - o : 4000);
            put_frame(NULL, 0);
            uint8_t *keep = realloc(shown, rn);
            if (keep) { memcpy(keep, rec, rn); shown = keep; shown_n = rn; }
            force = false;
            since = 0;
        }
        if (fflush(stdout) != 0 || ferror(stdout)) break;     /* the reader went away */
        nap(40);
    }
    free(shown);
    return 0;
}
