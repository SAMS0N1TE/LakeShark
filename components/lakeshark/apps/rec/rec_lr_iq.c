#include "rec_lr_iq.h"
#include "rec_file_open.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <math.h>
#include "rec_lr_ook.h"

bool rec_lr_iq_rate_supported(uint32_t rate)
{ return rate==4800 || rate==9600 || rate==19200 || rate==38400 || rate==76800 || rate==125000; }

size_t rec_lr_iq_test_fsk(uint8_t *out,size_t cap,uint32_t rate)
{
    if(!out || !rec_lr_iq_rate_supported(rate))return 0;
    /* 60 bits = 12.5 ms, four intervening 10 ms gaps. At low requested
       diagnostic rates the +/-20 kHz tones necessarily alias. */
    size_t n=(102500ull*rate+500000)/1000000;
    if(cap<n*2)return 0;
    memset(out,0,n*2);
    const double tau=6.283185307179586;
    for(unsigned r=0;r<5;r++) {
        double phase=0;
        size_t lo=((uint64_t)r*22500*rate+500000)/1000000;
        size_t hi=((uint64_t)(r*22500+12500)*rate+500000)/1000000;
        for(size_t j=lo;j<hi;j++) {
            /* Integrate across symbol boundaries, including fractional samples. */
            double t=(double)(j-lo)*4800/rate,remaining=4800.0/rate;
            out[2*j]=(uint8_t)(int8_t)lround(48*cos(phase));
            out[2*j+1]=(uint8_t)(int8_t)lround(48*sin(phase));
            while(remaining>1e-9) {
                unsigned b=(unsigned)t;
                unsigned bit=b<32?!(b%2):b<56?(0xA53C96u>>(55-b))&1:!(b%2);
                double step=fmin(remaining,b+1-t);
                phase+=tau*(bit?20000:-20000)*step/4800;
                remaining-=step;t=b+1;
            }
        }
    }
    return n*2;
}

size_t rec_lr_iq_test(uint8_t *out,size_t cap,uint32_t rate)
{
    if(!out || !rec_lr_iq_rate_supported(rate))return 0;
    /* Round absolute edge times, avoiding cumulative bit timing error. */
    size_t n=(232000ull*rate+500000)/1000000;
    if(cap<n*2)return 0;
    memset(out,0,n*2);
    for(unsigned r=0;r<5;r++)for(unsigned b=0;b<24;b++) {
        unsigned start=r*48400+b*1600;
        unsigned high=(0xA53C96u>>(23-b))&1?1200:400;
        size_t lo=((uint64_t)start*rate+500000)/1000000;
        size_t hi=((uint64_t)(start+high)*rate+500000)/1000000;
        for(size_t i=lo;i<hi;i++)out[2*i]=48;
    }
    return n*2;
}

static int mag_compare(const void *a,const void *b)
{ double x=*(const double *)a,y=*(const double *)b;return (x>y)-(x<y); }

static void iq_stats_emit(void *ctx,const int32_t *edge,int n,int peak)
{
    (void)peak;
    rec_lr_iq_stats_t *s=ctx;
    bool first=!s->bits[0];unsigned bits=0;
    for(int i=0;i<n;i++)if(edge[i]>0) {
        unsigned us=(unsigned)edge[i];s->pulses++;
        if(!s->short_us || us<s->short_us)s->short_us=us;
        if(us>s->long_us)s->long_us=us;
        if(first && bits<24)s->bits[bits++]=us>=300 && us<=500?'0':us>=1100 && us<=1300?'1':'?';
    }
}

static int sample(const uint8_t *p)
{ return (int16_t)((unsigned)p[0] | (unsigned)p[1]<<8); }

