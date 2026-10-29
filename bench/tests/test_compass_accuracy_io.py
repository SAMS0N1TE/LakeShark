"""Compile production boot/storage and ICM driver paths with deterministic bus fakes.

Only these tests are compiled/run; no serial, hardware, or unrelated suites.
"""
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest
from test_audio_reinit import body, ROOT

FIELD = (ROOT / 'components/apps/tui/ls_field.c').read_text()
IMU = (ROOT / 'components/lakeshark/board/ls_imu.c').read_text()
PRE = '''
#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <math.h>
#define CHECK(x) do {if(!(x)){fprintf(stderr,"line %d: %s\\n",__LINE__,#x);return 1;}}while(0)
'''


def run(source, sources=(), compile_only=False):
    with tempfile.TemporaryDirectory(prefix='compass-accuracy-') as tmp:
        p = Path(tmp)
        (p / 'test.c').write_text(source)
        cc = os.environ.get('CC') or shutil.which('gcc')
        if not cc:
            raise RuntimeError('A host C compiler is required (set CC)')
        cmd = [cc, '-std=c11', '-Wall', '-Wextra', '-Werror',
               '-Wno-unused-function', '-Wno-unused-variable', '-Wno-unused-parameter']
        for inc in ['bench/shims', 'components/lakeshark/board',
                    'components/lakeshark', 'components/apps/tui', 'main',
                    'bench/harness', 'components/apps/tui/tuilib', 'components/apps',
                    'components/lakeshark/core', 'components/lakeshark/nfc',
                    'components/lakeshark/radio', 'components/lakeshark/audio',
                    'components/meshcore', 'components/lakeshark/apps/fm', 'components/lakeshark/apps/p25',
                    'components/libcarto/upstream/include',
                    'components/lakeshark/rx', 'components/ls_textpack/include']:
            cmd += ['-I', str(ROOT / inc)]
        cmd += ['-include', 'ls_textpack.h']
        cmd += [str(p / 'test.c')] + [str(ROOT / f) for f in sources]
        cmd += ['-lm', '-o', str(p / 'test.exe')]
        if compile_only:
            # ls_field.c's UTC stamp range-checks every field first (20 chars into
            # 24); gcc's -Wformat-truncation does not follow those checks.
            cmd += ['-c', '-Wno-missing-field-initializers', '-Wno-format-truncation']
        built = subprocess.run(cmd, capture_output=True, text=True)
        if built.returncode:
            raise AssertionError(built.stdout + built.stderr)
        if compile_only:
            return
        result = subprocess.run([str(p / 'test.exe')], cwd=p, capture_output=True, text=True)
        if result.returncode:
            raise AssertionError(result.stdout + result.stderr)
        return result.stdout


