"""Exercise the real WAV playback worker's queued seek paths without audio hardware."""
import pathlib
import subprocess
import tempfile
import unittest
ROOT = pathlib.Path(__file__).resolve().parents[2]

class PlayerSeekTest(unittest.TestCase):
    def test_playing_and_paused_seek(self):
        source = (ROOT / "components/chmorgan__esp-audio-player/audio_player.cpp").read_text()
        body = source[source.index("static void seek_file("):source.index("static void audio_task(")]
        harness = r'''
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>
#include "audio_wav.h"
#define CONFIG_AUDIO_PLAYER_ENABLE_WAV 1
#undef LOGI_1
#undef LOGI_2
#undef LOGI_3
#undef ESP_LOGE
#define LOGI_1(...)
#define LOGI_2(...)
#define LOGI_3(...)
#define ESP_LOGE(...)
#define ESP_GOTO_ON_ERROR(ret,label,...) if(ret)goto label
using esp_err_t=int;using i2s_slot_mode_t=int;
constexpr int ESP_OK=0,pdPASS=1,portMAX_DELAY=-1,I2S_SLOT_MODE_MONO=1,I2S_SLOT_MODE_STEREO=2;
enum {AUDIO_PLAYER_REQUEST_NONE,AUDIO_PLAYER_REQUEST_PAUSE,AUDIO_PLAYER_REQUEST_RESUME,AUDIO_PLAYER_REQUEST_PLAY,AUDIO_PLAYER_REQUEST_STOP,AUDIO_PLAYER_REQUEST_SEEK};
enum {AUDIO_PLAYER_STATE_PLAYING,AUDIO_PLAYER_STATE_PAUSE};
enum {FILE_TYPE_UNKNOWN,FILE_TYPE_WAV,AUDIO_PLAYER_CALLBACK_EVENT_UNKNOWN_FILE_TYPE};
using FILE_TYPE=int;
struct audio_player_event_t {int type;FILE *fp;uint32_t seek_ms;};
static std::vector<audio_player_event_t> events;
static std::atomic<uint32_t> position_ms,duration_ms;
static std::atomic<bool> seekable;
static int writes,mode,state,second_sample;
struct audio_instance_t {decode_data output;wav_instance wav_data;void *event_queue;struct {int (*clk_set_fn)(int,unsigned,int);int (*write_fn)(void*,size_t,size_t*,int);}config;};
static void set_state(audio_instance_t*,int s){state=s;}
static void dispatch_callback(audio_instance_t*,int){abort();}
static int mono_to_stereo(unsigned,decode_data&){abort();}
static int xQueuePeek(void*,audio_player_event_t *e,int wait){if(events.empty()){if(wait)abort();return 0;}*e=events.front();return 1;}
static int xQueueReceive(void*,audio_player_event_t *e,int){*e=events.front();events.erase(events.begin());if(e->type==AUDIO_PLAYER_REQUEST_RESUME && (state!=AUDIO_PLAYER_STATE_PAUSE || position_ms!=2000))abort();return 1;}
static int clock_set(int,unsigned,int){return 0;}
static int write_audio(void *pcm,size_t n,size_t *written,int){
 ++writes;*written=n;
 if(writes==1){
  if(mode==1){events.push_back({AUDIO_PLAYER_REQUEST_PAUSE,nullptr,0});events.push_back({AUDIO_PLAYER_REQUEST_SEEK,nullptr,2000});events.push_back({AUDIO_PLAYER_REQUEST_RESUME,nullptr,0});}
  else events.push_back({AUDIO_PLAYER_REQUEST_SEEK,nullptr,0});
 }else {second_sample=*(int16_t*)pcm;events.push_back({AUDIO_PLAYER_REQUEST_STOP,nullptr,0});}
 return 0;
}
''' + body + r'''
static void u32(unsigned char *p,unsigned n){for(int i=0;i<4;i++)p[i]=n>>(i*8);}
int main(){
 for(mode=1;mode<=2;mode++){
  FILE *f=fopen("seek.wav","w+b");if(!f)return 2;
  unsigned char h[44]={};memcpy(h,"RIFF",4);u32(h+4,96036);memcpy(h+8,"WAVEfmt ",8);u32(h+16,16);h[20]=1;h[22]=2;u32(h+24,8000);u32(h+28,32000);h[32]=4;h[34]=16;memcpy(h+36,"data",4);u32(h+40,96000);fwrite(h,1,44,f);
  for(int frame=0;frame<24000;frame++){int16_t p[2]={(int16_t)((frame/8000+1)*100),0};fwrite(p,2,2,f);}rewind(f);
  unsigned char pcm[32000];audio_instance_t i={};i.output.samples=pcm;i.output.samples_capacity=sizeof(pcm);i.config.clk_set_fn=clock_set;i.config.write_fn=write_audio;
  writes=0;events.clear();if(aplay_file(&i,f)!=0 || writes!=2 || second_sample!=(mode==1?300:100) || position_ms!=(mode==1?3000:1000) || seekable)return 3;
  fclose(f);remove("seek.wav");
 }
 puts("paused forward seek, preserved pause, playing backward seek and position passed");
}
'''
        with tempfile.TemporaryDirectory() as directory:
            work = pathlib.Path(directory)
            (work / "seek.cpp").write_text(harness)
            audio = ROOT / "components/chmorgan__esp-audio-player"
            subprocess.run(["C:/mingw64/bin/g++.exe", "-std=c++17", str(work / "seek.cpp"),
                            str(audio / "audio_wav.cpp"), "-I", str(audio), "-I", str(ROOT / "bench/shims"),
                            "-o", str(work / "seek.exe")], check=True)
            subprocess.run([str(work / "seek.exe")], cwd=work, check=True)

if __name__ == "__main__": unittest.main()