bool rec_lr_iq_stats(const uint8_t *iq,size_t bytes,rec_lr_iq_stats_t *out)
{
    if(!iq || !out || !bytes || bytes%4 || bytes>REC_LR_IQ_BYTES)return false;
    size_t n=bytes/4;
    double *mag=malloc(n*sizeof(*mag));
    rec_lr_ook_t *ook=malloc(sizeof(*ook));
    if(!mag || !ook){free(mag);free(ook);return false;}
    memset(out,0,sizeof(*out));out->samples=n;
    for(size_t i=0;i<n;i++) {
        double si=sample(iq+4*i),sq=sample(iq+4*i+2);
        mag[i]=sqrt(si*si+sq*sq);
    }
    qsort(mag,n,sizeof(*mag),mag_compare);
    out->peak=mag[n-1];out->median=(mag[(n-1)/2]+mag[n/2])/2;
    double threshold=(out->median+out->peak)/2;
    for(size_t i=0;i<n;i++)if(mag[i]>threshold)out->above++;
    free(mag);
    const rec_lr_ook_cfg_t cfg={60,4000,2};
    rec_lr_ook_init(ook,&cfg);
    rec_lr_ook_feed(ook,iq,bytes,iq_stats_emit,out);
    /* End the final partial frame; padding is not included in the stats. */
    const uint8_t zero[4]={0};
    for(unsigned i=0;i<=500;i++)rec_lr_ook_feed(ook,zero,sizeof(zero),iq_stats_emit,out);
    free(ook);return true;
}

static int float_compare(const void *a,const void *b)
{ float x=*(const float *)a,y=*(const float *)b;return (x>y)-(x<y); }

enum { IQ_FIR_HALF=24,IQ_FIR_TAPS=2*IQ_FIR_HALF+1,IQ_FIR_PHASES=64 };
/* A Hann-windowed sinc reaches its pass/stop edges about two bins from fc. */
static const float iq_fir_margin=2.0f*REC_LR_IQ_RATE/IQ_FIR_TAPS;

/* Windowed spectra after carrier removal: enclose 99% of energy and allow
   two FFT bins of guard plus the FIR transition before output Nyquist.
   Too little signal, allocation failure, or broad energy means no decimation. */
static uint32_t iq_bandwidth_rate(const float *iq,size_t n,double *bandwidth)
{
    enum { N=256 };
    float *work=calloc(4*N,sizeof(*work));
    if(!work)return REC_LR_IQ_RATE;
    float *re=work,*im=work+N,*spectrum=work+2*N,*window=work+3*N;
    unsigned windows=0;
    const float tau=6.283185307179586f;
    for(unsigned j=0;j<N;j++)window[j]=0.5f-0.5f*cosf(tau*j/(N-1));
    for(size_t at=0;at+N<=n;at+=N/2) {
        unsigned on=0;
        for(unsigned j=0;j<N;j++) {
            float w=window[j];
            re[j]=iq[2*(at+j)]*w;im[j]=iq[2*(at+j)+1]*w;
            if(iq[2*(at+j)] || iq[2*(at+j)+1])on++;
        }
        if(on<N/2)continue;
        windows++;
        for(unsigned j=1,k=0;j<N;j++) {
            unsigned bit=N/2;
            for(;k&bit;bit>>=1)k^=bit;
            k^=bit;
            if(j<k) {
                float tmp=re[j];re[j]=re[k];re[k]=tmp;
                tmp=im[j];im[j]=im[k];im[k]=tmp;
            }
        }
        for(unsigned len=2;len<=N;len*=2) {
            float dc=cosf(tau/len),ds=-sinf(tau/len);
            for(unsigned a=0;a<N;a+=len) {
                float c=1,s=0;
                for(unsigned j=0;j<len/2;j++) {
                    unsigned u=a+j,v=u+len/2;
                    float x=c*re[v]-s*im[v],y=s*re[v]+c*im[v];
                    re[v]=re[u]-x;im[v]=im[u]-y;re[u]+=x;im[u]+=y;
                    float next=c*dc-s*ds;s=s*dc+c*ds;c=next;
                }
            }
        }
        for(unsigned j=0;j<N;j++)spectrum[j]+=re[j]*re[j]+im[j]*im[j];
    }
    float total=0;for(unsigned j=0;j<N;j++)total+=spectrum[j];
    uint32_t rate=REC_LR_IQ_RATE;
    if(windows>=2 && total>0) {
        float inside=spectrum[0];unsigned bin=0;
        while(inside<total*0.99f && bin<N/2) {
            bin++;inside+=spectrum[bin];
            if(bin<N/2)inside+=spectrum[N-bin];
        }
        float edge=(bin+2.0f)*REC_LR_IQ_RATE/N;
        *bandwidth=2.0f*bin*REC_LR_IQ_RATE/N;
        (void)edge;
        /* Measured on the LR2021 with a HackRF: at 76800 the chip sends FSK at half the
           deviation, at 38400 with the frequency inverted. Only 125000 is faithful. */
    }
    free(work);return rate;
}

