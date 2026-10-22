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
    assert(b.cold==3u*CARTO_MIB && b.decoded==CARTO_CELLS_DECODED_MIN);
    assert(carto_plan_budget(12u*CARTO_MIB,12u*CARTO_MIB,overhead,true,true,true,true,&b));
    assert(b.decoded==CARTO_CELLS_DECODED && b.cold>=2u*CARTO_MIB);
    assert(!carto_plan_budget(6u*CARTO_MIB,6u*CARTO_MIB,overhead,true,true,true,true,&b));
    assert(!carto_plan_budget(2u*CARTO_MIB,2u*CARTO_MIB,overhead,false,false,true,true,&b));
    assert(carto_plan_budget(free_bytes,free_bytes,overhead,false,false,true,true,&b));
    assert(b.cold==2u*CARTO_MIB && b.decoded==2u*CARTO_MIB && b.idle==2u*CARTO_MIB && b.cache==0);
    assert(carto_plan_budget(64u*CARTO_MIB,64u*CARTO_MIB,overhead,true,false,true,true,&b));
    assert(b.cold==16u*CARTO_MIB && b.idle==4u*CARTO_MIB && b.decoded==2u*CARTO_MIB);
    assert(carto_plan_budget(free_bytes,free_bytes,overhead,false,false,false,true,&b) && b.cache==4u*CARTO_MIB);
    puts("PASS: lean cell total, measured margin, reserve, fragmentation and unchanged single-file budgets");
}
