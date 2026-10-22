/* Caller-owned CartoCore buffers. Source buffers are already open when measured. */
#ifndef LS_CARTO_BUDGET_H
#define LS_CARTO_BUDGET_H
#include <stddef.h>
#include <stdbool.h>
#define CARTO_MIB (1024u*1024u)
/* 3,210,105 measured bytes * 1.5, rounded up to 256 KiB.
   Cell sources use the same scene decoder as single files. */
#define CARTO_CELLS_COLD (19u*256u*1024u)
/* Decoded scene space for cell sources. The US cells (2026-10-20) decode to
   1.16 MB per z12 tile and 3.96 MB per dense z8 tile, and a view decodes all
   its tiles at once, so 1.5 MiB (sized for the first NH cells) failed with
   "render/decode". Take 6 MiB when PSRAM allows, stepping down to the old
   1.5 MiB only when memory is short. */
#define CARTO_CELLS_DECODED (6u*CARTO_MIB)
#define CARTO_CELLS_DECODED_MIN (1536u*1024u)
#define CARTO_PSRAM_RESERVE (3u*CARTO_MIB)
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
    /* cells: give back decoded space in 1.5 MiB steps until 2 MiB of cold fits */
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
