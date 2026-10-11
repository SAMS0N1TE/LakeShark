#include "rec_catch_core.h"
#include <math.h>
#include <string.h>
#include <stdlib.h>

void catch_detector_init(catch_detector_t *d, uint32_t center)
{
    memset(d, 0, sizeof(*d)); d->center = center; d->allowance=UINT32_MAX;
    d->candidate_limit=CATCH_TRACKS;
    for (int i=0;i<CATCH_BINS;i++) {
        d->floor[i] = -90;
        d->window[i] = 0.5f - 0.5f*cosf(6.283185307f*i/(CATCH_BINS-1));
    }
    for(int i=0;i<CATCH_COARSE;i++) {
        d->coarse_window[i]=(int16_t)(32767*d->window[i*(CATCH_BINS/CATCH_COARSE)]);
        if(i<CATCH_COARSE/2) {
            d->coarse_tw_re[i]=(int16_t)(32767*cosf(6.283185307f*i/CATCH_COARSE));
            d->coarse_tw_im[i]=(int16_t)(-32767*sinf(6.283185307f*i/CATCH_COARSE));
        }
    }
    for (int i=0;i<CATCH_BINS/2;i++) {
        d->tw_re[i]=cosf(6.283185307f*i/CATCH_BINS);
        d->tw_im[i]=-sinf(6.283185307f*i/CATCH_BINS);
    }
}
static uint32_t bin_hz(const catch_detector_t *d, float bin)
{ return (uint32_t)((double)d->center+(bin-CATCH_BINS/2)*CATCH_RATE/CATCH_BINS); }
static unsigned coarse_bin(const catch_detector_t *d,uint32_t hz)
{
    int bin=(int)(((int64_t)hz-d->center)*CATCH_COARSE/CATCH_RATE)+CATCH_COARSE/2;
    return (unsigned)(bin<0?0:bin>=CATCH_COARSE?CATCH_COARSE-1:bin);
}
static void trace_track(catch_detector_t *d,int slot,const char *state,const char *reason)
{
    if(!d->trace)return;
    catch_burst_t *b=&d->track[slot].burst;
    unsigned bin=coarse_bin(d,b->frequency);
    catch_trace_t t={d->block_us,b->id,b->frequency,10*log10f(fmaxf(d->coarse_level[bin],1e-6f)),bin,
        d->fft_state?d->fft_state:"skipped",state,reason};
    d->trace(&t,d->trace_context);
}

const char *catch_reason_name(catch_reason_t reason)
{
    static const char *names[]={"accepted","too short","too long","too wide",
        "under margin","DC/image mask","merged","learned spur","awaiting duration"};
    return reason<CATCH_REASON_COUNT?names[reason]:"unknown";
}
static void peak_record(catch_detector_t *d, uint32_t hz, float db, float snr,
                        uint64_t now, catch_reason_t reason)
{
    catch_peak_t *recent=&d->recent[(now/1000000)%CATCH_RECENT_SECONDS];
    if(!recent->frequency || recent->peak_us/1000000!=now/1000000 || snr>recent->snr_db)
        *recent=(catch_peak_t){.first_us=now,.last_us=now,.peak_us=now,
            .frequency=hz,.snr_db=snr,.dbfs=db,.reason=reason};
    int slot=-1;
    for(unsigned j=0;j<d->peak_count;j++) {
        catch_peak_t *p=&d->peaks[j];
        if(now>=p->last_us && now-p->last_us<CATCH_MERGE_US &&
           llabs((long long)p->frequency-hz)<15000) {slot=(int)j;break;}
    }
    if(slot<0) {
        if(d->peak_count<CATCH_PEAKS)slot=(int)d->peak_count++;
        else {
            slot=0;
            for(unsigned j=1;j<CATCH_PEAKS;j++) {
                if(d->peaks[j].snr_db<d->peaks[slot].snr_db)slot=(int)j;
            }
            if(snr<d->peaks[slot].snr_db)return;
        }
        d->peaks[slot]=(catch_peak_t){.first_us=now,.peak_us=now,.frequency=hz,.snr_db=snr,.dbfs=db,.reason=reason};

    }
    catch_peak_t *p=&d->peaks[slot];p->last_us=now;
    if(now-p->peak_us>CATCH_PEAK_WINDOW_US) {p->snr_db=snr;p->dbfs=db;p->frequency=hz;p->peak_us=now;}
    if(reason!=CATCH_CANDIDATE)p->reason=reason;
    if(snr>p->snr_db) {p->snr_db=snr;p->dbfs=db;p->frequency=hz;p->peak_us=now;}
}
static void finish_track(catch_detector_t *d, int slot, uint64_t now,
                         catch_event_fn cb, void *ctx)
{
    catch_burst_t *b=&d->track[slot].burst;b->stop_us=d->track[slot].last_us;
    b->spur=catch_detector_is_spur(d,b->frequency,b->rtl_dbfs);
    catch_reason_t reason=b->spur?CATCH_SPUR:d->track[slot].rejected?CATCH_TOO_LONG:
        d->track[slot].on_us<CATCH_MIN_US?CATCH_TOO_SHORT:CATCH_ACCEPTED;
    d->reasons[reason]++;
    trace_track(d,slot,"closed",catch_reason_name(reason));
    for(unsigned i=0;i<d->peak_count;i++) {
        catch_peak_t *p=&d->peaks[i];
        if(p->last_us>=b->start_us && p->first_us<=b->stop_us &&
           llabs((long long)p->frequency-b->frequency)<(long long)(b->bandwidth+15000)) p->reason=reason;
    }
    for(unsigned i=0;i<CATCH_RECENT_SECONDS;i++) {
        catch_peak_t *p=&d->recent[i];
        if(p->peak_us>=b->start_us && p->peak_us<=b->stop_us &&
           llabs((long long)p->frequency-b->frequency)<(long long)(b->bandwidth+15000))p->reason=reason;
    }
    if(reason==CATCH_ACCEPTED) {
        b->detected_us=now;if(cb) {cb(b,true,ctx);cb(b,false,ctx);}
    }
    d->track[slot].used=false;
}
/* Linear selection avoids sorting two PSRAM arrays per full frame, while
   retaining sub-dB thresholds for weak bursts. */
