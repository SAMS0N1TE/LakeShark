/* Fake receivers exercise the production worker and evidence engine. */
#include "ls_test.h"
#include "../../components/apps/tui/ls_drone_watch.c"

static uint32_t pref=SETTINGS_DRONE_WATCH_DEFAULT|1u; /* tests run the watch on */
static bool mesh,foreground,background,lease,present=true,rx,unsupported,scan_ok=true;
static uint32_t caps=LS_LORA_CAP_LORA|LS_LORA_CAP_RSSI_INST|LS_LORA_CAP_RX_WIDE|LS_LORA_CAP_BAND_1G5_2G5;
static unsigned releases,notices,approaches,configs,reads,radio_releases;
static bool accept=true,attached=true,dc_only;
static int activity;
static ls_lora_cfg_t radio_cfg={.freq_hz=910525000,.sf=7,.bw_hz=62500};
uint32_t settings_get_drone_watch(void) {return pref;}
bool settings_set_drone_watch(uint32_t p) {if(!settings_drone_watch_valid(p))return false;pref=p;return true;}
bool ls_mesh_running(void) {return mesh;}
bool ls_mesh_radio_wanted(void) {return foreground;}
bool ls_mesh_radio_bg(void) {return background;}
bool ls_mesh_radio_hold_bg(bool on) {if(!on) {lease=false;releases++;return true;}lease=!foreground && !background;return lease;}
bool ls_lora_present(void) {return present;}
uint32_t ls_lora_caps(void) {return caps;}
const ls_lora_cfg_t *ls_lora_cfg(void) {return &radio_cfg;}
void ls_lora_cfg_default(ls_lora_cfg_t *c) {*c=(ls_lora_cfg_t){.freq_hz=910525000,.sf=7,.bw_hz=62500};}
bool ls_lora_is_receiving(void) {return rx;}
esp_err_t ls_lora_configure(const ls_lora_cfg_t *c) {
    LS_CHECK(lease);configs++;radio_cfg=*c;rx=false;
    return unsupported && c->freq_hz>=2400000000u?ESP_ERR_NOT_SUPPORTED:ESP_OK;
}
esp_err_t ls_lora_receive(void) {LS_CHECK(lease);rx=true;return ESP_OK;}
int ls_lora_activity(float *r) {LS_CHECK(lease);*r=-65;return activity;}
esp_err_t ls_lora_scan_begin(uint32_t lo,uint32_t hi) {(void)lo;(void)hi;LS_CHECK(lease);return ESP_OK;}
int ls_lora_scan_pass(float *out,int n,bool *done) {LS_CHECK(lease);for(int i=0;i<n;i++)out[i]=-110;*done=true;return scan_ok?n:0;}
esp_err_t ls_lora_scan_end(void) {LS_CHECK(lease);return ESP_OK;}
void ls_seen_drone(unsigned b,unsigned c,unsigned r,int dbm,unsigned h,int64_t us) {(void)b;(void)c;(void)r;(void)dbm;(void)h;(void)us;}
void ls_seen_observe(const uint8_t *a,uint8_t t,uint8_t s,uint8_t c,int r,bool v,bool f,uint8_t sec,int64_t us) {(void)a;(void)t;(void)s;(void)c;(void)r;(void)v;(void)f;(void)sec;(void)us;}
bool ls_seen_drone_watch(uint64_t id,int rssi,bool calibrated,unsigned band,bool approaching,int64_t us) {
    LS_CHECK(id && rssi<=0 && band && us>=0);if(!accept)return false;notices++;approaches+=approaching;return true;
}
ls_radio_err_t ls_radio_acquire(const char *owner,const ls_radio_requirements_t *req,ls_radio_session_t **out) {
    LS_CHECK(!strcmp(owner,"drone-watch") && req->min_hz>UINT32_MAX && req->required_caps==LS_RADIO_RX_IQ_U8);
    if(!attached)return LS_RADIO_ERR_UNAVAILABLE;*out=(ls_radio_session_t *)(uintptr_t)1;return LS_RADIO_OK;
}
ls_radio_err_t ls_radio_iq_configure(ls_radio_session_t *s,const ls_radio_iq_config_t *req,ls_radio_iq_config_t *actual) {(void)s;*actual=*req;return LS_RADIO_OK;}
ls_radio_err_t ls_radio_iq_start(ls_radio_session_t *s) {(void)s;return LS_RADIO_OK;}
ls_radio_err_t ls_radio_iq_stop(ls_radio_session_t *s) {(void)s;return LS_RADIO_OK;}
void ls_radio_release(ls_radio_session_t *s) {(void)s;radio_releases++;}
const char *ls_radio_err_name(ls_radio_err_t err) {(void)err;return "unavailable/busy";}
ls_radio_err_t ls_radio_iq_retune(ls_radio_session_t *s,uint64_t hz,bool fast,uint64_t *actual) {(void)s;(void)fast;*actual=hz;return LS_RADIO_OK;}
ls_radio_err_t ls_radio_iq_read(ls_radio_session_t *s,void *dst,size_t bytes,uint32_t timeout,size_t *got) {
    (void)s;LS_CHECK(timeout<=5);reads++;
    if(!attached) {*got=0;return LS_RADIO_ERR_DISCONNECTED;}
    uint8_t *d=dst;for(size_t i=0;i<bytes/2;i++) {double a=50+25*cos(6.283185307179586*15625*i/2000000), phase=dc_only?0:6.283185307179586*200000*i/2000000;d[2*i]=(uint8_t)(127.5+a*cos(phase));d[2*i+1]=(uint8_t)(127.5+a*sin(phase));}
    *got=bytes;return LS_RADIO_OK;
}
static unsigned hop_ch;
static void reset(void) {
    release();ls_dronelink_tick(false,0);ls_rid_clear();
    memset(tracks,0,sizeof(tracks));memset(evidence,0,sizeof(evidence));memset(&view,0,sizeof(view));
    mesh=foreground=background=lease=unsupported=rx=false;scan_ok=present=accept=attached=true;
    memset(hops,0,sizeof(hops));hop_ch=0;
    dc_only=false;activity=0;notices=approaches=releases=configs=reads=radio_releases=0;
    pref=SETTINGS_DRONE_WATCH_DEFAULT|1u;turn=LS_DW_SOURCES;turn_at=cycle_at=rid_at=elrs_hop_at=legacy_poll_at=0;
    elrs_channel=elrs_sf=fpv_channel=0;fallback=false;legacy=false;notice_fn=NULL;
    caps=LS_LORA_CAP_LORA|LS_LORA_CAP_RSSI_INST|LS_LORA_CAP_RX_WIDE|LS_LORA_CAP_BAND_1G5_2G5;
    radio_cfg=(ls_lora_cfg_t){.freq_hz=910525000,.sf=7,.bw_hz=62500};
    char text[4096];FILE *f=fopen(DRONE_DEFAULTS,"rb");LS_CHECK(f);size_t n=fread(text,1,sizeof(text),f);fclose(f);
    LS_CHECK(ls_drone_watch_configure(text,n));
}
static ls_drone_watch_entry_t entries[LS_DW_ENTRIES];
static size_t snapshot(void) {return ls_drone_watch_snapshot(entries,LS_DW_ENTRIES);}
static void quiet(float *row,size_t n) {for(size_t i=0;i<n;i++)row[i]=-110;}
static void hopper(unsigned events,bool fixed,int64_t cadence) {
    float r[71];quiet(r,71);ls_drone_watch_spectrum(LS_DW_HOPPER900,r,71,860000000,930000000,1000000);
    for(unsigned i=0;i<events;i++) {
        int64_t t=1100000+i*cadence;
        quiet(r,71);ls_drone_watch_spectrum(LS_DW_HOPPER900,r,71,860000000,930000000,t-1000);
        r[fixed?30:(i*7)%60]=-55;ls_drone_watch_spectrum(LS_DW_HOPPER900,r,71,860000000,930000000,t);
    }
}
/* One ELRS 2.4 hop: the next channel of the 80, as a real link moves. */
static void hop24(float dbm,int64_t us) {ls_drone_watch_activity(LS_DW_ELRS24,2400400000u+(hop_ch++%80)*1000000u,dbm,us);}
LS_CASE(hopper_requires_bursts_cadence_and_many_channels) {
    reset();hopper(12,true,20000);LS_EQ_INT(snapshot(),0);
    reset();hopper(12,false,20000);LS_EQ_INT(snapshot(),1);
    LS_EQ_INT(entries[0].source,LS_DW_HOPPER900);LS_EQ_INT(entries[0].confidence,LS_DW_POSSIBLE);
    reset();float r[71];quiet(r,71);ls_drone_watch_spectrum(LS_DW_HOPPER900,r,71,860000000,930000000,1000000);
    r[20]=-50;for(unsigned i=0;i<30;i++)ls_drone_watch_spectrum(LS_DW_HOPPER900,r,71,860000000,930000000,1100000+i*20000);
    LS_EQ_INT(snapshot(),0); /* steady single LoRa node */
    reset();int64_t t=1100000;quiet(r,71);ls_drone_watch_spectrum(LS_DW_HOPPER900,r,71,860000000,930000000,1000000);
    for(unsigned i=0;i<20;i++) {quiet(r,71);ls_drone_watch_spectrum(LS_DW_HOPPER900,r,71,860000000,930000000,t-1000);r[i*3%65]=-50;ls_drone_watch_spectrum(LS_DW_HOPPER900,r,71,860000000,930000000,t);t+=i%2?10000:90000;}
    LS_EQ_INT(snapshot(),0);
}
static void wide(unsigned start,unsigned width,int64_t cadence) {
    float r[84];quiet(r,84);ls_drone_watch_spectrum(LS_DW_WIDE24,r,84,2400000000u,2483500000u,1000000);
    for(unsigned k=0;k<12;k++) {
        int64_t t=1100000+k*cadence;quiet(r,84);ls_drone_watch_spectrum(LS_DW_WIDE24,r,84,2400000000u,2483500000u,t-1000);
        for(unsigned i=start;i<start+width;i++)r[i]=-60;
        ls_drone_watch_spectrum(LS_DW_WIDE24,r,84,2400000000u,2483500000u,t);
    }
}
LS_CASE(wifi_beacons_and_occupied_wifi_channels_do_not_warn) {
    reset();wide(2,22,102400);LS_EQ_INT(snapshot(),0);
    reset();wide(28,22,102400);LS_EQ_INT(snapshot(),0);
    reset();wide(53,22,102400);LS_EQ_INT(snapshot(),0);
    reset();wide(8,15,23000);LS_EQ_INT(snapshot(),0);
}
LS_CASE(repeated_digital_wide_bursts_warn_as_possible_only) {
    reset();wide(48,18,33000);LS_EQ_INT(snapshot(),1);
    LS_EQ_INT(entries[0].source,LS_DW_WIDE24);LS_EQ_INT(entries[0].confidence,LS_DW_POSSIBLE);
    reset();wide(48,3,33000);LS_EQ_INT(snapshot(),0);
}
LS_CASE(elrs24_single_channel_or_weak_hits_never_make_a_link) {
    /* Wi-Fi bursts or one LoRa node: one channel, however often. */
    reset();for(unsigned i=0;i<20;i++)ls_drone_watch_activity(LS_DW_ELRS24,2412400000u,-60,1000000+i*10000);
    LS_EQ_INT(snapshot(),0);
    /* Hopping but in the noise (the board saw -93..-97 dBm from Wi-Fi). */
    reset();for(unsigned i=0;i<10;i++)hop24(-95,1000000+i*10000);LS_EQ_INT(snapshot(),0);
    /* Three channels make one POSSIBLE link, six make it LIKELY. */
    reset();for(unsigned i=0;i<3;i++)hop24(-70,1000000+i*10000);
    LS_EQ_INT(snapshot(),1);LS_EQ_INT(entries[0].confidence,LS_DW_POSSIBLE);
    for(unsigned i=0;i<3;i++)hop24(-70,1100000+i*10000);
    LS_EQ_INT(snapshot(),1);LS_EQ_INT(entries[0].confidence,LS_DW_LIKELY);
}
LS_CASE(elrs24_preamble_activity_and_disabled_source) {
    reset();for(unsigned i=0;i<3;i++)hop24(-66,1000000+i*1000);LS_EQ_INT(snapshot(),1);
    uint64_t id=entries[0].id;hop24(-60,2000000);
    LS_EQ_INT(snapshot(),1);LS_CHECK(entries[0].id==id && entries[0].peak_dbm==-60);
    LS_CHECK(entries[0].confidence!=LS_DW_CONFIRMED);
    ls_drone_watch_source_enable(LS_DW_ELRS24,false);hop24(-60,3000000);LS_EQ_INT(snapshot(),1);
    ls_drone_watch_activity(LS_DW_ELRS24,2300000000u,-60,3000000);LS_EQ_INT(snapshot(),1);
}
LS_CASE(analog_line_rate_signature_rejects_cw_and_noise) {
    reset();float p[1024];
    for(unsigned i=0;i<1024;i++)p[i]=100+50*cosf(6.283185307f*15625*i/200000);
    LS_CHECK(ls_drone_watch_video(p,1024,200000));
    ls_drone_watch_analog(35,p,1024,200000,-44,false,1000000);LS_EQ_INT(snapshot(),1);
    LS_CHECK(!strcmp(entries[0].channel,"R4 5769"));LS_CHECK(entries[0].centre_hz==5769000000ULL && !entries[0].calibrated);
    for(unsigned i=0;i<1024;i++)p[i]=100;LS_CHECK(!ls_drone_watch_video(p,1024,200000));
    for(unsigned i=0;i<1024;i++)p[i]=100+50*cosf(6.283185307f*22000*i/200000);LS_CHECK(!ls_drone_watch_video(p,1024,200000));
    for(unsigned i=0;i<1024;i++)p[i]=100+50*cosf(6.283185307f*15734*i/200000);LS_CHECK(ls_drone_watch_video(p,1024,200000));
    p[1]=NAN;LS_CHECK(!ls_drone_watch_video(p,1024,200000));
}
LS_CASE(trend_history_rate_and_alert_cooldown) {
    reset();for(unsigned i=0;i<45;i++)hop24(-100+(int)i,1000000+(int64_t)i*1000000);
    LS_EQ_INT(snapshot(),1);LS_CHECK(entries[0].db_per_10s>9 && entries[0].db_per_10s<11);
    LS_EQ_INT(entries[0].trend,LS_DW_APPROACHING);LS_EQ_INT(entries[0].history_count,LS_DW_HISTORY);
    LS_EQ_INT(notices,2);LS_EQ_INT(approaches,1);LS_CHECK(entries[0].hits_per_second>=1 && entries[0].hits_per_second<=2);
    for(unsigned i=0;i<20;i++)hop24(-60-(int)i,50000000+(int64_t)i*1000000);
    LS_EQ_INT(snapshot(),1);LS_EQ_INT(entries[0].trend,LS_DW_RECEDING);
    LS_EQ_INT(notices,2);
}
static unsigned hooks;
static void hook(uint64_t id,bool approaching,void *ctx) {LS_CHECK(id && ctx==&hooks);(void)approaching;hooks++;}
LS_CASE(alert_off_and_backpressure_retry) {
    reset();hooks=0;ls_drone_watch_notice_hook(hook,&hooks);accept=false;
    for(unsigned i=0;i<3;i++)hop24(-60,1000000+i*1000);LS_EQ_INT(notices,0);LS_EQ_INT(hooks,0);
    accept=true;hop24(-60,2000000);LS_EQ_INT(notices,1);LS_EQ_INT(hooks,1);
    ls_drone_watch_alerts(false);hop24(-60,3000000);LS_EQ_INT(notices,1);
}
LS_CASE(worker_round_robin_restores_lease_and_yields_to_mesh) {
    reset();ls_drone_watch_step(10000000);LS_CHECK(lease && radio_cfg.freq_hz==903500000u);
    /* ELRS 900 also needs hits on three channels: three 250 ms hops. */
    activity=3;ls_drone_watch_step(10125000);ls_drone_watch_step(10375000);ls_drone_watch_step(10625000);ls_drone_watch_step(10875000);
    LS_EQ_INT(snapshot(),1);LS_EQ_INT(entries[0].source,LS_DW_ELRS900);
    ls_drone_watch_step(12000000);LS_CHECK(lease && radio_cfg.freq_hz==2400400000u && radio_cfg.sf==5 && radio_cfg.bw_hz==812500);
    /* A link needs hits on three channels: three 250 ms hops of the worker. */
    ls_drone_watch_step(12005000);ls_drone_watch_step(12255000);ls_drone_watch_step(12505000);ls_drone_watch_step(12755000);
    LS_CHECK(snapshot()>=2);
    foreground=true;ls_drone_watch_step(12800000);LS_CHECK(!lease && radio_cfg.freq_hz==910525000u);
    unsigned before=configs;ls_drone_watch_step(12900000);LS_EQ_INT(configs,before);
    foreground=false;ls_drone_watch_step(14000000);LS_CHECK(lease && scan);
    mesh=true;ls_drone_watch_step(14005000);LS_CHECK(!lease && !scan);mesh=false;
    ls_drone_watch_enable(false);ls_drone_watch_step(14010000);LS_CHECK(!lease && !sdr);
}
LS_CASE(worker_unsupported_parameters_fallback_and_missing_hardware_status) {
    reset();pref&=~(4u<<LS_DW_ELRS900);unsupported=true;
    ls_drone_watch_step(10000000);LS_CHECK(scan && !packet && lease);
    ls_drone_watch_status_t st;LS_CHECK(ls_drone_watch_status(&st));LS_CHECK(strstr(st.reason[LS_DW_ELRS24],"fallback"));
    ls_drone_watch_enable(false);ls_drone_watch_step(10010000);LS_CHECK(!lease);
    reset();pref&=~(4u<<LS_DW_ELRS900);caps=LS_LORA_CAP_LORA;ls_drone_watch_step(10000000);
    LS_CHECK(ls_drone_watch_status(&st));LS_CHECK(!st.running[LS_DW_ELRS24] && strstr(st.reason[LS_DW_ELRS24],"unavailable"));
}
LS_CASE(hackrf_fake_capture_and_disconnect_release) {
    reset();pref=0x10003u|(4u<<LS_DW_ANALOG58);
    for(unsigned i=0;i<4;i++)ls_drone_watch_step(10000000+i*10000);
    LS_CHECK(reads>=3);LS_EQ_INT(snapshot(),1);LS_EQ_INT(entries[0].source,LS_DW_ANALOG58);
    attached=false;ls_drone_watch_step(10050000);LS_CHECK(!sdr && radio_releases==1);
    ls_drone_watch_status_t st;LS_CHECK(ls_drone_watch_status(&st));LS_CHECK(!st.running[LS_DW_ANALOG58]);
}
LS_CASE(remote_id_identity_only_and_no_operator_data) {
    reset();uint8_t ad[31]={30,0x16,0xfa,0xff,0x0d,1,0,0x12};
    memcpy(ad+8,"AIRCRAFT-123",12);uint8_t mac[6]={1,2,3,4,5,6};
    LS_CHECK(ls_rid_advert(mac,0,-52,ad,sizeof(ad),1000000));
    rid_pass(1000000);LS_EQ_INT(snapshot(),1);LS_EQ_INT(entries[0].source,LS_DW_RID);LS_EQ_INT(entries[0].confidence,LS_DW_CONFIRMED);
    LS_CHECK(!strcmp(entries[0].identity,"AIRCRAFT-123"));LS_EQ_INT(entries[0].centre_hz,0);
    uint64_t id=entries[0].id;rid_pass(1000000);LS_EQ_INT(notices,1);LS_CHECK(snapshot()==1 && entries[0].id==id);
}
LS_CASE(configuration_and_settings_are_closed_versioned_schemas) {
    reset();LS_CHECK(settings_drone_watch_valid(SETTINGS_DRONE_WATCH_DEFAULT));
    LS_CHECK(!settings_drone_watch_valid(255));LS_CHECK(!settings_drone_watch_valid(0x200ff));LS_CHECK(!settings_drone_watch_valid(0x101ff));
    LS_CHECK(!ls_drone_watch_configure("{}",2));LS_CHECK(!ls_drone_watch_configure("{\"version\":2}",13));
    char text[4096];FILE *f=fopen(DRONE_DEFAULTS,"rb");LS_CHECK(f);size_t n=fread(text,1,sizeof(text)-2,f);fclose(f);text[n]=0;
    char *p=strstr(text,"\"version\": 1");LS_CHECK(p);p[11]='2';LS_CHECK(!ls_drone_watch_configure(text,n));p[11]='1';
    LS_CHECK(ls_drone_watch_configure(text,n));text[n++]='x';LS_CHECK(!ls_drone_watch_configure(text,n));
    LS_EQ_INT(cfg.version,1);LS_EQ_INT(cfg.wide_mhz,10);
    char *argv[]={"drones","src","wide24","off"};LS_EQ_INT(ls_drone_watch_command(4,argv),0);LS_CHECK(!enabled(LS_DW_WIDE24));
    argv[2]="invalid";LS_EQ_INT(ls_drone_watch_command(4,argv),1);
}
LS_CASE(table_bound_and_expiry) {
    reset();for(unsigned i=0;i<100;i++)ls_drone_watch_activity(LS_DW_ELRS24,2400400000u+(i%80)*1000000u,-60,1000000+i*1000);
    LS_CHECK(snapshot()<=LS_DW_ENTRIES);ls_drone_watch_enable(false);ls_drone_watch_step(70000000);LS_EQ_INT(snapshot(),0);
}
LS_CASE(rssi_fallback_requires_repeated_narrow_hops_and_stays_possible) {
    reset();float r[84];quiet(r,84);
    ls_drone_watch_spectrum(LS_DW_ELRS24,r,84,2400000000u,2483500000u,1000000);
    for(unsigned i=0;i<12;i++) {
        int64_t t=1100000+i*20000;
        quiet(r,84);ls_drone_watch_spectrum(LS_DW_ELRS24,r,84,2400000000u,2483500000u,t-1000);
        r[i*6]=-55;ls_drone_watch_spectrum(LS_DW_ELRS24,r,84,2400000000u,2483500000u,t);
    }
    LS_EQ_INT(snapshot(),1);LS_EQ_INT(entries[0].confidence,LS_DW_POSSIBLE);LS_EQ_STR(entries[0].channel,"RSSI fallback");
}
LS_CASE(dc_artifact_even_with_line_modulation_is_not_analog_rf) {
    reset();dc_only=true;pref=0x10003u|(4u<<LS_DW_ANALOG58);
    for(unsigned i=0;i<5;i++)ls_drone_watch_step(10000000+i*10000);
    LS_EQ_INT(snapshot(),0);LS_CHECK(reads>=3);
}
LS_CASE(scan_fault_releases_mesh_lease_and_reports_failure) {
    reset();pref=0x10003u|(4u<<LS_DW_HOPPER900);scan_ok=false;
    ls_drone_watch_step(10000000);LS_CHECK(!lease && !scan);
    ls_drone_watch_status_t st;LS_CHECK(ls_drone_watch_status(&st));LS_CHECK(strstr(st.reason[LS_DW_HOPPER900],"failed"));
    background=true;unsigned before=configs;ls_drone_watch_step(10010000);LS_CHECK(!lease && before==configs);
}
