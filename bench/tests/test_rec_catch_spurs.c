/* Synthetic RTL interference and continuous-phase FSK, no decoder imports. */
#include "rec_catch_core.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <string.h>
#include "catch_fsk_fixture.h"
static catch_detector_t detector;
static uint8_t iq[32768];
static catch_hold_t hold;
static catch_sources_t sources;
static unsigned captures, handed_extras;
static uint64_t clock_us;
static unsigned targets, extras, warm_extras, begins, ends, seen[8];
static uint32_t handed_hz;
static unsigned stronger;
static void stronger_event(const catch_burst_t *b,bool start,void *ctx)
{(void)ctx;if(start) {assert(!b->spur && b->rtl_dbfs>-30);stronger++;}}
static uint64_t now(void *ctx) {(void)ctx;return clock_us;}
static bool begin(void *ctx,const catch_burst_t *b)
{(void)ctx;begins++;handed_hz=b->frequency;return true;}
static void end(void *ctx) {(void)ctx;ends++;}
static const catch_handoff_ops_t ops={now,begin,end,NULL};
static void event(const catch_burst_t *b,bool start,void *ctx)
{
    (void)ctx;if(!start)return;
    catch_burst_t observed=*b;
    bool eligible=catch_source_observe(&sources,&observed,915000000,clock_us);
    if(abs((int)b->frequency-915600000)>20000) {
        if(b->start_us<3000000)warm_extras++;else extras++;
        if(eligible) {uint32_t latency; if(catch_handoff(&hold,&observed,&ops,&latency)) {handed_extras++;printf("non-target handed %u Hz %.1f dBFS repeat %u at %llu dur %llu\n",observed.frequency,(double)observed.rtl_dbfs,observed.repeats,(unsigned long long)observed.start_us,(unsigned long long)(observed.stop_us-observed.start_us));}}
        return;
    }
    unsigned packet=(unsigned)((b->start_us-12000000)/3000000);
    printf("target %u start %llu duration %llu repeat %u\n",packet,
        (unsigned long long)b->start_us,(unsigned long long)(b->stop_us-b->start_us),observed.repeats);
    assert(packet<8);seen[packet]++;targets++;
    if(eligible) {
        uint32_t latency;assert(catch_handoff(&hold,&observed,&ops,&latency));
        assert(abs((int)handed_hz-915600000)<20000);
    }
}
static double seconds(void)
{struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);return t.tv_sec+t.tv_nsec/1e9;}
/* Board-rate transport, no sleeps or skipped IQ. The first tooth walks
   across ten FFT bins, including the reported -21 dBFS peak. */
