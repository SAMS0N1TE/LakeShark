#include "rec_catch_core.h"
#include "subghz_file.h"
#include "ls_sub_fsk.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static catch_detector_t detector, fragment;
static float bins[CATCH_BINS];
static catch_burst_t last;
static int starts,stops;
static uint64_t clock_us;
static unsigned begins,ends;
static bool refuse;
static void event(const catch_burst_t *b,bool start,void *ctx)
{(void)ctx;last=*b;if(start)starts++;else stops++;}
static uint64_t now(void *ctx){(void)ctx;return clock_us;}
static bool begin(void *ctx,const catch_burst_t *b){(void)ctx;(void)b;begins++;clock_us+=3200;return !refuse;}
static void end(void *ctx){(void)ctx;ends++;}
static void noise(void){for(int i=0;i<CATCH_BINS;i++)bins[i]=-70;}
static void warm(void)
{catch_detector_init(&detector,433920000);starts=stops=0;noise();for(int i=0;i<300;i++)catch_detector_spectrum(&detector,bins,i*1000,event,NULL);}
int main(void)
{
    warm();bins[300]=-25;bins[299]=bins[301]=-40;
    for(int i=300;i<305;i++)catch_detector_spectrum(&detector,bins,i*1000,event,NULL);
    assert(!starts); /* Publish after the burst ends, so max duration is enforceable. */
    noise();catch_detector_spectrum(&detector,bins,314000,event,NULL);
    assert(starts==1 && stops==1 && last.stop_us==304000);
    assert(last.frequency==434126250 && last.bandwidth==14062 && last.modulation==CATCH_OOK);
    warm();bins[280]=-30;bins[290]=-32;
    for(int i=300;i<304;i++)catch_detector_spectrum(&detector,bins,i*1000,event,NULL);
    noise();catch_detector_spectrum(&detector,bins,314000,event,NULL);
    assert(starts==1 && last.modulation==CATCH_FSK && last.deviation==23437);
    assert(last.frequency==434055937);
    warm();bins[256]=-10;bins[0]=bins[511]=-10;
    for(int i=300;i<320;i++)catch_detector_spectrum(&detector,bins,i*1000,event,NULL);
    noise();catch_detector_spectrum(&detector,bins,340000,event,NULL);assert(!starts);
    warm();bins[300]=-30;catch_detector_spectrum(&detector,bins,300000,event,NULL);
    assert(detector.recent[0].reason==CATCH_CANDIDATE);
    noise();catch_detector_spectrum(&detector,bins,314000,event,NULL);assert(!starts && !stops);
    assert(detector.recent[0].reason==CATCH_TOO_SHORT);
    warm();bins[100]=-20;bins[350]=-20;
    for(int i=300;i<304;i++)catch_detector_spectrum(&detector,bins,i*1000,event,NULL);
    noise();catch_detector_spectrum(&detector,bins,314000,event,NULL);assert(starts==2);
    /* OOK holes and nearby frequency jitter belong to one burst. */
    warm();bins[100]=-20;
    for(int i=300;i<305;i++)catch_detector_spectrum(&detector,bins,i*1000,event,NULL);
    noise();catch_detector_spectrum(&detector,bins,309000,event,NULL);
    bins[101]=-22;
    for(int i=310;i<315;i++)catch_detector_spectrum(&detector,bins,i*1000,event,NULL);
    noise();catch_detector_spectrum(&detector,bins,324000,event,NULL);
    assert(starts==1 && stops==1 && last.start_us==300000 && last.stop_us==314000);
    /* Consecutive sparse observations are not an observed 10 ms silence. */
    warm();bins[384]=-5;
    for(int i=0;i<4;i++)catch_detector_spectrum(&detector,bins,300000+i*13653,event,NULL);
    noise();catch_detector_spectrum(&detector,bins,355000,event,NULL);
    assert(starts==1 && stops==1 && last.start_us==300000 && last.stop_us==340959);
    /* A real silent frame still splits bursts, and a long blind gap cannot
       turn two isolated sightings into duration-qualified evidence. */
    warm();bins[384]=-5;catch_detector_spectrum(&detector,bins,300000,event,NULL);
    catch_detector_spectrum(&detector,bins,351000,event,NULL);
    noise();catch_detector_spectrum(&detector,bins,365000,event,NULL);assert(!starts);
    warm();bins[384]=-5;
    catch_detector_spectrum(&detector,bins,300000,event,NULL);
    catch_detector_spectrum(&detector,bins,313653,event,NULL);
    noise();catch_detector_spectrum(&detector,bins,327306,event,NULL);
    bins[384]=-5;
    catch_detector_spectrum(&detector,bins,340959,event,NULL);
    catch_detector_spectrum(&detector,bins,354612,event,NULL);
    noise();catch_detector_spectrum(&detector,bins,368265,event,NULL);assert(starts==2);
    /* Saturation skirts are broad but well below the true peak. A strong
       alias at -10 dBc must not become a second detection either. */
    warm();detector.clip_active=true;
    for(int i=330;i<=430;i++)bins[i]=-30;
    bins[384]=-3;bins[383]=bins[385]=-10;bins[128]=-13;
    for(int i=300;i<340;i+=13)catch_detector_spectrum(&detector,bins,i*1000,event,NULL);
    noise();detector.clip_active=false;catch_detector_spectrum(&detector,bins,355000,event,NULL);
    assert(starts==1 && last.frequency==434520000 && !detector.reasons[CATCH_TOO_WIDE]);
    /* One wide region is one rejection, even across several FFT frames. */
    warm();for(int i=100;i<=170;i++)bins[i]=-30;
    for(int i=300;i<306;i++)catch_detector_spectrum(&detector,bins,i*1000,event,NULL);
    assert(!starts && detector.reasons[CATCH_TOO_WIDE]==1);
    /* An overlong spur never becomes a burst. */
    warm();bins[100]=-20;
    for(int i=300;i<600;i++)catch_detector_spectrum(&detector,bins,i*1000,event,NULL);
    noise();catch_detector_spectrum(&detector,bins,610000,event,NULL);assert(!starts);
    /* Broadband gain changes are relearned; slow tracking is time based. */
    warm();for(int i=0;i<CATCH_BINS;i++)bins[i]=-45;
    for(int i=300;i<600;i++)catch_detector_spectrum(&detector,bins,i*1000,event,NULL);
    assert(!starts && detector.reported_floor>-46);
    static uint8_t iq[8192];
    /* Closing a refined candidate never requires another full FFT. Two
       quiet transport blocks close it; a blind gap also has a hard timeout. */
    warm();bins[300]=-25;
    for(int i=300;i<305;i++)catch_detector_spectrum(&detector,bins,i*1000,event,NULL);
    memset(iq,128,sizeof(iq));catch_detector_allow(&detector,0);
    unsigned frames=detector.frames;
    catch_detector_iq(&detector,iq,sizeof(iq),315000,event,NULL);assert(!starts);
    catch_detector_iq(&detector,iq,sizeof(iq),322000,event,NULL);
    assert(starts==1 && stops==1 && last.stop_us==304000 && detector.frames==frames);
    warm();bins[300]=-25;
    for(int i=300;i<305;i++)catch_detector_spectrum(&detector,bins,i*1000,event,NULL);
    catch_detector_allow(&detector,0);frames=detector.frames;
    catch_detector_iq(&detector,iq,sizeof(iq),360000,event,NULL);
    assert(starts==1 && stops==1 && last.stop_us==304000 && detector.frames==frames);
    uint32_t random=42;starts=stops=0;
    catch_detector_init(&detector,915000000);catch_detector_init(&fragment,915000000);
    for(int block=0;block<240;block++) {
        for(int i=0;i<4096;i++) {
            double phase=6.283185307*(42.25)*(block*4096+i)/512;
            random=random*1664525u+1013904223u;
            int ni=(int)(random>>29)-4;
            random=random*1664525u+1013904223u;
            int nq=(int)(random>>29)-4;
            iq[2*i]=(uint8_t)(128+ni+(block>=190 && block<210?70:0)*cos(phase));
            iq[2*i+1]=(uint8_t)(128+nq+(block>=190 && block<210?70:0)*sin(phase));
        }
        uint64_t t=(uint64_t)(block+1)*4096*1000000/CATCH_RATE;
        catch_detector_iq(&detector,iq,sizeof(iq),t,event,NULL);
        catch_detector_iq(&fragment,iq,301,t-(8192-301)*500000ull/CATCH_RATE,NULL,NULL);
        catch_detector_iq(&fragment,iq+301,7891,t,NULL,NULL);
    }
    assert(detector.frames==fragment.frames);
    for(int i=0;i<CATCH_BINS;i++)assert(fabsf(detector.db[i]-fragment.db[i])<0.001f);
    assert(starts==1 && stops==1);
    assert(abs((int)last.frequency-915198047)<2000);
    catch_handoff_ops_t ops={now,begin,end,NULL};catch_hold_t hold={0};uint32_t latency;
    last.frequency=433920000;last.detected_us=10000;clock_us=10500;
    assert(catch_handoff(&hold,&last,&ops,&latency) && latency==3700 && begins==1);
    printf("fake handoff: 500 us scheduling + 3200 us session = %u us\n",latency);
    clock_us+=1000;assert(catch_handoff(&hold,&last,&ops,&latency) && latency==0 && begins==1);
    last.frequency=868300000;assert(!catch_handoff(&hold,&last,&ops,&latency));
    clock_us=hold.until_us;assert(catch_handoff(&hold,&last,&ops,&latency) && begins==2 && ends==1);
    catch_handoff_stop(&hold,&ops);catch_handoff_stop(&hold,&ops);assert(ends==2);
    refuse=true;assert(!catch_handoff(&hold,&last,&ops,&latency) && !hold.listening);
    const char *path="catch_roundtrip.sub";remove(path);
    subghz_file_t file={.freq_hz=433920000,.bitrate=76800,.deviation_hz=23437};
    strcpy(file.preset,"FuriHalSubGhzPresetCustom");
    int32_t raw[]={100,-200,100,-200,100,-200},loaded[8];static char line[SUBGHZ_FILE_LINE_BYTES];
    char preset_text[512];ls_sub_mod_t mod={76800,23437};
    assert(ls_sub_preset_text(&mod,preset_text,sizeof(preset_text))>0);
    assert(subghz_file_save(path,&file,raw,6,preset_text));assert(!subghz_file_save(path,&file,raw,6,preset_text));
    assert(subghz_file_load(path,&file,loaded,8,line,sizeof(line)));assert(subghz_file_is_fsk(&file));
    assert(file.edges==6 && !memcmp(raw,loaded,sizeof(raw)));remove(path);
    assert(subghz_file_is_cc_fsk(&file));
    puts("CATCH core OK");return 0;
}
