#include "ls_test.h"
#include "ls_music_backend.h"
#include "media_playlist.h"
#include "audio_player.h"
#include "ls_audio_hw.h"
#include "audio/audio_out.h"
#include <string.h>
#include <math.h>
static int acquire_result,new_result,delete_result,play_result,leases,releases,created,closed,parks,volume=35;
static bool muted,hw_muted,missing;
static audio_player_config_t config;
static audio_player_cb_t callback;
static audio_player_state_t state;
uint32_t audio_player_position_ms(void){return 0;}
uint32_t audio_player_duration_ms(void){return 0;}
static file_iterator_instance_t list;
static char *names[]={"test.wav"};
void lakeshark_radio_park(void){parks++;}
esp_err_t audio_out_media_acquire(void){leases++;return acquire_result;}
void audio_out_media_release(void){releases++;}
bool audio_is_muted(void){return muted;}
int audio_volume_get(void){return volume;}
void audio_volume_delta(int d){volume+=d;}
esp_err_t ls_audio_hw_mute(bool value){hw_muted=value;return ESP_OK;}
bool ls_audio_hw_output_is_mono(void){return true;}
esp_err_t ls_audio_hw_volume(int v,int *actual){*actual=v;return ESP_OK;}
esp_err_t ls_audio_hw_set_fs(uint32_t r,uint32_t b,i2s_slot_mode_t c){return ESP_OK;}
esp_err_t ls_audio_hw_write(void*d,size_t n,size_t*w,uint32_t t){*w=n;return ESP_OK;}
static double mic_hz=1000;static int mic_amp=8000;static bool mic_fail,has_mic=true;static float mic_gain_db;static unsigned mic_t;
esp_err_t ls_audio_hw_read(void*d,size_t n,size_t*got){
    if(mic_fail){*got=0;return ESP_FAIL;}
    int16_t*p=d;for(size_t i=0;i<n/4;i++,mic_t++)p[2*i]=p[2*i+1]=(int16_t)(mic_amp*sin(6.28318530718*mic_hz*mic_t/44100));
    *got=n;return ESP_OK;}
bool ls_audio_hw_has_mic(void){return has_mic;}
esp_err_t ls_audio_hw_in_gain(float db){mic_gain_db=db;return ESP_OK;}
esp_err_t audio_player_new(audio_player_config_t c){config=c;created++;return new_result;}
esp_err_t audio_player_delete(void){return delete_result;}
esp_err_t audio_player_callback_register(audio_player_cb_t cb,void*ctx){callback=cb;return ESP_OK;}
audio_player_state_t __wrap_audio_player_get_state(void){return state;}
esp_err_t audio_player_stop(void){state=AUDIO_PLAYER_STATE_IDLE;return ESP_OK;}
esp_err_t audio_player_pause(void){state=AUDIO_PLAYER_STATE_PAUSE;return ESP_OK;}
esp_err_t audio_player_resume(void){state=AUDIO_PLAYER_STATE_PLAYING;return ESP_OK;}
FILE *__wrap_fopen(const char*p,const char*m){return missing?NULL:ls_test_tmpfile();}
int __real_fclose(FILE*);
static bool capture_header;static uint8_t header[44];
int __wrap_fclose(FILE*f){
    if(capture_header){capture_header=false;memset(header,0,sizeof(header));rewind(f);if(fread(header,1,sizeof(header),f)!=sizeof(header))header[0]=0;}
    closed++;return __real_fclose(f);}