class CompassAccuracyIO(unittest.TestCase):
    def test_complete_field_and_screen_translation_units_compile(self):
        run('#define LS_FIELD_TEST 1\n#define _POSIX_C_SOURCE 200809L\n#include "ls_field.c"', compile_only=True)
        run('#include "screens/ext/scr_compass.c"', compile_only=True)

    def test_still_command_duration_missing_data_and_argument_validation(self):
        main = (ROOT / 'main/headless_main.c').read_text()
        branch = body(main, 'if (argc > 1 && !strcmp(argv[1], "still")) {')
        output = run(PRE + '''
#include "ls_compass_live.h"
#include "ls_compass_still.h"
typedef struct {int64_t time_us; bool imu_valid; ls_imu_sample_t imu;} ls_field_sample_t;
static int s_lock=1,mode;static int64_t now;
static int64_t esp_timer_get_time(void){return now;}
#define pdMS_TO_TICKS(x) (x)
static void vTaskDelay(int t){now+=(int64_t)t*1000;}
static void ls_field_compass_snapshot(ls_field_sample_t *p,ls_imu_sample_t *raw){
 memset(p,0,sizeof(*p));p->time_us=mode==1?0:now;p->imu_valid=true;
 p->imu.gx=1;p->imu.gy=2;p->imu.gz=3;*raw=p->imu;
}
static bool ls_field_compass_cal(ls_compass_cal_t *c){(void)c;return false;}
bool ls_compass_solve(const ls_imu_sample_t *p,const ls_compass_cal_t *c,bool *b,ls_compass_reading_t *r){
 (void)p;(void)c;(void)b;memset(r,0,sizeof(*r));r->valid=mode!=2;
 r->magnetic=359.9f;r->field_ut=52;r->dip=66;return r->valid;
}
float ls_compass_steady_sample(ls_compass_steady_t *s,float h,float p,float r,const ls_imu_sample_t *v,float dt){
 (void)p;(void)r;(void)v;(void)dt;s->stationary=true;return h;
}
''' + body(FIELD, 'void ls_field_compass_still(') +
            '\nstatic int command(int argc,char **argv){' + branch + 'return 2;}\n' + '''
int main(void){
 char *ok[]={"compass","still"};CHECK(command(2,ok)==0);
 ls_field_compass_still(5);mode=1;ls_field_compass_still(60);mode=2;ls_field_compass_still(60);
 const char *bad[]={"0","-1","nan","61x","601",""};
 for(int i=0;i<6;i++){char *args[]={"compass","still",(char*)bad[i]};CHECK(command(3,args)==1);}
 return 0;
}
''')
        self.assertEqual(output.count('compass still: PASS'), 1)
        self.assertEqual(output.count('compass still: FAIL'), 3)
        self.assertIn('gyro mean/sd 1.00/0.00 2.00/0.00 3.00/0.00', output)

    def test_live_rejects_endpoint_and_unsafe_slew_without_leaving_a_tail(self):
        run(PRE + '''
#include "ls_compass.h"
#include "ls_compass_learn.h"
typedef struct {ls_imu_sample_t imu;} ls_field_sample_t;
static ls_compass_learn_t s_learn,s_learn_pub;
static int s_profile,s_learn_profile;
static unsigned s_learn_version=1;
static bool s_learn_off,s_learn_reset;
static float s_expected_ut=52,s_expected_dip=66,s_learn_applied[3],s_learn_amps;
static int64_t s_learn_us;
#define LEARN_SLEW_UT 2.0f
static void lock(void){} static void unlock(void){}
static bool ls_gauge_peek(int16_t *ma,int64_t *age){*ma=0;*age=0;return true;}
''' + body(FIELD, 'static void learn_step(') + '''
static void poison(const float f[3],const float c[3]){
 ls_compass_learn_reset(&s_learn,52);s_learn.started=true;s_learn.applied=7;
 s_learn.up[2]=1;memcpy(s_learn.field,f,sizeof(s_learn.field));
 for(int i=0;i<3;i++){s_learn.x[i]=c[i];s_learn.P[i][i]=.25f;s_learn_applied[i]=c[i];}
}
int main(void){
 ls_compass_cal_t cal={.radius=52,.error=.02f};
 ls_field_sample_t p={.imu={.az=-1,.my=-21,.mz=47.5f,.mag_valid=true}};
 const float f[3]={21,0,-47.5f},c[3]={13,0,-29};poison(f,c);
 learn_step(100000,&p,&cal,1,false);
 CHECK(!s_learn.applied);CHECK(s_learn_applied[0]==0 && s_learn_applied[2]==0);
 CHECK(p.imu.my==-21 && p.imu.mz==47.5f);
 /* Endpoint is sound, but the first slewed correction would leave |m| < 80%. */
 const float weak[3]={38,0,0},fix[3]={-14,0,0};poison(weak,fix);
 memset(s_learn_applied,0,sizeof(s_learn_applied));s_expected_dip=NAN;
 p.imu.my=-38;p.imu.mz=0;
 learn_step(200000,&p,&cal,1,false);
 CHECK(!s_learn.applied);CHECK(s_learn_applied[0]==0);CHECK(p.imu.my==-38);
 /* OFF takes effect immediately, even with an old applied vector. */
 poison(f,c);s_learn_off=true;p.imu.my=-21;p.imu.mz=47.5f;
 learn_step(300000,&p,&cal,1,false);CHECK(s_learn_applied[0]==0);CHECK(p.imu.my==-21);
 return 0;
}
''', ['components/apps/tui/ls_compass.c', 'components/apps/tui/ls_compass_learn.c'])

    def test_boot_prefers_good_slot_and_discards_poisoned_learner(self):
        types = FIELD[FIELD.index('typedef struct {\n    ls_compass_cal_t cal;'):
                      FIELD.index('static long s_valid_bytes;')]
        boot = body(FIELD[FIELD.index('static void io_step(void)\n{'):], 'if (!s_loaded || reload) {')
        run(PRE + '#include "ls_compass.h"\n#define EXT_RAM_BSS_ATTR\n' + types + '''
typedef struct {uint32_t magic,cal_version;float d[3],k[3],amps_lo,amps_hi;uint32_t used,sum;} learn_record;
static bool s_loaded, s_learn_discarded[2];
static float expected=NAN; static bool reload;
static const char *s_directory=".";
static void lock(void){} static void unlock(void){} static void load_notes(void){}
''' + body(FIELD, 'static uint32_t checksum(') + body(FIELD, 'static bool path(') +
            '\nstatic void boot(void){' + boot + '}\n' + '''
static void put(int slot, unsigned gen, float radius, float error){
 char name[60];snprintf(name,sizeof(name),"compass-solo%d.cal",slot);
 compass_record r={.magic=0x43414c10u,.generation=gen,.cal={.radius=radius,.error=error}};
 r.sum=checksum(&r,sizeof(r)-sizeof(r.sum));FILE *f=fopen(name,"wb");fwrite(&r,sizeof(r),1,f);fclose(f);
}
int main(void){
 put(0,4,52.6f,.023f);put(1,5,39.6f,.041f);
 learn_record poison={.magic=0x4c524e01u,.cal_version=5,.d={-30,-22.4f,6.3f},.used=3907};
 poison.sum=checksum(&poison,sizeof(poison)-sizeof(poison.sum));
 FILE *f=fopen("compass-solo.learn","wb");fwrite(&poison,sizeof(poison),1,f);fclose(f);
 boot();CHECK(s_profiles[0].version==4);CHECK(fabsf(s_profiles[0].cal.radius-52.6f)<.01f);
 CHECK(s_learn_discarded[0]);
 CHECK(!fopen("compass-solo.learn","rb"));
 /* Slot order and generation must not defeat quality. */
 memset(s_profiles,0,sizeof(s_profiles));s_loaded=false;
 put(0,6,39.6f,.041f);put(1,5,52.6f,.023f);boot();CHECK(s_profiles[0].version==5);
 /* A lower residual in the wrong local field is still rejected. */
 memset(s_profiles,0,sizeof(s_profiles));s_loaded=false;expected=52;
 put(0,10,39.6f,.01f);boot();CHECK(s_profiles[0].version==5);
 /* A late WMM fix reselects an already loaded, lower-error but impossible fit. */
 expected=NAN;s_loaded=false;memset(s_profiles,0,sizeof(s_profiles));boot();CHECK(s_profiles[0].version==10);
 expected=52;reload=true;boot();CHECK(s_profiles[0].version==5);
 return 0;
}
''', ['components/apps/tui/ls_compass.c'])

    def test_icm_config_readback_failures_scale_endian_and_mount(self):
        macros = '\n'.join(line for line in IMU.splitlines() if line.startswith('#define '))
        run(PRE + macros + '''
#define LS_BOARD_IMU_MOUNT_DEG 90
#define LS_BOARD_IMU_I2C_ADDR 0x68
#define LS_I2C_SECONDARY 1
#define ESP_OK 0
#define ESP_ERR_INVALID_STATE 1
#define ESP_ERR_NOT_FOUND 2
#define ESP_LOGW(...) ((void)0)
#define ESP_LOGI(...) ((void)0)
#define pdMS_TO_TICKS(x) (x)
typedef int esp_err_t;
typedef struct {float ax,ay,az,gx,gy,gz,mx,my,mz,temp_c;bool mag_valid;} ls_imu_sample_t;
static int s_sample_lock, s_sample_lock_storage, s_dev;
static bool s_present,s_mag_present;
static uint8_t s_bank=255, selected, regs[4][128], raw[14];
static int fail_reg=-1, fail_bank=-1; static bool corrupt, read_fail;
static ls_imu_sample_t s_cache;static int64_t s_cache_us,s_mag_us,s_mag_retry_us;
static int xSemaphoreCreateMutexStatic(int *p){(void)p;return 1;}
static int ls_i2c_device(int b,int a,int hz,int *d){(void)b;(void)a;(void)hz;*d=1;return 0;}
static void vTaskDelay(int ms){(void)ms;}
static int64_t esp_timer_get_time(void){return 100000;}
static void mag_start(void){}
static bool mag_rd(int r,uint8_t *b,size_t n){(void)r;(void)b;(void)n;return false;}
static bool mag_wr(int r,int v){(void)r;(void)v;return false;}
static bool wr(uint8_t reg,uint8_t val){
 if(selected==fail_bank && reg==fail_reg && !corrupt)return false;
 if(reg==REG_BANK_SEL){selected=val>>4;return true;}
 regs[selected][reg]=val;
 if(reg==B0_PWR_MGMT_1 && val==PWR1_DEVICE_RESET){memset(regs,0,sizeof(regs));regs[0][0]=0xea;selected=0;}
 return true;
}
static bool rd(uint8_t reg,uint8_t *out,size_t n){
 if(read_fail)return false;
 if(selected==0 && reg==B0_ACCEL_XOUT_H){memcpy(out,raw,n);return true;}
 memcpy(out,&regs[selected][reg],n);
 if(corrupt && selected==fail_bank && reg==fail_reg)*out^=2;
 return true;
}
static bool sample_locked(ls_imu_sample_t *out);
static bool ls_imu_read(ls_imu_sample_t *out){return sample_locked(out);}
''' + body(IMU, 'static bool checked_wr(') + body(IMU, 'static bool bank(') +
            body(IMU, 'esp_err_t ls_imu_start(void)') + body(IMU, 'static void mount_xy(') +
            body(IMU, 'static int16_t be16(') + body(IMU, 'static bool sample_locked(') + '''
static void word(int i,int v){raw[i]=(uint16_t)v>>8;raw[i+1]=v&255;}
int main(void){
 regs[0][0]=0xea;CHECK(ls_imu_start()==0);CHECK(s_present);
 CHECK(regs[2][B2_GYRO_CONFIG_1]==0x31 && regs[2][B2_ACCEL_CFG]==0x31);
 CHECK(regs[2][B2_GYRO_SMPLRT]==10 && regs[2][B2_ACCEL_SMPLRT_L]==10);
 word(0,8192);word(2,-4096);word(4,-16384);word(6,1310);word(8,-2620);word(10,3930);
 s_cache_us=0;ls_imu_sample_t p;CHECK(sample_locked(&p));
 CHECK(fabsf(p.ax-.25f)<.0001f && fabsf(p.ay-.5f)<.0001f && p.az==-1);
 CHECK(p.gx==20 && p.gy==10 && p.gz==30);
 read_fail=true;s_cache_us=0;CHECK(!sample_locked(&p));read_fail=false;
 for(int mode=0;mode<2;mode++) for(int reg=0;reg<4;reg++){
  const int addresses[]={B2_GYRO_SMPLRT,B2_GYRO_CONFIG_1,B2_ACCEL_SMPLRT_L,B2_ACCEL_CFG};
  s_present=false;s_bank=255;selected=0;fail_bank=2;fail_reg=addresses[reg];corrupt=mode;
  CHECK(ls_imu_start()!=0);CHECK(!s_present);
 }
 return 0;
}
''')


if __name__ == '__main__':
    unittest.main()
