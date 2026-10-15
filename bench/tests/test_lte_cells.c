/* LS_TEST_SOURCES: (none: includes lte_sync.c for its sequences) */
/* LS_TEST_INCLUDE: ${APP}/cell */
#include "ls_test.h"
#include "../../components/lakeshark/apps/cell/lte_sync.c"
#include <stdlib.h>

/* Synthetic FDD normal-CP downlink holding only PSS and SSS, built from the
 * same 3GPP sequences the receiver uses: enough to put two cells with
 * different PCIs and timings in one capture, which no public recording here
 * does. Each half-frame (9600 samples) carries SSS then PSS, 137 samples
 * apart, with the 9-sample cyclic prefix of each symbol. */
#define SAMPLES 57600
static workspace_t *ref;
static void symbol(cx *acc,int useful_start,const cx *time128,float gain,int cfo)
{
    for(int n=-9;n<128;n++) {
        int at=useful_start+n;
        if(at<0 || at>=SAMPLES) continue;
        cx v=time128[(n+128)%128];
        cx rot=cis(TAU*cfo*(float)at/1920000.f);
        v=mul((cx){v.r*gain,v.i*gain},rot);
        acc[at].r+=v.r;acc[at].i+=v.i;
    }
}
static void add_cell(cx *acc,int pci,int offset,float gain,int cfo,int first_half)
{
    int nid2=pci%3,id1=pci/3;
    for(int h=0;offset+h*9600+128<=SAMPLES;h++) {
        int pos=offset+h*9600;
        int half=(h+first_half)&1;
        cx sss[128];memset(sss,0,sizeof(sss));
        for(int k=0;k<62;k++){int bin=k<31?97+k:k-30;sss[bin]=(cx){(float)ref->sss[nid2][2*id1+half][k],0};}
        fft(sss,128,true,ref->tw);
        symbol(acc,pos-137,sss,gain,cfo);
        symbol(acc,pos,ref->td[nid2],gain,cfo);
    }
}
static void quantise(const cx *acc,uint8_t *iq,float sigma,unsigned seed)
{
    ls_rng_t r;ls_rng_seed(&r,seed);
    for(int n=0;n<SAMPLES;n++) {
        float i=acc[n].r+sigma*ls_rng_noise(&r)/.289f+127.5f,q=acc[n].i+sigma*ls_rng_noise(&r)/.289f+127.5f;
        iq[2*n]=(uint8_t)(i<0?0:i>255?255:i+.5f);iq[2*n+1]=(uint8_t)(q<0?0:q>255?255:q+.5f);
    }
}
static workspace_t *workspace(void)
{
    workspace_t *w=calloc(1,sizeof(*w));
    LS_CHECK(w);
    return w;
}
static void build(uint8_t *iq,const int *pci,const int *offset,const float *gain,int n,int cfo)
{
    cx *acc=calloc(SAMPLES,sizeof(cx));LS_CHECK(acc);
    for(int i=0;i<n;i++) add_cell(acc,pci[i],offset[i],gain[i],cfo,i);
    quantise(acc,iq,6.f,99);
    free(acc);
}
static bool never(void *arg){(void)arg;return false;}
static bool counted_yield(void *arg){(*(int *)arg)++;return false;}

