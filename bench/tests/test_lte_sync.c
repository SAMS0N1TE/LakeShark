/* LS_TEST_SOURCES: ${APP}/cell/lte_sync.c */
/* LS_TEST_INCLUDE: ${APP}/cell */
#include "ls_test.h"
#include "lte_sync.h"
#include <stdlib.h>
#include <string.h>
LS_CASE(noise_and_dc_do_not_become_cell_identities)
{
    uint8_t *iq=malloc(57600);void *ws=malloc(lte_sync_workspace_size());lte_sync_result_t r;
    LS_CHECK(iq && ws);
    memset(iq,180,57600);
    LS_CHECK(!lte_sync_find(iq,28800,ws,&r,NULL,NULL));LS_EQ_INT(r.pci,-1);
    uint32_t seed=1234567;
    for(unsigned i=0;i<57600;i++){seed=1664525u*seed+1013904223u;iq[i]=seed>>24;}
    LS_CHECK(!lte_sync_find(iq,28800,ws,&r,NULL,NULL));LS_EQ_INT(r.pci,-1);
    free(ws);free(iq);
}
static bool stop(void *arg){(*(int *)arg)++;return true;}
LS_CASE(bounded_capture_and_cancellation_do_not_publish_partial_results)
{
    uint8_t *iq=calloc(57600,1);void *ws=malloc(lte_sync_workspace_size());lte_sync_result_t r;
    LS_CHECK(iq && ws);int calls=0;
    LS_CHECK(!lte_sync_find(iq,28800,ws,&r,stop,&calls));LS_EQ_INT(calls,1);LS_EQ_INT(r.pci,-1);
    LS_CHECK(!lte_sync_find(iq,100,ws,&r,NULL,NULL));LS_EQ_INT(r.pci,-1);
    LS_CHECK(!lte_sync_find(iq,200000,ws,&r,NULL,NULL));LS_EQ_INT(r.pci,-1);
    LS_EQ_INT(lte_sync_frame_start(ws),-1);
    free(ws);free(iq);
}

#include "lte_recorded_sync.h"
static unsigned nibble(char c){return c<='9'?(unsigned)(c-'0'):(unsigned)(c-'a'+10);}
static bool counted(void *arg){(*(int *)arg)++;return false;}
static bool stop_at(void *arg){int *n=arg;return --*n==0;}
LS_CASE(independent_recorded_cell_requires_three_consistent_observations)
{
    uint8_t *iq=malloc(57600);void *ws=malloc(lte_sync_workspace_size());lte_sync_result_t r;
    LS_CHECK(iq && ws);memset(iq,127,57600);
    for(unsigned i=0;i<3;i++) {
        const char *h=recorded_sync[i].hex;
        for(unsigned j=0;h[2*j];j++)iq[recorded_sync[i].offset+j]=(uint8_t)(16*nibble(h[2*j])+nibble(h[2*j+1]));
    }
    int calls=0;
    LS_CHECK(lte_sync_find(iq,28800,ws,&r,counted,&calls));
    LS_EQ_INT(r.pci,301);LS_CHECK(r.hits>=3 && r.pairs>=2);
    LS_CHECK(lte_sync_frame_start(ws)>7000 && lte_sync_frame_start(ws)<9000);
    LS_CHECK(r.cfo_hz>12000 && r.cfo_hz<16500);
    LS_CHECK(!lte_sync_find(iq,28800,ws,&r,stop_at,&calls));LS_EQ_INT(r.pci,-1);
    LS_EQ_INT(lte_sync_frame_start(ws),-1);
    /* PSS alone and two surviving SSS observations must not publish a PCI. */
    memset(iq+2*(18194-137),127,2*128);
    LS_CHECK(!lte_sync_find(iq,28800,ws,&r,NULL,NULL));LS_EQ_INT(r.pci,-1);
    free(ws);free(iq);
}
