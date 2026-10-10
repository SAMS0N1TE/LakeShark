"""Execute Compass production paths with failed storage, radio and sensor fakes."""
import unittest
from test_audio_reinit import ROOT, body, run_c

SCREEN = (ROOT / 'components/apps/tui/screens/ext/scr_compass.c').read_text()
SOURCES = (ROOT / 'components/apps/tui/ls_df_sources.c').read_text()
PRE = r"""
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <math.h>
#define EXT_RAM_BSS_ATTR
#define CHECK(x) do {if(!(x)){fprintf(stderr,"line %d: %s\n",__LINE__,#x);return 1;}}while(0)
"""

class CompassRegressions(unittest.TestCase):
    def test_note_refusal_preserves_retry_and_fix_reports_failure(self):
        includes = '\n'.join('#include "' + (ROOT / ('components/apps/tui/' + h)).as_posix() + '"' for h in ['ls_df.h', 'ls_df_sources.h'])
        harness = PRE + includes + r"""
static ls_df_estimate_t s_est={.valid=true,.bearing=112,.spread=5};
static ls_df_sweep_t s_sw[1]={{.samples=42,.top=-60}};
static ls_dfs_status_t st={.source=LS_DFS_HACKRF,.unit="dB",.tunable=true};
static ls_df_bearing_t s_bearings[LS_DF_BEARINGS];
static ls_df_fix_t s_fix;
static int s_sel,s_bearing_count,clears,notes,targets;
static bool accept,gps;
static char message[128];
static float shown_heading(void){return 0;}
static float to_true(float h){return h;}
static int64_t esp_timer_get_time(void){return 123;}
static uint32_t track_freq(int t){(void)t;return 915000000;}
static const ls_dfs_status_t *slot_status(int k){(void)k;return &st;}
const char *ls_dfs_name(ls_dfs_t src){(void)src;return "HackRF";}
static bool position(double *lat,double *lon){*lat=1;*lon=2;return gps;}
static void clear_track(int t){(void)t;clears++;s_sw[0].samples=0;}
static void say(const char *m){snprintf(message,sizeof(message),"%s",m);}
static bool ls_notes_mark(const char *t,const char *b){(void)t;(void)b;notes++;return accept;}
#define ls_note_fmt_bearing(...) ((void)0)
bool ls_df_triangulate(const ls_df_bearing_t *b,int n,ls_df_fix_t *f){(void)b;(void)n;f->valid=true;return true;}
static void ls_compass_set_target(double lat,double lon,const char *name){(void)lat;(void)lon;(void)name;targets++;}
""" + body(SCREEN, 'static void save_bearing(void)') + '\n' + body(SCREEN, 'static void make_fix(void)') + r"""
int main(void){
 save_bearing();CHECK(notes==1 && !clears && s_sw[0].samples==42 && s_est.valid && !s_bearing_count);
 CHECK(strstr(message,"kept"));gps=true;save_bearing();CHECK(!s_bearing_count && !clears);
 accept=true;save_bearing();CHECK(s_bearing_count==1 && clears==1 && !s_est.valid);
 accept=false;make_fix();CHECK(targets==1 && strstr(message,"busy") && !strstr(message,"saved"));
 accept=true;make_fix();CHECK(targets==2 && strstr(message,"saved"));return 0;
}
"""
        run_c(harness)

    def test_failed_primary_rolls_back_secondary_and_exits_stop_both(self):
        harness = PRE + r"""
enum {P_FIND=1,LS_DFS_COUNT=10,LS_DF_CAL_OFF=0,O_DWELL=0};
typedef int ls_dfs_t;
typedef int ls_dfs_reading_t;
static int s_dfs_source=0,s_src2=1,s_ch[2][1]={{915},{433}},s_nch[2]={1,1};
static bool s_find_on,active[2],fail=true,s_logmode;
static int s_page,button_focus;
static char s_feedback[96];
static struct {int phase;} s_cal;
#define OPTV(x) 400
static const char *ls_dfs_unavailable(int src){return fail && src==0?"off":NULL;}
static bool ls_dfs_select_slot(int k,int src,uint32_t f){(void)src;(void)f;active[k]=true;return true;}
static int ls_dfs_take(int *r,int n){(void)r;(void)n;return 0;}
static void ls_dfs_set_channels(int k,int *f,int n,uint32_t ms){(void)k;(void)f;(void)n;(void)ms;}
static void ls_dfs_stop_slot(int k){active[k]=false;}
static void cal_end(void){}
static void ls_field_watch(bool on){(void)on;}
""" + body(SCREEN, 'static void find_stop(void)') + '\n' + body(SCREEN, 'static void find_start(void)') + '\n' + body(SCREEN, 'static void show_page(int p)\n{') + '\n' + body(SCREEN, 'static void leave(void)') + r"""
int main(void){
 active[0]=true;active[1]=true;find_start();CHECK(!s_find_on && !active[0] && !active[1]);
 fail=false;find_start();CHECK(s_find_on && active[0] && active[1]);show_page(0);CHECK(!active[0] && !active[1]);
 active[1]=true;s_find_on=false;show_page(0);CHECK(!active[1]);
 active[1]=true;leave();CHECK(!active[0] && !active[1]);return 0;
}
"""
        run_c(harness)

    def test_locked_bearing_survives_reference_switch_and_late_model(self):
        harness = PRE + r"""
static float s_lock=98;
static bool s_magnetic,s_lock_true;
static struct {float declination;} s_r={.declination=14};
""" + body(SCREEN, 'static bool true_mode(void)') + '\n' + body(SCREEN, 'static float to_dial(float t)') + '\n' + body(SCREEN, 'static float locked_heading(void)') + r"""
int main(void){
 CHECK(locked_heading()==112);s_magnetic=true;CHECK(locked_heading()==98);
 s_lock=98;s_lock_true=false;s_r.declination=NAN;s_magnetic=false;CHECK(locked_heading()==98);
 s_r.declination=14;CHECK(locked_heading()==112 && s_lock==112 && s_lock_true);
 s_r.declination=15;CHECK(locked_heading()==112);s_magnetic=true;CHECK(locked_heading()==97);return 0;
}
"""
        run_c(harness)

    def test_frequency_entry_rejects_before_conversion(self):
        harness = PRE + r"""
#define LS_DFS_CHANNELS 8
static int s_edit_slot,s_edit_ch,s_nch[2]={1,1},applied;
static uint32_t s_ch[2][8];
static void say(const char *s){(void)s;}
static int slot_source(int k){return k;}
static bool ls_dfs_in_range(int k,uint32_t hz){(void)k;return hz>1200000;}
static const char *ls_dfs_name(int k){(void)k;return "HackRF";}
static void apply_channels(int k){(void)k;applied++;}
static void channels_open(int k){(void)k;}
""" + body(SCREEN, 'static void channel_freq_done(double mhz)') + r"""
int main(void){
 channel_freq_done(5000);channel_freq_done(INFINITY);channel_freq_done(NAN);channel_freq_done(-2);CHECK(!applied);
 channel_freq_done(4294.967295);CHECK(applied==1 && s_ch[0][0]==UINT32_MAX);
 channel_freq_done(915);CHECK(applied==2 && s_ch[0][0]==915000000);return 0;
}
"""
        run_c(harness)

    def test_console_frequency_rejects_overflow_and_malformed_input(self):
        command = body(SCREEN, 'if (!strcmp(verb, "ch") && argc > 2) {')
        harness = PRE + r"""
#define LS_DFS_CHANNELS 8
enum {RQ_CH=1};
static int s_rq_n,s_rq_slot,s_rq_kind;
static uint32_t s_rq_hz[8];
static bool parse(char *input){
 char *argv[]={"find","ch",input};int argc=3,slot=0;const char *verb="ch";
""" + command + r"""
 return true;
}
int main(void){
 CHECK(!parse("5000") && !s_rq_kind);CHECK(!parse("nan"));CHECK(!parse("915x"));
 CHECK(!parse("915,5000") && !s_rq_kind);CHECK(parse("915,4294.967295"));
 CHECK(s_rq_n==2 && s_rq_hz[0]==915000000 && s_rq_hz[1]==UINT32_MAX && s_rq_kind==RQ_CH);return 0;
}
"""
        run_c(harness)

    def test_address_handoff_is_cleared_on_source_change_and_restart(self):
        header = '#include "' + (ROOT / 'components/apps/tui/ls_df_sources.h').as_posix() + '"\n'
        slot = SOURCES[SOURCES.index('typedef struct {'):SOURCES.index('} slot_t;') + len('} slot_t;')]
        harness = PRE + header + slot + r"""
static struct {slot_t slot[2];} s;
static int observers;
static void lock(void){}
static void unlock(void){}
static bool slot_ok(int k){return k>=0 && k<2;}
const char *ls_dfs_unavailable(ls_dfs_t src){(void)src;return NULL;}
const char *ls_dfs_conflict(int k,ls_dfs_t src){(void)k;(void)src;return NULL;}
bool ls_dfs_in_range(ls_dfs_t src,uint32_t f){(void)src;(void)f;return true;}
static void wireless_refresh(void){observers++;}
""" + body(SOURCES, 'static bool tunable(') + '\n' + body(SOURCES, 'bool ls_dfs_select_slot(') + r"""
int main(void){
 s.slot[0].active=true;s.slot[0].source=LS_DFS_BLE;s.slot[0].address_target=true;s.slot[0].target=0;
 CHECK(ls_dfs_select_slot(0,LS_DFS_WIFI,0));CHECK(!s.slot[0].address_target && s.slot[0].target==-1 && !s.slot[0].target_key[0]);
 s.slot[0].address_target=true;s.slot[0].active=false;
 CHECK(ls_dfs_select_slot(0,LS_DFS_WIFI,0));CHECK(!s.slot[0].address_target && s.slot[0].target==-1);
 s.slot[0].address_target=true;CHECK(ls_dfs_select_slot(0,LS_DFS_WIFI,0));CHECK(s.slot[0].address_target && observers==3);return 0;
}
"""
        run_c(harness)

    def test_borrowed_sdr_rejects_other_frequency_and_uses_current_channel(self):
        harness = PRE + r"""
enum {LS_RADIO_OK=0};
static void *s_session;
static int k,err=1,published,channel;
static uint32_t first=915000000,borrowed=461000000;
static struct {int cur;} w[1]={{3}};
static const char *borrowed_value(float *level,uint32_t *hz){*level=-50;*hz=borrowed;return "P25";}
static const char *ls_radio_err_name(int e){(void)e;return "busy";}
static void status(int k,const char *m){(void)k;(void)m;}
static void publish(int k,int ch,uint32_t hz,float level,const char *unit,int flags){(void)k;(void)hz;(void)level;(void)unit;(void)flags;published++;channel=ch;}
static void attempt(void){
""" + body(SOURCES, 'if (err != LS_RADIO_OK) {') + r"""
}
int main(void){attempt();CHECK(!published);borrowed=first;attempt();CHECK(published==1 && channel==3);return 0;}
"""
        run_c(harness)

    def test_magnetic_conversion_ages_without_accel_refresh_and_recovers(self):
        source = (ROOT / 'components/lakeshark/board/ls_imu.c').read_text()
        harness = PRE + r"""
#define CACHE_US 50000
#define MAG_HOLD_US 250000
#define MAG_RETRY_US 1000000
#define B0_ACCEL_XOUT_H 0x2d
#define MAG_ST1 0x10
#define MAG_HXL 0x11
#define MAG_CNTL3 0x32
#define MAG_CNTL2 0x31
#define MAG_SRST 1
#define MAG_MODE_100HZ 8
#define ACCEL_LSB_PER_G 16384.0f
#define GYRO_LSB_PER_DPS 131.0f
#define MAG_UT_PER_LSB 0.15f
#define ESP_LOGW(...) ((void)0)
#define pdMS_TO_TICKS(x) (x)
typedef struct {float ax,ay,az,gx,gy,gz,mx,my,mz,temp_c;bool mag_valid;} ls_imu_sample_t;
static ls_imu_sample_t s_cache;
static int64_t s_cache_us,s_mag_us,s_mag_retry_us,now=100000;
static bool s_present=true,s_mag_present=true,ready=true,st1_ok=true,data_ok=true,overflow;
static int restarts;
static int64_t esp_timer_get_time(void){return now;}
static bool bank(int n){(void)n;return true;}
static bool rd(int reg,uint8_t *b,size_t n){(void)reg;memset(b,0,n);return true;}
static bool mag_rd(int reg,uint8_t *b,size_t n){memset(b,0,n);if(reg==MAG_ST1){*b=ready;return st1_ok;}b[0]=100;b[7]=overflow?8:0;return data_ok;}
static bool mag_wr(int reg,int val){(void)val;if(reg==MAG_CNTL3)restarts++;return true;}
static void vTaskDelay(int t){(void)t;}
static void mount_xy(float x,float y,float *sx,float *sy){*sx=x;*sy=y;}
""" + body(source, 'static int16_t be16(') + '\n' + body(source, 'static bool sample_locked(') + r"""
int main(void){
 ls_imu_sample_t s;CHECK(sample_locked(&s) && s.mag_valid);int64_t good=s_mag_us;
 ready=false;now+=100000;CHECK(sample_locked(&s) && s.mag_valid && s_mag_us==good);
 st1_ok=false;now+=100000;CHECK(sample_locked(&s) && s.mag_valid && s_mag_us==good);
 now+=100000;CHECK(sample_locked(&s) && !s.mag_valid && s_mag_us==good);
 now=1100000;CHECK(sample_locked(&s) && !s.mag_valid && restarts==1);
 ready=true;st1_ok=true;now+=100000;CHECK(sample_locked(&s) && s.mag_valid && s_mag_us==now);
 data_ok=false;now+=300000;CHECK(sample_locked(&s) && !s.mag_valid);
 data_ok=true;overflow=true;now+=100000;CHECK(sample_locked(&s) && !s.mag_valid);
 overflow=false;now+=100000;CHECK(sample_locked(&s) && s.mag_valid);return 0;
}
"""
        run_c(harness)

if __name__ == '__main__':
    unittest.main()
