#include "ls_test.h"
#include <setjmp.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
/* The host shim does not otherwise need these two ESP task APIs. */
TaskHandle_t xTaskCreateStatic(TaskFunction_t,const char *,uint32_t,void *,UBaseType_t,StackType_t *,StaticTask_t *);
UBaseType_t uxTaskGetStackHighWaterMark(TaskHandle_t);
static size_t strlcpy(char *out,const char *in,size_t n)
{ size_t len=strlen(in); if(n) snprintf(out,n,"%s",in); return len; }
#include "../../components/lakeshark/core/settings.c"
#include "../../components/lakeshark/audio/tone.c"

static int saved=-1, volume=100, pcm_n, job=-1, writes, commits;
static bool muted, reject;
static int16_t pcm[20000];
static set_write_t pending;
static bool has_write;
static jmp_buf done;
static int sound_q, settings_q;

__attribute__((noinline)) BaseType_t xQueueSend(QueueHandle_t q,const void *p,TickType_t wait)
{
    LS_EQ_INT(wait,0);
    if(reject) return pdFALSE;
    if(q==&sound_q) job=*(const uint8_t *)p;
    else { pending=*(const set_write_t *)p; has_write=true; }
    return pdTRUE;
}
__attribute__((noinline)) BaseType_t xQueueReceive(QueueHandle_t q,void *p,TickType_t wait)
{
    if(q==&sound_q && job>=0) { *(uint8_t *)p=(uint8_t)job; job=-1; return pdTRUE; }
    if(q==&settings_q && has_write) { *(set_write_t *)p=pending; has_write=false; return pdTRUE; }
    if(wait==portMAX_DELAY) longjmp(done,1);
    return pdFALSE;
}
QueueHandle_t xQueueCreateStatic(UBaseType_t n,UBaseType_t size,uint8_t *store,StaticQueue_t *ctrl)
{ (void)n;(void)size;(void)store;(void)ctrl; return &settings_q; }
TaskHandle_t xTaskCreateStatic(TaskFunction_t f,const char *name,uint32_t depth,void *arg,UBaseType_t priority,StackType_t *stack,StaticTask_t *tcb)
{ (void)f;(void)name;(void)arg;(void)priority;(void)stack;(void)tcb; LS_EQ_UINT(depth,SND_WORKER_STACK_BYTES); return &sound_q; }
UBaseType_t uxTaskGetStackHighWaterMark(TaskHandle_t t) { (void)t;return 0; }
void vTaskDelay(TickType_t n) { (void)n; }
void audio_out_play_now(void) {}
uint32_t audio_out_ring_avail(void) { return 0; }
uint32_t ls_audio_hw_starved(int64_t *first_us, int64_t *last_us) { (void)first_us; (void)last_us; return 0; }
TickType_t xTaskGetTickCount(void) { return 0; }
void audio_write_cue(const int16_t *buf,int n)
{ LS_CHECK(pcm_n+n<=20000); memcpy(pcm+pcm_n,buf,n*sizeof(*buf)); pcm_n+=n; }
static int cue_writes;
bool audio_write_cue_wait(const int16_t *buf,int n,int timeout_ms)
{ (void)timeout_ms; cue_writes++; audio_write_cue(buf,n); return true; }
bool audio_is_muted(void) { return muted; }
int audio_volume_get(void) { return volume; }
void audio_out_ensure_unmuted(void) { LS_CHECK(!muted); }
bool speech_available(void) { return false; }
speech_result_t speech_say(const char *s) { (void)s; return 0; }
esp_err_t ls_nvs_run(ls_nvs_fn_t f,void *p,unsigned n) { (void)n; return f(p); }
TaskHandle_t ls_flash_task_create_static(TaskFunction_t f,const char *name,uint32_t bytes,void *arg,UBaseType_t pri,StackType_t *stack,StaticTask_t *tcb,BaseType_t core)
{ (void)f;(void)name;(void)bytes;(void)arg;(void)pri;(void)stack;(void)tcb;(void)core;return &settings_q; }
esp_err_t nvs_flash_init(void) { return ESP_OK; }
esp_err_t nvs_flash_erase(void) { return ESP_OK; }
esp_err_t nvs_open(const char *name,int mode,nvs_handle_t *out) { (void)name;(void)mode;*out=1;return ESP_OK; }
esp_err_t nvs_commit(nvs_handle_t n) { (void)n;++commits;return ESP_OK; }
esp_err_t nvs_set_u8(nvs_handle_t n,const char *key,uint8_t v)
{ (void)n;if(!strcmp(key,"notify_snd")) {saved=v;++writes;}return ESP_OK; }
__attribute__((noinline)) esp_err_t (nvs_get_u8)(nvs_handle_t n,const char *key,uint8_t *v)
{ (void)n;if(!strcmp(key,"notify_snd") && saved>=0) {*v=(uint8_t)saved;return ESP_OK;}return ESP_ERR_NVS_NOT_FOUND; }
#define NVS_FAKE(t,T) \
 esp_err_t nvs_set_##t(nvs_handle_t h,const char *k,T v) {(void)h;(void)k;(void)v;return ESP_OK;} \
 esp_err_t (nvs_get_##t)(nvs_handle_t h,const char *k,T *v) {(void)h;(void)k;(void)v;return ESP_ERR_NVS_NOT_FOUND;}
NVS_FAKE(i8,int8_t)
NVS_FAKE(u32,uint32_t)
NVS_FAKE(i32,int32_t)
NVS_FAKE(u64,uint64_t)
esp_err_t nvs_erase_key(nvs_handle_t h,const char *k) { (void)h;(void)k;return ESP_OK; }
esp_err_t nvs_entry_find(const char *a,const char *b,int t,nvs_iterator_t *it)
{ (void)a;(void)b;(void)t;*it=NULL;return ESP_ERR_NVS_NOT_FOUND; }
esp_err_t nvs_entry_next(nvs_iterator_t *it) { (void)it;return ESP_ERR_NVS_NOT_FOUND; }
void nvs_entry_info(nvs_iterator_t it,nvs_entry_info_t *info) { (void)it;(void)info; }
void nvs_release_iterator(nvs_iterator_t it) { (void)it; }
__attribute__((noinline)) static void flush_settings(void)
{
    if(!setjmp(done)) set_worker(NULL);
}
static void play(int sound)
{
    s_test_q=&sound_q; s_test_busy=false; s_notification_sound=sound;
    pcm_n=0; job=-1;
    LS_CHECK(snd_alert_start());
    if(!setjmp(done)) test_worker(NULL);
    s_test_busy=false;
}
static double goertzel(int start,int n,double hz)
{
    double a=2*cos(2*M_PI*hz/AUDIO_RATE_HZ),s0=0,s1=0,s2=0;
    for(int i=0;i<n;++i) {s0=pcm[start+i]+a*s1-s2;s2=s1;s1=s0;}
    return s1*s1+s2*s2-a*s1*s2;
}
LS_CASE(radiokey_pcm_has_two_55ms_1800hz_blips_and_35ms_silence)
{
    muted=false;volume=100;play(SETTINGS_NOTIFY_RADIOKEY);
    LS_EQ_INT(pcm_n,2320);
    for(int i=880;i<1440;++i) LS_EQ_INT(pcm[i],0);
    LS_CHECK(goertzel(80,640,1800)>100*goertzel(80,640,1000));
    LS_CHECK(goertzel(1520,640,1800)>100*goertzel(1520,640,1500));
    LS_EQ_INT(pcm[0],0);LS_EQ_INT(pcm[2319],0);
}
LS_CASE(roger_pair_has_1khz_then_1point5khz_and_real_gap)
{
    muted=false;volume=100;play(SETTINGS_NOTIFY_ROGER);
    LS_EQ_INT(pcm_n,1840);
    for(int i=720;i<1120;++i) LS_EQ_INT(pcm[i],0);
    LS_CHECK(goertzel(80,560,1000)>100*goertzel(80,560,1500));
    LS_CHECK(goertzel(1200,560,1500)>100*goertzel(1200,560,1000));
}
LS_CASE(chirp_rises_and_all_presets_have_headroom_at_max_board_volume)
{
    muted=false;volume=100;
    for(int s=SETTINGS_NOTIFY_RADIOKEY;s<=SETTINGS_NOTIFY_CHIRP;++s) {
        play(s);int peak=0;
        for(int i=0;i<pcm_n;++i) {int a=abs(pcm[i]);if(a>peak)peak=a;}
        LS_CHECK(peak>4000 && peak<=4800);
    }
    LS_EQ_INT(pcm_n,1360);
    LS_CHECK(goertzel(80,160,1175)>goertzel(80,160,2220));
    LS_CHECK(goertzel(1040,160,2220)>goertzel(1040,160,1175));
}
LS_CASE(alert_dispatch_default_off_mute_and_queued_mute)
{
    s_test_q=&sound_q;s_test_busy=false;muted=false;volume=100;
    s_notification_sound=SETTINGS_NOTIFY_ALERT;job=-1;
    LS_CHECK(snd_alert_start()); LS_EQ_INT(job,SND_JOB_ALERT);
    for(int s=SETTINGS_NOTIFY_RADIOKEY;s<=SETTINGS_NOTIFY_CHIRP;++s) {
        s_notification_sound=s;LS_CHECK(snd_alert_start());
        LS_EQ_INT(job,SND_TEST_RADIOKEY+s-SETTINGS_NOTIFY_RADIOKEY);
        pcm_n=0;muted=true;
        if(!setjmp(done)) test_worker(NULL);
        s_test_busy=false;LS_EQ_INT(pcm_n,0);
        job=-1;LS_CHECK(!snd_alert_start());LS_EQ_INT(job,-1);muted=false;
    }
    s_notification_sound=SETTINGS_NOTIFY_OFF;LS_CHECK(!snd_alert_start());
    s_notification_sound=SETTINGS_NOTIFY_RADIOKEY;volume=0;LS_CHECK(!snd_alert_start());volume=100;
    LS_EQ_INT(snd_test_from_name("radiokey"),SND_TEST_RADIOKEY);
    LS_EQ_INT(snd_test_from_name("ROGER"),SND_TEST_ROGER);
    LS_EQ_INT(snd_test_from_name("chirp"),SND_TEST_UPCHIRP);
    LS_EQ_INT(snd_test_from_name("p25chirp"),SND_TEST_CHIRP);
}
LS_CASE(notification_setting_defaults_validates_persists_and_exposes_ui_choices)
{
    saved=-1;reject=false;LS_CHECK(settings_init());
    LS_EQ_INT(settings_get_notification_sound(),SETTINGS_NOTIFY_ALERT);
    int count,index;const char *const *names=settings_notification_sound_names(&count,&index);
    LS_EQ_INT(count,5);LS_EQ_INT(index,0);LS_EQ_STR(names[1],"RADIO KEY");
    LS_CHECK(!settings_set_notification_sound(-1));LS_CHECK(!settings_set_notification_sound(5));
    LS_CHECK(settings_cycle_notification_sound());LS_EQ_INT(saved,-1);
    flush_settings();
    LS_EQ_INT(saved,SETTINGS_NOTIFY_RADIOKEY);LS_CHECK(writes>0);
    LS_CHECK(settings_init());LS_EQ_INT(settings_get_notification_sound(),SETTINGS_NOTIFY_RADIOKEY);
    reject=true;LS_CHECK(!settings_set_notification_sound(SETTINGS_NOTIFY_ROGER));
    LS_EQ_INT(settings_get_notification_sound(),SETTINGS_NOTIFY_RADIOKEY);reject=false;
    saved=255;LS_CHECK(settings_init());LS_EQ_INT(settings_get_notification_sound(),SETTINGS_NOTIFY_ALERT);
    LS_EQ_INT(settings_notification_sound_from_name("radio key"),SETTINGS_NOTIFY_RADIOKEY);
    LS_EQ_INT(settings_notification_sound_from_name("radiokey"),SETTINGS_NOTIFY_RADIOKEY);
    LS_EQ_INT(settings_notification_sound_from_name("off"),SETTINGS_NOTIFY_OFF);
    LS_EQ_INT(settings_notification_sound_from_name("garbage"),-1);
}
LS_CASE(json_tuning_is_versioned_validated_and_atomic)
{
    FILE *f=fopen(NOTIFICATION_DEFAULTS,"rb");LS_CHECK(f!=NULL);
    char json[512];size_t n=fread(json,1,sizeof(json)-1,f);fclose(f);json[n]=0;
    LS_CHECK(notification_tone_configure(json));
    LS_CHECK(!notification_tone_configure("{\"version\":2,\"presets\":[]}"));
    LS_CHECK(!notification_tone_configure("{\"version\":1,\"presets\":[[0,0,55,35,1800,1800,55,1,3]]}"));
    char *hz=strstr(json,"1800");LS_CHECK(hz!=NULL);memcpy(hz,"1900",4);
    LS_CHECK(notification_tone_configure(json));
    LS_CHECK(!notification_tone_configure("{} garbage"));
    muted=false;volume=100;play(SETTINGS_NOTIFY_RADIOKEY);
    LS_EQ_INT(pcm_n,2320);
    /* Restore default file for any subsequent case. */
    memcpy(hz,"1800",4);LS_CHECK(notification_tone_configure(json));
}


LS_CASE(chunk_boundaries_do_not_change_pcm_or_duration)
{
    int16_t whole[2400], part[160];
    size_t total=notification_tone_render(0,0,whole,2400),offset=0;
    LS_EQ_UINT(total,2320);
    while(offset<total) {
        size_t n=notification_tone_render(0,offset,part,160);
        LS_CHECK(n>0 && n<=160);
        LS_CHECK(!memcmp(whole+offset,part,n*sizeof(*part)));
        offset+=n;
    }
    LS_EQ_UINT(notification_tone_render(0,total,part,160),0);
    LS_EQ_UINT(notification_tone_render(-1,0,part,160),0);
    LS_EQ_UINT(notification_tone_render(3,0,part,160),0);
}

LS_CASE(every_choice_survives_a_restart_and_off_cycles_to_alert)
{
    reject=false;has_write=false;saved=-1;LS_CHECK(settings_init());
    for(int choice=0;choice<SETTINGS_NOTIFY_COUNT;++choice) {
        LS_CHECK(settings_set_notification_sound(choice));
        flush_settings();
        LS_CHECK(settings_init());
        LS_EQ_INT(settings_get_notification_sound(),choice);
    }
    LS_CHECK(settings_cycle_notification_sound());
    LS_EQ_INT(settings_get_notification_sound(),SETTINGS_NOTIFY_ALERT);
    flush_settings();
    LS_CHECK(settings_init());LS_EQ_INT(settings_get_notification_sound(),SETTINGS_NOTIFY_ALERT);
}
LS_CASE(invalid_partial_table_preserves_the_last_complete_table)
{
    int16_t before[2400],after[2400];
    size_t n=notification_tone_render(0,0,before,2400);
    LS_CHECK(!notification_tone_configure(
        "{\"version\":1,\"presets\":[[1500,1500,55,35,1800,1800,55,1,3],"
        "[100,1000,45,25,1500,1500,45,1,3],[1000,2500,85,0,1000,1000,0,1,4]]}"));
    LS_EQ_UINT(notification_tone_render(0,0,after,2400),n);
    LS_CHECK(!memcmp(before,after,n*sizeof(*before)));
}

LS_CASE(unavailable_settings_worker_rejects_without_synchronous_flash_io)
{
    reject=false;LS_CHECK(settings_init());
    int before=settings_get_notification_sound(),old_writes=writes,old_commits=commits;
    QueueHandle_t q=s_wq;s_wq=NULL;
    LS_CHECK(!settings_set_notification_sound((before+1)%SETTINGS_NOTIFY_COUNT));
    LS_EQ_INT(settings_get_notification_sound(),before);
    LS_EQ_INT(writes,old_writes);LS_EQ_INT(commits,old_commits);
    s_wq=q;
}

LS_CASE(boot_chirp_is_one_write_short_and_moderate) {
    pcm_n=0; cue_writes=0;
    snd_boot();
    LS_CHECK(cue_writes==1);
    LS_CHECK(pcm_n>AUDIO_RATE_HZ/4 && pcm_n<=AUDIO_RATE_HZ*4/10);
    int peak=0;
    for(int i=0;i<pcm_n;i++) { int v=abs(pcm[i]); if(v>peak) peak=v; }
    LS_CHECK(peak>4000 && peak<8000);
    LS_CHECK(pcm[0]==0 && abs(pcm[pcm_n-1])<500);
}
