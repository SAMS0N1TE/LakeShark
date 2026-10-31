/* Caller-owned CartoCore buffers. Source buffers are already open when measured. */
#ifndef LS_CARTO_BUDGET_H
#define LS_CARTO_BUDGET_H
#include <stddef.h>
#include <stdbool.h>
#define CARTO_MIB (1024u*1024u)
/* The scene arena for cell sources. 4.75 MiB (3,210,105 measured bytes *
   1.5, for one NH cell) ran out on the LR2021 board for a z8 view over
   Tampa that straddles several cells (2026-10-30: peak 3.69 MB of a 3.93 MB
   arena, then the coarse fallback drew it as broken dashes). 6 MiB fixed z8 but
   pushed decoded to 3 MiB, and a z12 view there then ran out (peak 5.88 MB):
   the two trade, and 5.25 + 4.5 MiB fits that board with both drawn. */
#define CARTO_CELLS_COLD (21u*256u*1024u)
/* Decoded scene space for cell sources. The US cells (2026-10-20) decode to
   1.16 MB per z12 tile and 3.96 MB per dense z8 tile, and a view decodes all
   its tiles at once, so 1.5 MiB (sized for the first NH cells) failed with
   "render/decode". Take 4.5 MiB when PSRAM allows (it covers both, and the
   2026-10-30 cells, whose z8 lines are joined, decode to under 0.5 MB a
   tile), stepping down to the old 1.5 MiB only when memory is short. */
#define CARTO_CELLS_DECODED (3u*CARTO_CELLS_DECODED_MIN)
#define CARTO_CELLS_DECODED_MIN (1536u*1024u)
/* PSRAM left free for the rest of the board while MAP is open. 3 MiB left
   the SX1262 board with ADS-B running (13.6 MB free at MAP start) 0.5 MiB short
   of the scene arena plus decoded cache both fitting, and a z12 frame fell
   back coarse there (2026-10-31). */
#define CARTO_PSRAM_RESERVE (2u*CARTO_MIB)
typedef struct { size_t cold,decoded,cache,idle; } carto_budget;
static bool carto_plan_budget(size_t free_bytes,size_t largest,size_t overhead,
                              bool v7,bool cells,bool source,bool tilecache,carto_budget *b) {
    *b=(carto_budget){v7?(cells?CARTO_CELLS_COLD:16u*CARTO_MIB):2u*CARTO_MIB,
                     cells?CARTO_CELLS_DECODED:2u*CARTO_MIB,
                     tilecache && !source?4u*CARTO_MIB:0,
                     tilecache || source?(cells?0:v7?4u*CARTO_MIB:2u*CARTO_MIB):0};
    /* Guard covers allocator alignment/metadata and small lazy cell handles.
       Overhead includes hot/output cells and both future publication buffers. */
    size_t fixed=CARTO_PSRAM_RESERVE+256u*1024u+overhead+b->decoded+b->cache+b->idle;
    /* cells: the scene arena first. Decoded gives back 1.5 MiB steps, down
       to 3 MiB, until the whole arena fits; a short arena is a frame that
       falls back coarse, a short decoded cache only decodes again. */
    while(cells && b->decoded>2u*CARTO_CELLS_DECODED_MIN && (free_bytes<=fixed+b->cold || b->decoded>largest)) {
        b->decoded-=CARTO_CELLS_DECODED_MIN; fixed-=CARTO_CELLS_DECODED_MIN;
    }
    /* then, only when memory is short, down to 1.5 MiB until 2 MiB of arena fits */
    while(cells && b->decoded>CARTO_CELLS_DECODED_MIN && (free_bytes<=fixed+2u*CARTO_MIB || b->decoded>largest)) {
        b->decoded-=CARTO_CELLS_DECODED_MIN; fixed-=CARTO_CELLS_DECODED_MIN;
    }
    if(free_bytes<=fixed) return false;
    size_t available=free_bytes-fixed;
    if(available>largest) available=largest;
    available=available/(256u*1024u)*(256u*1024u);
    if(b->cold>available) b->cold=available;
    return b->cold>=2u*CARTO_MIB;
}
#endif
