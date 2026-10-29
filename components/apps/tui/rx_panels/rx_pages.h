/* The decoded pages list and the page behind it: a panel of the RX HEAD,
   moved here from the FM screen so the RADIO head and FM draw the same one.

   The list reads a source (count, at) newest first; the view state holds the
   selection on its message as new pages push it down (see rx_pages_refind).
   UP / OPEN n/m / DOWN keys under it, three rows a page in portrait, one in
   landscape; tapping a page twice opens it. */
#ifndef RX_PAGES_H
#define RX_PAGES_H

#include "../ls_tui_ui.h"
#include "apps/fm/fm_state.h"

typedef struct {
    int  (*count)(void);
    const fm_page_t *(*at)(int i);    /* newest first */
} rx_pages_src_t;

typedef struct {
    int  sel, top;            /* selection, first list row shown           */
    bool open;                /* the full view of the selected page        */
    tui_rect list, back;      /* where the list and BACK landed, for taps  */
    tui_rect hit[3];          /* UP, OPEN, DOWN                            */
    int64_t  sel_ts;          /* the selected message, held across arrivals */
    uint32_t sel_addr;
    bool held, missing;
} rx_pages_t;

int  rx_pages_row_h(void);
int  rx_pages_at_row(const rx_pages_t *v, tui_rect r, int row);
void rx_pages_refind(rx_pages_t *v, const rx_pages_src_t *src);
void rx_pages_remember(rx_pages_t *v, const rx_pages_src_t *src);
void rx_pages_move(rx_pages_t *v, const rx_pages_src_t *src, int delta);
/* The UP / OPEN / DOWN keys at the bottom of area; returns the rows used. */
int  rx_pages_controls(rx_pages_t *v, const rx_pages_src_t *src, tui_surface *sf, tui_rect area);
void rx_pages_detail(rx_pages_t *v, const rx_pages_src_t *src, tui_surface *sf, tui_rect pane);
/* The list in area, titled; `empty` (may be NULL) is a second line under
   "listening..." while there is nothing yet. */
void rx_pages_list(rx_pages_t *v, const rx_pages_src_t *src, tui_surface *sf, tui_rect area,
                   const char *title, const char *empty);
/* Keys and taps the list takes; false for anything else. */
bool rx_pages_key(rx_pages_t *v, const rx_pages_src_t *src, ls_tk_t k);
bool rx_pages_touch(rx_pages_t *v, const rx_pages_src_t *src, int col, int row);

#endif
