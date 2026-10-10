#include "ls_test.h"
#include "freertos/task.h"
/* Missing scheduler declarations in the portable shim; no scheduler executes. */
TaskHandle_t xTaskCreateStatic(TaskFunction_t,const char *,uint32_t,void *,UBaseType_t,StackType_t *,StaticTask_t *);
UBaseType_t uxTaskGetStackHighWaterMark(TaskHandle_t);
#include "../../components/lakeshark/audio/tone.c"
static bool muted;
static int volume=23,notes,samples,peak;
TaskHandle_t xTaskCreateStatic(TaskFunction_t fn,const char *name,uint32_t n,void *arg,UBaseType_t priority,StackType_t *stack,StaticTask_t *tcb) {return NULL;}
UBaseType_t uxTaskGetStackHighWaterMark(TaskHandle_t t) {return 0;}
bool speech_available(void) {return false;}
speech_result_t speech_say(const char *text) {LS_CHECK(false);return (speech_result_t)0;}
uint32_t audio_out_ring_avail(void) {return 0;}
void audio_out_ensure_unmuted(void) {LS_CHECK(false);}
bool audio_is_muted(void) {return muted;}
int audio_volume_get(void) {return volume;}
void audio_out_play_now(void) {notes++;}
void audio_write_cue(const int16_t *p,int n) {
    samples+=n;for(int i=0;i<n;i++) {int v=abs(p[i]);if(v>peak) peak=v;}
}
LS_CASE(each_pattern_uses_current_volume_and_mute_without_hardware) {
    const int note_counts[]={2,2,3,1,3};
    for(int c=0;c<5;c++) {
        muted=false;volume=23;notes=samples=peak=0;sweep_pattern(c);
        LS_CHECK(notes==note_counts[c] && samples==note_counts[c]*560 && peak<=2200 && peak>2000 && volume==23);
        muted=true;notes=samples=0;sweep_pattern(c);LS_CHECK(notes==0 && samples==0 && volume==23);
        muted=false;volume=0;sweep_pattern(c);LS_CHECK(notes==0 && samples==0 && volume==0);
    }
}

LS_CASE(hunt_click_is_short_and_respects_mute_and_selected_volume) {
    muted=false;volume=23;notes=samples=peak=0;sweep_click_pattern();
    LS_CHECK(notes==1 && samples==128 && peak<=1800 && volume==23);
    muted=true;notes=samples=0;sweep_click_pattern();LS_CHECK(notes==0 && samples==0 && volume==23);
    muted=false;volume=0;sweep_click_pattern();LS_CHECK(notes==0 && samples==0 && volume==0);
}
