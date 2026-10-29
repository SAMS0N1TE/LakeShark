#include <atomic>
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>
#include "audio_wav.h"
#include "codecs/ls_decode.h"
#define CONFIG_AUDIO_PLAYER_ENABLE_STREAMING 1
#define CONFIG_AUDIO_PLAYER_ENABLE_WAV 1
#undef LOGI_1
#undef LOGI_2
#undef LOGI_3
#define LOGI_1(...)
#define LOGI_2(...)
#define LOGI_3(...)
#define ESP_LOGE(...)
#define ESP_GOTO_ON_ERROR(ret,label,...) if(ret)goto label
using esp_err_t=int;using i2s_slot_mode_t=int;
constexpr int ESP_OK=0,ESP_ERR_NO_MEM=-1,ESP_ERR_INVALID_ARG=-2,pdPASS=1,portMAX_DELAY=-1,I2S_SLOT_MODE_MONO=1,I2S_SLOT_MODE_STEREO=2;
enum {AUDIO_PLAYER_REQUEST_NONE,AUDIO_PLAYER_REQUEST_PAUSE,AUDIO_PLAYER_REQUEST_RESUME,AUDIO_PLAYER_REQUEST_PLAY,AUDIO_PLAYER_REQUEST_STOP,AUDIO_PLAYER_REQUEST_SEEK};
enum {AUDIO_PLAYER_STATE_PLAYING,AUDIO_PLAYER_STATE_PAUSE};
enum {FILE_TYPE_UNKNOWN,FILE_TYPE_WAV,FILE_TYPE_STREAM,AUDIO_PLAYER_CALLBACK_EVENT_UNKNOWN_FILE_TYPE};
using FILE_TYPE=int;
struct audio_player_event_t {int type;FILE *fp;uint32_t seek_ms;};
static std::vector<audio_player_event_t> events;
static std::atomic<uint32_t> position_ms,duration_ms;
static std::atomic<bool> seekable;
static int writes,mode,state,output_rate,clock_changes,services,wraps;
static uint32_t previous_position;
struct audio_instance_t {
    decode_data output;wav_instance wav_data;ls_decode *stream;void *event_queue;
    struct {int (*clk_set_fn)(uint32_t,uint32_t,int);int (*write_fn)(void*,size_t,size_t*,uint32_t);
        void (*source_format_fn)(uint32_t,uint32_t,unsigned);void (*service_fn)(void);}config;
};
static void set_state(audio_instance_t*,int s){state=s;}
static void dispatch_callback(audio_instance_t*,int){abort();}
static int xQueuePeek(void*,audio_player_event_t *e,int wait){if(events.empty()){assert(!wait);return 0;}*e=events.front();return 1;}
static int xQueueReceive(void*,audio_player_event_t *e,int){
    assert(!events.empty());*e=events.front();events.erase(events.begin());
    if(e->type==AUDIO_PLAYER_REQUEST_RESUME)assert(state==AUDIO_PLAYER_STATE_PAUSE && position_ms==1000);
    return 1;
}
static int clock_set(uint32_t rate,uint32_t bits,int slots){assert(bits==16 && slots==2);output_rate=rate;clock_changes++;return 0;}
static void source_format(uint32_t rate,uint32_t bits,unsigned channels){assert(rate==44100 && bits==16 && channels==2);}
static void service(void){services++;}
static int write_audio(void*,size_t n,size_t *written,uint32_t);
#include "music_worker.inc"
static int write_audio(void*,size_t n,size_t *written,uint32_t){
    writes++;*written=n;
    if(mode==1 && writes==1)events.push_back({AUDIO_PLAYER_REQUEST_SEEK,nullptr,1000});
    if(mode==2 && writes==1) {
        events.push_back({AUDIO_PLAYER_REQUEST_PAUSE,nullptr,0});
        events.push_back({AUDIO_PLAYER_REQUEST_SEEK,nullptr,1000});
        events.push_back({AUDIO_PLAYER_REQUEST_RESUME,nullptr,0});
    }
    if(mode==3) {
        assert(position_ms>=250 && position_ms<=300);
        if(position_ms<previous_position)wraps++;
        previous_position=position_ms;
    }
    if(mode==4 && writes==1){assert(output_rate==22050);assert(audio_player_speed(2000)==0);}
    if(writes==(mode==3?8:2))events.push_back({AUDIO_PLAYER_REQUEST_STOP,nullptr,0});
    return 0;
}
int main(int argc,char **argv){
    assert(argc==2);
    for(const char *ext:{"wav","mp3","flac","ogg"})for(mode=1;mode<=4;mode++) {
        char path[512];snprintf(path,sizeof(path),"%s/tone.%s",argv[1],ext);
        FILE *f=fopen(path,"rb");assert(f);std::vector<uint8_t> pcm(4608);
        audio_instance_t i={};i.output.samples=pcm.data();i.output.samples_capacity=4608;i.output.samples_capacity_max=4608;
        i.config={clock_set,write_audio,source_format,service};
        events.clear();writes=clock_changes=services=wraps=0;previous_position=250;
        audio_player_speed(mode==4?500:1000);audio_player_loop(250,300,mode==3);
        assert(aplay_file(&i,f)==0 && !seekable && !i.stream && services>=3);
        if(mode<3)assert(position_ms>1000 && position_ms<1100);
        if(mode==3)assert(wraps>=2 && writes==8);
        if(mode==4)assert(output_rate==88200 && clock_changes==2 && position_ms<100);
        fclose(f);
    }
    assert(audio_player_speed(499)!=0 && audio_player_speed(2001)!=0);
    assert(audio_player_loop(100,99,true)!=0);
    puts("PASS production worker: all four formats, playing/paused seek, sample-bounded A-B loop, 0.5x-to-2x varispeed");
}
