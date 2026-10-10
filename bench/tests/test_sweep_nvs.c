#include "ls_test.h"
#include "ls_sweep_app.h"
#include "ls_nvs_safe.h"
#include "nvs.h"
static bool safe,fail_save;
static unsigned jobs,commits;
static ls_sweep_settings_t saved;
static uint32_t magic=SWEEP_SETTINGS_MAGIC, build=SWEEP_SETTINGS_BUILD;
static size_t blob_size=sizeof(ls_sweep_settings_blob_t);
esp_err_t ls_nvs_run(ls_nvs_fn_t fn,void *ctx,unsigned stack) {LS_CHECK(!safe);safe=true;jobs++;esp_err_t e=fn(ctx);safe=false;return e;}
esp_err_t nvs_open(const char *space,int mode,nvs_handle_t *h) {LS_CHECK(safe && !strcmp(space,"sweep"));*h=1;return ESP_OK;}
esp_err_t nvs_get_blob(nvs_handle_t h,const char *key,void *p,size_t *n) {LS_CHECK(safe && !strcmp(key,"settings_v1"));ls_sweep_settings_blob_t blob={magic,build,saved};memcpy(p,&blob,sizeof(blob));*n=blob_size;return ESP_OK;}
esp_err_t nvs_set_blob(nvs_handle_t h,const char *key,const void *p,size_t n) {LS_CHECK(safe && n==sizeof(ls_sweep_settings_blob_t));if(fail_save) return ESP_FAIL;const ls_sweep_settings_blob_t *blob=p;magic=blob->magic;build=blob->build;saved=blob->value;return ESP_OK;}
esp_err_t nvs_commit(nvs_handle_t h) {LS_CHECK(safe);commits++;return ESP_OK;}
void nvs_close(nvs_handle_t h) {LS_CHECK(safe);}

LS_CASE(settings_round_trip_and_corruption_only_on_nvs_safe_worker) {
    jobs=commits=0;
    ls_sweep_settings_t s={.version=1,.enabled=31,.muted=9,.filter=4,.logging=true,.receive_only=true,.hunt=true,.hunt_mac={1,2,3,4,5,6},.hunt_radio=1,.hunt_type=0};
    LS_CHECK(ls_sweep_settings_set(&s));LS_CHECK(jobs==1 && commits==1);
    fail_save=true;ls_sweep_settings_t changed=s;changed.enabled=0;changed.logging=false;
    LS_CHECK(!ls_sweep_settings_set(&changed));fail_save=false;ls_sweep_settings_load();
    ls_sweep_settings_t out;ls_sweep_settings_get(&out);
    LS_CHECK(out.enabled==31 && out.muted==9 && out.filter==4 && out.logging && out.hunt && !memcmp(out.hunt_mac,s.hunt_mac,6) && out.hunt_radio==1);
    saved.version=99;ls_sweep_settings_load();ls_sweep_settings_get(&out);LS_CHECK(out.version==3 && out.enabled==15 && out.alerts_muted);
    saved=s;saved.filter=99;ls_sweep_settings_load();ls_sweep_settings_get(&out);LS_CHECK(out.filter==0 && out.alerts_muted);
    s.enabled=32;LS_CHECK(!ls_sweep_settings_set(&s));LS_CHECK(!safe);
}
LS_CASE(live_adverts_and_screen_updates_never_persist) {
    ls_sweep_init();ls_sweep_start(true);
    const unsigned before_jobs=jobs,before_commits=commits;
    const uint8_t mac[6]={0,0x40,0x8c,1,2,3};
    const uint8_t ad[]={2,1,6};
    for(int i=0;i<10000;i++) {
        const int64_t now=(int64_t)i*125000;
        ls_sweep_ble(mac,0,-50,ad,sizeof(ad),now);
        ls_sweep_wifi(mac,"camera",-60,now);
        ls_sweep_status_t status;ls_sweep_status(&status,now);
        (void)ls_sweep_alert(now);
        ls_sweep_summary_t summary;ls_sweep_summary(&summary,now);
    }
    LS_CHECK(jobs==before_jobs && commits==before_commits);
    ls_sweep_start(false);
    LS_CHECK(jobs==before_jobs+1 && commits==before_commits+1);
}

