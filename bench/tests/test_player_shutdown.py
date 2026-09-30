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


if __name__ == '__main__':
    unittest.main()
