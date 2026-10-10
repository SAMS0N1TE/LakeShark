"""Run the production audio owner/reinit and startup bodies with resource fakes."""
import pathlib
import shutil
import subprocess
import tempfile
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[2]


def body(source, start):
    first = source.index(start)
    opening = source.index('{', first)
    depth = 0
    for end in range(opening, len(source)):
        depth += (source[end] == '{') - (source[end] == '}')
        if depth == 0:
            return source[first:end + 1]
    raise ValueError(start)


def run_c(source):
    with tempfile.TemporaryDirectory() as tmp:
        path = pathlib.Path(tmp)
        (path / 'test.c').write_text(source)
        subprocess.run([shutil.which('gcc'), '-std=c11', '-Wall', '-Wextra',
                        str(path / 'test.c'), '-o', str(path / 'test.exe')], check=True,
                       capture_output=True, text=True)
        result = subprocess.run([str(path / 'test.exe')], capture_output=True, text=True)
        if result.returncode:
            raise AssertionError(result.stdout + result.stderr)


class AudioReinitTest(unittest.TestCase):
    def test_owner_timeout_media_exclusion_and_selected_settings(self):
        out = (ROOT / 'components/lakeshark/audio/audio_out.c').read_text()
        hw = (ROOT / 'components/lakeshark/board/ls_audio_hw.c').read_text()
        command = body(out, 'esp_err_t audio_out_reinit(bool swap)')
        execute = body(out, 'if (__atomic_compare_exchange_n(&s_reinit_state, &pending, 2')
        rebuild = body(hw, 'esp_err_t ls_audio_hw_reinit(bool swap, int volume, bool muted)')
        harness = r'''
#include <stdbool.h>
#include <stdio.h>
typedef int esp_err_t;
enum {ESP_OK,ESP_ERR_TIMEOUT,ESP_ERR_INVALID_STATE,ESP_FAIL};
#define CHECK(x) do { if(!(x)){fprintf(stderr,"line %d: %s\n",__LINE__,#x);return 1;} }while(0)
#define pdMS_TO_TICKS(x) (x)
#define portENTER_CRITICAL(x) ((void)(x))
#define portEXIT_CRITICAL(x) ((void)(x))
#define LS_BOARD_CODEC_I2C_BUS 1
static unsigned s_reinit_state;
static int s_owner_lock, s_volume=17;
static bool s_ready=true,s_media_request,s_media_owned,s_reinit_swap,s_muted=true;
static bool s_volume_applied,s_reprime;
static esp_err_t s_reinit_result;
static int ls_audio_ws_gpio=3,ls_audio_dout_gpio=4;
static int cleans, inits, applied_volume, applied_mute, init_error;
static bool quiescent=true, execute_command=true;
static bool hw_lock(void){return quiescent;}
static void hw_unlock(void){}
static void settings_lock(void){}
static void settings_unlock(void){}
static void codec_cleanup(void){++cleans;}
static int hw_init(bool speaker){(void)speaker;++inits;return init_error;}
static int hw_volume(int v,int*a){*a=v;applied_volume=v;return ESP_OK;}
static int hw_mute(bool m){applied_mute=m;return ESP_OK;}
''' + rebuild + '\nstatic void vTaskDelay(int ticks){(void)ticks;if(execute_command){unsigned pending=1;' + execute + '}}\n' + command + r'''
int main(void){
 s_media_request=true;
 CHECK(audio_out_reinit(true)==ESP_ERR_INVALID_STATE);CHECK(!cleans && !inits);
 s_media_request=false;execute_command=false;
 CHECK(audio_out_reinit(true)==ESP_ERR_TIMEOUT);CHECK(s_reinit_state==0);
 CHECK(!cleans && !inits && ls_audio_ws_gpio==3);
 execute_command=true;quiescent=false;
 CHECK(audio_out_reinit(true)==ESP_ERR_TIMEOUT);
 CHECK(!cleans && !inits && ls_audio_ws_gpio==3 && ls_audio_dout_gpio==4);
 quiescent=true;
 CHECK(audio_out_reinit(true)==ESP_OK);
 CHECK(cleans==1 && inits==1 && applied_volume==17 && applied_mute==true);
 CHECK(s_volume==17 && s_muted==true && ls_audio_ws_gpio==4 && ls_audio_dout_gpio==3);
 s_volume=63;s_muted=false;
 CHECK(audio_out_reinit(false)==ESP_OK);
 CHECK(applied_volume==63 && applied_mute==false && s_volume==63 && !s_muted);
 init_error=ESP_FAIL;
 CHECK(audio_out_reinit(false)==ESP_FAIL);CHECK(s_volume==63 && !s_muted);
 return 0;
}
'''
        run_c(harness)

    def test_startup_allocation_failures_unwind_and_retry(self):
        source = (ROOT / 'components/lakeshark/audio/audio_out.c').read_text()
        init = body(source, 'esp_err_t audio_out_init(void)')
        harness = r'''
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
typedef int esp_err_t;typedef int BaseType_t;typedef void* StackType_t;
enum {ESP_OK,ESP_ERR_NO_MEM,ESP_FAIL,pdTRUE=1,pdPASS=1};
#define LS_HAS_AUDIO 1
#define LS_BOARD_CODEC_I2C_BUS 1
#define AUDIO_DIAG_TONE 0
#define ESP_LOGW(...) ((void)0)
#define ESP_LOGE(...) ((void)0)
#define ESP_LOGI(...) ((void)0)
#define AUDIO_RATE_HZ 16000
#define I2S_SLOT_MODE_STEREO 2
#define RING_BYTES 32000
#define AUDIO_TASK_STACK 4096
#define MALLOC_CAP_SPIRAM 1
#define MALLOC_CAP_8BIT 2
static bool s_ready,s_volume_applied,hardware;
static int s_volume=23,s_ring_ctrl;
static void *s_push_lock,*s_ring,*s_ring_buf,*s_task;
static int stage,resources;
static esp_err_t ls_audio_hw_init(bool only){(void)only;hardware=true;return ESP_OK;}
static void ls_audio_hw_deinit(void){hardware=false;}
static esp_err_t ls_audio_hw_set_fs(int a,int b,int c){(void)a;(void)b;(void)c;return ESP_OK;}
static esp_err_t ls_audio_hw_volume(int v,int*a){*a=v;return ESP_OK;}
static bool s_muted;
static bool settings_get_audio_mute(void){return true;}
static int ls_audio_hw_mute(bool m){s_muted=m;return ESP_OK;}
static int settings_get_volume(void){return 23;}
static void audio_eq_init(int rate){(void)rate;}
static void *xSemaphoreCreateMutex(void){if(stage==1)return NULL;++resources;return (void*)1;}
static void vSemaphoreDelete(void*p){if(p)--resources;}
static size_t audio_pcm_storage_bytes(size_t n){return n+1;}
static void *heap_caps_malloc(size_t n,int caps){(void)n;(void)caps;if(stage==2)return NULL;++resources;return (void*)2;}
static void heap_caps_free(void*p){if(p)--resources;}
static void *xStreamBufferCreateStatic(size_t n,int trigger,void*p,int*c){(void)n;(void)trigger;(void)p;(void)c;if(stage==3)return NULL;++resources;return (void*)3;}
static void vStreamBufferDelete(void*p){if(p)--resources;}
static void audio_player_task(void*p){(void)p;}
static BaseType_t xTaskCreatePinnedToCoreWithCaps(void(*fn)(void*),const char*n,int bytes,void*arg,int pri,void**task,int core,int caps){
 (void)fn;(void)n;(void)bytes;(void)arg;(void)pri;(void)core;(void)caps;
 if(stage==4)return 0;*task=(void*)4;return pdPASS;
}
static void snd_test_init(void){}
#define CHECK(x) do {if(!(x)){fprintf(stderr,"line %d: %s\n",__LINE__,#x);return 1;}}while(0)
''' + init + r'''
int main(void){
 for(stage=1;stage<=4;++stage){
  CHECK(audio_out_init()==ESP_ERR_NO_MEM);CHECK(!hardware && !resources && !s_ready);
  CHECK(!s_push_lock && !s_ring && !s_ring_buf && !s_task);
 }
 stage=0;CHECK(audio_out_init()==ESP_OK);CHECK(hardware && resources==3 && s_ready);
 CHECK(audio_out_init()==ESP_OK);CHECK(resources==3 && s_volume==23 && s_muted);
 return 0;
}
'''
        run_c(harness)


    def test_mute_persistence_explicit_commands_and_toggle(self):
        out = (ROOT / 'components/lakeshark/audio/audio_out.c').read_text()
        settings = (ROOT / 'components/lakeshark/core/settings.c').read_text()
        for console in ['main/headless_main.c', 'components/lakeshark/core/ls_ctl.c']:
            command = body((ROOT / console).read_text(), 'static int cmd_mute(')
            harness = r'''
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
static bool s_muted,s_audio_muted,s_reprime,s_ready=true,stored;
enum {ESP_OK=0};
static int s_nvs;
static int volume=23,writes,hardware;
static int nvs_get_u8(int h,const char*k,unsigned char*v){(void)h;if(strcmp(k,"audio_mute"))return 1;*v=stored;return ESP_OK;}
static void settings_lock(void){}
static void settings_unlock(void){}
static void ls_audio_hw_mute(bool m){hardware=m;}
static void sput_u8(const char*k,int v){if(strcmp(k,"audio_mute"))__builtin_abort();stored=v;++writes;}
''' + body(settings, 'bool settings_get_audio_mute(void)') + '\n' + body(settings, 'void settings_set_audio_mute(bool muted)') + '\n' + body(out, 'static void audio_mute_change(') + '\n' + body(out, 'void audio_toggle_mute(void)') + '\n' + body(out, 'void audio_mute_set(bool muted)') + '\n' + body(out, 'bool audio_is_muted(void)') + '\n' + command + '\nstatic void boot_load(void){' + settings[settings.index('    uint8_t audio_muted=0;'):settings.index('    /* settings_init runs on the cache-safe boot task')] + '}\n' + r'''
#define CHECK(x) do {if(!(x))return __LINE__;}while(0)
int main(void){
 char *on[]={"mute","on"},*off[]={"mute","off"},*bad[]={"mute","bad"};
 CHECK(!cmd_mute(2,on));CHECK(s_muted && stored && hardware && s_reprime && writes==1);
 CHECK(!cmd_mute(2,on));CHECK(writes==1);
 /* Boot cache restored from committed byte; audio init consumes cached value. */
 s_muted=false;s_audio_muted=false;boot_load();s_muted=settings_get_audio_mute();CHECK(s_muted);
 CHECK(!cmd_mute(2,off));CHECK(!s_muted && !stored && !hardware && writes==2);
 s_audio_muted=true;boot_load();CHECK(!settings_get_audio_mute());
 CHECK(cmd_mute(2,bad)==1);CHECK(writes==2);
 CHECK(!cmd_mute(1,on));CHECK(s_muted && stored && writes==3);
 s_ready=false;CHECK(!cmd_mute(2,off));CHECK(!stored && writes==4 && hardware);
 CHECK(volume==23);return 0;
}
'''
            run_c(harness)

if __name__ == '__main__':
    unittest.main()
