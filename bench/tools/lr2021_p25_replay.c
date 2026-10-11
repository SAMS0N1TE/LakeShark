/* Offline LR2021 IQ16 bridge into the production P25 acquisition harness.
 * Selftests use synthetic fixtures; inspect accepts external IQ16 recordings. */
#define main legacy_p25_replay_main
#include "p25_iq_replay.c"
#undef main
#include "p25_lr_iq.h"
#include "p25_lr_frontend.h"
/* Benchmark selection only; normal replay keeps its established behavior. */
static unsigned candidate_frontend;
static float candidate_center;
static float bridge_iq16_scale = 128.0f; /* Synthetic fixture units. Files use 1. */

/* Windowed-sinc rate conversion, deliberately offline. Twelve input samples
 * either side, cutoff below both Nyquist limits. No device real-time claim. */
static float *resample(const float *in, size_t n, double from, double to, size_t *count)
{
    *count = (size_t)floor(n * to / from);
    float *out = calloc(*count * 2, sizeof(float));
    if (!out) return NULL;
    double cutoff = 0.45 * fmin(1.0, to / from);
    for (size_t j = 0; j < *count; ++j) {
        double x = j * from / to, a = 0, b = 0, weight = 0;
        long base = (long)floor(x);
        for (long k = base - 12; k <= base + 12; ++k) {
            if (k < 0 || (size_t)k >= n) continue;
            double d = x-k;
            if (fabs(d) > 12) continue;
            double w = (fabs(d)<1e-9 ? 2*cutoff : sin(2*M_PI*cutoff*d)/(M_PI*d))
                     * (0.5+0.5*cos(M_PI*d/12));
            a += in[2*k]*w; b += in[2*k+1]*w; weight += w;
        }
        out[2*j]=(float)(a/weight); out[2*j+1]=(float)(b/weight);
    }
    return out;
}
static uint8_t *to_u8(const float *in, size_t n)
{
    double peak = 1;
    for (size_t i=0;i<2*n;i++) if (fabs(in[i])>peak) peak=fabs(in[i]);
    uint8_t *out=malloc(2*n);
    if (out) for(size_t i=0;i<2*n;i++) out[i]=(uint8_t)lround(127.5+110*in[i]/peak);
    return out;
}
static float *fixture(const char *mode, size_t *n)
{
    const uint8_t *iq=!strcmp(mode,"c4fm") ? protocol_iq() : protocol_cqpsk_iq();
    *n=IQ_BYTES/2;
    float *out=malloc(*n*2*sizeof(float));
    if(out) for(size_t i=0;i<*n*2;i++) out[i]=iq[i]-127.5f;
    return out;
}
static int decode(const float *iq, size_t n, double rate, const char *mode, unsigned minimum_tsbks)
{
    float *precise = NULL;
    size_t m; float *converted=resample(iq,n,rate,240000,&m);
    if(!converted)return 2;
    uint8_t *bytes=to_u8(converted,m); free(converted);
    if (rate == 125000 && n <= 31314) {
        uint8_t *raw = malloc(n*4), *device = malloc(240512);
        if (!raw || !device) { free(raw); free(device); free(bytes); return 2; }
        for (size_t k=0; k<2*n; k++) {
            int v=(int)lroundf(iq[k]*bridge_iq16_scale);
            if(v < -32768) v=-32768;
            if(v > 32767) v=32767;
            raw[2*k]=(uint8_t)v; raw[2*k+1]=(uint8_t)((unsigned)v>>8);
        }
        size_t output = p25_lr_iq_convert(raw,n*4,device,240512);
        if (candidate_frontend) {
            p25_lr_frontend_t frontend;
            if (!p25_lr_frontend_init(&frontend,candidate_center,candidate_frontend==2)) {
                free(raw); free(device); free(bytes); return 2;
            }
            precise=malloc(240512*sizeof(float));
            if (!precise) { free(raw); free(device); free(bytes); return 2; }
            size_t used=0;
            /* Exercise real arbitrary FIFO-sized boundaries, not a whole-window
             * shortcut. Float DSP input avoids the U8 quantization below. */
            for(size_t pos=0;pos<n*4;) {
                size_t chunk=n*4-pos; if(chunk>188)chunk=188;
                size_t got=p25_lr_frontend_process(&frontend,raw+pos,chunk,
                                                  precise+2*used,120256-used);
                if(got==SIZE_MAX) {free(precise);free(raw);free(device);free(bytes);return 2;}
                used+=got;pos+=chunk;
            }
            output=used*2;
        }
        free(raw); free(bytes); bytes=device; m=output/2;
        if (!output) { free(bytes); return 2; }
    }
    if(!bytes)return 2;
    replay_init(bytes,m*2,4096); external_replay=true; external_voice_dispatches=0;
    replay.float_iq=precise;
    dsp_set_mode(&replay.dsp,!strcmp(mode,"c4fm") ? DEMOD_C4FM : DEMOD_CQPSK);
    dsp_set_gain(&replay.dsp,!strcmp(mode,"c4fm") ? -9000.0f : 9000.0f);
    LS_CHECK(prepare(false,false));
    unsigned frames=0;
    while(!exitflag) if(replay_frame())frames++;
    unsigned tsbk=replay.state.p25_tsbk_valid_count;
    printf("LRP25 mode=%s rate=%.0f pairs=%zu duration_ms=%.3f sync=%u frames=%u nid_ok=%d nid_fail=%d tsbk=%u wacn=%05lX sysid=%03X voice_dispatch=%u nac=%03X pcm=not-tested\n",
      mode,rate,n,1000*n/rate,replay.state.acquisition_hunt.raw_syncs,frames,
      autoscan_bch_ok_flag,dsd_bch_fail_counter,tsbk,(unsigned long)replay.state.p25_tsbk_wacn,
      replay.state.p25_tsbk_sysid,external_voice_dispatches,replay.state.nac);
    bool good=tsbk>=minimum_tsbks && replay.state.p25_tsbk_wacn==0xabcde && replay.state.p25_tsbk_sysid==0x789;
    free(precise); free(bytes); return minimum_tsbks && !good ? 1 : 0;
}
static int benchmark(void)
{
    struct scenario { const char *name; float noise, blocker, offset; bool modulated; } cases[]={
        {"clean",0,0,0,false}, {"noise-low",6,0,0,false}, {"noise-high",24,0,0,false},
        {"noise-severe",100,0,0,false},
        {"adjacent",0,110,0,false}, {"adjacent-noise",6,110,0,false},
        {"adjacent-modulated",6,110,0,true}, {"offset-known",0,0,3000,false}
    };
    puts("SIMULATION: 250 ms windows; synthetic control frames, no vocoder or hardware FSK model.");
    puts("Offset correction uses the known injected offset; this is not automatic acquisition.");
    int status=0;
    for(unsigned mode=0;mode<2;mode++) {
        const char *name=mode?"cqpsk":"c4fm";
        size_t n,m; float *in=fixture(name,&n); if(!in)return 2;
        float *lr=resample(in,n,240000,125000,&m);free(in);if(!lr)return 2;
        if(m<46250){free(lr);return 2;}
        for(unsigned c=0;c<sizeof(cases)/sizeof(cases[0]);c++) {
            unsigned totals[3]={0};
            for(unsigned seed=0;seed<3;seed++) {
                float *window=malloc(62500*sizeof(float));if(!window){free(lr);return 2;}
                ls_rng_t rng;ls_rng_seed(&rng,0x821u+seed);
                for(size_t k=0;k<31250;k++) {
                    double a=2*M_PI*cases[c].offset*k/125000;
                    double b=2*M_PI*(12500+cases[c].offset)*k/125000+seed;
                    size_t at=2*(k+seed*5000);
                    size_t other=2*((k+17000)%m);
                    double bi=cases[c].modulated ? lr[other]/127.5 : 1;
                    double bq=cases[c].modulated ? lr[other+1]/127.5 : 0;
                    /* Desired IQ scaled to 25% of the fixture; blocker remains
                     * below IQ16 clipping. Noise is a seeded sum of four
                     * uniforms (the harness approximation, not exact AWGN). */
                    window[2*k]=(float)(.25*(lr[at]*cos(a)-lr[at+1]*sin(a))+
                        cases[c].blocker*(bi*cos(b)-bq*sin(b))+cases[c].noise*ls_rng_noise(&rng));
                    window[2*k+1]=(float)(.25*(lr[at]*sin(a)+lr[at+1]*cos(a))+
                        cases[c].blocker*(bi*sin(b)+bq*cos(b))+cases[c].noise*ls_rng_noise(&rng));
                }
                for(candidate_frontend=0;candidate_frontend<3;candidate_frontend++) {
                    candidate_center=cases[c].offset;
                    status|=decode(window,31250,125000,name,c==0?2:0);
                    totals[candidate_frontend]+=replay.state.p25_tsbk_valid_count;
                }
                free(window);
            }
            printf("COMPARE,%s,%s,legacy=%u,float=%u,filtered=%u\n",name,cases[c].name,totals[0],totals[1],totals[2]);
            /* Fixed deterministic regression bounds, not a claimed sensitivity
             * threshold. Both paths see identical integer-quantized samples. */
            if(cases[c].blocker && (totals[2]<6 || totals[2]<=totals[0])) status=1;
        }
        free(lr);
    }
    candidate_frontend=0;
    return status;
}
int main(int argc,char **argv)
{
    if(argc==2 && !strcmp(argv[1],"benchmark")) return benchmark();
    if(argc==2 && !strcmp(argv[1],"selftest")) {
        int status=0;
        for(int mode=0;mode<2;mode++) {
            const char *name=mode?"cqpsk":"c4fm";
            size_t n,m;float *in=fixture(name,&n);if(!in)return 2;
            float *lr=resample(in,n,240000,125000,&m);free(in);if(!lr)return 2;
            /* IQ16 quantization and arbitrary window phase, as with FIFO starts. */
            for(size_t i=0;i<2*m;i++)lr[i]=roundf(lr[i]);
            status |= decode(lr,m,125000,name,mode ? 19 : 20);
            for(candidate_frontend=0;candidate_frontend<3;candidate_frontend++)
            for(size_t offset=0;offset<15000;offset+=5000)
                status |= decode(lr+2*offset,31250,125000,name,2);
            candidate_frontend=0;
            free(lr);
            float *noise=calloc(62500,sizeof(float));if(!noise)return 2;
            ls_rng_t rng;ls_rng_seed(&rng,0x193u);
            for(size_t i=0;i<62500;i++)noise[i]=ls_rng_noise(&rng)*100;
            for(candidate_frontend=0;candidate_frontend<3;candidate_frontend++) {
                status |= decode(noise,31250,125000,name,0);
                if(replay.state.p25_tsbk_valid_count || autoscan_bch_ok_flag)status=1;
            }
            memset(noise,0,62500*sizeof(float));
            for(candidate_frontend=0;candidate_frontend<3;candidate_frontend++) {
                status |= decode(noise,31250,125000,name,0);
                if(replay.state.p25_tsbk_valid_count || autoscan_bch_ok_flag)status=1;
            }
            candidate_frontend=0;
            free(noise);
        }
        return status;
    }
    if(argc==4 && !strcmp(argv[1],"generate") &&
       (!strcmp(argv[2],"c4fm") || !strcmp(argv[2],"cqpsk"))) {
        size_t n,m;float *in=fixture(argv[2],&n);if(!in)return 2;
        float *tx=resample(in,n,240000,2000000,&m);free(in);if(!tx)return 2;
        FILE *f=fopen(argv[3],"wbx");if(!f){free(tx);return 2;}
        /* Fixed LO 100 kHz below receiver; amplitude <=32, RF amplifier off. */
        for(size_t i=0;i<m;i++) {
            double a=2*M_PI*100000*i/2000000;
            double re=(tx[2*i]*cos(a)-tx[2*i+1]*sin(a))*0.20;
            double im=(tx[2*i]*sin(a)+tx[2*i+1]*cos(a))*0.20;
            int8_t pair[2]={(int8_t)lround(fmax(-32,fmin(32,re))),(int8_t)lround(fmax(-32,fmin(32,im)))};
            if(fwrite(pair,1,2,f)!=2){fclose(f);free(tx);return 2;}
        }
        free(tx);return fclose(f)?2:0;
    }
    if(argc==5 && (!strcmp(argv[1],"replay") || !strcmp(argv[1],"inspect") || !strcmp(argv[1],"inspect-filtered")) &&
       (!strcmp(argv[4],"c4fm") || !strcmp(argv[4],"cqpsk"))) {
        char *end;double rate=strtod(argv[3],&end);
        if(!*argv[3]||*end||!isfinite(rate)||rate<48000||rate>1000000)return 2;
        FILE *f=fopen(argv[2],"rb");if(!f)return 2;
        if(fseek(f,0,SEEK_END)){fclose(f);return 2;}
        long len=ftell(f);
        if(len<=0||len>4*1024*1024||(len%4)||fseek(f,0,SEEK_SET)){fclose(f);return 2;}
        float *iq=malloc((size_t)len/2*sizeof(float));if(!iq){fclose(f);return 2;}
        for(long i=0;i<len/2;i++) {
            int lo=fgetc(f),hi=fgetc(f);if(lo<0||hi<0){free(iq);fclose(f);return 2;}
            int v=lo|(hi<<8);iq[i]=(float)(v<32768?v:v-65536);
        }
        fclose(f);
        bridge_iq16_scale=1.0f;
        bool inspect=strncmp(argv[1],"inspect",7)==0;
        if(!strcmp(argv[1],"inspect-filtered")) {
            if(rate!=125000 || len>125256){free(iq);return 2;}
            candidate_frontend=2;
        }
        int rc=decode(iq,(size_t)len/4,rate,argv[4],inspect ? 0 : 1);
        if(inspect && !rc && autoscan_bch_ok_flag==0)rc=1;
        free(iq);return rc;
    }
    fprintf(stderr,"lr2021_p25_replay selftest|benchmark\nlr2021_p25_replay generate c4fm|cqpsk output.iq8\nlr2021_p25_replay replay input.iq16 rate c4fm|cqpsk\nlr2021_p25_replay inspect input.iq16 rate c4fm|cqpsk (any NAC; NID only, no audio verification)\nlr2021_p25_replay inspect-filtered input.iq16 125000 c4fm|cqpsk (max 125256 bytes; experimental float filter)\n");
    return 2;
}