static float median(float *values, int count)
{
    int lo=0,hi=count-1,target=count/2;
    while(lo<hi) {
        float pivot=values[(lo+hi)/2];int left=lo,right=hi;
        while(left<=right) {
            while(values[left]<pivot)left++;
            while(values[right]>pivot)right--;
            if(left<=right) {float t=values[left];values[left++]=values[right];values[right--]=t;}
        }
        if(target<=right)hi=right;
        else if(target>=left)lo=left;
        else break;
    }
    return values[target];
}
static bool spur_bin(const catch_detector_t *d,int bin,float dbfs)
{
    for(int g=(bin-12)/4;g<=(bin+12)/4;g++)if(g>=0 && g<CATCH_SPUR_GROUPS &&
        d->spur[g].learned && bin>=(int)d->spur[g].low_bin-3 &&
        bin<=(int)d->spur[g].high_bin+3 && dbfs<d->spur[g].level+10)return true;
    return false;
}
bool catch_detector_is_spur(const catch_detector_t *d,uint32_t hz,float dbfs)
{
    int bin=(int)(((int64_t)hz-d->center)*CATCH_BINS/CATCH_RATE)+CATCH_BINS/2;
    return spur_bin(d,bin,dbfs);
}
static bool spur_primary(const catch_detector_t *d,int g)
{
    if(!d->spur[g].learned)return false;
    /* Overlapping occurrence windows describe one tooth, not a family. */
    for(int j=g-5;j<g;j++)if(j>=0 && d->spur[j].learned &&
        abs((int)d->spur[j].bin-d->spur[g].bin)<=16)return false;
    return true;
}
static void spur_family(catch_detector_t *d,int origin,int spacing)
{
    if(spacing<16)return;
    unsigned members=0;
    for(int g=1;g<CATCH_SPUR_GROUPS-1;g++)if(spur_primary(d,g)) {
        int offset=abs((int)d->spur[g].bin-origin);
        if(abs(offset-((offset+spacing/2)/spacing)*spacing)<=3)members++;
    }
    if(members<3)return;
    /* Spacing labels observed teeth only. Never mask an inferred harmonic
       without its own occurrence and level evidence. */
    for(int g=1;g<CATCH_SPUR_GROUPS-1;g++)if(d->spur[g].learned) {
        int offset=abs((int)d->spur[g].bin-origin);
        if(abs(offset-((offset+spacing/2)/spacing)*spacing)<=3)d->spur[g].comb=true;
    }
}
unsigned catch_detector_spurs(const catch_detector_t *d,catch_spur_info_t *out,unsigned capacity)
{
    unsigned count=0,used=0;
    for(int g=0;g<CATCH_SPUR_GROUPS;g++)if(spur_primary(d,g)) {
        count++;
        catch_spur_info_t s={bin_hz(d,d->spur[g].bin),d->spur[g].level,d->spur[g].comb};
        unsigned pos=0;while(pos<used && out[pos].dbfs>=s.dbfs)pos++;
        if(pos<capacity) {
            if(used<capacity)used++;
            for(unsigned j=used-1;j>pos;j--)out[j]=out[j-1];
            out[pos]=s;
        }
    }
    return count;
}
static void learn_spurs(catch_detector_t *d,const float *db,float noise,uint64_t now,uint64_t dt)
{
    /* Count occurrences / looks, not edges or microseconds credited to a
       detection. Fade in wall time so full-rate and sparse looks use the same
       duty threshold. Overlapping neighbourhoods pool a wandering tooth;
       the mask follows only its observed extent, plus three skirt bins. */
    float step=1,decay=fmaxf(0,1-(float)dt/10000000);
    for(int g=1;g<CATCH_SPUR_GROUPS-1;g++) {
        catch_spur_t *s=&d->spur[g];int lo=g*4-6,hi=g*4+9,peak=-1;
        if(lo<CATCH_DC_BINS)lo=CATCH_DC_BINS;
        if(hi>=CATCH_BINS-CATCH_DC_BINS)hi=CATCH_BINS-CATCH_DC_BINS-1;
        for(int j=lo;j<=hi;j++)if(abs(j-CATCH_BINS/2)>CATCH_DC_BINS &&
            isfinite(db[j]) && db[j]>noise+12 && db[j]>=db[j-1] && db[j]>=db[j+1] &&
            (peak<0 || db[j]>db[peak]))peak=j;
        s->hits*=decay;s->looks=s->looks*decay+step;
        /* Count the tooth's local maximum, not its Hann skirts in adjacent
           groups. The mask itself covers those skirts. */
        bool on=peak>=0 && (!s->learned || db[peak]<s->level+10);
        if(on) {
            if(!s->last_us) {
                s->level=db[peak];s->first_us=now;
                s->low_bin=s->high_bin=(uint16_t)peak;
            }
            else s->level+=0.05f*(db[peak]-s->level);
            if(peak<s->low_bin)s->low_bin=(uint16_t)peak;
            if(peak>s->high_bin)s->high_bin=(uint16_t)peak;
            s->bin=(uint16_t)peak;s->hits+=step;
            if(!s->on) {
                uint64_t period=now-s->edge_us;
                if(s->edge_us && period>=10000 && period<500000 &&
                   now-s->last_us>=5000 && now-s->last_us<100000 &&
                   s->last_us-s->edge_us<60000) {
                    if(!s->period_us || llabs((long long)period-s->period_us)<(long long)s->period_us/3) {
                        if(s->repeats<UINT16_MAX)s->repeats++;
                        s->period_us=(uint32_t)period;
                    } else {s->repeats=0;s->period_us=(uint32_t)period;}
                } else if(period>=500000)s->repeats=0;
                s->edge_us=now;
            }
            s->last_us=now;
            if(now-s->first_us>=500000 &&
               (s->repeats>=5 || s->hits>s->looks*0.35f))s->learned=true;
        }
        s->on=on;
        if(s->last_us && now-s->last_us>3000000)memset(s,0,sizeof(*s));
    }
    /* Try centre harmonics and spacing between adjacent distinct teeth.
       The latter also learns a comb whose origin is offset from DC. */
    for(int g=1;g<CATCH_SPUR_GROUPS-1;g++)d->spur[g].comb=false;
    for(int g=1;g<CATCH_SPUR_GROUPS-1;g++)if(spur_primary(d,g)) {
        spur_family(d,CATCH_BINS/2,abs((int)d->spur[g].bin-CATCH_BINS/2));
        for(int j=g+1;j<CATCH_SPUR_GROUPS-1;j++)if(spur_primary(d,j)) {
            spur_family(d,d->spur[g].bin,abs((int)d->spur[j].bin-d->spur[g].bin));
            break;
        }
    }
    memset(d->coarse_spur,0,sizeof(d->coarse_spur));
    for(int g=0;g<CATCH_SPUR_GROUPS;g++)if(d->spur[g].learned) {
        float limit=powf(10,(d->spur[g].level+10)/10)*(16384.0f*65536.0f)/(float)((CATCH_BINS/CATCH_COARSE)*(CATCH_BINS/CATCH_COARSE));
        for(int j=0;j<CATCH_COARSE;j++)if(j*(CATCH_BINS/CATCH_COARSE)>=(int)d->spur[g].low_bin-24 &&
            j*(CATCH_BINS/CATCH_COARSE)<=(int)d->spur[g].high_bin+24 &&
            limit>d->coarse_spur[j])d->coarse_spur[j]=limit;
    }
}
void catch_detector_spectrum(catch_detector_t *d, const float *db, uint64_t now,
                             catch_event_fn cb, void *ctx)
{
    if(!d->frames) d->first_us=now;
    uint64_t dt=d->frames && now>=d->previous_us?now-d->previous_us:0;
    d->previous_us=now;
    bool warm=now-d->first_us<CATCH_WARM_US;
    float alpha=fminf((float)dt/3000000.0f,0.1f);
    int raised=0;
    for(int i=CATCH_DC_BINS;i<CATCH_BINS-CATCH_DC_BINS;i++)
        if(isfinite(db[i]) && db[i]>d->floor[i]+6) raised++;
    /* A broadband gain change is not a transmitter. Relearn after the step. */
    bool gain_step=!warm && !d->clip_active && raised>CATCH_BINS/3;
    if(gain_step) {
        memset(d->track,0,sizeof(d->track));
        d->first_us=now;warm=true;
    }
    int count=0;
    for(int i=CATCH_DC_BINS;i<CATCH_BINS-CATCH_DC_BINS;i++)if(abs(i-CATCH_BINS/2)>CATCH_DC_BINS && isfinite(db[i]))d->re[count++]=db[i];
    /* Exponential FFT noise has median power 0.693 times its mean. */
    float slice_noise=count?median(d->re,count)+1.6f:-90;
    learn_spurs(d,db,slice_noise,now,dt);
    count=0;
    for(int i=0;i<CATCH_BINS;i++) {
        if(!isfinite(db[i])) continue;
        if(!d->frames || gain_step) d->floor[i]=db[i];
        else if(warm) d->floor[i]+=0.1f*(db[i]-d->floor[i]);
        else if(db[i]<d->floor[i]+6) d->floor[i]+=alpha*(db[i]-d->floor[i]);
        if(d->floor[i]<slice_noise)d->floor[i]=slice_noise;
        if(i>=CATCH_DC_BINS && i<CATCH_BINS-CATCH_DC_BINS && abs(i-CATCH_BINS/2)>CATCH_DC_BINS) {d->re[count++]=d->floor[i];}
    }
    d->reported_floor=count?median(d->re,count):-90;
    /* A temporal log average can be biased down by deep random FFT nulls.
       Bound each bin by the slice median so sparse tracking cannot promote
       ordinary noise into a sustained burst. Retain higher local spur floors. */
    for(int i=0;i<CATCH_BINS;i++)if(d->floor[i]<d->reported_floor-1)
        d->floor[i]=d->reported_floor-1;
    d->frames++;
    if(warm) return;
    float strongest=-120;
    for(int i=CATCH_DC_BINS;i<CATCH_BINS-CATCH_DC_BINS;i++)if(abs(i-CATCH_BINS/2)>CATCH_DC_BINS && db[i]>strongest)strongest=db[i];
    /* A saturated ADC creates many harmonics and aliases. Characterise the
       dominant signal; expose weaker distortion peaks as image rejections. */
    /* Retain local maxima above +6 dB, including deliberately masked bins. */
    for(int i=0;i<CATCH_BINS;i++) {
        float snr=db[i]-d->floor[i];
        if(!isfinite(snr) || snr<6 || (i && db[i]<db[i-1]) ||
           (i+1<CATCH_BINS && db[i]<db[i+1]))continue;
        catch_reason_t reason=(i<CATCH_DC_BINS || i>=CATCH_BINS-CATCH_DC_BINS || abs(i-CATCH_BINS/2)<=CATCH_DC_BINS || (d->clip_active && db[i]<strongest-12))?
            CATCH_MASKED:spur_bin(d,i,db[i])?CATCH_SPUR:
            snr<CATCH_MARGIN_DB?CATCH_UNDER_MARGIN:CATCH_ACCEPTED;
        if(reason!=CATCH_ACCEPTED && (!d->candidate_us[i] || now-d->candidate_us[i]>=CATCH_MERGE_US))
            d->reasons[reason]++;
        if(reason!=CATCH_ACCEPTED)d->candidate_us[i]=now;
        peak_record(d,bin_hz(d,(float)i),db[i],snr,now,
                    reason==CATCH_ACCEPTED?CATCH_CANDIDATE:reason);
    }
    for(int i=0;i<CATCH_BINS;i++)d->re[i]=db[i];
    unsigned limit=d->candidate_limit;
    if(d->clip_active && limit>1)limit=1;
    for(unsigned candidate=0;candidate<limit;candidate++) {
        int i=-1;
        for(int j=CATCH_DC_BINS;j<CATCH_BINS-CATCH_DC_BINS;j++)if(abs(j-CATCH_BINS/2)>CATCH_DC_BINS &&
            isfinite(d->re[j]) && d->re[j]>=d->floor[j]+CATCH_MARGIN_DB &&
            !(d->clip_active && db[j]<strongest-12) &&
            !spur_bin(d,j,db[j]) &&
            (i<0 || db[j]>db[i]))i=j;
        if(i<0)break;
        int dominant=i;
        while(i>CATCH_DC_BINS && abs(i-1-CATCH_BINS/2)>CATCH_DC_BINS && isfinite(d->re[i-1]) &&
              (!d->clip_active || db[i-1]>=strongest-12) &&
              db[i-1]>=d->floor[i-1]+CATCH_MARGIN_DB && !spur_bin(d,i-1,db[i-1]))i--;
        /* Strength ordering may select the upper FSK lobe first. Start at
           its qualifying lower companion so the existing valley test sees
           both lobes and preserves a stable centre across symbols. */
        int companion=-1;
        for(int j=i-2;j>=CATCH_DC_BINS && i-j<=24;j--)if(abs(j-CATCH_BINS/2)>CATCH_DC_BINS &&
            isfinite(d->re[j]) && db[j]>=d->floor[j]+CATCH_MARGIN_DB &&
            fabsf(db[j]-db[dominant])<6 && db[(j+i)/2]<fminf(db[j],db[dominant])-6 &&
            !spur_bin(d,j,db[j]) &&
            (companion<0 || db[j]>db[companion]))companion=j;
        if(companion>=0) {
            i=companion;
            while(i>CATCH_DC_BINS && db[i-1]>=d->floor[i-1]+CATCH_MARGIN_DB && isfinite(d->re[i-1]) &&
                  (!d->clip_active || db[i-1]>=strongest-12))i--;
        }
        /* Mask LO/DC leakage and the wrapped Nyquist image guard bins. */
        int lo=i, hi=i, peak=i, second=-1;
        while(hi+1<CATCH_BINS-CATCH_DC_BINS &&
              abs(hi+1-CATCH_BINS/2)>CATCH_DC_BINS && db[hi+1]>d->floor[hi+1]+6 &&
              (!d->clip_active || db[hi+1]>=strongest-12)) hi++;
        for(int j=lo;j<=hi;j++) if(db[j]>db[peak]) peak=j;
        /* A second nearby lobe separated by a valley suggests 2-FSK.
           Two unrelated carriers can also look like this: it is a guess. */
        int tail=hi+1;
        while(tail<CATCH_BINS-CATCH_DC_BINS && tail-hi<=24 && db[tail]<d->floor[tail]+12) tail++;
        if(tail<CATCH_BINS-CATCH_DC_BINS && tail-hi<=24 &&
           abs(tail-CATCH_BINS/2)>CATCH_DC_BINS && !spur_bin(d,tail,db[tail]) &&
           !((hi<CATCH_BINS/2) && (tail>CATCH_BINS/2))) {
            int end=tail;
            while(end+1<CATCH_BINS-CATCH_DC_BINS && abs(end+1-CATCH_BINS/2)>CATCH_DC_BINS && db[end+1]>d->floor[end+1]+6 &&
                  (!d->clip_active || db[end+1]>=strongest-12)) end++;
            second=tail;
            for(int j=tail;j<=end;j++) if(db[j]>db[second]) second=j;
            if(fabsf(db[second]-db[peak])<6 && db[(hi+tail)/2]<fminf(db[second],db[peak])-6) hi=end;
            else second=-1;
        }
        float p=(float)peak;
        if(peak>0 && peak<CATCH_BINS-1) {
            float denom=db[peak-1]-2*db[peak]+db[peak+1];
            if(fabsf(denom)>0.01f) p+=fmaxf(-0.5f,fminf(0.5f,0.5f*(db[peak-1]-db[peak+1])/denom));
        }
        if(second>=0) p=(p+second)*0.5f;
        uint32_t hz=bin_hz(d,p), bw=(hi-lo+1)*CATCH_RATE/CATCH_BINS;
        for(int j=lo;j<=hi;j++)d->re[j]=-INFINITY;
        if(bw>250000) {
            peak_record(d,hz,db[peak],db[peak]-d->floor[peak],now,CATCH_TOO_WIDE);
            if(!d->candidate_us[lo] || now-d->candidate_us[lo]>=CATCH_MERGE_US)d->reasons[CATCH_TOO_WIDE]++;
            d->candidate_us[lo]=now;continue;
        }
        int slot=-1;
        for(int j=0;j<CATCH_TRACKS;j++) if(d->track[j].used &&
            llabs((long long)d->track[j].burst.frequency-hz)<(long long)(bw/2+15000)) {slot=j;break;}
        /* MERGE measures observed holes, not time spent without an FFT.
           At a 13 ms cadence the old test discarded every 47 ms packet as
           single-frame fragments. Never bridge a transport/long blind gap. */
        if(!d->iq_mode && slot>=0 && (now-d->track[slot].last_us>CATCH_OBSERVATION_US ||
           (d->track[slot].frame+1<d->frames && now-d->track[slot].last_us>=CATCH_MERGE_US))) {
            finish_track(d,slot,now,cb,ctx);
        }
        if(slot<0) for(int j=0;j<CATCH_TRACKS;j++) if(!d->track[j].used) {slot=j;break;}
        if(slot<0) d->dropped++;
        else {
            if(d->track[slot].used && d->track[slot].frame+1<d->frames)
                d->reasons[CATCH_MERGED]++;
            catch_burst_t *b=&d->track[slot].burst;
            if(!d->track[slot].used) {
                memset(b,0,sizeof(*b)); b->id=++d->next_id; b->start_us=now;
                b->frequency=hz; b->rtl_dbfs=db[peak]; b->bandwidth=bw;
                d->track[slot].low_hz=d->track[slot].high_hz=hz;
                b->modulation=second>=0?CATCH_FSK:CATCH_OOK;
                b->deviation=second>=0?(uint32_t)abs(second-peak)*CATCH_RATE/CATCH_BINS/2:0;
                d->track[slot].used=true; d->track[slot].sent=false;d->track[slot].rejected=false;
                d->track[slot].on_us=0;d->track[slot].frame=0;
                d->track[slot].quiet=0;
                if(d->iq_mode) {
                    unsigned band=coarse_bin(d,hz);
                    d->track[slot].last_us=now;
                    if(d->coarse_first[band] && now>=d->coarse_last[band] &&
                       now-d->coarse_last[band]<CATCH_MERGE_US) {
                        b->start_us=d->coarse_first[band];
                        d->track[slot].last_us=d->coarse_last[band];
                        d->track[slot].on_us=d->coarse_last[band]-b->start_us;
                    }
                    trace_track(d,slot,"refined","coarse candidate");
                }
            }
            if(!d->iq_mode && d->track[slot].frame+1==d->frames)
                d->track[slot].on_us+=now-d->track[slot].last_us;
            d->track[slot].frame=d->frames;
            if(!d->iq_mode)d->track[slot].last_us=now;
            /* Sparse 4800-baud windows can contain only mark or space.
               Centre the observed peak excursion, not the first symbol or
               the much wider clipping skirts. */
            if(hz<d->track[slot].low_hz)d->track[slot].low_hz=hz;
            if(hz>d->track[slot].high_hz)d->track[slot].high_hz=hz;
            uint32_t span=d->track[slot].high_hz-d->track[slot].low_hz;
            b->frequency=d->track[slot].low_hz+span/2;
            if(span>CATCH_RATE/CATCH_BINS) {
                b->modulation=CATCH_FSK;
                if(span/2>b->deviation)b->deviation=span/2;
            }
            if(db[peak]>b->rtl_dbfs) b->rtl_dbfs=db[peak];
            if(now-b->start_us>CATCH_MAX_US) d->track[slot].rejected=true;
        }
    }
    for(int j=0;j<CATCH_TRACKS;j++) if(!d->iq_mode && d->track[j].used && now-d->track[j].last_us>=CATCH_MERGE_US) {
        finish_track(d,j,now,cb,ctx);
    }
}
static void fft(catch_detector_t *d)
{
    for(unsigned i=1,j=0;i<CATCH_BINS;i++) {
        unsigned bit=CATCH_BINS/2;
        for(;j&bit;bit>>=1) j^=bit;
        j^=bit;
        if(i<j) { float t=d->re[i];d->re[i]=d->re[j];d->re[j]=t;
                  t=d->im[i];d->im[i]=d->im[j];d->im[j]=t; }
    }
    for(unsigned n=2;n<=CATCH_BINS;n*=2) for(unsigned i=0;i<CATCH_BINS;i+=n)
        for(unsigned j=0;j<n/2;j++) {
            unsigned k=j*CATCH_BINS/n,a=i+j,b=a+n/2;
            float r=d->re[b]*d->tw_re[k]-d->im[b]*d->tw_im[k];
            float q=d->re[b]*d->tw_im[k]+d->im[b]*d->tw_re[k];
            d->re[b]=d->re[a]-r;d->im[b]=d->im[a]-q;d->re[a]+=r;d->im[a]+=q;
        }
    for(int i=0;i<CATCH_BINS;i++) {
        int j=(i+CATCH_BINS/2)%CATCH_BINS;
        float power=(d->re[j]*d->re[j]+d->im[j]*d->im[j])/(16384.0f*65536.0f);
        d->db[i]=10*log10f(fmaxf(power,1e-12f));
    }
}
static bool coarse_check(catch_detector_t *d,uint64_t now,catch_event_fn cb,void *ctx)
{
    for(unsigned i=1,j=0;i<CATCH_COARSE;i++) {
        unsigned bit=CATCH_COARSE/2;
        for(;j&bit;bit>>=1)j^=bit;
        j^=bit;
        if(i<j) {
            int32_t t=d->coarse_re[i];d->coarse_re[i]=d->coarse_re[j];d->coarse_re[j]=t;
            t=d->coarse_im[i];d->coarse_im[i]=d->coarse_im[j];d->coarse_im[j]=t;
        }
    }
    for(unsigned n=2;n<=CATCH_COARSE;n*=2)for(unsigned i=0;i<CATCH_COARSE;i+=n)
        for(unsigned j=0;j<n/2;j++) {
            unsigned k=j*CATCH_COARSE/n,a=i+j,b=a+n/2;
            int32_t r=(d->coarse_re[b]*d->coarse_tw_re[k]-d->coarse_im[b]*d->coarse_tw_im[k])/32768;
            int32_t q=(d->coarse_re[b]*d->coarse_tw_im[k]+d->coarse_im[b]*d->coarse_tw_re[k])/32768;
            d->coarse_re[b]=d->coarse_re[a]-r;d->coarse_im[b]=d->coarse_im[a]-q;
            d->coarse_re[a]+=r;d->coarse_im[a]+=q;
        }
    bool flagged=false;
    /* Three-bin energy is less sensitive to symbol changes and random FFT
       nulls than one 32-point bin. These checks run even with zero allowance. */
    for(unsigned i=2;i<CATCH_COARSE-1;i++) {
        if(i==CATCH_COARSE/2)continue;
        float power=0,floor=0;
        for(unsigned j=i-1;j<=i+1;j++) {
            power+=(float)d->coarse_re[j]*d->coarse_re[j]+(float)d->coarse_im[j]*d->coarse_im[j];
            floor+=d->coarse_floor[j];
        }
        unsigned band=(i+CATCH_COARSE/2)%CATCH_COARSE;
        float ratio=power/fmaxf(floor,192);
        d->coarse_level[band]=ratio;
        if(d->coarse_first[band] && now-d->coarse_last[band]>=CATCH_MERGE_US) {
            if(d->trace) {
                catch_trace_t t={d->block_us,0,bin_hz(d,(float)band*CATCH_BINS/CATCH_COARSE),
                    10*log10f(fmaxf(ratio,1e-6f)),band,d->fft_state,"coarse end","near floor"};
                d->trace(&t,d->trace_context);
            }
            d->coarse_first[band]=0;
        }
        if(d->coarse_frames>=150 && ratio>4.0f) {
            if(!d->coarse_first[band] && ratio>8) {
                d->coarse_first[band]=now;
                if(d->trace) {
                    catch_trace_t t={d->block_us,0,bin_hz(d,(float)band*CATCH_BINS/CATCH_COARSE),
                        10*log10f(ratio),band,d->fft_state,"coarse start","above floor"};
                    d->trace(&t,d->trace_context);
                }
            }
            d->coarse_last[band]=now;
        }
    }
    for(int j=0;j<CATCH_TRACKS;j++)if(d->track[j].used) {
        if(catch_detector_is_spur(d,d->track[j].burst.frequency,d->track[j].burst.rtl_dbfs)) {
            finish_track(d,j,now,cb,ctx);continue;
        }
        /* Check timeout BEFORE accepting a new positive after a blind gap. */
        if(now>=d->track[j].last_us && now-d->track[j].last_us>=CATCH_OBSERVATION_US) {
            trace_track(d,j,"end","positive timeout");finish_track(d,j,now,cb,ctx);continue;
        }
        unsigned band=coarse_bin(d,d->track[j].burst.frequency);
        if(d->coarse_last[band]==now) {
            d->track[j].last_us=now;
            d->track[j].on_us=now-d->track[j].burst.start_us;
            if(d->track[j].on_us>CATCH_MAX_US)d->track[j].rejected=true;
        }
    }
    for(unsigned i=2;i<CATCH_COARSE-1;i++) {
        if(i==CATCH_COARSE/2)continue; /* Nyquist image. DC is bins 0,1,31. */
        float power=(float)d->coarse_re[i]*d->coarse_re[i]+(float)d->coarse_im[i]*d->coarse_im[i];
        float *floor=&d->coarse_floor[i];
        if(!d->coarse_frames)*floor=fmaxf(power,64.0f);
        bool warm=d->coarse_frames<150;
        int shifted=(i+CATCH_COARSE/2)%CATCH_COARSE;
        /* The coarse window has 1/8 the voltage of the full window. Compare
           in the same dBFS domain and cover its wider main lobe. */
        bool spur=power<d->coarse_spur[shifted];
        if(!warm && power>*floor*8.0f && !spur)flagged=true;
        if(warm || power<*floor*4) *floor+=0.02f*(power-*floor);
    }
    d->coarse_frames++;return flagged;
}
void catch_detector_allow(catch_detector_t *d,unsigned frames)
{d->allowance=frames;d->candidate_limit=frames?CATCH_TRACKS:4;}
void catch_detector_iq(catch_detector_t *d,const uint8_t *iq,size_t n,uint64_t end,
                       catch_event_fn cb,void *ctx)
{
    d->iq_mode=true;d->block_us=end;d->fft_state="skipped cadence";
    uint32_t frames=d->frames;
    /* Walk complex samples once rather than branching/writing PSRAM state
       for both bytes. Preserve a trailing I byte across USB fragments. */
    for(size_t i=0;i<n;i++) {
        uint8_t first;
        if(d->half) {first=d->first;d->half=false;}
        else {
            first=iq[i++];
            if(i==n) {d->first=first;d->half=true;break;}
        }
        int re=(int)first-128, im=(int)iq[i]-128;
        if(first<=1 || first>=254 || iq[i]<=1 || iq[i]>=254) {d->clips++;d->chunk_clips++;}
        d->power_sum+=(uint32_t)(re*re+im*im);d->power_re+=re;d->power_im+=im;
        if(++d->power_samples==CATCH_BINS) {
            int mr=d->power_re/CATCH_BINS, mi=d->power_im/CATCH_BINS;
            int variance=(int)(d->power_sum/CATCH_BINS)-mr*mr-mi*mi;
            uint32_t power=(uint32_t)(variance>0?variance:1)*256;
            d->iq_rms=sqrtf((float)d->power_sum/(2*CATCH_BINS))/128;
            if(!d->power_floor)d->power_floor=power;
            d->clip_active=d->chunk_clips>8;
            bool jump=power>d->power_floor*4 || d->chunk_clips>d->clip_floor/256+8;
            if(jump)d->probe=true;
            else {
                d->power_floor=(uint32_t)((int64_t)d->power_floor+((int64_t)power-d->power_floor)/256);
                d->clip_floor=(uint32_t)((int64_t)d->clip_floor+((int64_t)d->chunk_clips*256-d->clip_floor)/256);
            }
            d->chunk_clips=0;d->power_samples=d->power_sum=0;d->power_re=d->power_im=0;
        }
        unsigned phase=d->phase;
        d->phase=(phase+1)%4096;
        uint64_t now=0;
        if(phase==CATCH_COARSE-1 || (!d->skip && d->fill+1==CATCH_BINS)) {
            uint64_t back=(uint64_t)(n-i-1)*500000/CATCH_RATE;
            now=end>back?end-back:0;
        }
        if(phase<CATCH_COARSE) {
            d->coarse_re[phase]=re*d->coarse_window[phase]/32768;
            d->coarse_im[phase]=im*d->coarse_window[phase]/32768;
            if(phase==CATCH_COARSE-1) {
                uint64_t check_begin=d->clock_us?d->clock_us(d->clock_context):0;
                d->probe=coarse_check(d,now,cb,ctx) || d->probe;
                if(d->clock_us)d->checks_us+=d->clock_us(d->clock_context)-check_begin;
                bool tracking=false;
                for(int j=0;j<CATCH_TRACKS;j++)if(d->track[j].used && !d->track[j].rejected)tracking=true;
                /* Real execution time caps backlog replay too. Host replay falls
                   back to sample time when no clock is installed. */
                uint64_t work_now=d->clock_us?d->clock_us(d->clock_context):now;
                uint64_t second=work_now/1000000;
                if(second!=d->budget_second) {d->budget_second=second;d->second_frames=0;}
                if(d->allowance && (!d->clock_us || work_now>=d->expensive_ready_us) &&
                   d->second_frames<CATCH_FULL_PER_SECOND &&
                   (!d->frames || now-d->last_background_us>=8000) &&
                   (d->probe || tracking || !d->frames || now-d->last_background_us>=20000)) {
                    d->fill=0;d->fft_clips=0;d->skip=0;d->last_background_us=now;d->second_frames++;
                    if(d->allowance && d->allowance!=UINT32_MAX)d->allowance--;
                } else {
                    d->skip=4096;
                    if(d->clock_us && work_now<d->expensive_ready_us)d->fft_state="skipped duty";
                    else if(!d->allowance)d->fft_state="skipped allowance";
                    else if(d->second_frames>=CATCH_FULL_PER_SECOND)d->fft_state="skipped cap";
                }
                d->probe=false;
            }
        }
        if(d->skip) {
            /* Outside acquisition windows, sample energy/clips at 1/16 rate.
               Advance only whole complex samples, preserving odd fragments. */
            unsigned extra=phase>=CATCH_COARSE && phase<4080?15:0;
            if(extra>(n-i-1)/2)extra=(unsigned)((n-i-1)/2);
            if(extra>=d->skip)extra=d->skip-1;
            d->phase=(d->phase+extra)%4096;i+=2*extra;
            d->skip-=extra+1;continue;
        }
        if(first<=1 || first>=254 || iq[i]<=1 || iq[i]>=254)d->fft_clips++;
        unsigned k=d->fill++;
        d->re[k]=re*d->window[k];d->im[k]=im*d->window[k];
        if(d->fill==CATCH_BINS) {
            uint64_t stage=d->clock_us?d->clock_us(d->clock_context):0;
            fft(d);
            uint64_t after=d->clock_us?d->clock_us(d->clock_context):0;
            d->fft_us+=after-stage;
            d->clip_active=d->fft_clips>8;
            d->fft_state="used";
            catch_detector_spectrum(d,d->db,now,cb,ctx);
            if(d->clock_us) {
                uint64_t done=d->clock_us(d->clock_context);
                d->spectrum_us+=done-after;
                /* Pay expensive-stage debt by skipping refinements, never IQ. */
                d->expensive_ready_us=done+2*(done-stage);
            }
            d->fill=0;d->skip=4096;
        }
    }
    if(d->frames!=frames)d->fft_state="used";
    for(int j=0;j<CATCH_TRACKS;j++)if(d->track[j].used) {
        uint64_t last=d->track[j].last_us;
        bool quiet=end>=last && end-last>=CATCH_MERGE_US;
        if(quiet)d->track[j].quiet++;else d->track[j].quiet=0;
        if((end>=last && end-last>=CATCH_OBSERVATION_US) || d->track[j].quiet>=CATCH_QUIET_BLOCKS) {
            trace_track(d,j,"end",d->track[j].quiet>=CATCH_QUIET_BLOCKS?"quiet blocks":"positive timeout");
            finish_track(d,j,end,cb,ctx);
        } else trace_track(d,j,quiet?"quiet":"active",quiet?"near floor":"coarse positive");
    }
}
void catch_detector_gap(catch_detector_t *d)
{
    d->fill=d->skip=d->power_samples=d->power_sum=0;d->half=false;
    d->power_re=d->power_im=0;d->phase=d->chunk_clips=0;d->skip=4096;
    memset(d->track,0,sizeof(d->track));
    memset(d->coarse_first,0,sizeof(d->coarse_first));
    memset(d->coarse_last,0,sizeof(d->coarse_last));
}
void catch_handoff_stop(catch_hold_t *h,const catch_handoff_ops_t *ops)
{ if(h->listening) ops->end(ops->context); memset(h,0,sizeof(*h)); }
bool catch_handoff(catch_hold_t *h,const catch_burst_t *b,const catch_handoff_ops_t *ops,uint32_t *latency)
{
    if(b->spur)return false;
    uint64_t now=ops->now(ops->context);
    if(h->listening && !catch_handoff_expired(h,now)) {
        if(llabs((long long)h->frequency-b->frequency)>15000) {
            if(!(b->repeats>=2 && !h->repeated) &&
               (h->repeated || b->rtl_dbfs<h->dbfs+10))return false;
        } else {
            h->repeated=h->repeated || b->repeats>=2;h->until_us=now+CATCH_HOLD_US;h->activity_us=now;h->dbfs=b->rtl_dbfs; *latency=0; return true;
        }
    }
    catch_handoff_stop(h,ops);
    if(!ops->begin(ops->context,b)) return false;
    now=ops->now(ops->context);
    uint64_t elapsed=now>=b->detected_us?now-b->detected_us:0;
    *latency=elapsed>UINT32_MAX?UINT32_MAX:(uint32_t)elapsed;
    h->frequency=b->frequency;h->until_us=now+CATCH_HOLD_US;h->activity_us=now;
    h->dbfs=b->rtl_dbfs;h->listening=true;h->repeated=b->repeats>=2;
    return true;
}
bool catch_handoff_expired(const catch_hold_t *h,uint64_t now)
{
    /* Release unproven holds after five seconds. RTL repeats and socket
       captures renew the window; learned spurs release independently. */
    return h->listening && (now>=h->until_us || now-h->activity_us>=CATCH_SILENT_US);
}
bool catch_handoff_prefer(const catch_burst_t *candidate,const catch_burst_t *pending)
{
    if(candidate->spur)return false;
    if(pending->spur)return true;
    if(candidate->repeats!=pending->repeats)return candidate->repeats>pending->repeats;
    return candidate->detected_us>pending->detected_us+300000 ||
        candidate->rtl_dbfs>pending->rtl_dbfs ||
        (candidate->rtl_dbfs==pending->rtl_dbfs && candidate->detected_us>=pending->detected_us);
}

