/* Colour themes for the TUI. */

#ifndef LS_THEME_H
#define LS_THEME_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    const char *name;
    const char *desc;
    uint16_t    palette[16];   /* indexed by the cell attribute's nibbles */
} ls_tui_theme_t;

extern const ls_tui_theme_t ls_theme_terminal_bay;
extern const ls_tui_theme_t ls_theme_amber;
extern const ls_tui_theme_t ls_theme_phosphor;
extern const ls_tui_theme_t ls_theme_ice;
extern const ls_tui_theme_t ls_theme_synth;
extern const ls_tui_theme_t ls_theme_vfd;
extern const ls_tui_theme_t ls_theme_arcade;

/* The white one. Not in the table below, so the count, the index,
   the lookup by name and the cycle never see it: it is reached through
   ls_tui_set_daylight and nothing else. */
extern const ls_tui_theme_t ls_theme_daylight;

/* Themes in registration order, for a settings list. */
int                    ls_tui_theme_count(void);
const ls_tui_theme_t  *ls_tui_theme_at(int index);

/* Where a theme sits in the cycle, and -1 for one that is not in it
   (NULL, Daylight, anything foreign). The index is what settings stores. */
int                    ls_tui_theme_index(const ls_tui_theme_t *theme);

/* The theme after `cur`, wrapping. The Theme row and F9 both step
   with this, so the two cannot disagree about the order. From anything that
   is not in the cycle - Daylight included - it answers the first theme, so a
   press always lands on something the table knows. */
const ls_tui_theme_t  *ls_tui_theme_next(const ls_tui_theme_t *cur);

const ls_tui_theme_t  *ls_tui_theme_effective(const ls_tui_theme_t *chosen,
                                              bool daylight);

/* Look a theme up the way a person types it. */

const ls_tui_theme_t  *ls_tui_theme_by_name(const char *name);

/* Takes effect on the next present. The coverage ramp cache is keyed on the
   attribute byte, not on the colours behind it, so changing theme has to
   invalidate it - that is done here, not left to the caller. */
void                   ls_tui_set_theme(const ls_tui_theme_t *theme);
const ls_tui_theme_t  *ls_tui_get_theme(void);

/* Daylight is a toggle over the chosen theme, not a theme. */

void                   ls_tui_set_daylight(bool on);
bool                   ls_tui_daylight(void);
/* The palette being drawn with now: Daylight, or the chosen theme. */
const ls_tui_theme_t  *ls_tui_active_theme(void);

#ifdef __cplusplus
}
#endif

#endif /* LS_THEME_H */
