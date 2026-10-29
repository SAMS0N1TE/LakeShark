/* The SSTV picture's palette onto the grid. See fm_sstv_view.h. */
#include "fm_sstv_view.h"
#include "../../ls_tui.h"

void fm_sstv_palette(tui_surface *sf, tui_rect cells, const uint16_t palette[16])
{
    ls_tui_basemap(sf, cells, palette);
}