bool catch_source_observe(catch_sources_t *s,catch_burst_t *b,uint32_t center,uint64_t elapsed)
{
    if(b->spur || llabs((long long)b->frequency-center)<=CATCH_DC_BINS*CATCH_RATE/CATCH_BINS ||
       llabs(llabs((long long)b->frequency-center)-CATCH_RATE/2)<=CATCH_DC_BINS*CATCH_RATE/CATCH_BINS)return false;
    uint32_t duration=b->stop_us>b->start_us?(uint32_t)(b->stop_us-b->start_us):0;
    int slot=-1,oldest=0;
    for(int i=0;i<32;i++) {
        if(s->source[i].last_us<s->source[oldest].last_us)oldest=i;
        if(s->source[i].frequency && llabs((long long)s->source[i].frequency-b->frequency)<=15000) {slot=i;break;}
    }
    if(slot<0)slot=oldest;
    /* Budget gaps within one maximum-length burst are not a new sighting. */
    bool repeat=s->source[slot].frequency && b->start_us>s->source[slot].last_us+CATCH_MAX_US &&
        b->start_us-s->source[slot].last_us<30000000 &&
        llabs((long long)duration-s->source[slot].duration)<=s->source[slot].duration/3+3000;
    b->repeats=repeat?s->source[slot].repeats+1:1;
    if(b->repeats>255)b->repeats=255;
    s->source[slot].frequency=b->frequency;s->source[slot].duration=duration;
    s->source[slot].last_us=b->stop_us;s->source[slot].repeats=b->repeats;
    return elapsed>=CATCH_HANDOFF_WARM_US && (b->repeats>=2 || b->rtl_dbfs>=CATCH_STRONG_DBFS);
}
