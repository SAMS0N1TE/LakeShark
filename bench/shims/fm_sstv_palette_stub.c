/* Host screen tests have no panel to give the SSTV picture its palette. */
#include <stdint.h>
#include "tuilib/tui_core.h"

void fm_sstv_palette(tui_surface *sf, tui_rect cells, const uint16_t palette[16])
{
    (void)sf; (void)cells; (void)palette;
}
