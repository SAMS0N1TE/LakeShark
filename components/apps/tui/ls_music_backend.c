/* Native-rate decoder, using the board abstraction rather than the
   Waveshare-specific BSP player (which is not the T-Display-P4 codec). */
#include "ls_music_backend.h"
#include "../media_gui/media_playlist.h"
#include "audio_player.h"
#include "audio/audio_out.h"
#include "audio/call_store.h"
#include "ls_audio_hw.h"
#include "lakeshark_backend.h"
#include <stdio.h>
#include <stdatomic.h>
#include <math.h>
#include <stdlib.h>
#include <sys/stat.h>
#include "esp_attr.h"
#ifdef ESP_PLATFORM
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#endif

static void music_heap_checkpoint(const char *name, int result)
{
    /* Heap walks and serial output on entry used to run on every switch.
       Opt in when investigating decoder allocation, rather than taxing UI. */
#if defined(ESP_PLATFORM) && defined(LS_MUSIC_HEAP_TRACE)
    if (result >= 0) printf("music heap %s result=%d internal=%u/%u dma=%u/%u\n",
        name, result,
        (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
        (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
        (unsigned)heap_caps_get_free_size(MALLOC_CAP_DMA),
        (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_DMA));
    else printf("music heap %s internal=%u/%u dma=%u/%u\n", name,
        (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
        (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
        (unsigned)heap_caps_get_free_size(MALLOC_CAP_DMA),
        (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_DMA));
#else
    (void)name; (void)result;
#endif
}

static const char *const roots[] = {"/sdcard/music", "/spiffs/music"};
static file_iterator_instance_t *s_list;
static bool s_up;
static int s_source;
static const char *s_error = "";
static atomic_bool s_bad_format;
static atomic_uint s_peak[2];
static bool s_mono_output;
static atomic_bool s_warm;
/* A conservative bass/treble tilt: DC gain 1, treble gain 2/3.
   A convex mix cannot boost peaks beyond full scale. Decoder thread owns
   filter history; UI only changes the atomic target. */
static int32_t s_low[2];
static unsigned s_alpha=160, s_tone_mix;
bool ls_music_warm(void){return atomic_load(&s_warm);}
void ls_music_set_warm(bool enabled){atomic_store(&s_warm,enabled);}

/* Spectrum buffers live in PSRAM for the life of the player. The decoder
   thread only appends the output mix to a ring; the FFT runs on the thread
   that asks for it, so the audio path never waits on the analysis. */
#define FFT_N LS_MUSIC_FFT_SIZE
/* Eight windows of history, so the spectrogram can fall a few frames behind
   the decoder and still read every sample. */
#define RING_N (8*FFT_N)
typedef struct {
    int16_t ring[RING_N];
    float re[FFT_N], im[FFT_N], window[FFT_N], cos_t[FFT_N/2], sin_t[FFT_N/2];
    float window_power;
} spectrum_t;
static spectrum_t *s_spectrum;
static atomic_uint s_ring_end, s_ring_fill;
static atomic_uint s_rate, s_bits, s_channels;
static bool analysing(void);
static void *spectrum_alloc(void)
{
#ifdef ESP_PLATFORM
    return heap_caps_calloc(1,sizeof(spectrum_t),MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);
#else
    return calloc(1,sizeof(spectrum_t));
#endif
}
static void spectrum_free(void *p)
{
#ifdef ESP_PLATFORM
    heap_caps_free(p);
#else
    free(p);
#endif
}
static void spectrum_open(void)
{
    atomic_store(&s_ring_end,0);atomic_store(&s_ring_fill,0);atomic_store(&s_rate,0);
    if(s_spectrum)return;
    spectrum_t *sp=spectrum_alloc();
    if(!sp)return;
    const float tau=6.28318530718f;
    sp->window_power=0;
    for(int n=0;n<FFT_N;n++) {
        sp->window[n]=0.5f-0.5f*cosf(tau*n/FFT_N);
        sp->window_power+=sp->window[n]*sp->window[n];
    }
    for(int k=0;k<FFT_N/2;k++){sp->cos_t[k]=cosf(tau*k/FFT_N);sp->sin_t[k]=-sinf(tau*k/FFT_N);}
    s_spectrum=sp;
}
/* Only once the decoder thread is gone. */
static void spectrum_close(void)
{
    spectrum_free(s_spectrum);s_spectrum=NULL;
}
static void fft(spectrum_t *sp)
{
    float *re=sp->re,*im=sp->im;
    for(unsigned i=1,j=0;i<FFT_N;i++) {
        unsigned bit=FFT_N>>1;
        for(;j&bit;bit>>=1)j^=bit;
        j^=bit;
        if(i<j){float t=re[i];re[i]=re[j];re[j]=t;t=im[i];im[i]=im[j];im[j]=t;}
    }
    for(unsigned len=2;len<=FFT_N;len<<=1) {
        unsigned half=len/2,step=FFT_N/len;
        for(unsigned i=0;i<FFT_N;i+=len)
            for(unsigned k=0;k<half;k++) {
                float wr=sp->cos_t[k*step],wi=sp->sin_t[k*step];
                unsigned a=i+k,b=a+half;
                float xr=re[b]*wr-im[b]*wi,xi=re[b]*wi+im[b]*wr;
                re[b]=re[a]-xr;im[b]=im[a]-xi;re[a]+=xr;im[a]+=xi;
            }
    }
}
/* Window and transform FFT_N samples starting at ring position `start`. */
static void analyse(spectrum_t *sp,unsigned start)
{
    for(unsigned n=0;n<FFT_N;n++) {
        sp->re[n]=sp->ring[(start+n)&(RING_N-1)]*sp->window[n];
        sp->im[n]=0;
    }
    fft(sp);
}
static float bin_power(const spectrum_t *sp,int k)
{
    return sp->re[k]*sp->re[k]+sp->im[k]*sp->im[k];
}
/* Parseval over the one-sided spectrum gives each band's mean square;
   a full-scale sine has a mean square of 32768^2/2. */
static float power_scale(const spectrum_t *sp)
{
    return 2.0f/(FFT_N*sp->window_power)/(32768.0f*32768.0f/2.0f);
}
static float to_db(double value)
{
    return value>1e-12?10.0f*log10f((float)value):-120.0f;
}
int ls_music_spectrum(float lo_hz,float hi_hz,int bands,float *db)
{
    for(int b=0;db && b<bands;b++)db[b]=-INFINITY;
    spectrum_t *sp=s_spectrum;
    unsigned rate=atomic_load(&s_rate);
    if(!db || bands<2 || !(lo_hz>0) || !(hi_hz>lo_hz) || !sp || !rate ||
       !analysing() || atomic_load(&s_ring_fill)<FFT_N)return 0;
    analyse(sp,atomic_load(&s_ring_end)-FFT_N);
    const float scale=power_scale(sp);
    const float step=powf(hi_hz/lo_hz,1.0f/(bands-1)),edge=sqrtf(step);
    const float per_bin=(float)FFT_N/rate;
    int measured=0;
    for(int b=0;b<bands;b++) {
        float centre=lo_hz*powf(step,(float)b);
        if(centre*edge*2>=rate)break;
        int first=(int)ceilf(centre/edge*per_bin),last=(int)floorf(centre*edge*per_bin);
        if(first<1)first=1;
        double power=0;
        if(last<first) {
            /* Narrower than a bin: read between the two nearest bins, so
               neighbouring bands differ instead of repeating one bin. */
            float at=centre*per_bin;
            int k=(int)at;
            if(k<1){k=1;at=1;}
            float frac=at-k;
            power=bin_power(sp,k)*(1-frac)+bin_power(sp,k+1)*frac;
        } else
            for(int k=first;k<=last;k++)power+=bin_power(sp,k);
        db[b]=to_db(power*scale);
        measured++;
    }
    return measured;
}
bool ls_music_spectrogram(ls_music_cursor_t *cursor,int hops,int rows,float *db)
{
    spectrum_t *sp=s_spectrum;
    unsigned rate=atomic_load(&s_rate);
    if(!cursor || !db || rows<1 || hops<1 || hops>4 || !sp || !rate)return false;
    unsigned end=atomic_load(&s_ring_end),fill=atomic_load(&s_ring_fill);
    unsigned need=(unsigned)hops*FFT_N,lag=end-cursor->next;
    /* Unstarted, or the decoder has lapped the reader: resume at the newest
       complete column rather than read samples being overwritten. */
    if(!cursor->started || cursor->rate!=rate || lag>fill || lag>RING_N-FFT_N) {
        cursor->started=true;cursor->rate=rate;
        if(fill<need){cursor->next=end;return false;}
        cursor->next=end-need;lag=need;
    }
    if(lag<need)return false;
    for(int r=0;r<rows;r++)db[r]=0;
    const int half=FFT_N/2;
    for(int h=0;h<hops;h++) {
        analyse(sp,cursor->next+(unsigned)h*FFT_N);
        for(int r=0;r<rows;r++) {
            int lo=r*half/rows,hi=(r+1)*half/rows;
            if(lo<1)lo=1;
            if(hi<=lo)hi=lo+1;
            double power=0;
            for(int k=lo;k<hi && k<half;k++)power+=bin_power(sp,k);
            db[r]+=(float)power;
        }
    }
    const float scale=power_scale(sp)/hops;
    for(int r=0;r<rows;r++)db[r]=to_db((double)db[r]*scale);
    cursor->next+=need;
    return true;
}
bool ls_music_format(ls_music_format_t *out)
{
    if(!out)return false;
    out->rate_hz=atomic_load(&s_rate);out->bits=atomic_load(&s_bits);
    out->channels=atomic_load(&s_channels);out->mono_out=s_mono_output;
    return s_up && out->rate_hz;
}
/* The board's codec curve is -50..0 dB in 0.5 dB volume steps. */
float ls_music_volume_db(void){return (audio_volume_get()-100)*0.5f;}

static void ring_push(const int16_t *mono,size_t frames)
{
    spectrum_t *sp=s_spectrum;
    if(!sp || !frames)return;
    unsigned end=atomic_load(&s_ring_end);
    for(size_t i=0;i<frames;i++)sp->ring[(end+i)&(RING_N-1)]=mono[i];
    atomic_store(&s_ring_end,end+(unsigned)frames);
    unsigned fill=atomic_load(&s_ring_fill);
    if(fill<RING_N)atomic_store(&s_ring_fill,fill+frames<RING_N?fill+(unsigned)frames:RING_N);
}
static void peaks_publish(const unsigned peaks[2])
{
    for(int ch=0;ch<2;ch++) {
        unsigned old=atomic_load(&s_peak[ch]);
        while(old<peaks[ch] && !atomic_compare_exchange_weak(&s_peak[ch],&old,peaks[ch])) {}
    }
}
static int16_t tone(int ch,int32_t x)
{
    s_low[ch]+=(x-s_low[ch])*(int32_t)s_alpha/4096;
    int32_t tilted=(2*x+s_low[ch])/3;
    return (int16_t)(x+(tilted-x)*(int32_t)s_tone_mix/1024);
}

/* Peaks are the decoded source channels, before downmix and tone, so the
   meters stay stereo on a mono board. The spectrum ring takes what the codec
   is sent. Exchange-on-read collects transients between UI frames without a
   lock. A mono file arrives one sample per frame. */
static esp_err_t write_pcm(void *data,size_t len,size_t *written,uint32_t timeout)
{
    int16_t *pcm=data;
    const bool mono=atomic_load(&s_channels)==1;
    const size_t step=mono?1:2,count=len/sizeof(*pcm);
    unsigned peaks[2]={0,0};
    bool warm=atomic_load(&s_warm);
    int16_t mix[128];
    size_t mixed=0;
    for(size_t i=0;i+step<=count;i+=step) {
        int32_t l=pcm[i],r=mono?l:pcm[i+1];
        unsigned ml=l<0?-l:l,mr=r<0?-r:r;
        if(ml>peaks[0])peaks[0]=ml;
        if(mr>peaks[1])peaks[1]=mr;
        /* This board has a mono DAC: preserve both source channels rather
           than letting the codec select only the left slot. No gain boost. */
        if(s_mono_output)l=r=(l+r)/2;
        if(warm && s_tone_mix<1024)s_tone_mix++;
        if(!warm && s_tone_mix)s_tone_mix--;
        pcm[i]=tone(0,l);
        if(!mono)pcm[i+1]=tone(1,r);
        mix[mixed++]=mono?pcm[i]:(int16_t)((pcm[i]+pcm[i+1])/2);
        if(mixed==sizeof(mix)/sizeof(*mix)){ring_push(mix,mixed);mixed=0;}
    }
    ring_push(mix,mixed);
    esp_err_t err=ls_audio_hw_write(data,len,written,timeout);
    if(err==ESP_OK)peaks_publish(peaks);
    return err;
}

/* THE MICROPHONE, as a second source for the same meters and plots.

   A permanent worker reads the codec's input in blocks and feeds the ring,
   and can write what it hears to a mono WAV on the card, where it joins the
   track list to be played back and looked at again. Its stack is in PSRAM:
   it only touches the SD card, never flash, and internal RAM is what the
   rest of the app is short of. */
#define MIC_RATE 44100u
#define MIC_FRAMES 1024
#define MIC_GAIN_DB 30.0f
static atomic_bool s_mic_run, s_mic_busy;
static FILE *s_mic_file;
static uint32_t s_mic_bytes;
EXT_RAM_BSS_ATTR static char s_mic_path[48];
static int16_t *s_mic_buf;
#ifdef ESP_PLATFORM
static TaskHandle_t s_mic_task;
static StackType_t *s_mic_stack;
static StaticTask_t *s_mic_tcb;
static void mic_worker(void *arg)
{
    (void)arg;
    for(;;) {
        ulTaskNotifyTake(pdTRUE,portMAX_DELAY);
        while(atomic_load(&s_mic_run))
            if(!ls_music_mic_step()) {
                s_error="Microphone read failed";
                atomic_store(&s_mic_run,false);
            }
        atomic_store(&s_mic_busy,false);
    }
}
#endif
static bool analysing(void)
{
    return ls_music_state()==LS_MUSIC_PLAYING || atomic_load(&s_mic_busy);
}
static void wav_header(FILE *f,uint32_t data_bytes)
{
    uint8_t h[44]={'R','I','F','F',0,0,0,0,'W','A','V','E','f','m','t',' ',16,0,0,0,1,0,1,0,
                   0,0,0,0,0,0,0,0,2,0,16,0,'d','a','t','a'};
    uint32_t riff=36+data_bytes,rate=MIC_RATE,byte_rate=MIC_RATE*2;
    for(int i=0;i<4;i++) {
        h[4+i]=(uint8_t)(riff>>(8*i));h[24+i]=(uint8_t)(rate>>(8*i));
        h[28+i]=(uint8_t)(byte_rate>>(8*i));h[40+i]=(uint8_t)(data_bytes>>(8*i));
    }
    fwrite(h,1,sizeof(h),f);
}
static bool mic_open_file(void)
{
    for(int n=1;n<1000;n++) {
        snprintf(s_mic_path,sizeof(s_mic_path),"%s/REC-%03d.wav",roots[0],n);
        struct stat st;
        if(stat(s_mic_path,&st)==0)continue;
        s_mic_file=fopen(s_mic_path,"wb");
        if(!s_mic_file)break;
        wav_header(s_mic_file,0);
        s_mic_bytes=0;
        return true;
    }
    s_mic_path[0]=0;
    return false;
}
static void mic_close_file(void)
{
    if(!s_mic_file)return;
    if(fseek(s_mic_file,0,SEEK_SET)==0)wav_header(s_mic_file,s_mic_bytes);
    fclose(s_mic_file);
    s_mic_file=NULL;
}
bool ls_music_mic_step(void)
{
    int16_t *buf=s_mic_buf;
    if(!buf)return false;
    size_t got=0;
    if(ls_audio_hw_read(buf,MIC_FRAMES*2*sizeof(*buf),&got)!=ESP_OK || !got)return false;
    size_t frames=got/(2*sizeof(*buf));
    unsigned peaks[2]={0,0};
    for(size_t i=0;i<frames;i++) {
        int32_t l=buf[2*i],r=buf[2*i+1];
        unsigned ml=l<0?-l:l,mr=r<0?-r:r;
        if(ml>peaks[0])peaks[0]=ml;
        if(mr>peaks[1])peaks[1]=mr;
        buf[i]=(int16_t)((l+r)/2);
    }
    ring_push(buf,frames);
    peaks_publish(peaks);
    if(s_mic_file) {
        size_t wrote=fwrite(buf,sizeof(*buf),frames,s_mic_file);
        s_mic_bytes+=(uint32_t)(wrote*sizeof(*buf));
        if(wrote!=frames){s_error="Card write failed; recording stopped";mic_close_file();}
    }
    return true;
}
void ls_music_mic_stop(void)
{
    atomic_store(&s_mic_run,false);
#ifdef ESP_PLATFORM
    for(int i=0;i<100 && atomic_load(&s_mic_busy);i++)vTaskDelay(pdMS_TO_TICKS(5));
#else
    atomic_store(&s_mic_busy,false);
#endif
    mic_close_file();
}
bool ls_music_mic_start(bool record)
{
    if(!s_up || !s_spectrum)return false;
    if(atomic_load(&s_mic_busy)) {
        if(record && !s_mic_file && !mic_open_file()){s_error="Cannot create a file on the card";return false;}
        return true;
    }
    ls_music_stop();
    if(!s_mic_buf) {
#ifdef ESP_PLATFORM
        s_mic_buf=heap_caps_malloc(MIC_FRAMES*2*sizeof(int16_t),MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);
#else
        s_mic_buf=malloc(MIC_FRAMES*2*sizeof(int16_t));
#endif
        if(!s_mic_buf){s_error="Microphone memory unavailable";return false;}
    }
    if(!ls_audio_hw_has_mic()){s_error="No microphone on this board";return false;}
    if(ls_audio_hw_set_fs(MIC_RATE,16,I2S_SLOT_MODE_STEREO)!=ESP_OK){s_error="Microphone clock failed";return false;}
    ls_audio_hw_in_gain(MIC_GAIN_DB);
    if(atomic_load(&s_rate)!=MIC_RATE)atomic_store(&s_ring_fill,0);
    atomic_store(&s_rate,MIC_RATE);atomic_store(&s_bits,16);atomic_store(&s_channels,1);
    if(record && !mic_open_file()){s_error="Cannot create a file on the card";return false;}
#ifdef ESP_PLATFORM
    if(!s_mic_task) {
        /* Allocated on first use rather than held from boot: the control
           block has to be internal, and boot has none to spare. */
        s_mic_stack=heap_caps_malloc(4096,MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);
        s_mic_tcb=heap_caps_malloc(sizeof(StaticTask_t),MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT);
        if(s_mic_stack && s_mic_tcb)
            s_mic_task=xTaskCreateStaticPinnedToCore(mic_worker,"music_mic",4096,NULL,4,
                                                     s_mic_stack,s_mic_tcb,tskNO_AFFINITY);
        if(!s_mic_task) {
            heap_caps_free(s_mic_stack);heap_caps_free(s_mic_tcb);
            s_mic_stack=NULL;s_mic_tcb=NULL;mic_close_file();
            s_error="Microphone task unavailable";return false;
        }
    }
#endif
    s_error="";
    atomic_store(&s_mic_busy,true);
    atomic_store(&s_mic_run,true);
#ifdef ESP_PLATFORM
    xTaskNotifyGive(s_mic_task);
#endif
    return true;
}
/* With the app closing: stop the worker, and once it is parked waiting for
   the next start, delete it and give its memory back. */
static void mic_release(void)
{
    ls_music_mic_stop();
#ifdef ESP_PLATFORM
    if(s_mic_task && !atomic_load(&s_mic_busy)) {
        vTaskDelete(s_mic_task);
        s_mic_task=NULL;
        heap_caps_free(s_mic_stack);heap_caps_free(s_mic_tcb);
        s_mic_stack=NULL;s_mic_tcb=NULL;
    }
    if(!s_mic_task){heap_caps_free(s_mic_buf);s_mic_buf=NULL;}
#else
    free(s_mic_buf);s_mic_buf=NULL;
#endif
}
ls_music_mic_t ls_music_mic_state(void)
{
    return !atomic_load(&s_mic_busy)?LS_MIC_OFF:s_mic_file?LS_MIC_RECORDING:LS_MIC_LIVE;
}
const char *ls_music_mic_file(void){return s_mic_path;}
uint32_t ls_music_mic_ms(void){return (uint32_t)((uint64_t)s_mic_bytes*1000/(MIC_RATE*2));}

unsigned ls_music_peak(int channel)
{
    if(channel<0 || channel>1)return 0;
    unsigned peak=atomic_exchange(&s_peak[channel],0);
    return analysing()?peak:0;
}

static esp_err_t mute(AUDIO_PLAYER_MUTE_SETTING setting)
{
    return ls_audio_hw_mute(setting == AUDIO_PLAYER_MUTE || audio_is_muted());
}
static esp_err_t clock_set(uint32_t rate, uint32_t bits, i2s_slot_mode_t channels)
{
    s_alpha=4096u*1885u/(rate+1885u); /* approximately 300 Hz */
    s_low[0]=s_low[1]=0;
    if(rate!=atomic_load(&s_rate))atomic_store(&s_ring_fill,0);
    atomic_store(&s_rate,rate);atomic_store(&s_bits,bits);
    atomic_store(&s_channels,channels==I2S_SLOT_MODE_MONO?1u:2u);
    esp_err_t err = ls_audio_hw_set_fs(rate, bits, channels);
    if (err == ESP_OK) { int actual; err = ls_audio_hw_volume(audio_volume_get(), &actual); }
    return err;
}
static void event(audio_player_cb_ctx_t *ctx)
{
    if (ctx->audio_event == AUDIO_PLAYER_CALLBACK_EVENT_UNKNOWN_FILE_TYPE)
        atomic_store(&s_bad_format, true);
}
bool ls_music_open(void)
{
    if (s_up) return true;
    s_low[0]=s_low[1]=0;s_tone_mix=0;
    s_mono_output=ls_audio_hw_output_is_mono();
    spectrum_open();
    lakeshark_radio_park();
    music_heap_checkpoint("before media acquire", -1);
    esp_err_t err = audio_out_media_acquire();
    music_heap_checkpoint("after media acquire", err);
    if (err != ESP_OK) { spectrum_close(); s_error = "Audio busy / unavailable"; return false; }
    audio_player_config_t config = {.mute_fn=mute, .clk_set_fn=clock_set,
        .write_fn=write_pcm, .priority=5, .coreID=1, .file_close_fn=call_store_playback_close};
    err = audio_player_new(config);
    music_heap_checkpoint("after player new", err);
    if (err != ESP_OK) {
        audio_out_media_release();
        music_heap_checkpoint("after media release", -1);
        spectrum_close();
        s_error = "Decoder memory unavailable"; return false;
    }
    s_up = true;
    atomic_store(&s_peak[0],0);atomic_store(&s_peak[1],0);
    audio_player_callback_register(event, NULL);
    s_error = ""; atomic_store(&s_bad_format, false);
    return true;
}
void ls_music_close(void)
{
    mic_release();
    if (s_up) {
        music_heap_checkpoint("before player delete", -1);
        esp_err_t err = audio_player_delete();
        music_heap_checkpoint("after player delete", err);
        if (err != ESP_OK) {
            s_error = "Decoder busy: audio retained"; return;
        }
    }
    if (s_up) {
        s_up = false; audio_out_media_release(); spectrum_close();
        music_heap_checkpoint("after media release", -1);
    }
    music_heap_checkpoint("before playlist close", -1);
    ls_media_playlist_close(s_list); s_list = NULL;
    music_heap_checkpoint("after playlist close", -1);
}
bool ls_music_scan(int source)
{
    if (source < 0 || source > 1) return false;
    if (!s_up && !ls_music_open()) return false;
    ls_music_mic_stop();
    ls_music_stop();
    music_heap_checkpoint("before playlist close", -1);
    ls_media_playlist_close(s_list);
    music_heap_checkpoint("after playlist close", -1);
    music_heap_checkpoint("before playlist open", -1);
    s_source = source; s_list = ls_media_playlist_open(roots[source]);
    music_heap_checkpoint("after playlist open", -1);
    s_error = s_list ? "" : "Music folder missing / unreadable";
    atomic_store(&s_bad_format, false);
    return s_list != NULL;
}
int ls_music_count(void) { return s_list ? file_iterator_get_count(s_list) : 0; }
const char *ls_music_name(int index)
{
    return index >= 0 && index < ls_music_count() ? file_iterator_get_name_from_index(s_list,index) : "";
}
const char *ls_music_error(void) { return atomic_load(&s_bad_format) ? "Unsupported or damaged audio file" : s_error; }
bool ls_music_play(int index)
{
    if (!s_up || index < 0 || index >= ls_music_count()) return false;
    ls_music_mic_stop();
    char path[512];
    int len = snprintf(path,sizeof(path),"%s/%s",roots[s_source],ls_music_name(index));
    if (len < 0 || (size_t)len >= sizeof(path)) { s_error="Track path too long"; return false; }
    return ls_music_play_path(path);
}
bool ls_music_is_open(void) { return s_up; }
bool ls_music_play_path(const char *path)
{
    if (!s_up || !path) return false;
    ls_music_mic_stop();
    FILE *fp = call_store_playback_open(path);
    if (!fp) { s_error="Track unavailable; rescan card"; return false; }
    if (audio_player_play(fp) != ESP_OK) { call_store_playback_close(fp); s_error="Player queue busy"; return false; }
    s_error=""; atomic_store(&s_bad_format,false); return true;
}
bool ls_music_seek_ms(uint32_t ms) { return s_up && audio_player_seek_ms(ms)==ESP_OK; }
bool ls_music_toggle(void)
{
    if (!s_up) return false;
    return (audio_player_get_state() == AUDIO_PLAYER_STATE_PAUSE ? audio_player_resume() : audio_player_pause()) == ESP_OK;
}
void ls_music_stop(void) { if (s_up) audio_player_stop(); }
ls_music_state_t ls_music_state(void)
{
    if (!s_up) return LS_MUSIC_STOPPED;
    audio_player_state_t s = audio_player_get_state();
    return s == AUDIO_PLAYER_STATE_PLAYING ? LS_MUSIC_PLAYING : s == AUDIO_PLAYER_STATE_PAUSE ? LS_MUSIC_PAUSED : LS_MUSIC_STOPPED;
}
int ls_music_volume(void) { return audio_volume_get(); }
uint32_t ls_music_position_ms(void) { return s_up ? audio_player_position_ms() : 0; }
uint32_t ls_music_duration_ms(void) { return s_up ? audio_player_duration_ms() : 0; }
void ls_music_volume_step(int delta) { audio_volume_delta(delta); }
const char *ls_music_output(void) { return audio_is_muted() ? "MUTED" : "BOARD AUDIO"; }
bool ls_music_live(void) { return analysing(); }