esp_err_t audio_player_play(FILE*f){if(!play_result){state=AUDIO_PLAYER_STATE_PLAYING;fclose(f);}return play_result;}
file_iterator_instance_t *ls_media_playlist_open(const char*root){list.count=1;list.list=names;return &list;}
void ls_media_playlist_close(file_iterator_instance_t*p){(void)p;}
size_t file_iterator_get_count(file_iterator_instance_t*p){return p->count;}
const char*file_iterator_get_name_from_index(file_iterator_instance_t*p,size_t i){return p->list[i];}
static void reset(void){delete_result=0;ls_music_close();ls_music_set_warm(false);volume=35;acquire_result=new_result=play_result=0;leases=releases=created=closed=parks=0;muted=missing=false;}
LS_CASE(acquisition_failure_never_starts_decoder){reset();acquire_result=ESP_FAIL;LS_CHECK(!ls_music_open());LS_EQ_INT(created,0);LS_EQ_INT(releases,0);LS_CHECK(*ls_music_error());reset();}
LS_CASE(decoder_allocation_failure_returns_lease){reset();new_result=ESP_ERR_NO_MEM;LS_CHECK(!ls_music_open());LS_EQ_INT(releases,1);reset();}
LS_CASE(close_timeout_retains_lease_until_decoder_stops){reset();LS_CHECK(ls_music_open());delete_result=ESP_FAIL;ls_music_close();LS_EQ_INT(releases,0);LS_CHECK(ls_music_open());LS_EQ_INT(created,1);delete_result=0;ls_music_close();LS_EQ_INT(releases,1);reset();}
LS_CASE(play_failure_closes_file_pause_resume_and_rescan){
    reset();LS_CHECK(ls_music_open());LS_CHECK(ls_music_scan(0));
    LS_CHECK(!ls_music_play(-1));missing=true;LS_CHECK(!ls_music_play(0));LS_EQ_INT(closed,0);missing=false;
    play_result=ESP_FAIL;LS_CHECK(!ls_music_play(0));LS_EQ_INT(closed,1);play_result=0;
    LS_CHECK(ls_music_play(0));LS_EQ_INT(ls_music_state(),LS_MUSIC_PLAYING);
    LS_CHECK(ls_music_toggle());LS_EQ_INT(ls_music_state(),LS_MUSIC_PAUSED);
    LS_CHECK(ls_music_toggle());LS_EQ_INT(ls_music_state(),LS_MUSIC_PLAYING);
    audio_player_cb_ctx_t e={.audio_event=AUDIO_PLAYER_CALLBACK_EVENT_UNKNOWN_FILE_TYPE};callback(&e);LS_CHECK(*ls_music_error());
    LS_CHECK(ls_music_scan(1));LS_CHECK(!*ls_music_error());LS_EQ_INT(ls_music_state(),LS_MUSIC_STOPPED);reset();
}
LS_CASE(decoder_unmute_respects_global_mute){reset();LS_CHECK(ls_music_open());muted=true;config.mute_fn(AUDIO_PLAYER_UNMUTE);LS_CHECK(hw_muted);muted=false;config.mute_fn(AUDIO_PLAYER_UNMUTE);LS_CHECK(!hw_muted);reset();}
LS_CASE(output_peaks_capture_transients_and_mono_keeps_both_channels){
    reset();volume=100;LS_CHECK(ls_music_open());state=AUDIO_PLAYER_STATE_PLAYING;
    int16_t pcm[]={-32768,-32768,0,24000,-20000,-20000};size_t written=0;
    LS_EQ_INT(config.write_fn(pcm,sizeof(pcm),&written,100),ESP_OK);
    LS_EQ_INT(written,sizeof(pcm));
    LS_EQ_INT(pcm[0],-32768);LS_EQ_INT(pcm[1],-32768);
    LS_EQ_INT(pcm[2],12000);LS_EQ_INT(pcm[3],12000);
    LS_EQ_INT(pcm[4],-20000);LS_EQ_INT(pcm[5],-20000);
    LS_EQ_INT(ls_music_peak(0),32768);LS_EQ_INT(ls_music_peak(1),32768);
    LS_EQ_INT(ls_music_peak(0),0);LS_EQ_INT(ls_music_peak(2),0);
    config.write_fn(pcm,sizeof(pcm),&written,100);state=AUDIO_PLAYER_STATE_PAUSE;
    LS_EQ_INT(ls_music_peak(0),0);reset();
}
LS_CASE(meter_reads_program_level_independent_of_volume_and_mute){
    reset();LS_CHECK(ls_music_open());state=AUDIO_PLAYER_STATE_PLAYING;
    int16_t pcm[]={30000,-12000};size_t written;
    volume=20;config.write_fn(pcm,sizeof(pcm),&written,100);
    LS_EQ_INT(ls_music_peak(0),30000);LS_EQ_INT(ls_music_peak(1),12000);
    pcm[0]=30000;pcm[1]=-12000;volume=0;muted=true;config.write_fn(pcm,sizeof(pcm),&written,100);
    LS_EQ_INT(ls_music_peak(0),30000);muted=false;
    state=AUDIO_PLAYER_STATE_PAUSE;pcm[0]=30000;config.write_fn(pcm,sizeof(pcm),&written,100);
    LS_EQ_INT(ls_music_peak(0),0);reset();
}
LS_CASE(meters_stay_stereo_before_mono_downmix){
    reset();LS_CHECK(ls_music_open());state=AUDIO_PLAYER_STATE_PLAYING;
    int16_t pcm[]={20000,0,-16000,2000};size_t written;
    config.write_fn(pcm,sizeof(pcm),&written,100);
    LS_EQ_INT(pcm[0],10000);LS_EQ_INT(pcm[1],10000);
    LS_EQ_INT(ls_music_peak(0),20000);LS_EQ_INT(ls_music_peak(1),2000);reset();
}
LS_CASE(volume_db_follows_codec_curve){
    reset();volume=100;LS_CHECK(ls_music_volume_db()==0.0f);
    volume=20;LS_CHECK(ls_music_volume_db()==-40.0f);
    volume=0;LS_CHECK(ls_music_volume_db()==-50.0f);reset();
}

