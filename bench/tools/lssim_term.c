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
#else
#include <sys/select.h>
#include <termios.h>
#include <unistd.h>
#endif

#include "ls_vt.h"
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
        const size_t got = tty_read(bytes, sizeof(bytes));
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
    ls_vt_leave(out_stdout, NULL);
    fflush(stdout);
    tty_restore();
    free(now); free(shown); free(top); free(bot);
    return 0;
}
