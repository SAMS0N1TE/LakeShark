/* LS_TEST_SOURCES: ${APP}/cell/lte_sync.c */
/* LS_TEST_INCLUDE: ${APP}/cell */
/* PC captures can lose their first synchronization interval. Keep the
 * embedded short search unchanged, but search the full PC capture for seeds.
 * The fixture excerpts are real received PSS/SSS, moved together in time. */
#include "test_lte_sync.c"

LS_CASE(pc_search_finds_repeated_cell_after_missing_initial_interval)
{
    uint8_t *iq=malloc(115200);void *ws=malloc(lte_sync_workspace_size());lte_sync_result_t r;
    LS_CHECK(iq && ws);memset(iq,127,115200);
    for(unsigned i=0;i<3;i++) {
        const char *h=recorded_sync[i].hex;
        for(unsigned j=0;h[2*j];j++)iq[12000+recorded_sync[i].offset+j]=(uint8_t)(16*nibble(h[2*j])+nibble(h[2*j+1]));
    }
    LS_CHECK(lte_sync_find(iq,57600,ws,&r,NULL,NULL));
    LS_EQ_INT(r.pci,301);LS_CHECK(r.hits>=3 && r.pairs>=2);
    /* One missing SSS still invalidates the chain in the broader search. */
    memset(iq+12000+2*(18194-137),127,2*128);
    LS_CHECK(!lte_sync_find(iq,57600,ws,&r,NULL,NULL));LS_EQ_INT(r.pci,-1);
    free(ws);free(iq);
}