/* Synthetic PCM through the real write path; nothing is played. */
enum { BANDS=13 };
static const float LO=63.0f,HI=16000.0f;
static void feed_sine(unsigned rate,double hz,double amplitude,double hz2,double amplitude2,unsigned frames){
    config.clk_set_fn(rate,16,I2S_SLOT_MODE_STEREO);
    int16_t pcm[256];size_t written;
    for(unsigned done=0;done<frames;done+=128) {
        for(unsigned j=0;j<128;j++) {
            double t=(double)(done+j)/rate;
            double v=amplitude*sin(6.28318530718*hz*t)+amplitude2*sin(6.28318530718*hz2*t);
            pcm[2*j]=pcm[2*j+1]=(int16_t)v;
        }
        config.write_fn(pcm,sizeof(pcm),&written,100);
    }
}
static int loudest(const float *db,int n){int best=0;for(int i=1;i<n;i++)if(db[i]>db[best])best=i;return best;}
LS_CASE(spectrum_places_tones_in_their_bands_at_true_level){
    reset();LS_CHECK(ls_music_open());state=AUDIO_PLAYER_STATE_PLAYING;
    float db[BANDS];
    /* Band 6 is 1 kHz and band 2 is 160 Hz on the 2/3-octave series. */
    feed_sine(44100,1000,16384,0,0,4096);
    LS_EQ_INT(ls_music_spectrum(LO,HI,BANDS,db),BANDS);
    LS_EQ_INT(loudest(db,BANDS),6);
    printf("    1 kHz at -6 dBFS reads %.2f dB\n",db[6]);
    LS_CHECK(db[6]>-7.0f && db[6]<-5.0f);
    LS_CHECK(db[2]<-60.0f);LS_CHECK(db[12]<-60.0f);
    feed_sine(44100,160,8192,6300,4096,4096);
    ls_music_spectrum(LO,HI,BANDS,db);
    LS_CHECK(db[2]>-13.0f && db[2]<-11.0f);
    LS_CHECK(db[10]>-19.0f && db[10]<-17.0f);
    LS_CHECK(db[6]<-60.0f);
    reset();
}
LS_CASE(spectrum_does_not_measure_above_nyquist){
    reset();LS_CHECK(ls_music_open());state=AUDIO_PLAYER_STATE_PLAYING;
    float db[BANDS];
    feed_sine(16000,2500,16384,0,0,4096);
    int measured=ls_music_spectrum(LO,HI,BANDS,db);
    /* 4 kHz tops out at 5 kHz; the 6.3 kHz band would reach 8.01 kHz. */
    LS_EQ_INT(measured,10);
    LS_EQ_INT(loudest(db,measured),8);
    LS_CHECK(isinf(db[10]) && db[10]<0);LS_CHECK(isinf(db[12]) && db[12]<0);
    ls_music_format_t f;LS_CHECK(ls_music_format(&f));
    LS_EQ_INT(f.rate_hz,16000);LS_EQ_INT(f.bits,16);LS_EQ_INT(f.channels,2);LS_CHECK(f.mono_out);
    reset();
}
LS_CASE(spectrum_needs_a_full_window_and_playback){
    reset();LS_CHECK(ls_music_open());state=AUDIO_PLAYER_STATE_PLAYING;
    float db[BANDS];
    feed_sine(44100,1000,16384,0,0,1024);
    LS_EQ_INT(ls_music_spectrum(LO,HI,BANDS,db),0);
    feed_sine(44100,1000,16384,0,0,1024);
    LS_EQ_INT(ls_music_spectrum(LO,HI,BANDS,db),BANDS);
    /* A new sample rate discards the old window. */
    feed_sine(48000,1000,16384,0,0,512);
    LS_EQ_INT(ls_music_spectrum(LO,HI,BANDS,db),0);
    feed_sine(48000,1000,16384,0,0,2048);
    state=AUDIO_PLAYER_STATE_PAUSE;LS_EQ_INT(ls_music_spectrum(LO,HI,BANDS,db),0);
    state=AUDIO_PLAYER_STATE_PLAYING;LS_EQ_INT(ls_music_spectrum(LO,HI,BANDS,db),BANDS);
    LS_EQ_INT(ls_music_spectrum(LO,HI,1,db),0);LS_EQ_INT(ls_music_spectrum(LO,HI,BANDS,NULL),0);
    ls_music_close();LS_EQ_INT(ls_music_spectrum(LO,HI,BANDS,db),0);
    reset();
}
LS_CASE(silence_reads_as_floor){
    reset();LS_CHECK(ls_music_open());state=AUDIO_PLAYER_STATE_PLAYING;
    float db[BANDS];feed_sine(44100,1000,0,0,0,4096);
    LS_EQ_INT(ls_music_spectrum(LO,HI,BANDS,db),BANDS);
    for(int i=0;i<BANDS;i++)LS_CHECK(db[i]<=-100.0f);
    reset();
}
static double tone_power(unsigned frequency,bool warm_tone){
    reset();ls_music_set_warm(warm_tone);ls_music_open();config.clk_set_fn(44100,16,I2S_SLOT_MODE_STEREO);
    double energy=0;size_t written;int16_t pcm[128];
    for(unsigned block=0;block<128;block++) {
        for(unsigned j=0;j<64;j++)pcm[2*j]=pcm[2*j+1]=(int16_t)(28000*sin(6.28318530718*frequency*(block*64+j)/44100));
        config.write_fn(pcm,sizeof(pcm),&written,100);
        if(block>64)for(int j=0;j<128;j++){LS_CHECK(pcm[j]>=-28000 && pcm[j]<=28000);energy+=(double)pcm[j]*pcm[j];}
    }
    return energy;
}
LS_CASE(warm_tilt_preserves_bass_without_boosting_peaks){
    double low=tone_power(80,true)/tone_power(80,false);
    double high=tone_power(5000,true)/tone_power(5000,false);
    LS_CHECK(low>.85 && low<=1.01);LS_CHECK(high>.40 && high<.55);reset();
}