/* Offline OOK slicing or phase-preserving, rational FIR IQ conversion. */
size_t rec_lr_iq_convert_report(const uint8_t *iq,size_t bytes,uint8_t *out,size_t cap,uint32_t rate,rec_lr_iq_classifier_t *report)
{
    rec_lr_iq_classifier_t classification={0};
    if(!report)report=&classification;
    *report=classification;
    if(!iq || !out || !bytes || bytes%4 || bytes>REC_LR_IQ_BYTES ||
       !rec_lr_iq_rate_supported(rate))return 0;
    size_t n=bytes/4,m=(uint64_t)n*rate/REC_LR_IQ_RATE;
    if(!m || cap<m*2)return 0;
    float *power=malloc(n*sizeof(*power));
    float *mixed=malloc(n*2*sizeof(*mixed));
    if(!power || !mixed){free(power);free(mixed);return 0;}
    for(size_t i=0;i<n;i++) {
        float x=sample(iq+4*i),y=sample(iq+4*i+2);power[i]=x*x+y*y;
    }
    qsort(power,n,sizeof(*power),float_compare);
    float floor=fmaxf(1.0f,power[n/5]),level=power[4*n/5];
    if(level<10){free(power);free(mixed);return 0;}
    /* Require two well separated, populated envelope clusters. Broad AM and
       constant-envelope FSK retain phase and amplitude in the IQ path. */
    size_t off=0,on_count=0;
    for(size_t i=0;i<n;i++) {
        if(power[i]<level*0.25f)off++;
        if(power[i]>level*0.64f && power[i]<level*1.44f)on_count++;
    }
    report->bimodal=(float)(off+on_count)/n;
    bool ook=off>n/20 && on_count>n/20 && off+on_count>=n*98/100;
    float gate=floor*10;
    size_t first=0;while(first<n && power[first]<=gate)first++;
    if(first==n){gate=1;first=0;}
    float median=power[first+(n-first)/2];
    float gain=48.0f/sqrtf(fmaxf(1.0f,median));
    /* Slice into separate storage before writing (conversion is in-place safe).
       Hysteresis prevents chatter; reject short runs and weak pulse segments. */
    bool state=false;size_t start=0;
    float peak=0,hi=fmaxf(gate,level*0.25f),lo=hi*0.64f;
    for(size_t i=0;i<=n;i++) {
        float x=i<n?sample(iq+4*i):0,y=i<n?sample(iq+4*i+2):0;
        float p=x*x+y*y;
        bool next=i<n && p>(state?lo:hi);
        if(next && !state){start=i;peak=0;}
        if(next && p>peak)peak=p;
        if(state && !next && ((i-start)*8<100 || peak<median*0.25f))
            for(size_t k=start;k<i;k++)mixed[2*k]=0;
        if(i<n)mixed[2*i]=next?1:0;
        state=next;
    }
    /* Width evidence tolerates noisy amplitude/phase and a minority of junk.
       Sorted widths form groups within 25% (at least 24 us); take top three. */
    unsigned pulses=0;
    for(size_t i=0;i<n;) {
        if(!mixed[2*i]){i++;continue;}
        size_t begin=i;while(i<n && mixed[2*i])i++;
        power[pulses++]=(float)(i-begin);
    }
    qsort(power,pulses,sizeof(*power),float_compare);
    unsigned top[3]={0};
    for(unsigned i=0;i<pulses;) {
        unsigned begin=i;float limit=power[i]+fmaxf(3,power[i]*0.25f);
        while(i<pulses && power[i]<=limit)i++;
        unsigned count=i-begin;
        for(unsigned k=0;k<3;k++)if(count>top[k]) {
            unsigned tmp=top[k];top[k]=count;count=tmp;
        }
    }
    report->pulses=pulses;
    report->clustered=pulses?(float)(top[0]+top[1]+top[2])/pulses:0;
    free(power);
    /* The OOK slicer intentionally rejects weak pulses. IQ must keep a weak
       FSK tone: cap its noise gate 14 dB below the strong envelope cluster. */
    float iq_gate=fmaxf(1.0f,fminf(gate,level*0.04f));
    /* Use the IQ gate, not the OOK slicer: a weak FSK tone is still on.
       Spare odd slots hold interior markers, then frequency angles. Guard
       128 us at both edges, including a ~120 us CC1101 turn-on transient. */
    const size_t edge_guard=(128ull*REC_LR_IQ_RATE+999999)/1000000;
    size_t on_samples=0;
    for(size_t i=0;i<n;i++) {
        float x=sample(iq+4*i),y=sample(iq+4*i+2);
        mixed[2*i+1]=x*x+y*y>iq_gate?1:0;
    }
    for(size_t i=0;i<n;) {
        if(!mixed[2*i+1]){i++;continue;}
        size_t begin=i;while(i<n && mixed[2*i+1])i++;
        on_samples+=i-begin;
        for(size_t j=begin;j<i;j++)
            mixed[2*j+1]=j-begin>edge_guard && i-j>edge_guard?1:0;
    }
    enum { FREQ_BINS=512 };
    /* Keep the histogram off the REC worker's 6 KiB stack. */
    struct frequency_bin { unsigned count;float sum; };
    struct frequency_bin *freq=calloc(FREQ_BINS,sizeof(*freq));
    if(!freq){free(mixed);return 0;}
    unsigned pairs=0;
    /* Accumulate frequency evidence with coherence in the same input pass.
       Interior on-pairs get one vote, regardless of amplitude. NaN marks
       excluded pairs so switch counting cannot bridge an envelope edge. */
    {
        float re=0,im=0,total=0;
        for(size_t i=1;i<n;i++) {
            bool interior=mixed[2*i+1]>0;
            mixed[2*i+1]=NAN;
            float x=sample(iq+4*i),y=sample(iq+4*i+2);
            float px=sample(iq+4*i-4),py=sample(iq+4*i-2);
            float p=x*x+y*y,pp=px*px+py*py;
            if(p<=iq_gate || pp<=iq_gate)continue;
            float dot=px*x+py*y,cross=px*y-py*x;
            float norm=1.0f/sqrtf(p*pp);
            re+=dot*norm;im+=cross*norm;total+=1;
            if(!interior)continue;
            float angle=atan2f(cross,dot);
            mixed[2*i+1]=angle;
            unsigned bin=(unsigned)((angle+3.141592654f)*(FREQ_BINS/6.283185307f));
            if(bin>=FREQ_BINS)bin=FREQ_BINS-1;
            freq[bin].count++;freq[bin].sum+=angle;pairs++;
        }
        report->phase=total>0?hypotf(re,im)/total:0;
    }
    /* Two-means on a compact frequency histogram, seeded at the 10th/90th
       percentiles. Midpoint of tone centers is independent of tone power
       AND symbol population. A single cluster falls back to its mean. */
    float centers[2]={0},slope=0;unsigned cumulative=0;
    for(unsigned b=0;b<FREQ_BINS;b++) {
        unsigned before=cumulative;cumulative+=freq[b].count;
        if(before<=pairs/10 && cumulative>pairs/10)centers[0]=freq[b].sum/freq[b].count;
        if(before<=9*pairs/10 && cumulative>9*pairs/10)centers[1]=freq[b].sum/freq[b].count;
        slope+=freq[b].sum;
    }
    slope=pairs?slope/pairs:0;
    unsigned counts[2]={0};
    for(unsigned iteration=0;iteration<=8 && pairs;iteration++) {
        float sum[2]={0};counts[0]=counts[1]=0;
        for(unsigned b=0;b<FREQ_BINS;b++)if(freq[b].count) {
            float angle=freq[b].sum/freq[b].count;
            unsigned k=fabsf(angle-centers[1])<fabsf(angle-centers[0]);
            sum[k]+=freq[b].sum;counts[k]+=freq[b].count;
        }
        /* The final pass measures shares at the converged centers. */
        if(iteration==8)break;
        for(unsigned k=0;k<2;k++)if(counts[k])centers[k]=sum[k]/counts[k];
    }
    report->tone_separation_hz=fabsf(centers[1]-centers[0])*REC_LR_IQ_RATE/6.283185307f;
    for(unsigned k=0;k<2;k++)report->tone_share[k]=pairs?(double)counts[k]/pairs:0;
    bool two_tones=report->tone_separation_hz>=4000 &&
                   report->tone_share[0]>=0.15 && report->tone_share[1]>=0.15;
    int previous=-1;
    for(size_t i=1;two_tones && i<n;i++) {
        float angle=mixed[2*i+1];
        if(isnan(angle)){previous=-1;continue;}
        int tone=fabsf(angle-centers[1])<fabsf(angle-centers[0]);
        if(previous>=0 && tone!=previous)report->mid_burst_switches++;
        previous=tone;
    }
    /* No baud estimate is available; use the capture diagnostic's nominal
       4800 symbols/s for 2% of on-time in symbols, rounded upward. */
    unsigned minimum_switches=(on_samples*4800ull+50ull*REC_LR_IQ_RATE-1)/(50ull*REC_LR_IQ_RATE);
    if(minimum_switches<8)minimum_switches=8;
    bool fsk=two_tones && report->mid_burst_switches>=minimum_switches;
    if(two_tones)slope=(centers[0]+centers[1])*0.5f;
    free(freq);
    /* Only tone changes inside bursts veto envelope-based regeneration. */
    ook=(ook || (off>n/20 && pulses>=8 && report->clustered>=0.80f)) && !fsk;
    report->ook=ook;
    report->rate=rate;
    if(ook) {
        memset(out,0,m*2);
        /* Round absolute sliced edges, preserving frame gaps without FIR smear. */
        for(size_t i=0;i<n;) {
            if(!mixed[2*i]){i++;continue;}
            size_t begin=i;while(i<n && mixed[2*i])i++;
            size_t a=((uint64_t)begin*rate+REC_LR_IQ_RATE/2)/REC_LR_IQ_RATE;
            size_t b=((uint64_t)i*rate+REC_LR_IQ_RATE/2)/REC_LR_IQ_RATE;
            for(size_t j=a;j<b && j<m;j++)out[2*j]=48;
        }
        free(mixed);return m*2;
    }
    /* Use the strong envelope level for IQ gain, so an equally populated
       weak tone does not set gain high enough to clip its partner. */
    gain=48.0f/sqrtf(level);
    float c=1,s=0,dc=cosf(slope),ds=sinf(slope);
    for(size_t i=0;i<n;i++) {
        float x=sample(iq+4*i),y=sample(iq+4*i+2);
        bool on=x*x+y*y>iq_gate;
        mixed[2*i]=on?(x*c+y*s)*gain:0;
        mixed[2*i+1]=on?(y*c-x*s)*gain:0;
        float next=c*dc-s*ds;s=s*dc+c*ds;c=next;
        if(i%256==255){float norm=1/sqrtf(c*c+s*s);c*=norm;s*=norm;}
    }
    uint32_t measured=iq_bandwidth_rate(mixed,n,&report->bandwidth_hz);
    if(rate<measured)rate=measured;
    report->rate=rate;
    m=(uint64_t)n*rate/REC_LR_IQ_RATE;
    if(cap<m*2){free(mixed);return 0;}
    if(rate==REC_LR_IQ_RATE) {
        /* Native-rate IQ: no FIR, so retain amplitude and exact gated edges. */
        for(size_t j=0;j<n;j++) {
            float x=mixed[2*j],y=mixed[2*j+1],radius=hypotf(x,y);
            if(radius>55){x*=55/radius;y*=55/radius;}
            out[2*j]=(uint8_t)(int8_t)lroundf(x);out[2*j+1]=(uint8_t)(int8_t)lroundf(y);
        }
        free(mixed);return m*2;
    }
    /* Short fractional-delay polyphase FIR. Quantizing the fractional input
       position to 1/64 sample bounds timing error to 0.063 us. The passband
       follows occupied bandwidth; reserve the Hann transition before Nyquist.
       Native-rate fallback above avoids filtering bandwidth we cannot retain. */
    unsigned phases=rate/200;
    enum { HALF=IQ_FIR_HALF,TAPS=IQ_FIR_TAPS,FILTER_PHASES=IQ_FIR_PHASES };
    float *weights=malloc((FILTER_PHASES+1)*TAPS*sizeof(*weights));
    if(!weights){free(mixed);return 0;}
    const float pi=3.141592653589793f;
    float edge=(float)report->bandwidth_hz*0.5f+2.0f*REC_LR_IQ_RATE/256;
    float fc=(edge+iq_fir_margin)/REC_LR_IQ_RATE;
    for(unsigned phase=0;phase<=FILTER_PHASES;phase++) {
        float sum=0;
        for(int k=-HALF;k<=HALF;k++) {
            float d=k-(float)phase/FILTER_PHASES;
            float w=fabsf(d)>HALF?0:(fabsf(d)<1e-9f?2*fc:sinf(2*pi*fc*d)/(pi*d))*(0.5f+0.5f*cosf(pi*d/HALF));
            weights[phase*TAPS+k+HALF]=w;sum+=w;
        }
        for(int k=0;k<TAPS;k++)weights[phase*TAPS+k]/=sum;
    }
    for(size_t j=0;j<m;j++) {
        size_t pos=j*625;int center=(int)(pos/phases);
        if(!mixed[2*center] && !mixed[2*center+1]) {
            out[2*j]=out[2*j+1]=0;continue;
        }
        unsigned phase=((pos%phases)*FILTER_PHASES+phases/2)/phases;
        const float *w=weights+phase*TAPS;
        float x=0,y=0;
        for(int k=-HALF;k<=HALF;k++)if(center+k>=0 && (size_t)(center+k)<n) {
            x+=w[k+HALF]*mixed[2*(center+k)];y+=w[k+HALF]*mixed[2*(center+k)+1];
        }
        float radius=hypotf(x,y);
        if(radius>55){x*=55/radius;y*=55/radius;}
        out[2*j]=(uint8_t)(int8_t)lroundf(x);out[2*j+1]=(uint8_t)(int8_t)lroundf(y);
    }
    free(weights);
    free(mixed);return m*2;
}

