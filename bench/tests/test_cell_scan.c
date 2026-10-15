/* LS_TEST_SOURCES: ${APP}/cell/cell_core.c */
/* LS_TEST_INCLUDE: ${APP}/cell */
#include "ls_test.h"
#include "cell_core.h"
#include <string.h>
#include <math.h>

/* ---- band table and EARFCN (TS 36.101 table 5.7.3-1) ---- */
static int band_index(unsigned number)
{
    for(unsigned i=0;i<cell_band_total;i++) if(cell_bands[i].band==number) return (int)i;
    return -1;
}
LS_CASE(known_earfcns_convert_to_their_published_downlink_frequencies)
{
    static const struct {unsigned band,earfcn;uint32_t hz;} known[]={
        {5,2400,869000000},{5,2525,881500000},{12,5110,739000000},{13,5230,751000000},
        {14,5330,763000000},{17,5780,739000000},{20,6300,806000000},{8,3750,955000000},
        {26,8690,859000000},{71,68586,617000000},{2,900,1960000000},{4,2175,2132500000},
        {25,8040,1930000000},{66,66436,2110000000},
    };
    for(unsigned i=0;i<sizeof(known)/sizeof(known[0]);i++) {
        int b=band_index(known[i].band);LS_CHECK(b>=0);
        uint32_t hz=0;unsigned back=0;
        LS_CHECK(cell_earfcn_to_hz((unsigned)b,known[i].earfcn,&hz));
        LS_EQ_UINT(hz,known[i].hz);
        LS_CHECK(cell_hz_to_earfcn((unsigned)b,hz,&back));
        LS_EQ_UINT(back,known[i].earfcn);
    }
}
LS_CASE(every_band_round_trips_and_its_edges_are_exact)
{
    for(unsigned b=0;b<cell_band_total;b++) {
        unsigned first,last;cell_band_earfcns(b,&first,&last);
        LS_EQ_UINT(last-first+1,(cell_bands[b].high-cell_bands[b].low)/CELL_RASTER_HZ);
        uint32_t hz;
        LS_CHECK(cell_earfcn_to_hz(b,first,&hz));LS_EQ_UINT(hz,cell_bands[b].low);
        LS_CHECK(cell_earfcn_to_hz(b,last,&hz));LS_EQ_UINT(hz,cell_bands[b].high-CELL_RASTER_HZ);
        LS_CHECK(!cell_earfcn_to_hz(b,last+1,&hz));
        LS_CHECK(!cell_earfcn_to_hz(b,first-1,&hz));
        unsigned n;
        LS_CHECK(!cell_hz_to_earfcn(b,cell_bands[b].high,&n));           /* the top edge is not a carrier */
        LS_CHECK(!cell_hz_to_earfcn(b,cell_bands[b].low+50000,&n));      /* off the raster */
        LS_CHECK(!cell_hz_to_earfcn(b,cell_bands[b].low-CELL_RASTER_HZ,&n));
        for(unsigned e=first;e<=last;e+=7){
            LS_CHECK(cell_earfcn_to_hz(b,e,&hz));
            LS_CHECK(cell_hz_to_earfcn(b,hz,&n) && n==e);
        }
    }
    unsigned n;
    LS_CHECK(!cell_hz_to_earfcn(cell_band_total,880000000,&n));
}
LS_CASE(the_us_and_eu_set_is_there_and_the_rtl_ones_come_first)
{
    static const unsigned want[]={2,4,5,8,12,13,14,17,20,25,26,66,71};
    for(unsigned i=0;i<sizeof(want)/sizeof(want[0]);i++) LS_CHECK(band_index(want[i])>=0);
    LS_EQ_UINT(cell_band_total,13);
    for(unsigned b=0;b<cell_band_total;b++) {
        bool rtl=cell_bands[b].high<1766000000u;
        LS_CHECK((b<cell_band_count)==rtl);
        LS_CHECK(cell_bands[b].name && strstr(cell_bands[b].name,"/ B"));
    }
    /* Saved baselines name a band by position: the first five must not move. */
    LS_EQ_UINT(cell_bands[0].band,5);LS_EQ_UINT(cell_bands[1].band,8);LS_EQ_UINT(cell_bands[2].band,12);
    LS_EQ_UINT(cell_bands[3].band,13);LS_EQ_UINT(cell_bands[4].band,20);
    LS_EQ_INT(cell_band_for_hz(881500000),0);
    LS_EQ_INT(cell_band_for_hz(1960000000),band_index(2));
    LS_EQ_INT(cell_band_for_hz(100000000),-1);
}
LS_CASE(no_band_needs_more_bins_than_a_baseline_holds)
{
    for(unsigned b=0;b<cell_band_count;b++)
        LS_CHECK((cell_bands[b].high-cell_bands[b].low)/CELL_BIN<=CELL_MAX_BINS);
}

