#include "ls_carto_budget.h"
#include <assert.h>
#include <stdio.h>
int main(void) {
    carto_budget b;
    size_t overhead=128u*1024u,free_bytes=20890868;
    assert(carto_plan_budget(free_bytes,free_bytes,overhead,true,true,true,true,&b));
    assert(b.idle==0 && b.cache==0 && b.decoded==CARTO_CELLS_DECODED);
    assert(b.cold==CARTO_CELLS_COLD);
    assert(b.cold>=3210105u*3u/2u);
    /* a dense z8 US tile decodes to 3.96 MB, a z12 view to about 4.6 MB */
    assert(b.decoded>=4640000u);
    assert(b.cold+b.idle+b.decoded+overhead+8192u<=23u*CARTO_MIB/2u);
    assert(carto_plan_budget(free_bytes,3u*CARTO_MIB,overhead,true,true,true,true,&b));
    assert(b.cold==3u*CARTO_MIB && b.decoded<=3u*CARTO_MIB);
    assert(carto_plan_budget(8u*CARTO_MIB,8u*CARTO_MIB,overhead,true,true,true,true,&b));
    /* with a 2 MiB reserve, 8 MiB keeps 3 MiB decoded and a 2.5 MiB arena */
    assert(b.cold>=2u*CARTO_MIB && b.decoded==2u*CARTO_CELLS_DECODED_MIN);
    /* the arena comes first: decoded steps down to 3 MiB to make room */
    assert(carto_plan_budget(12u*CARTO_MIB,12u*CARTO_MIB,overhead,true,true,true,true,&b));
    assert(b.decoded==2u*CARTO_CELLS_DECODED_MIN && b.cold>=5u*CARTO_MIB);
    /* the LR2021 board opening MAP (2026-10-30): about 13.5 MB free */
    assert(carto_plan_budget(13500000u,13500000u,overhead,true,true,true,true,&b));
    assert(b.cold>=5u*CARTO_MIB && b.decoded>=3u*CARTO_MIB);
    assert(!carto_plan_budget(5u*CARTO_MIB,5u*CARTO_MIB,overhead,true,true,true,true,&b));
    /* the SX1262 board with ADS-B running, MAP opening (2026-10-31): both fit */
    assert(carto_plan_budget(13580344u,13580344u,401492u,true,true,true,true,&b));
    assert(b.cold==CARTO_CELLS_COLD && b.decoded==CARTO_CELLS_DECODED);
    assert(!carto_plan_budget(2u*CARTO_MIB,2u*CARTO_MIB,overhead,false,false,true,true,&b));
    assert(carto_plan_budget(free_bytes,free_bytes,overhead,false,false,true,true,&b));
    assert(b.cold==2u*CARTO_MIB && b.decoded==2u*CARTO_MIB && b.idle==2u*CARTO_MIB && b.cache==0);
    assert(carto_plan_budget(64u*CARTO_MIB,64u*CARTO_MIB,overhead,true,false,true,true,&b));
    assert(b.cold==16u*CARTO_MIB && b.idle==4u*CARTO_MIB && b.decoded==2u*CARTO_MIB);
    assert(carto_plan_budget(free_bytes,free_bytes,overhead,false,false,false,true,&b) && b.cache==4u*CARTO_MIB);
    puts("PASS: lean cell total, measured margin, reserve, fragmentation and unchanged single-file budgets");
}
