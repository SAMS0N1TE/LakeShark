"""Exercise the real player shutdown against task-state and timeout fakes."""
import pathlib
import shutil
import subprocess
import tempfile
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[2]


class PlayerShutdownTest(unittest.TestCase):
    def test_shutdown_ownership(self):
        compiler = shutil.which('g++')
        if not compiler:
            self.skipTest('Host C++ compiler required')
        source = (ROOT / 'components/chmorgan__esp-audio-player/audio_player.cpp').read_text()
        start = source.index('esp_err_t audio_player_delete()')
        opening = source.index('{', start)
        depth = 0
        for end in range(opening, len(source)):
            depth += (source[end] == '{') - (source[end] == '}')
            if depth == 0:
                break
        body = source[start:end + 1]
        harness = r'''
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstddef>
using esp_err_t=int;
constexpr int ESP_OK=0,ESP_FAIL=-1,eSuspended=1,eRunning=2;
struct Instance { std::atomic<bool> running; void *task_handle; bool resources; } instance;
static int state,delays,deletes,cleanups,suspend_after;
#define pdMS_TO_TICKS(n) (n)
#define CHECK(x) do {if(!(x)){fprintf(stderr,"line%d: %s\n",__LINE__,#x);return 2;}}while(0)
static int eTaskGetState(void *h){if(h!=(void*)1)abort();return state;}
static void audio_player_stop(){}
static void _internal_audio_player_shutdown_thread(){}
static void vTaskDelay(int ticks){if(ticks!=100)abort();++delays;if(suspend_after && delays>=suspend_after){instance.running=false;state=eSuspended;}}
static void vTaskDeleteWithCaps(void *h){if(h!=(void*)1 || state!=eSuspended || instance.running || !instance.resources)abort();++deletes;}
static void cleanup_memory(Instance &i){if(i.task_handle)abort();i.resources=false;++cleanups;}
''' + body + r'''
int main(){
 instance.running=true;instance.task_handle=(void*)1;instance.resources=true;state=eRunning;
 CHECK(audio_player_delete()==ESP_FAIL);
 CHECK(delays==5 && deletes==0 && cleanups==0 && instance.resources && instance.task_handle);
 /* running=false can become visible before worker suspension. */
 instance.running=false;delays=0;
 CHECK(audio_player_delete()==ESP_FAIL);
 CHECK(delays==5 && deletes==0 && cleanups==0 && instance.resources);
 delays=0;suspend_after=2;
 CHECK(audio_player_delete()==ESP_OK);
 CHECK(delays==2 && deletes==1 && cleanups==1 && !instance.task_handle && !instance.resources);
 CHECK(audio_player_delete()==ESP_OK);CHECK(deletes==1);
 /* A second instance can close; owner never deletes a NULL/unrelated handle. */
 instance.running=true;instance.task_handle=(void*)1;instance.resources=true;
 state=eRunning;delays=0;suspend_after=1;
 CHECK(audio_player_delete()==ESP_OK);CHECK(deletes==2 && !instance.task_handle);
 puts("active timeout, suspension race, retry, owner deletion and repeat close passed");
}
'''
        with tempfile.TemporaryDirectory() as d:
            src = pathlib.Path(d) / 'shutdown.cpp'
            exe = pathlib.Path(d) / 'shutdown.exe'
            src.write_text(harness)
            subprocess.run([compiler, '-std=c++17', str(src), '-o', str(exe)], check=True)
            subprocess.run([str(exe)], check=True)

    def test_music_timeout_retains_worker_spectrum_and_lease(self):
        source = (ROOT / 'components/apps/tui/ls_music_backend.c').read_text()
        def body(signature):
            start = source.index(signature)
            opening = source.index('{', start)
            depth = 0
            for end in range(opening, len(source)):
                depth += (source[end] == '{') - (source[end] == '}')
                if depth == 0:
                    return source[start:end + 1]
        harness = r"""
#include <atomic>
#include <cstdio>
#include <cstdlib>
#define ESP_PLATFORM 1
#define pdMS_TO_TICKS(n) (n)
#define atomic_store(p,v) (p)->store(v)
#define atomic_load(p) (p)->load()
using esp_err_t=int;
constexpr int ESP_OK=0;
static std::atomic<bool> s_mic_run{true},s_mic_busy{true};
static void *s_mic_task=(void*)1,*s_mic_stack=(void*)2,*s_mic_tcb=(void*)3,*s_mic_buf=(void*)4;
static bool s_up=true;
static void *s_list=(void*)5;
static const char *s_error;
static int delays,finish_after,closes,frees,deletes,decoder_deletes,releases,spectra,lists;
static void mic_close_file(){++closes;}
static void vTaskDelay(int){if(++delays==finish_after){mic_close_file();s_mic_busy=false;}}
static void vTaskDelete(void*){if(s_mic_busy)abort();++deletes;}
static void heap_caps_free(void *p){if(p)++frees;}
static int audio_player_delete(){++decoder_deletes;return 0;}
static void audio_out_media_release(){++releases;}
static void spectrum_close(){++spectra;}
static void ls_media_playlist_close(void*){++lists;}
static void music_heap_checkpoint(const char*,int){}
""" + body('void ls_music_mic_stop(void)') + body('static void mic_release(void)') + body('void ls_music_close(void)') + r"""
#define CHECK(x) do{if(!(x)){fprintf(stderr,"line%d: %s\n",__LINE__,#x);return 2;}}while(0)
int main(){
 ls_music_close();
 CHECK(!s_mic_run && s_mic_busy && closes==0 && frees==0 && deletes==0);
 CHECK(s_up && releases==0 && spectra==0 && decoder_deletes==0 && lists==0);
 delays=0;finish_after=2;ls_music_close();
 CHECK(!s_mic_busy && !s_mic_task && !s_mic_buf && frees==3 && deletes==1);
 CHECK(!s_up && releases==1 && spectra==1 && decoder_deletes==1 && lists==1);
 puts("microphone timeout retains file, buffers, spectrum and lease until worker completion");
}
"""
        with tempfile.TemporaryDirectory() as directory:
            work = pathlib.Path(directory)
            (work / 'mic.cpp').write_text(harness)
            subprocess.run(['C:/mingw64/bin/g++.exe', '-std=c++17', str(work / 'mic.cpp'), '-o', str(work / 'mic.exe')], check=True)
            subprocess.run([str(work / 'mic.exe')], check=True)

    def test_stop_barrier_requires_matching_completion(self):
        source = (ROOT / 'components/chmorgan__esp-audio-player/audio_player.cpp').read_text()
        start = source.index('esp_err_t audio_player_stop_wait(')
        end = source.index('\n/**', start)
        body = source[start:end]
        harness = r"""
#include <atomic>
#include <cstdint>
#include <cstdio>
using esp_err_t=int;
constexpr int ESP_OK=0,ESP_FAIL=-1,AUDIO_PLAYER_REQUEST_STOP=1;
#define pdMS_TO_TICKS(n) (n)
struct audio_player_event_t{int type;void *fp;uint32_t seek_ms;};
static int instance,send_result,delays,complete_after;
static std::atomic<uint32_t> stop_serial,stop_done;
static audio_player_event_t queued;
static int audio_send_event(int*,audio_player_event_t e){queued=e;return send_result;}
static void vTaskDelay(int){if(++delays==complete_after)stop_done=queued.seek_ms;}
""" + body + r"""
#define CHECK(x) do{if(!(x)){fprintf(stderr,"line%d: %s\n",__LINE__,#x);return 2;}}while(0)
int main(){
 complete_after=3;CHECK(audio_player_stop_wait(50)==ESP_OK && delays==3);
 delays=0;complete_after=0;CHECK(audio_player_stop_wait(10)==ESP_FAIL && delays==2);
 stop_done=queued.seek_ms;delays=0;complete_after=1;
 CHECK(audio_player_stop_wait(50)==ESP_OK && delays==1);
 send_result=-3;delays=0;CHECK(audio_player_stop_wait(50)==-3 && delays==0);
 puts("STOP barrier checks completion serial, stalled output timeout and queue failure");
}
"""
        with tempfile.TemporaryDirectory() as directory:
            work = pathlib.Path(directory)
            (work / 'stop.cpp').write_text(harness)
            subprocess.run(['C:/mingw64/bin/g++.exe', '-std=c++17', str(work / 'stop.cpp'), '-o', str(work / 'stop.exe')], check=True)
            subprocess.run([str(work / 'stop.exe')], check=True)


if __name__ == '__main__':
    unittest.main()