/* ---- dongle clock from cells ---- */
LS_CASE(median_over_cells_gives_the_crystal_error_and_shrugs_off_a_bad_one)
{
    cell_ppm_t p;cell_ppm_reset(&p);
    cell_ppm_est_t e;
    LS_CHECK(!cell_ppm_estimate(&p,&e));
    /* A dongle 12 ppm fast hears every carrier low by 12 ppm of its frequency. */
    const uint32_t f[]={739000000,751000000,881500000,892000000};
    for(unsigned i=0;i<4;i++) LS_CHECK(cell_ppm_add(&p,f[i],100+(int)i,-(int)lroundf(12e-6f*f[i]),0));
    LS_CHECK(cell_ppm_estimate(&p,&e));
    LS_EQ_UINT(e.n,4);LS_EQ_UINT(e.agree,4);LS_NEAR(e.ppm,12.f,.01);LS_CHECK(e.spread<.1f);
    /* One cell that disagrees by 20 ppm moves the median a little, not the answer. */
    LS_CHECK(cell_ppm_add(&p,800000000,9,-(int)lroundf(32e-6f*800000000.f),0));
    LS_CHECK(cell_ppm_estimate(&p,&e));
    LS_EQ_UINT(e.n,5);LS_EQ_UINT(e.agree,4);LS_NEAR(e.ppm,12.f,.1);
    /* Even count: the middle two. */
    cell_ppm_reset(&p);
    cell_ppm_add(&p,800000000,1,-8000,0);cell_ppm_add(&p,800000000,2,-9600,0);
    LS_CHECK(cell_ppm_estimate(&p,&e));LS_NEAR(e.ppm,11.,.01);LS_NEAR(e.spread,2.,.01);
}
LS_CASE(an_offset_measured_under_a_correction_is_added_back_and_a_cell_counts_once)
{
    cell_ppm_t p;cell_ppm_reset(&p);cell_ppm_est_t e;
    /* 12 ppm fast, 10 already corrected: the sync sees the 2 ppm left over. */
    LS_CHECK(cell_ppm_add(&p,800000000,5,-1600,10));
    LS_CHECK(cell_ppm_estimate(&p,&e));LS_NEAR(e.ppm,12.,.01);
    /* Hearing the same cell again replaces it. */
    LS_CHECK(cell_ppm_add(&p,800000000,5,-1760,10));
    LS_CHECK(cell_ppm_estimate(&p,&e));LS_EQ_UINT(e.n,1);LS_NEAR(e.ppm,12.2,.01);
    /* The sign: a cell above its nominal means the dongle tunes low. */
    cell_ppm_reset(&p);cell_ppm_add(&p,800000000,5,+4000,0);
    cell_ppm_estimate(&p,&e);LS_NEAR(e.ppm,-5.,.01);
    /* A reading no crystal explains is refused, and so is no carrier. */
    LS_CHECK(!cell_ppm_add(&p,800000000,6,130000,0));
    LS_CHECK(!cell_ppm_add(&p,0,6,10,0));
    LS_CHECK(!cell_ppm_add(NULL,800000000,6,10,0));
    /* A full table overwrites its oldest. */
    cell_ppm_reset(&p);
    for(int i=0;i<CELL_PPM_MAX+5;i++) LS_CHECK(cell_ppm_add(&p,800000000+(uint32_t)i*100000,i,-8000,0));
    LS_EQ_UINT(p.n,CELL_PPM_MAX);
}
LS_CASE(the_search_window_is_wide_until_the_clock_is_known_then_narrow)
{
    int c,s;
    cell_cfo_window(false,0,0,880000000,&c,&s);LS_EQ_INT(c,0);LS_EQ_INT(s,40000);
    /* 12 ppm fast at 881.5 MHz: expect about -10.6 kHz. */
    cell_cfo_window(true,12.f,0,881500000,&c,&s);LS_EQ_INT(c,-10000);LS_EQ_INT(s,5000);
    /* With 12 ppm applied nothing is left. */
    cell_cfo_window(true,12.f,12.f,881500000,&c,&s);LS_EQ_INT(c,0);LS_EQ_INT(s,5000);
    /* Lower carriers see less of the same ppm. */
    int c881;cell_cfo_window(true,12.f,0,881500000,&c881,&s);
    int c2;cell_cfo_window(true,12.f,0,617000000,&c2,&s);LS_CHECK(c2>c881 && c2==-5000);
    cell_cfo_window(true,-30.f,0,881500000,&c,&s);LS_EQ_INT(c,25000);
    /* Never past what the PSS search and the MIB accept. */
    cell_cfo_window(true,150.f,0,960000000,&c,&s);LS_EQ_INT(c,-35000);
    LS_CHECK(abs(c)+s<=40000);
    cell_cfo_window(true,1.f,0,0,&c,&s);LS_EQ_INT(c,0);LS_EQ_INT(s,40000);
    /* Narrow is 3 templates a root instead of 17. */
    LS_EQ_INT((2*5000)/CELL_CFO_STEP+1,3);LS_EQ_INT((2*40000)/CELL_CFO_STEP+1,17);
}