size_t rec_lr_iq_convert_path(const uint8_t *iq,size_t bytes,uint8_t *out,size_t cap,uint32_t rate,bool *clean_ook)
{
    rec_lr_iq_classifier_t report;
    size_t used=rec_lr_iq_convert_report(iq,bytes,out,cap,rate,&report);
    if(clean_ook)*clean_ook=report.ook;
    return used;
}

bool rec_lr_iq_dump(const uint8_t *iq,size_t bytes,uint32_t hz,rec_lr_iq_dump_emit_t emit,void *ctx)
{
    if(!iq || !bytes || bytes%4 || bytes>REC_LR_IQ_BYTES || !emit)return false;
    char line[160];
    snprintf(line,sizeof(line),"LRIQ-BEGIN bytes=%lu rate=%u freq=%lu format=IQ16LE\n",
             (unsigned long)bytes,REC_LR_IQ_RATE,(unsigned long)hz);
    if(!emit(ctx,line))return false;
    static const char b64[]="ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    uint32_t crc=0xffffffffu;
    for(size_t at=0;at<bytes;at+=48) {
        size_t n=bytes-at<48?bytes-at:48,k=10;
        memcpy(line,"LRIQ-DATA ",10);
        for(size_t i=0;i<n;i++) {
            crc^=iq[at+i];
            for(unsigned bit=0;bit<8;bit++)crc=(crc>>1)^((crc&1)?0xedb88320u:0);
        }
        for(size_t i=0;i<n;i+=3) {
            unsigned v=(unsigned)iq[at+i]<<16;
            if(i+1<n)v|=(unsigned)iq[at+i+1]<<8;
            if(i+2<n)v|=iq[at+i+2];
            line[k++]=b64[v>>18];line[k++]=b64[(v>>12)&63];
            line[k++]=i+1<n?b64[(v>>6)&63]:'=';line[k++]=i+2<n?b64[v&63]:'=';
        }
        line[k++]='\n';line[k]=0;
        if(!emit(ctx,line))return false;
    }
    snprintf(line,sizeof(line),"LRIQ-END crc32=%08lX\n",(unsigned long)(crc^0xffffffffu));
    return emit(ctx,line);
}

