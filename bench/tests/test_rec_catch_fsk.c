#include "rec_catch_core.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include "catch_fsk_fixture.h"
static catch_detector_t detector;
static uint8_t iq[32768];
static unsigned detected, seen[8], extra;
static void event(const catch_burst_t *b,bool start,void *ctx)
{
    (void)ctx;if(!start)return;
    if(abs((int)b->frequency-915600000)>20000) {extra++;fprintf(stderr,"extra %u Hz %llu..%llu %.1f dBFS\n",b->frequency,(unsigned long long)b->start_us,(unsigned long long)b->stop_us,(double)b->rtl_dbfs);return;}
    unsigned index=(unsigned)(b->start_us/3000000);
    assert(index<8);seen[index]++;detected++;
}
static double seconds(void)
{struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);return t.tv_sec+t.tv_nsec/1e9;}
int main(void)
{
    const float snrs[]={-6,0,6,20,32};
    for(unsigned run=0;run<6;run++) {
        catch_fixture_t f={.random=0x12345678+run,.re=1,
            .amplitude=run<5?4.964819187*pow(10,snrs[run]/20):0};
        detected=extra=0;for(unsigned i=0;i<8;i++)seen[i]=0;
        catch_detector_init(&detector,915000000);
        double active=0;
        for(unsigned block=0;block<3516;block++) {
            fixture_block(&f,iq,sizeof(iq));
            uint64_t end=f.sample*1000000/CATCH_RATE;
            double begin=seconds();
            /* Worst bounded allowance: one full FFT per real-time USB block.
               Scheduling uses sample timestamps, never compressed replay time. */
            catch_detector_allow(&detector,1);
            catch_detector_iq(&detector,iq,sizeof(iq),end,event,NULL);
            active+=seconds()-begin;
        }
        printf("FSK %s SNR %.0f dB: %u/8 target bursts, extra %u, clips %llu, FFT %u checks %u, host duty %.2f%%\n",
            run<5?"burst":"noise",run<5?(double)snrs[run]:0.0,detected,extra,
            (unsigned long long)detector.clips,detector.frames,detector.coarse_frames,100*active/24);
        fflush(stdout);
        if(run<5) {
            assert(detected==8);for(unsigned i=0;i<8;i++)assert(seen[i]==1);
            /* Completed strong events survive subsequent ordinary noise. */
            unsigned retained=0;
            for(unsigned i=0;i<detector.peak_count;i++)if(abs((int)detector.peaks[i].frequency-915600000)<20000 &&
                detector.peaks[i].reason==CATCH_ACCEPTED)retained++;
            assert(retained>=8);
        }
        else assert(!detected);
        assert(!extra);assert(detector.coarse_frames==3516*4);
        if(run==4)assert(detector.clips>0);
    }
    printf("FSK 4800 baud +/-5 kHz 47 ms +600 kHz OK; detector %zu bytes\n",sizeof(detector));
    return 0;
}
