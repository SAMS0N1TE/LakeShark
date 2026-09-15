#ifndef LS_TUI_DENSITY_H
#define LS_TUI_DENSITY_H
#include <stdbool.h>
#include <string.h>

/* Preserve the user's preferred face.  The large face is intentionally
   limited to the launcher, radio hub and settings: tool layouts depend on
   the original grid density. */
static inline int ls_tui_font_for_view(int preferred, const char *screen,
                                     bool full_waterfall)
{
    if (preferred != 2) return preferred;
    if (full_waterfall) return 0;
    if (screen && (!strcmp(screen, "HOME") || !strcmp(screen, "RADIOS") ||
                   !strcmp(screen, "SET")))
        return preferred;
    return 0;
}
#endif
