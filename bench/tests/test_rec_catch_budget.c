/* Real 2.4 Msps IQ workload; timing excludes fixture generation and sleep. */
#include "rec_catch_core.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
static catch_detector_t detector;
static unsigned starts,stops;
static catch_burst_t burst;
static void event(const catch_burst_t *b,bool start,void *ctx)
{(void)ctx;if(start){starts++;burst=*b;}else stops++;}
static uint64_t replay_us, block_wall_us;
static uint64_t wall_us(void)
{struct timespec t;timespec_get(&t,TIME_UTC);return (uint64_t)t.tv_sec*1000000+t.tv_nsec/1000;}
static uint64_t clock_us(void *ctx)
{(void)ctx;return replay_us+wall_us()-block_wall_us;}
static double seconds(void)
{
    struct timespec t;timespec_get(&t,TIME_UTC);
    return (double)t.tv_sec+(double)t.tv_nsec/1e9;
}
int main(void)
{
    uint8_t *iq=malloc(2*CATCH_RATE);assert(iq);
    uint32_t random=12345;
    const double offset=-722500.0;
    for(unsigned i=0;i<CATCH_RATE;i++) {
        random=random*1664525u+1013904223u;int re=(int)(random>>28)-8;
        random=random*1664525u+1013904223u;int im=(int)(random>>28)-8;
        /* DC leakage and a Nyquist image persist, then change mid-run. */
        int dc=i<CATCH_RATE/2?15:25;
        re+=dc+(i&1?12:-12);im+=dc;
        bool on=i>=CATCH_RATE*65/100 && i<CATCH_RATE*69/100 &&
                (i/2400)%3!=0; /* Sparse 433.1975 MHz OOK with 1 ms holes. */
        double angle=6.283185307179586*offset*i/CATCH_RATE;
        iq[2*i]=(uint8_t)(128+re+(on?55*cos(angle):0));
        iq[2*i+1]=(uint8_t)(128+im+(on?55*sin(angle):0));
    }
    double costs[3];
    for(int run=0;run<3;run++) {
        starts=stops=0;catch_detector_init(&detector,433920000);detector.clock_us=clock_us;
        double begin=seconds();
        replay_us=0;block_wall_us=wall_us();
        uint64_t work_begin=clock_us(NULL);
        for(size_t pos=0;pos<2*CATCH_RATE;) {
            size_t n=32768;if(n>2*CATCH_RATE-pos)n=2*CATCH_RATE-pos;
            uint64_t end=(pos+n)*500000ull/CATCH_RATE;
            /* Pace the execution clock to sample delivery without sleeping.
               A one-second capture replayed in 2 ms must not compress a 40 ms
               packet into less than one expensive-stage cooldown interval.
               Stage timing within each block still measures real host work. */
            uint64_t previous=clock_us(NULL);
            replay_us=end>previous?end:previous;block_wall_us=wall_us();
            catch_detector_iq(&detector,iq+pos,n,end,event,NULL);pos+=n;
        }
        costs[run]=(seconds()-begin)*1000;
        fprintf(stderr,"run %d: %.3f ms, %u starts %u stops, %u FFT, %.6f MHz\n",
                run,costs[run],starts,stops,detector.frames,burst.frequency/1e6);
        assert(starts==1 && stops==1);
        assert(abs((int)burst.frequency-433197500)<3000);
        assert(burst.stop_us-burst.start_us>=CATCH_MIN_US && burst.stop_us-burst.start_us<=CATCH_MAX_US);
        /* Require detection and the duty invariant, not a fixed FFT count. */
        assert(detector.frames>0 && detector.frames<=250);
        uint64_t done=clock_us(NULL);
        uint64_t debt=detector.expensive_ready_us>done?detector.expensive_ready_us-done:0;
        assert(3*(detector.fft_us+detector.spectrum_us)<=done-work_begin+debt);
        assert(costs[run]<350); /* Host regression ceiling, not a P4 prediction. */
    }
    for(int i=0;i<3;i++)for(int j=i+1;j<3;j++)if(costs[j]<costs[i]){double t=costs[i];costs[i]=costs[j];costs[j]=t;}
    printf("CATCH host: median %.3f ms per 2.4 Msps second (%.2f%% host wall), detector %zu bytes\n",
           costs[1],costs[1]/10,sizeof(detector));
    printf("host stages: checks %llu us / %u (%.2f us/check), FFT %llu us / %u (%.2f us/frame), spectrum %llu us; full ceiling %u/s\n",
        (unsigned long long)detector.checks_us,detector.coarse_frames,(double)detector.checks_us/detector.coarse_frames,
        (unsigned long long)detector.fft_us,detector.frames,(double)detector.fft_us/detector.frames,
        (unsigned long long)detector.spectrum_us,CATCH_FULL_PER_SECOND);
    catch_detector_init(&detector,433920000);catch_detector_allow(&detector,0);
    catch_detector_iq(&detector,iq,2*CATCH_RATE,1000000,NULL,NULL);
    assert(!detector.frames && detector.coarse_frames>0); /* Zero means zero. */
    catch_detector_init(&detector,433920000);
    catch_detector_iq(&detector,iq,2*CATCH_RATE,1000000,NULL,NULL);
    assert(detector.frames<=CATCH_FULL_PER_SECOND+1); /* Two timestamp buckets. */
    free(iq);return 0;
}
