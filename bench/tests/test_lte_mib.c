/* LS_TEST_SOURCES: ${APP}/cell/lte_mib.c */
/* LS_TEST_INCLUDE: ${APP}/cell */
#include "ls_test.h"
#include "lte_mib.h"
#include "lte_recorded_mib.h"
#include <stdlib.h>
#include <string.h>
#define SAMPLES 76800
static unsigned nibble(char c){return c<='9'?(unsigned)(c-'0'):(unsigned)(c-'a'+10);}
static uint8_t *fixture(unsigned count)
{
    uint8_t *iq=malloc(SAMPLES*2);
    if(!iq)return NULL;
    memset(iq,127,SAMPLES*2);
    for(unsigned i=0;i<count;i++)for(unsigned j=0;recorded_mib[i].hex[2*j];j++)
        iq[recorded_mib[i].offset+j]=(uint8_t)(16*nibble(recorded_mib[i].hex[2*j])+nibble(recorded_mib[i].hex[2*j+1]));
    return iq;
}
static bool counted(void *arg){(*(int *)arg)++;return false;}
static bool stop_at(void *arg){return --*(int *)arg==0;}
LS_CASE(independent_public_pbch_crc_and_sfn_progression)
{
    uint8_t *iq=fixture(4);void *ws=malloc(lte_mib_workspace_size());lte_mib_result_t r;
    LS_CHECK(iq && ws);int calls=0;
    LS_CHECK(lte_mib_find(iq,SAMPLES,301,7756,14000,ws,&r,counted,&calls));
    LS_EQ_INT(r.n_rb,100);LS_EQ_INT(r.antenna_ports,2);LS_EQ_INT(r.sfn,13);
    LS_EQ_INT(r.frames,4);LS_EQ_INT(r.first_frame,0);LS_EQ_INT(r.phich_resource,2);
    LS_EQ_INT(r.phich_duration,0);
    /* Cancellation after earlier valid frames must clear the whole result. */
    LS_CHECK(!lte_mib_find(iq,SAMPLES,301,7756,14000,ws,&r,stop_at,&calls));
    LS_EQ_INT(r.frames,0);LS_EQ_INT(r.n_rb,0);
    free(ws);free(iq);
}
LS_CASE(one_crc_and_repeated_identical_frames_are_insufficient)
{
    uint8_t *iq=fixture(1);void *ws=malloc(lte_mib_workspace_size());lte_mib_result_t r;
    LS_CHECK(iq && ws);
    LS_CHECK(!lte_mib_find(iq,SAMPLES,301,7756,14000,ws,&r,NULL,NULL));
    LS_EQ_INT(r.frames,0);
    /* Identical symbols replayed 10 ms apart retain their old SFN. */
    for(unsigned n=1;n<4;n++)memcpy(iq+recorded_mib[n].offset,iq+recorded_mib[0].offset,1352);
    LS_CHECK(!lte_mib_find(iq,SAMPLES,301,7756,14000,ws,&r,NULL,NULL));
    LS_EQ_INT(r.frames,0);
    free(ws);free(iq);
}
LS_CASE(noise_dc_wrong_cell_and_invalid_inputs_do_not_publish_mib)
{
    uint8_t *iq=fixture(4);void *ws=malloc(lte_mib_workspace_size());lte_mib_result_t r;
    LS_CHECK(iq && ws);
    LS_CHECK(!lte_mib_find(iq,SAMPLES,302,7756,14000,ws,&r,NULL,NULL));
    LS_CHECK(!lte_mib_find(iq,SAMPLES,301,7756,-20000,ws,&r,NULL,NULL));
    LS_CHECK(!lte_mib_find(iq,SAMPLES,301,-1,14000,ws,&r,NULL,NULL));
    LS_CHECK(!lte_mib_find(iq,SAMPLES,504,7756,14000,ws,&r,NULL,NULL));
    LS_CHECK(!lte_mib_find(iq,153601,301,7756,14000,ws,&r,NULL,NULL));
    LS_CHECK(!lte_mib_find(iq,28000,301,7756,14000,ws,&r,NULL,NULL));
    LS_CHECK(!lte_mib_find(NULL,SAMPLES,301,7756,14000,ws,&r,NULL,NULL));
    for(int value=0;value<256;value+=17) {
        memset(iq,value,SAMPLES*2);
        LS_CHECK(!lte_mib_find(iq,SAMPLES,301,7756,0,ws,&r,NULL,NULL));
        LS_EQ_INT(r.frames,0);
    }
    uint32_t seed=936237;
    for(int trial=0;trial<24;trial++) {
        for(int n=0;n<SAMPLES*2;n++){seed=1664525u*seed+1013904223u;iq[n]=(uint8_t)(seed>>24);}
        LS_CHECK(!lte_mib_find(iq,SAMPLES,(trial*17)%504,7756,0,ws,&r,NULL,NULL));
        LS_EQ_INT(r.frames,0);LS_EQ_INT(r.n_rb,0);
    }
    free(ws);free(iq);
}