/* ---- occupancy plateaus and the raster points worth searching ---- */
#define BINS 1000          /* 25 MHz of B5 in 25 kHz bins */
static int8_t spectrum[BINS];
static void floor_noise(int level,unsigned seed)
{
    ls_rng_t r;ls_rng_seed(&r,seed);
    for(unsigned i=0;i<BINS;i++) spectrum[i]=(int8_t)(level+(int)(ls_rng_u32(&r)%5)-2);
}
static void carrier(uint32_t lo_hz,uint32_t hi_hz,int level,unsigned seed)
{
    ls_rng_t r;ls_rng_seed(&r,seed);
    for(unsigned i=0;i<BINS;i++) {
        uint32_t f=869000000u+i*25000u+12500u;
        if(f>=lo_hz && f<hi_hz) spectrum[i]=(int8_t)(level+(int)(ls_rng_u32(&r)%5)-2);
    }
}
static bool has(const uint32_t *list,unsigned n,uint32_t hz)
{
    for(unsigned i=0;i<n;i++) if(list[i]==hz) return true;
    return false;
}
LS_CASE(two_carriers_become_two_plateaus_with_their_edges_and_their_centres_are_candidates)
{
    floor_noise(-85,1);
    carrier(877000000,886000000,-55,2);        /* 10 MHz carrier, centre 881.5 */
    carrier(886750000,891250000,-62,3);        /* 5 MHz carrier, centre 889.0 */
    cell_plateau_t pl[CELL_PLATEAU_MAX];int fl;
    unsigned np=cell_plateaus(spectrum,BINS,869000000,25000,pl,CELL_PLATEAU_MAX,&fl);
    LS_EQ_UINT(np,2);
    LS_CHECK(fl<=-83 && fl>=-88);
    LS_CHECK(pl[0].lo>=876800000 && pl[0].lo<=877300000 && pl[0].hi>=885700000 && pl[0].hi<=886200000);
    LS_CHECK(pl[1].lo>=886500000 && pl[1].lo<=887100000 && pl[1].hi>=891000000 && pl[1].hi<=891500000);
    LS_CHECK(pl[0].level>pl[1].level);
    uint32_t cand[32];
    unsigned nc=cell_candidates(pl,np,869700000,893300000,cand,32);
    LS_CHECK(has(cand,nc,881500000) && has(cand,nc,889000000));
    LS_CHECK(nc<=5);
    for(unsigned i=0;i<nc;i++) {
        LS_CHECK((cand[i]-869700000u)%CELL_RASTER_HZ==0);
        LS_CHECK(i==0 || cand[i]>cand[i-1]);
    }
    /* Under a tenth of the 237-point raster. */
    LS_CHECK(nc*10<237);
}
LS_CASE(an_empty_band_a_narrow_spur_and_a_missing_pass_give_nothing_to_search)
{
    floor_noise(-85,4);
    cell_plateau_t pl[CELL_PLATEAU_MAX];int fl;uint32_t cand[8];
    LS_EQ_UINT(cell_plateaus(spectrum,BINS,869000000,25000,pl,CELL_PLATEAU_MAX,&fl),0);
    carrier(880000000,880100000,-40,5);        /* a 100 kHz spur is not a carrier */
    LS_EQ_UINT(cell_plateaus(spectrum,BINS,869000000,25000,pl,CELL_PLATEAU_MAX,&fl),0);
    /* A weak hump a couple of dB up is noise, not a carrier. */
    carrier(875000000,885000000,-82,6);
    LS_EQ_UINT(cell_plateaus(spectrum,BINS,869000000,25000,pl,CELL_PLATEAU_MAX,&fl),0);
    LS_EQ_UINT(cell_candidates(pl,0,869700000,893300000,cand,8),0);
    int8_t gone[BINS];memset(gone,CELL_MISSING,sizeof(gone));
    LS_EQ_UINT(cell_plateaus(gone,BINS,869000000,25000,pl,CELL_PLATEAU_MAX,&fl),0);
    LS_EQ_UINT(cell_plateaus(NULL,BINS,869000000,25000,pl,CELL_PLATEAU_MAX,&fl),0);
    LS_EQ_UINT(cell_plateaus(spectrum,4,869000000,25000,pl,CELL_PLATEAU_MAX,&fl),0);
}
LS_CASE(one_carrier_filling_the_band_still_has_a_floor_in_its_guard_bands)
{
    /* B13 is one 10 MHz carrier: 746-756 MHz, 90% occupied, guard at each end. */
    static int8_t b13[400];
    ls_rng_t r;ls_rng_seed(&r,7);
    for(unsigned i=0;i<400;i++){
        uint32_t f=746000000u+i*25000u+12500u;
        b13[i]=(int8_t)(((f>=746500000u && f<755500000u)?-58:-85)+(int)(ls_rng_u32(&r)%5)-2);
    }
    cell_plateau_t pl[CELL_PLATEAU_MAX];int fl;uint32_t cand[8];
    unsigned np=cell_plateaus(b13,400,746000000,25000,pl,CELL_PLATEAU_MAX,&fl);
    LS_EQ_UINT(np,1);
    unsigned nc=cell_candidates(pl,np,746700000,755300000,cand,8);
    LS_CHECK(has(cand,nc,751000000));LS_CHECK(nc<=2);
}
LS_CASE(a_merged_pair_and_an_odd_width_fall_back_to_edge_distances_and_stay_bounded)
{
    floor_noise(-85,8);
    /* A 3 MHz and a 10 MHz carrier with no gap between: one 11.7 MHz plateau
     * that is no standard width. */
    carrier(871050000,873750000,-60,9);   /* 3 MHz, centre 872.4 */
    carrier(873800000,882800000,-60,10);  /* 10 MHz, centre 878.3 */
    cell_plateau_t pl[CELL_PLATEAU_MAX];int fl;uint32_t cand[48];
    unsigned np=cell_plateaus(spectrum,BINS,869000000,25000,pl,CELL_PLATEAU_MAX,&fl);
    LS_EQ_UINT(np,1);
    unsigned nc=cell_candidates(pl,np,869700000,893300000,cand,48);
    LS_CHECK(has(cand,nc,878300000));
    LS_CHECK(has(cand,nc,872400000));
    LS_CHECK(nc<=14);
    /* A cap keeps the strongest plateau's points. */
    unsigned few=cell_candidates(pl,np,869700000,893300000,cand,3);LS_EQ_UINT(few,3);
}
LS_CASE(candidates_stay_inside_the_scanned_window_and_the_strongest_plateau_wins_a_cap)
{
    cell_plateau_t pl[2]={{869000000,871000000,-70},{890000000,893000000,-50}};
    uint32_t cand[8];
    unsigned nc=cell_candidates(pl,2,869700000,893300000,cand,1);
    LS_EQ_UINT(nc,1);LS_CHECK(cand[0]>=890000000 && cand[0]<=893000000);
    nc=cell_candidates(pl,2,869700000,893300000,cand,8);
    for(unsigned i=0;i<nc;i++) LS_CHECK(cand[i]>=869700000 && cand[i]<=893300000);
    LS_EQ_UINT(cell_candidates(pl,2,893300000,869700000,cand,8),0);
}
LS_CASE(gating_cuts_a_b5_pass_by_an_order_of_magnitude)
{
    /* A plausible US 850 MHz band: 10 MHz at 879.0 (873.5-884.5) is not what
     * matters; the count of searched points against the raster is. */
    floor_noise(-85,11);
    carrier(874000000,883000000,-56,12);
    carrier(885250000,889750000,-60,13);
    carrier(890450000,893150000,-63,14);
    cell_plateau_t pl[CELL_PLATEAU_MAX];int fl;uint32_t cand[48];
    unsigned np=cell_plateaus(spectrum,BINS,869000000,25000,pl,CELL_PLATEAU_MAX,&fl);
    unsigned nc=cell_candidates(pl,np,869700000,893300000,cand,48);
    LS_EQ_UINT(np,3);
    LS_CHECK(nc>=3 && nc*8<=237);
    /* At ~1.4 s a point: the 237-point raster is 5.5 minutes, this is under 20 s. */
    LS_CHECK(nc*1.4<20.);
}