/* The spectrogram reads every sample once, in order, with no gaps. */
enum { ROWS=20 };
static int loudest_row(const float *db){int best=0;for(int i=1;i<ROWS;i++)if(db[i]>db[best])best=i;return best;}
LS_CASE(spectrogram_columns_follow_the_audio_in_order){
    reset();LS_CHECK(ls_music_open());state=AUDIO_PLAYER_STATE_PLAYING;
    ls_music_cursor_t cur={0};float db[ROWS];
    config.clk_set_fn(44100,16,I2S_SLOT_MODE_STEREO);
    LS_CHECK(!ls_music_spectrogram(&cur,1,ROWS,db));
    /* 1.1 kHz slice rows at 44.1 kHz / 20 rows: 1 kHz is row 0, 5 kHz row 4,
       12 kHz row 10. One window of each, back to back. */
    feed_sine(44100,5000,16384,0,0,2048);
    feed_sine(44100,12000,16384,0,0,2048);
    feed_sine(44100,1500,16384,0,0,2048);
    LS_CHECK(ls_music_spectrogram(&cur,1,ROWS,db));LS_EQ_INT(loudest_row(db),4);
    printf("    5 kHz column peak %.2f dB\n",db[4]);
    LS_CHECK(db[4]>-7.5f && db[4]<-4.5f);
    LS_CHECK(ls_music_spectrogram(&cur,1,ROWS,db));LS_EQ_INT(loudest_row(db),10);
    LS_CHECK(ls_music_spectrogram(&cur,1,ROWS,db));LS_EQ_INT(loudest_row(db),1);
    LS_CHECK(!ls_music_spectrogram(&cur,1,ROWS,db));
    /* Two hops per column wait for both windows. */
    feed_sine(44100,5000,16384,0,0,2048);
    LS_CHECK(!ls_music_spectrogram(&cur,2,ROWS,db));
    feed_sine(44100,5000,16384,0,0,2048);
    LS_CHECK(ls_music_spectrogram(&cur,2,ROWS,db));LS_EQ_INT(loudest_row(db),4);
    reset();
}
LS_CASE(spectrogram_skips_ahead_when_lapped_and_restarts_on_rate_change){
    reset();LS_CHECK(ls_music_open());state=AUDIO_PLAYER_STATE_PLAYING;
    ls_music_cursor_t cur={0};float db[ROWS];
    feed_sine(44100,5000,16384,0,0,2048);
    LS_CHECK(ls_music_spectrogram(&cur,1,ROWS,db));
    /* Far more than the ring holds: the reader resumes at the newest column. */
    feed_sine(44100,5000,16384,0,0,8*2048*3);
    feed_sine(44100,12000,16384,0,0,2048);
    LS_CHECK(ls_music_spectrogram(&cur,1,ROWS,db));LS_EQ_INT(loudest_row(db),10);
    LS_CHECK(!ls_music_spectrogram(&cur,1,ROWS,db));
    /* 16 kHz: 400 Hz slices, so 2.5 kHz is row 6. */
    feed_sine(16000,2500,16384,0,0,2048);
    LS_CHECK(ls_music_spectrogram(&cur,1,ROWS,db));LS_EQ_INT(loudest_row(db),6);
    LS_CHECK(!ls_music_spectrogram(NULL,1,ROWS,db));LS_CHECK(!ls_music_spectrogram(&cur,5,ROWS,db));
    reset();
}
LS_CASE(narrow_bands_interpolate_instead_of_repeating_a_bin){
    reset();LS_CHECK(ls_music_open());state=AUDIO_PLAYER_STATE_PLAYING;
    float db[64];
    feed_sine(44100,1000,16384,0,0,4096);
    int n=ls_music_spectrum(40,16000,64,db);LS_EQ_INT(n,64);
    int repeats=0;for(int i=1;i<16;i++)if(db[i]==db[i-1])repeats++;
    LS_EQ_INT(repeats,0);
    reset();
}