LS_CASE(synthetic_cell_is_found_and_two_sectors_are_both_reported)
{
    ref=workspace();sequences(ref);
    uint8_t *iq=malloc(SAMPLES*2);workspace_t *w=workspace();LS_CHECK(iq);
    const int pci[]={301,17},off[]={2500,6000};const float gain[]={260.f,190.f};
    build(iq,pci,off,gain,2,12000);
    lte_sync_set_t set;
    LS_CHECK(lte_sync_find_cells(iq,SAMPLES,w,&set,never,NULL,0,40000));
    LS_EQ_INT(set.count,2);
    bool a=false,b=false;
    for(int i=0;i<set.count;i++) {
        const lte_sync_result_t *r=&set.cell[i].r;
        a|=r->pci==301;b|=r->pci==17;
        LS_CHECK(r->pairs>=2 && r->hits>=3);
        LS_CHECK(r->cfo_hz>10500 && r->cfo_hz<13500);
        LS_CHECK(set.cell[i].frame_start>=0 && set.cell[i].frame_start<19200);
    }
    LS_CHECK(a && b);
    /* Strongest first, and the single result API still names the strongest. */
    LS_CHECK(set.cell[0].r.pairs>=set.cell[1].r.pairs);
    lte_sync_result_t one;
    LS_CHECK(lte_sync_find(iq,SAMPLES,w,&one,never,NULL));
    /* On equal pairs the single-result API keeps the later cell; either way it
     * is one of the set with the most pairs, with that cell's frame boundary. */
    LS_EQ_INT(one.pairs,set.cell[0].r.pairs);
    int mine=set.cell[0].r.pci==one.pci?0:1;
    LS_EQ_INT(one.pci,set.cell[mine].r.pci);
    LS_EQ_INT(lte_sync_frame_start(w),set.cell[mine].frame_start);
    /* Their timings differ by the 3500 samples they were placed apart. */
    int fa=set.cell[0].r.pci==301?set.cell[0].frame_start:set.cell[1].frame_start;
    int fb=set.cell[0].r.pci==301?set.cell[1].frame_start:set.cell[0].frame_start;
    int d=((fb-fa)%9600+9600+4800)%9600-4800;
    LS_CHECK(abs(d-3500)<=12);
    free(w);free(iq);free(ref);
}
LS_CASE(search_window_follows_the_known_clock_and_cuts_the_work)
{
    ref=workspace();sequences(ref);
    uint8_t *iq=malloc(SAMPLES*2);workspace_t *w=workspace();LS_CHECK(iq);
    const int pci[]={301},off[]={4000};const float gain[]={260.f};
    build(iq,pci,off,gain,1,12000);
    lte_sync_set_t set;int calls=0;
    /* Wide: 17 templates a root. Narrow around the offset: 3. */
    LS_CHECK(lte_sync_find_cells(iq,SAMPLES,w,&set,counted_yield,&calls,0,40000));
    int wide=calls;calls=0;
    LS_CHECK(lte_sync_find_cells(iq,SAMPLES,w,&set,counted_yield,&calls,10000,5000));
    LS_EQ_INT(set.count,1);LS_EQ_INT(set.cell[0].r.pci,301);
    LS_CHECK(abs(set.cell[0].r.cfo_hz-12000)<=1500);
    LS_CHECK(calls*3<wide);
    /* A window that does not contain the carrier finds nothing, and says so. */
    LS_CHECK(!lte_sync_find_cells(iq,SAMPLES,w,&set,never,NULL,-30000,5000));
    LS_EQ_INT(set.count,0);
    LS_CHECK(!lte_sync_find_cells(iq,SAMPLES,w,&set,never,NULL,30000,0));
    /* Windows the search cannot honour are refused. */
    LS_CHECK(!lte_sync_find_cells(iq,SAMPLES,w,&set,never,NULL,0,45000));
    LS_CHECK(!lte_sync_find_cells(iq,SAMPLES,w,&set,never,NULL,41000,5000));
    LS_CHECK(!lte_sync_find_cells(iq,SAMPLES,w,&set,never,NULL,0,-1));
    LS_CHECK(!lte_sync_find_cells(iq,SAMPLES,NULL,&set,never,NULL,0,40000));
    LS_CHECK(!lte_sync_find_cells(iq,SAMPLES,w,NULL,never,NULL,0,40000));
    free(w);free(iq);free(ref);
}
LS_CASE(noise_alone_yields_no_cell_in_any_window)
{
    uint8_t *iq=malloc(SAMPLES*2);workspace_t *w=workspace();LS_CHECK(iq);
    ls_rng_t r;ls_rng_seed(&r,5);
    for(int n=0;n<SAMPLES*2;n++)iq[n]=(uint8_t)(127.5f+30.f*ls_rng_noise(&r)/.289f);
    lte_sync_set_t set;
    LS_CHECK(!lte_sync_find_cells(iq,SAMPLES,w,&set,never,NULL,0,40000));LS_EQ_INT(set.count,0);
    LS_CHECK(!lte_sync_find_cells(iq,SAMPLES,w,&set,never,NULL,0,5000));LS_EQ_INT(set.count,0);
    free(w);free(iq);
}
static bool stop_after(void *arg){return --*(int *)arg<=0;}
LS_CASE(cancelling_a_multi_cell_search_publishes_nothing)
{
    ref=workspace();sequences(ref);
    uint8_t *iq=malloc(SAMPLES*2);workspace_t *w=workspace();LS_CHECK(iq);
    const int pci[]={301,17},off[]={2500,6000};const float gain[]={260.f,190.f};
    build(iq,pci,off,gain,2,12000);
    lte_sync_set_t set;
    for(int when=1;when<80;when+=13) {
        int left=when;
        LS_CHECK(!lte_sync_find_cells(iq,SAMPLES,w,&set,stop_after,&left,0,40000));
        LS_EQ_INT(set.count,0);
    }
    free(w);free(iq);free(ref);
}
