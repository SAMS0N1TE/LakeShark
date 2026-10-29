/* RX HEAD panels: what a decoder plugin shows inside the RADIO head.

   The head draws the shared rail, waterfall strip, tabs and deck the same for
   every plugin; inside the content rect each plugin's panel keeps its own
   look (the pager's codeword tape and pages, the voice's speaker, APRS's
   stations). Panels run on the UI task and read snapshots only: FM's state
   and the stores the decoders fill, never a plugin's own state, which its
   worker may free on a preset change.

   Built-in plugins (components/lakeshark/rx) cannot reference the UI, so the
   panel is found here by the plugin's id. */
#ifndef LS_RX_PANEL_H
#define LS_RX_PANEL_H

#include "../ls_tui_ui.h"
#include "../ls_radio_panel.h"
#include "ls_rx_plugin.h"

typedef struct ls_rx_panel {
    const char *tab;                    /* the tab's word: "PAGES"           */
    const char *hint;                   /* hint row while it has the focus   */
    void (*draw)(tui_surface *sf, tui_rect r, uint32_t frame);
    bool (*key)(ls_tk_t k, char ch);    /* may be NULL                       */
    bool (*touch)(int col, int row);    /* may be NULL                       */
    /* Extra chips for the rail (sync, baud); returns how many. */
    int  (*chips)(ls_radio_chip_t *out, int max);
} ls_rx_panel_t;

/* The panel for a plugin, or a plain status panel when it has none. */
const ls_rx_panel_t *ls_rx_panel_for(const ls_rx_plugin_t *p);
/* The colour a plugin's chip and tab are drawn in (maturity: TRYING is
   yellow, WORKS its own hue). */
uint8_t ls_rx_plugin_hue(const ls_rx_plugin_t *p);

#endif