/* The microphone: a synthetic tone arrives through the codec read. */
static void mic_feed(int blocks){for(int i=0;i<blocks;i++)LS_CHECK(ls_music_mic_step());}
LS_CASE(microphone_feeds_meters_and_spectrum_at_its_own_rate){
    reset();LS_CHECK(ls_music_open());
    mic_hz=3150;mic_amp=8000;
    LS_CHECK(ls_music_mic_start(false));
    LS_EQ_INT(ls_music_mic_state(),LS_MIC_LIVE);LS_CHECK(ls_music_live());
    LS_CHECK(mic_gain_db>=24.0f);
    ls_music_format_t f;LS_CHECK(ls_music_format(&f));
    LS_EQ_INT(f.rate_hz,44100);LS_EQ_INT(f.channels,1);
    mic_feed(4);
    unsigned peak=ls_music_peak(0);LS_CHECK(peak>7700 && peak<=8000);
    float db[BANDS];LS_EQ_INT(ls_music_spectrum(LO,HI,BANDS,db),BANDS);
    LS_EQ_INT(loudest(db,BANDS),8);
    ls_music_mic_stop();LS_EQ_INT(ls_music_mic_state(),LS_MIC_OFF);LS_CHECK(!ls_music_live());
    mic_feed(1);LS_EQ_INT(ls_music_peak(0),0);
    LS_EQ_INT(ls_music_spectrum(LO,HI,BANDS,db),0);
    reset();
}
LS_CASE(recording_writes_a_playable_mono_wav){
    reset();LS_CHECK(ls_music_open());mic_hz=1000;mic_amp=12000;
    LS_CHECK(ls_music_mic_start(true));
    LS_EQ_INT(ls_music_mic_state(),LS_MIC_RECORDING);
    LS_CHECK(strstr(ls_music_mic_file(),"/sdcard/music/REC-001.wav")!=NULL);
    mic_feed(3);
    LS_EQ_INT(ls_music_mic_ms(),69);
    capture_header=true;ls_music_mic_stop();capture_header=false;
    LS_CHECK(!memcmp(header,"RIFF",4));LS_CHECK(!memcmp(header+8,"WAVEfmt ",8));
    LS_EQ_INT(header[22],1);                                  /* mono */
    LS_EQ_INT(header[24]|header[25]<<8|header[26]<<16,44100);
    LS_EQ_INT(header[34],16);
    unsigned data=header[40]|header[41]<<8|header[42]<<16|(unsigned)header[43]<<24;
    LS_EQ_INT(data,3*1024*2);
    unsigned riff=header[4]|header[5]<<8|header[6]<<16|(unsigned)header[7]<<24;
    LS_EQ_INT(riff,36+data);
    reset();
}
LS_CASE(playing_a_track_stops_the_microphone_and_failures_say_why){
    reset();LS_CHECK(ls_music_open());LS_CHECK(ls_music_scan(0));
    LS_CHECK(ls_music_mic_start(true));
    capture_header=true;LS_CHECK(ls_music_play(0));capture_header=false;
    LS_EQ_INT(ls_music_mic_state(),LS_MIC_OFF);LS_CHECK(!memcmp(header,"RIFF",4));
    mic_fail=true;LS_CHECK(ls_music_mic_start(false));LS_CHECK(!ls_music_mic_step());mic_fail=false;
    ls_music_mic_stop();
    has_mic=false;LS_CHECK(!ls_music_mic_start(false));LS_CHECK(*ls_music_error());has_mic=true;
    ls_music_close();LS_CHECK(!ls_music_mic_start(false));
    reset();
}
LS_CASE(mono_files_analyse_at_their_true_rate){
    reset();LS_CHECK(ls_music_open());state=AUDIO_PLAYER_STATE_PLAYING;
    config.clk_set_fn(16000,16,I2S_SLOT_MODE_MONO);
    int16_t pcm[256];size_t written;
    for(unsigned done=0;done<4096;done+=256) {
        for(unsigned j=0;j<256;j++)pcm[j]=(int16_t)(16384*sin(6.28318530718*1600*(done+j)/16000));
        LS_EQ_INT(config.write_fn(pcm,sizeof(pcm),&written,100),ESP_OK);
    }
    float db[BANDS];LS_CHECK(ls_music_spectrum(LO,HI,BANDS,db)>0);
    LS_EQ_INT(loudest(db,10),7);
    LS_EQ_INT(ls_music_peak(0),ls_music_peak(1));
    reset();
}
