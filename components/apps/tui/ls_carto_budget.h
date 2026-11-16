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
/* 2026-11-14, Buenos Aires at z12 (cell 8-86-154, view centred -34.6,-58.5036:
   1.4 MB of tiles): raising this to 6.5 MiB was tried on both boards and is
   not the cure. Tiles the decoded cache cannot hold are decoded into the
   arena, so the two are one budget: that view needs 5.9 MB of arena with a
   4.5 MiB cache, 7.3 MB with 3 MiB, 11.8 MB with 1.5 MiB, about 10.5 MB
   together either way (lssim, LSSIM_CARTO_COLD / LSSIM_CARTO_DECODED). The
   boards open MAP with 11.9 to 12.9 MB free, which after the reserve leaves
   about 10 MB for both: with the larger arena the SX1262 board still drew
   the same two frames of the walk coarse (the LR2021 opened with less free
   that time and got a smaller arena than before). A bigger cap
   also steps the cache down sooner on a board with 13 to 14.8 MB free, the
   worse side of the trade. What that view lacks is half a megabyte of free
   PSRAM at MAP open, or an engine that degrades one tile, not the frame.
   2026-11-15: the engine needs less now, and degrades one tile, not the
   frame. A dense v7 tile decodes its CT3 straight into the result, a decode
   left in the arena keeps only that, and a tile with nothing in view lets its
   bytes go (host, same view, 3 MiB cache: 7.3 MB of arena down to 5.5 MB). A
   tile the arena still cannot hold is drawn from its nearest ancestor that
   fits, clipped to its square, and the frame keeps the rest at full detail;
   the whole-frame fallback in render_frame is the last resort. Both boards
   walked it with this budget, no frame coarse and no tile degraded: the arena
   high-water mark was 5.08 MB (5.49 MB and two coarse frames before). */
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
/* The scene arena for a single map file. 2 MiB drew central_nh_compact down
   to z9 and failed its lowest zoom for good (z8: one 185 KB tile holding the
   whole region, 2026-11-14, LR2021 board, 11.5 MB free); with 4 MiB it draws.
   Taken only when PSRAM has it: short of memory the arena is what is left,
   down to the old 2 MiB. */
#define CARTO_FILE_COLD (4u*CARTO_MIB)
typedef struct { size_t cold,decoded,cache,idle; } carto_budget;
static bool carto_plan_budget(size_t free_bytes,size_t largest,size_t overhead,
                              bool v7,bool cells,bool source,bool tilecache,carto_budget *b) {
    *b=(carto_budget){v7?(cells?CARTO_CELLS_COLD:16u*CARTO_MIB):CARTO_FILE_COLD,
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
