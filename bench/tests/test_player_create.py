"""Exercise the real player creation against allocation and task fakes."""
import pathlib
import shutil
import subprocess
import tempfile
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[2]


def function_body(source, signature):
    start = source.index(signature)
    opening = source.index('{', start)
    depth = 0
    for end in range(opening, len(source)):
        depth += (source[end] == '{') - (source[end] == '}')
        if depth == 0:
            return source[start:end + 1]
    raise ValueError(signature)


class PlayerCreateTest(unittest.TestCase):
    def test_every_failure_returns_what_it_took(self):
        compiler = shutil.which('g++')
        if not compiler:
            self.skipTest('Host C++ compiler required')
        source = (ROOT / 'components/chmorgan__esp-audio-player/audio_player.cpp').read_text()
        cleanup = function_body(source, 'static void cleanup_memory(audio_instance_t &i)')
        create = function_body(source, 'esp_err_t audio_player_new(audio_player_config_t config)')
        harness = r'''
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <cstddef>
using esp_err_t=int;
using BaseType_t=int;
using UBaseType_t=unsigned;
using TaskFunction_t=void(*)(void*);
constexpr int ESP_OK=0,ESP_ERR_NO_MEM=0x101,ESP_ERR_INVALID_STATE=0x103,pdPASS=1;
constexpr unsigned MALLOC_CAP_INTERNAL=1,MALLOC_CAP_DMA=2,MALLOC_CAP_8BIT=4;
static const char *TAG="player";
#define ESP_RETURN_ON_FALSE(a,err,tag,...) do{if(!(a))return err;}while(0)
#define ESP_GOTO_ON_FALSE(a,err,label,tag,...) do{if(!(a)){ret=err;goto label;}}while(0)
#define LOGI_1(...)
enum { AUDIO_PLAYER_MUTE=0, AUDIO_PLAYER_UNMUTE=1 };
typedef int AUDIO_PLAYER_MUTE_SETTING;
typedef struct { esp_err_t (*mute_fn)(AUDIO_PLAYER_MUTE_SETTING); int priority; int coreID; } audio_player_config_t;
typedef int audio_player_event_t;
struct output_t { uint8_t *samples; size_t samples_capacity, samples_capacity_max; };
typedef struct audio_instance { audio_player_config_t config; void *event_queue; void *task_handle; bool running; output_t output; } audio_instance_t;
static audio_instance_t instance;
static size_t stack_free_bytes;
static int queues,queue_deletes,buffers,buffer_frees,tasks,mutes,last_mute=-1;
static bool fail_queue,fail_buffer,fail_task;
#define CHECK(x) do {if(!(x)){fprintf(stderr,"line%d: %s\n",__LINE__,#x);return 2;}}while(0)
static void audio_instance_init(audio_instance_t &i){memset(&i,0,sizeof(i));}
static void *xQueueCreate(int n,size_t size){(void)n;(void)size;if(fail_queue)return nullptr;++queues;return (void*)0x10;}
static void vQueueDelete(void *q){if(q!=(void*)0x10)abort();++queue_deletes;}
static void *test_malloc(size_t n){(void)n;if(fail_buffer)return nullptr;++buffers;return std::malloc(n);}
static void test_free(void *p){if(p){++buffer_frees;std::free(p);}}
#define malloc test_malloc
#define free test_free
static void audio_task(void *){}
static BaseType_t xTaskCreatePinnedToCoreWithCaps(TaskFunction_t f,const char *name,unsigned stack,void *arg,UBaseType_t prio,void **handle,BaseType_t core,unsigned caps){
 (void)f;(void)name;(void)arg;(void)prio;(void)core;
 if(stack<1024 || !(caps&MALLOC_CAP_INTERNAL))abort();
 if(fail_task)return 0;++tasks;*handle=(void*)0x20;return pdPASS;}
static esp_err_t mute(AUDIO_PLAYER_MUTE_SETTING s){++mutes;last_mute=s;return ESP_OK;}
''' + cleanup + '\n' + create + r'''
static bool owns_nothing(){return !instance.task_handle && !instance.event_queue && !instance.output.samples && !instance.running;}
int main(){
 audio_player_config_t cfg={mute,5,1};
 fail_queue=true;
 CHECK(audio_player_new(cfg)!=ESP_OK);CHECK(owns_nothing());CHECK(queues==0 && buffers==0 && tasks==0);
 fail_queue=false;fail_buffer=true;
 CHECK(audio_player_new(cfg)==ESP_ERR_NO_MEM);CHECK(owns_nothing());
 CHECK(queues==1 && queue_deletes==1 && buffers==0);
 fail_buffer=false;fail_task=true;
 CHECK(audio_player_new(cfg)==ESP_ERR_NO_MEM);CHECK(owns_nothing());
 CHECK(queues==2 && queue_deletes==2 && buffers==1 && buffer_frees==1 && tasks==0 && mutes==0);
 /* After any failure the next attempt starts clean and succeeds. */
 fail_task=false;
 CHECK(audio_player_new(cfg)==ESP_OK);
 CHECK(tasks==1 && instance.task_handle==(void*)0x20 && instance.event_queue && instance.output.samples && instance.running);
 CHECK(mutes==1 && last_mute==AUDIO_PLAYER_MUTE);
 /* A live player is never re-created over its own resources. */
 CHECK(audio_player_new(cfg)==ESP_ERR_INVALID_STATE);
 CHECK(queues==3 && buffers==2 && tasks==1 && instance.task_handle==(void*)0x20);
 puts("queue, buffer and task failures release everything; retry and double create checked");
 return 0;
}
'''
        with tempfile.TemporaryDirectory() as d:
            src = pathlib.Path(d) / 'create.cpp'
            exe = pathlib.Path(d) / 'create.exe'
            src.write_text(harness)
            subprocess.run([compiler, '-std=c++17', str(src), '-o', str(exe)], check=True)
            subprocess.run([str(exe)], check=True)


if __name__ == '__main__':
    unittest.main()