size_t rec_lr_iq_convert(const uint8_t *iq,size_t bytes,uint8_t *out,size_t cap,uint32_t rate)
{ return rec_lr_iq_convert_path(iq,bytes,out,cap,rate,NULL); }

bool rec_lr_iq_save(const char *path,uint32_t hz,const uint8_t *iq,size_t bytes)
{
    if(!path || !iq || hz<150000000u || hz>999999999u || !bytes || bytes%4 || bytes>REC_LR_IQ_BYTES)return false;
    FILE *f=rec_file_open_new(path);
    if(!f)return false;
    bool ok=fprintf(f,"Filetype: LakeShark SubGhz IQ File\nVersion: 1\nFrequency: %lu\nProtocol: LR2021_IQ\nSampleRate: 125000\nSamples: %lu\n",
        (unsigned long)hz,(unsigned long)(bytes/4))>0;
    static const char hex[]="0123456789ABCDEF";
    char line[139];
    for(size_t at=0;ok && at<bytes;at+=64) {
        size_t n=bytes-at<64?bytes-at:64;
        memcpy(line,"IQ_Data: ",9);
        for(size_t i=0;i<n;i++) {line[9+i*2]=hex[iq[at+i]>>4];line[10+i*2]=hex[iq[at+i]&15];}
        line[9+n*2]='\n';
        ok=fwrite(line,1,10+n*2,f)==10+n*2;
    }
    if(fclose(f))ok=false;
    if(!ok)unlink(path);
    return ok;
}

