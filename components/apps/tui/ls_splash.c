#include "ls_splash.h"
#include "ls_wordmark.h"
#include <string.h>

static const uint8_t SPLASH_LADDER[5] = {
    TUI_BLUE, TUI_BLUE | TUI_BRIGHT, TUI_CYAN,
    TUI_CYAN | TUI_BRIGHT, TUI_WHITE | TUI_BRIGHT,
};
#define SPLASH_FADE_PCT 55

void ls_splash_draw(tui_surface *sf, int cols, int rows, int frame,
                       int frames)
{
    tui_rect all = tui_surface_rect(sf);
    tui_frame_begin(sf);

    if (frames < 1) frames = 1;
    const int pct = frame * 100 / frames;

    /* Split before anything is positioned: the assembly's height depends on
       whether the mark needed two lines, and its top on the height. */
    char line_a[32], line_b[32];
    const bool compact = cols - 2 < ls_wordmark_width("TERMINAL");
    int (*mark_width)(const char *) = compact ? ls_wordmark_compact_width : ls_wordmark_width;
    void (*mark_row)(tui_surface *, tui_rect, int, int, int, const char *, uint8_t) =
        compact ? ls_wordmark_compact_row : ls_wordmark_row;
    const bool two = (compact ? ls_wordmark_compact_split : ls_wordmark_split)("TERMINAL BAY", cols - 2,
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

        mark_row(sf, all, cols / 2 - mark_width(line_a) / 2,
                        top, r, line_a, at);
        if (two)
            mark_row(sf, all,
                            cols / 2 - mark_width(line_b) / 2,
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

