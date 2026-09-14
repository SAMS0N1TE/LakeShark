/* LS_TEST_SOURCES: ${APP}/cell/cell_core.c ${FW}/components/lakeshark/dsp/ls_sweep_core.c */
/* LS_TEST_INCLUDE: ${APP}/cell ${FW}/components/lakeshark/dsp */
#include "ls_test.h"
#include "cell_core.h"
#include "ls_sweep_core.h"
#include <string.h>
#include <math.h>
LS_CASE(change_requires_three_complete_comparable_passes)
{
    int8_t base[]={-90,-60,-128,-95}, now[]={-60,-59,-50,-80}; uint8_t hits[4]={0};
    LS_EQ_INT(cell_compare(base,now,hits,4,true),0);
    LS_EQ_INT(cell_compare(base,now,hits,4,true),0);
    LS_EQ_INT(cell_compare(base,now,hits,4,true),1);
    LS_EQ_INT(cell_compare(base,now,hits,4,false),0);
    LS_EQ_INT(cell_compare(base,now,hits,4,true),0);
    now[0]=-128;
    LS_EQ_INT(cell_compare(base,now,hits,4,true),0);
    LS_EQ_INT(hits[0],0);
}
LS_CASE(baseline_rejects_corruption_wrong_site_band_gain_and_antenna)
{
    cell_baseline_t b={0}; b.magic=0x43454c4c;b.version=1;b.band=0;
    b.count=1000;b.rate=CELL_RATE;b.gain=CELL_GAIN;b.passes=3;
    b.located=1;b.lat_tile=43000;b.lon_tile=-71000;
    memset(b.power,-90,sizeof(b.power));b.checksum=cell_checksum(&b);
    LS_CHECK(cell_baseline_valid(&b,0,43000,-71000,true,false));
    LS_CHECK(!cell_baseline_valid(&b,1,43000,-71000,true,false));
    LS_CHECK(!cell_baseline_valid(&b,0,43001,-71000,true,false));
    LS_CHECK(!cell_baseline_valid(&b,0,43000,-71000,true,true));
    b.power[1]++;LS_CHECK(!cell_baseline_valid(&b,0,43000,-71000,true,false));
    b.power[1]=-128;b.checksum=cell_checksum(&b);
    LS_CHECK(!cell_baseline_valid(&b,0,43000,-71000,true,false));
    b.power[1]=-90;b.gain++;b.checksum=cell_checksum(&b);
    LS_CHECK(!cell_baseline_valid(&b,0,43000,-71000,true,false));
}
LS_CASE(orientation_is_not_stationarity)
{
    LS_CHECK(cell_motion_quiet(0,0,1,0,0,0));
    LS_CHECK(!cell_motion_quiet(0,0,1,0,10,0));
    LS_CHECK(!cell_motion_quiet(.6f,0,1,0,0,0));
    LS_CHECK(!cell_motion_quiet(NAN,0,1,0,0,0));
}
LS_CASE(all_band_sweeps_fit_tuner_and_cover_every_bin)
{
    static int8_t bins[CELL_MAX_BINS];float fft[512];
    for(int i=0;i<512;i++)fft[i]=-70;
    for(unsigned b=0;b<cell_band_count;b++) {
        ls_sweep_plan_t p;
        LS_CHECK(ls_sweep_plan(cell_bands[b].low,cell_bands[b].high,CELL_BIN,CELL_RATE,&p));
        LS_CHECK(p.n_bins<=CELL_MAX_BINS);
        LS_CHECK(p.start_hz-p.half_span_hz>24000000);
        LS_CHECK(p.stop_hz+p.half_span_hz<1766000000);
        ls_sweep_reset(&p,bins);
        for(unsigned i=0;i<p.n_tunes;i++)ls_sweep_fold(&p,ls_sweep_tune_center(&p,i),fft,512,bins);
        LS_EQ_INT(ls_sweep_gaps(&p,bins),0);
    }
}
