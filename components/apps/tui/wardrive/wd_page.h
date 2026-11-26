/* WARDRIVE's added pages, one file each, drawn by scr_wardrive.c in the
   area above its key bar (or under the 3D keys). HUNT: the targets and the
   vehicles; RADIO: the radio jobs; ATLAS: the routes and the memory. */
#ifndef WD_PAGE_H
#define WD_PAGE_H

#include <stdbool.h>
#include <stdint.h>
#include "../ls_tui_screen.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    void (*draw)(tui_surface *sf, tui_rect a, uint32_t now);
    bool (*key)(ls_tk_t k, char ch, uint32_t now);      /* true: used */
    bool (*touch)(int x, int y, uint32_t now);          /* cell coordinates */
    void (*leave)(void);                                /* may be NULL */
} wd_page_t;

extern const wd_page_t wd_page_hunt, wd_page_radio, wd_page_atlas;
/* HUNT's alerts on any screen (an ls_notify probe); once, from WARDRIVE's enter. */
void wd_hunt_attach(void);

#ifdef __cplusplus
}
#endif
#endif