static unsigned full_rows, full_extras, full_seen[8];
static void full_event(const catch_burst_t *b,bool start,void *ctx)
{
    (void)ctx;if(!start || b->start_us<3000000)return;
    if(abs((int)b->frequency-915600000)>15000) {full_extras++;return;}
    assert(!b->spur && b->start_us>=12000000);
    unsigned packet=(unsigned)((b->start_us-12000000)/3000000);
    printf("full packet %u start %llu stop %llu hz %u\n",packet,(unsigned long long)b->start_us,(unsigned long long)b->stop_us,b->frequency);
    assert(packet<8);full_seen[packet]++;full_rows++;
}
static void full_rate_wander(void)
{
    catch_fixture_t f={.random=0x615193,.re=1,.amplitude=12,.delay_samples=11ull*CATCH_RATE};
    const int wander[]={-835000,-827000,-804000,-802000,-789000};
    double re[4]={1,1,1,1},im[4]={0},active=0;
    catch_detector_init(&detector,915000000);
    for(unsigned block=0;f.sample<45ull*CATCH_RATE;block++) {
        size_t n=17242; /* 278.39 blocks/s, 4.800 MB/s. */
        fixture_block(&f,iq,n);
        fixture_wandering_spurs(iq,n,block,re,im);
        uint64_t now=f.sample*1000000/CATCH_RATE;
        catch_detector_allow(&detector,1);
        double t=seconds();catch_detector_iq(&detector,iq,n,now,full_event,NULL);
        active+=seconds()-t;
    }
    printf("full-rate wandering comb: rows %u/8 extras %u, checks %u FFT %u, host duty %.2f%%, RAM %zu bytes\n",
        full_rows,full_extras,detector.coarse_frames,detector.frames,100*active/45,sizeof(detector));
    fflush(stdout);
    assert(full_rows==8 && full_extras==0);
    for(unsigned j=0;j<8;j++)assert(full_seen[j]==1);
    for(unsigned j=0;j<5;j++)assert(catch_detector_is_spur(&detector,915000000+ wander[j],-21));
    assert(!catch_detector_is_spur(&detector,915600000,-35));
}
static void offset_comb(void)
{
    float db[CATCH_BINS];catch_spur_info_t info[CATCH_SPUR_TOP];
    for(int i=0;i<CATCH_BINS;i++)db[i]=-70;
    db[70]=db[150]=db[230]=-35;
    catch_detector_init(&detector,915000000);
    for(unsigned t=0;t<2000000;t+=3592)catch_detector_spectrum(&detector,db,t,NULL,NULL);
    unsigned count=catch_detector_spurs(&detector,info,CATCH_SPUR_TOP);
    assert(count==3);
    for(unsigned j=0;j<count;j++)assert(info[j].comb);
    /* A single wandering tooth cannot supply three family members. */
    db[150]=db[230]=-70;catch_detector_init(&detector,915000000);
    for(unsigned t=0;t<2000000;t+=3592)catch_detector_spectrum(&detector,db,t,NULL,NULL);
    assert(catch_detector_spurs(&detector,info,CATCH_SPUR_TOP)==1 && !info[0].comb);
}
int main(void)
{
    offset_comb();
    full_rate_wander();
    for(unsigned mode=0;mode<4;mode++) {
        catch_fixture_t f={.random=0x76123456+mode,.re=1,.amplitude=7,.delay_samples=11ull*CATCH_RATE};
        double re[4]={1,1,1,1},im[4]={0},cr[4],ci[4];
        const int offset[4]={-800000,-400000,400000,800000};
        double nr=1,ni=0; /* Intermittent near-DC leakage at +30 kHz. */
        const double nc=cos(6.283185307179586*30000/CATCH_RATE),ns=sin(6.283185307179586*30000/CATCH_RATE);
        for(unsigned j=0;j<4;j++) {
            double a=6.283185307179586*offset[j]/CATCH_RATE;
            cr[j]=cos(a);ci[j]=sin(a);
        }
        catch_detector_init(&detector,915000000);hold=(catch_hold_t){0};
        targets=extras=warm_extras=begins=ends=captures=handed_extras=0;memset(&sources,0,sizeof(sources));memset(seen,0,sizeof(seen));
        double active=0;
        unsigned late_frames=0;
        for(unsigned block=0;block<5275;block++) {
            fixture_block(&f,iq,sizeof(iq));
            /* RTL-like Gaussian noise, DC and four ~18 dB SNR
               narrow interferers; either steady, alternating blocks, or
               two blocks on followed by two off. */
            for(unsigned i=0;i<sizeof(iq)/2;i++) {
                double r=iq[2*i],q=iq[2*i+1];
                for(unsigned j=0;j<4;j++) {
                    double nr=re[j]*cr[j]-im[j]*ci[j];
                    im[j]=re[j]*ci[j]+im[j]*cr[j];re[j]=nr;
                    bool on=mode==0 || (mode==1?(block+j)%2==0:((block+j)/2)%2==0);
                    if(on) {r+=5*re[j];q+=5*im[j];}
                }
                double nnext=nr*nc-ni*ns;ni=nr*ns+ni*nc;nr=nnext;
                if(block%4<2) {r+=8*nr;q+=8*ni;}
                /* Three unrelated one-off bursts during the first ten seconds. */
                uint64_t sample=f.sample-sizeof(iq)/2+i;
                for(unsigned one=0;one<3;one++) {
                    uint64_t start=(2+one*3)* (uint64_t)CATCH_RATE;
                    if(sample>=start && sample<start+CATCH_RATE*47/1000) {
                        double a=6.283185307179586*(200000+one*100000)*(double)sample/CATCH_RATE;
                        r+=4*cos(a);q+=4*sin(a);
                    }
                }
                iq[2*i]=fixture_u8(r);iq[2*i+1]=fixture_u8(q);
            }
            clock_us=f.sample*1000000/CATCH_RATE;
            /* Poll hard bits before reporting the completed RTL burst. Once
               tuned after packet 2, the socket receives packets 3..8. */
            for(unsigned j=0;j<8;j++) {
                uint64_t packet_end=12000515ull+j*3000000ull+47000;
                uint64_t block_start=(f.sample-sizeof(iq)/2)*1000000/CATCH_RATE;
                if(block_start<packet_end && clock_us>=packet_end && hold.listening &&
                   abs((int)hold.frequency-915600000)<20000)captures++;
            }
            if(catch_handoff_expired(&hold,clock_us))catch_handoff_stop(&hold,&ops);
            double t=seconds();catch_detector_allow(&detector,mode==3?4:1);
            catch_detector_iq(&detector,iq,sizeof(iq),clock_us,event,NULL);
            active+=seconds()-t;
            if(block==147)late_frames=detector.frames;
        }
        catch_spur_info_t info[CATCH_SPUR_TOP];
        unsigned learned=catch_detector_spurs(&detector,info,CATCH_SPUR_TOP),comb=0;
        for(unsigned j=0;j<learned && j<CATCH_SPUR_TOP;j++)if(info[j].comb)comb++;
        printf("comb mode %u: targets %u/8 extras after 3 s %u warm %u learned %u comb %u FFT %u late %u host duty %.2f%% detector %zu bytes\n",
            mode,targets,extras,warm_extras,learned,comb,detector.frames,detector.frames-late_frames,100*active/36,sizeof(detector));
        fflush(stdout);
        assert(targets==8 && learned>=4 && comb>=4);
        /* Sparse windows may split one-offs; they must never prove a repeat. */
        for(unsigned j=0;j<8;j++)assert(seen[j]==1);
        printf("repeat policy: captures %u/8, non-target handoffs %u\n",captures,handed_extras);fflush(stdout);
        assert(captures>=6 && handed_extras==0);
        assert(begins==1 && hold.listening && abs((int)hold.frequency-915600000)<20000 &&
               handed_hz>915580000 && handed_hz<915620000);
        assert(detector.frames-late_frames<2800); /* <80 FFT/s after learning. */
    }
    /* Direct spectral evidence verifies +10 dB escape and quiet decay. */
    float db[CATCH_BINS];
    for(int i=0;i<CATCH_BINS;i++)db[i]=-70;
    catch_detector_init(&detector,915000000);
    for(unsigned t=0;t<=1000000;t+=2000) {
        db[341]=t%20000<6000?-40:-70;
        catch_detector_spectrum(&detector,db,t,NULL,NULL);
    }
    uint32_t hz=915000000+85*CATCH_RATE/CATCH_BINS;
    assert(catch_detector_is_spur(&detector,hz,-40));
    assert(!catch_detector_is_spur(&detector,hz,-29));
    db[341]=-25;stronger=0;
    for(unsigned t=1002000;t<1049000;t+=2000)catch_detector_spectrum(&detector,db,t,stronger_event,NULL);
    db[341]=-70;
    for(unsigned t=1050000;t<1070000;t+=2000)catch_detector_spectrum(&detector,db,t,stronger_event,NULL);
    assert(stronger==1);
    db[341]=-70;
    for(unsigned t=1070000;t<4200000;t+=2000)catch_detector_spectrum(&detector,db,t,NULL,NULL);
    assert(!catch_detector_is_spur(&detector,hz,-40));
    hold=(catch_hold_t){0};clock_us=5000000;uint32_t latency;
    catch_burst_t b={.frequency=914200000,.rtl_dbfs=-40,.detected_us=clock_us,.spur=true};
    assert(!catch_handoff(&hold,&b,&ops,&latency) && !hold.listening);
    b.spur=false;assert(catch_handoff(&hold,&b,&ops,&latency));
    b.frequency=915600000;b.rtl_dbfs=-31;assert(!catch_handoff(&hold,&b,&ops,&latency));
    b.rtl_dbfs=-30;assert(catch_handoff(&hold,&b,&ops,&latency) && hold.frequency==915600000);
    clock_us+=CATCH_SILENT_US;assert(catch_handoff_expired(&hold,clock_us));
    catch_burst_t pending=b;b.rtl_dbfs=-40;b.detected_us+=1000;
    assert(!catch_handoff_prefer(&b,&pending));b.detected_us+=300000;
    assert(catch_handoff_prefer(&b,&pending));
    /* Repeat evidence outranks an unproven stronger held channel. */
    catch_handoff_stop(&hold,&ops);hold=(catch_hold_t){0};
    memset(&sources,0,sizeof(sources));clock_us=8000000;
    b=(catch_burst_t){.frequency=914500000,.rtl_dbfs=-20,.start_us=clock_us,.stop_us=clock_us+47000};
    assert(catch_source_observe(&sources,&b,915000000,clock_us));
    assert(catch_handoff(&hold,&b,&ops,&latency));
    b.frequency=915600000;b.rtl_dbfs=-41;b.start_us=9000000;b.stop_us=9047000;
    assert(!catch_source_observe(&sources,&b,915000000,b.start_us));
    b.start_us=12000000;b.stop_us=12047000;clock_us=b.stop_us;
    assert(catch_source_observe(&sources,&b,915000000,clock_us) && b.repeats==2);
    assert(catch_handoff(&hold,&b,&ops,&latency) && hold.frequency==915600000);
    assert(hold.repeated);
    catch_burst_t intruder=b;intruder.frequency=914400000;intruder.rtl_dbfs=-5;intruder.repeats=1;
    assert(!catch_handoff(&hold,&intruder,&ops,&latency));
    b.start_us+=3000000;b.stop_us=b.start_us+150000;
    assert(!catch_source_observe(&sources,&b,915000000,b.stop_us));
    b.frequency=915030000;b.rtl_dbfs=-10;
    assert(!catch_source_observe(&sources,&b,915000000,20000000));
    b.frequency=915026000;assert(!catch_source_observe(&sources,&b,915000000,20000000));
    b.frequency=914300000;assert(!catch_source_observe(&sources,&b,915000000,1000000));
    puts("repeat/duration preference, warm-up and near-DC handoff veto OK");
    puts("spur decay, strong override, repeat preemption, expiry and queue preference OK");
    return 0;
}
