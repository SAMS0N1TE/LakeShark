"""Exercise production PCM submission and codec writes with injected failures."""
import unittest

from test_audio_reinit import ROOT, body, run_c


class AudioDropAccountingTest(unittest.TestCase):
    def test_rejected_pcm_and_failed_codec_writes_count_once(self):
        source = (ROOT / 'components/lakeshark/audio/audio_out.c').read_text()
        harness = r'''
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdatomic.h>
#define CHECK(x) do {if(!(x)){fprintf(stderr,"line %d: %s\n",__LINE__,#x);return 1;}}while(0)
#define pdTRUE 1
#define ESP_OK 0
#define portMAX_DELAY 0xffffffffu
#define pdMS_TO_TICKS(x) (x)
#define AUDIO_NOW_TICK() 42u
static bool s_media_request, s_muted, s_ready=true, s_live_seen;
static void *s_ring=(void*)1, *s_push_lock=(void*)2;
static uint32_t s_live_tick, s_speech_end, s_rd_bytes, s_skip_to, s_tts_yielded;
static _Atomic uint32_t s_audio_drops;
static int lock_available=1, lock_takes, lock_gives, sends, writes, delays;
static int codec_error;
static size_t accepted=320;
static int xSemaphoreTake(void *lock,int ticks){(void)lock;(void)ticks;++lock_takes;return lock_available;}
static void xSemaphoreGive(void *lock){(void)lock;++lock_gives;}
static size_t ring_send_locked(const void *p,size_t want){(void)p;++sends;return accepted<want?accepted:want;}
static int ls_audio_hw_write(void *p,size_t bytes,size_t *written,unsigned timeout){
 (void)p;(void)timeout;++writes;*written=codec_error?0:bytes;return codec_error;
}
static void vTaskDelay(int ticks){if(ticks!=5)__builtin_abort();++delays;}
''' + body(source, 'void audio_write_mono(') + '\n' + body(
            source, 'static void __attribute__((noinline)) player_write(') + r'''
int main(void){
 int16_t pcm[160]={0};
 /* Intentional suppression is not an audio loss. */
 s_media_request=true;audio_write_mono(pcm,160);s_media_request=false;
 s_muted=true;audio_write_mono(pcm,160);s_muted=false;
 s_ready=false;audio_write_mono(pcm,160);s_ready=true;
 audio_write_mono(pcm,0);
 CHECK(s_audio_drops==0 && lock_takes==0 && sends==0);
 /* A producer rejected by the mutex loses its whole chunk. */
 lock_available=0;audio_write_mono(pcm,160);
 CHECK(s_audio_drops==1 && sends==0 && lock_gives==0);
 CHECK(s_live_seen && s_live_tick==42);
 lock_available=1;audio_write_mono(pcm,160);
 CHECK(s_audio_drops==1 && sends==1 && lock_gives==1);
 /* Existing partial/full ring handling still counts one rejected chunk. */
 accepted=160;audio_write_mono(pcm,160);
 CHECK(s_audio_drops==2 && sends==2 && lock_gives==2);
 accepted=0;audio_write_mono(pcm,160);
 CHECK(s_audio_drops==3 && sends==3 && lock_gives==3);
 /* The consumer counts a codec error exactly once and retains its backoff. */
 codec_error=-1;player_write(pcm,sizeof(pcm));
 CHECK(s_audio_drops==4 && writes==1 && delays==1);
 codec_error=ESP_OK;player_write(pcm,sizeof(pcm));
 CHECK(s_audio_drops==4 && writes==2 && delays==1);
 return 0;
}
'''
        run_c(harness)


if __name__ == '__main__':
    unittest.main()