LS_CASE(global_mute_persists_and_v1_padding_migrates_without_writes) {
    ls_sweep_settings_t s={.version=2,.enabled=15,.muted=5,.receive_only=true,.alerts_muted=true};
    unsigned before=commits;LS_CHECK(ls_sweep_settings_set(&s));LS_CHECK(commits==before+1);
    s.alerts_muted=false;LS_CHECK(ls_sweep_settings_set(&s));saved.alerts_muted=true;
    before=commits;ls_sweep_settings_load();ls_sweep_settings_get(&s);
    LS_CHECK(s.alerts_muted && s.muted==5 && commits==before);
    saved.version=1;memset((char *)&saved+offsetof(ls_sweep_settings_t,alerts_muted),0xff,1);
    ls_sweep_settings_load();ls_sweep_settings_get(&s);
    LS_CHECK(!s.alerts_muted && s.version==3 && s.muted==5 && commits==before);
}


LS_CASE(on_off_round_trip_is_event_driven_and_independent_of_categories) {
    _Static_assert(sizeof(ls_sweep_settings_t)==20,"keep the v1/v2 blob ABI");
    ls_sweep_start(false);unsigned before=commits;
    ls_sweep_start(true);LS_EQ_UINT(commits,before+1);
    LS_CHECK(saved.version==3 && saved.on);
    ls_sweep_start(true);LS_EQ_UINT(commits,before+1);
    ls_sweep_settings_t disk=saved;
    ls_sweep_start(false);ls_sweep_runtime_status(false,"reboot");saved=disk;
    before=commits;ls_sweep_settings_load();LS_EQ_UINT(commits,before);
    ls_sweep_status_t status;ls_sweep_status(&status,0);LS_CHECK(status.requested && !status.running);
    ls_sweep_settings_t s;ls_sweep_settings_get(&s);s.enabled=0;LS_CHECK(ls_sweep_settings_set(&s));
    LS_CHECK(saved.on && saved.enabled==0);
    ls_sweep_start(false);disk=saved;LS_CHECK(!disk.on);
    ls_sweep_start(true);saved=disk;ls_sweep_settings_load();ls_sweep_status(&status,0);LS_CHECK(!status.requested);
    for(unsigned version=1;version<=2;version++) {
        saved.version=version;((unsigned char *)&saved)[offsetof(ls_sweep_settings_t,on)]=255;
        before=commits;ls_sweep_settings_load();ls_sweep_settings_get(&s);
        LS_CHECK(s.version==3 && !s.on && commits==before);
    }
    saved.version=3;((unsigned char *)&saved)[offsetof(ls_sweep_settings_t,on)]=2;
    ls_sweep_settings_load();ls_sweep_settings_get(&s);LS_CHECK(!s.on);
}
LS_CASE(failed_on_save_reports_failure_and_preserves_previous_boot_preference) {
    ls_sweep_start(false);ls_sweep_settings_t initial;ls_sweep_settings_get(&initial);
    LS_CHECK(ls_sweep_settings_set(&initial));ls_sweep_settings_t disk=saved;
    fail_save=true;unsigned before=commits;ls_sweep_start(true);fail_save=false;
    LS_EQ_UINT(commits,before);LS_CHECK(!memcmp(&saved,&disk,sizeof(saved)));
    ls_sweep_status_t status;ls_sweep_status(&status,0);
    LS_CHECK(status.requested && strstr(status.status,"NVS save failed"));
    ls_sweep_settings_load();ls_sweep_status(&status,0);LS_CHECK(!status.requested);
}

LS_CASE(settings_header_mismatch_and_legacy_size_restore_muted_defaults) {
    for(int bad=0;bad<3;bad++) {
        ls_sweep_settings_t s={.version=3,.enabled=31,.receive_only=true,.on=true};
        LS_CHECK(ls_sweep_settings_set(&s));
        LS_EQ_UINT(magic,SWEEP_SETTINGS_MAGIC); LS_EQ_UINT(build,SWEEP_SETTINGS_BUILD);
        if(bad==0) magic^=1;
        if(bad==1) build^=1;
        if(bad==2) blob_size=sizeof(saved);
        unsigned before=commits;ls_sweep_settings_load();ls_sweep_settings_get(&s);
        LS_CHECK(s.alerts_muted && !s.on && s.enabled==15);LS_EQ_UINT(commits,before);
        blob_size=sizeof(ls_sweep_settings_blob_t);
    }
    magic=SWEEP_SETTINGS_MAGIC;build=SWEEP_SETTINGS_BUILD;
}

LS_CASE(fresh_install_starts_muted_without_nvs_write) {
    ls_sweep_settings_t s;ls_sweep_settings_get(&s);
    LS_CHECK(s.version==3 && s.alerts_muted && s.enabled==15 && !s.logging && s.receive_only);
    LS_EQ_UINT(commits,0);
}