static int nibble(char c)
{ return c>='0' && c<='9'?c-'0':c>='A' && c<='F'?c-'A'+10:c>='a' && c<='f'?c-'a'+10:-1; }

static bool number(const char *line,const char *key,uint32_t *out)
{
    size_t n=strlen(key);
    if(strncmp(line,key,n) || line[n]<'0' || line[n]>'9')return false;
    char *end;errno=0;
    unsigned long long v=strtoull(line+n,&end,10);
    if(errno || v>UINT32_MAX || strcmp(end,"\n"))return false;
    *out=(uint32_t)v;return true;
}

bool rec_lr_iq_load(const char *path,uint32_t *hz,uint8_t *iq,size_t cap,size_t *bytes)
{
    if(!path || !hz || !iq || !bytes)return false;
    *bytes=0;
    FILE *f=fopen(path,"rb");if(!f)return false;
    char *io=malloc(8192);if(io)setvbuf(f,io,_IOFBF,8192);
    char line[160];uint32_t freq=0,count=0;
    bool ok=fgets(line,sizeof(line),f) && !strcmp(line,"Filetype: LakeShark SubGhz IQ File\n") &&
        fgets(line,sizeof(line),f) && !strcmp(line,"Version: 1\n") &&
        fgets(line,sizeof(line),f) && number(line,"Frequency: ",&freq) && freq>=150000000u && freq<=999999999u &&
        fgets(line,sizeof(line),f) && !strcmp(line,"Protocol: LR2021_IQ\n") &&
        fgets(line,sizeof(line),f) && !strcmp(line,"SampleRate: 125000\n") &&
        fgets(line,sizeof(line),f) && number(line,"Samples: ",&count) && count && count<=REC_LR_IQ_SAMPLES && count*4<=cap;
    size_t used=0;
    while(ok && fgets(line,sizeof(line),f)) {
        size_t n=strlen(line);
        if(n<18 || line[n-1]!='\n' || strncmp(line,"IQ_Data: ",9) || (n-10)%8 || used+(n-10)/2>count*4) {ok=false;break;}
        for(size_t i=9;i<n-1;i+=2) {
            int hi=nibble(line[i]),lo=nibble(line[i+1]);
            if(hi<0 || lo<0){ok=false;break;}
            iq[used++]=(uint8_t)(hi*16+lo);
        }
    }
    ok=ok && !ferror(f) && used==count*4;
    fclose(f);free(io);
    if(ok){*hz=(uint32_t)freq;*bytes=used;}
    return ok;
}
