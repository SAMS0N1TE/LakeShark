"""Run production Wi-Fi storage/dispatch functions against counted NVS fakes.

No firmware build or device access. Extract functions just as test_cell_worker
does, so the polling test exercises the actual dispatch, not a copied policy.
"""
import pathlib
import re
import shutil
import subprocess
import tempfile
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[2]


def function(source, signature):
    start = source.index(signature)
    opening = source.index('{', start)
    depth = 0
    for i in range(opening, len(source)):
        depth += (source[i] == '{') - (source[i] == '}')
        if depth == 0:
            return source[start:i + 1]
    raise ValueError(signature)


class FlashPolling(unittest.TestCase):
    def test_iram_declarations_are_inherited_by_definitions(self):
        compiler = shutil.which('gcc') or shutil.which('cc')
        if not compiler:
            self.skipTest('Host C compiler required')
        with tempfile.TemporaryDirectory(prefix='ls-iram-') as tmp:
            tmp = pathlib.Path(tmp)
            (tmp / 'freertos').mkdir()
            (tmp / 'esp_attr.h').write_text(
                '#define IRAM_ATTR __attribute__((section(".iram1")))\n')
            (tmp / 'freertos/FreeRTOS.h').write_text(
                'typedef int BaseType_t;typedef void *TaskHandle_t;\n')
            (tmp / 'freertos/task.h').write_text('')
            c = tmp / 'test.c'
            c.write_text('''
#include "ls_freertos_cache_safe.h"
void vTaskSuspendAll(void) {}
BaseType_t xTaskResumeAll(void) {return 0;}
TaskHandle_t xTaskGetCurrentTaskHandleForCore(BaseType_t core) {return 0;}
char *pcTaskGetName(TaskHandle_t task) {return 0;}
void vPortYield(void) {}
''')
            assembly = tmp / 'test.s'
            subprocess.run([compiler, '-I' + str(tmp), '-I' + str(ROOT / 'main'),
                            '-ffunction-sections', '-S', str(c), '-o', str(assembly)], check=True)
            symbols = assembly.read_text()
            for name in ('vTaskSuspendAll', 'xTaskResumeAll',
                         'xTaskGetCurrentTaskHandleForCore', 'pcTaskGetName', 'vPortYield'):
                before = symbols[:symbols.index(name + ':')]
                sections = re.findall(r'^\s*\.section\s+([^,\s]+)', before, re.M)
                self.assertEqual('.iram1', sections[-1])

    def test_updates_migration_failure_and_explicit_mutations(self):
        source = (ROOT / 'main/ls_wifi.c').read_text()
        store_source = (ROOT / 'main/ls_wifi_store.inc').read_text()
        compiler = shutil.which('gcc') or shutil.which('cc')
        if not compiler:
            self.skipTest('Host C compiler required')
        enum_start = source.index('typedef enum {\n    WIFI_OP_AP_START')
        enum_end = source.index('} wifi_operation_kind_t;', enum_start) + len('} wifi_operation_kind_t;')
        request_start = source.index('typedef struct {\n    wifi_operation_kind_t kind;')
        request_end = source.index('} wifi_operation_request_t;', request_start) + len('} wifi_operation_request_t;')
        code = r'''
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
typedef int esp_err_t;
typedef int nvs_handle_t;
#define ESP_OK 0
#define ESP_FAIL (-1)
#define ESP_ERR_NOT_FOUND (-2)
#define ESP_ERR_NVS_NOT_FOUND (-3)
#define ESP_ERR_INVALID_STATE (-4)
#define ESP_ERR_INVALID_ARG (-5)
#define NVS_READWRITE 1
#define LS_WIFI_SAVED_MAX 8
#define LS_WIFI_NVS_KEY_SSID "ssid"
#define LS_WIFI_NVS_KEY_PASS "pass"
#define CHECK(x) do {if(!(x)){fprintf(stderr,"%d: %s\n",__LINE__,#x);exit(1);}} while(0)
typedef struct {char ssid[33];uint32_t sequence;} saved_t;
static saved_t s_saved[8], disk[8];
static bool s_survey,s_sta_running=true,s_ap_running,s_sta_connected=true;
static int opens,reads,writes,commits,jobs,drains,legacy,fail_open;
static int s_link_saved_count = -1;
typedef struct {int op;const char *ssid,*pass;char *password;} store_job_t;
static void wipe(void *p,size_t n){memset(p,0,n);}
static void slot_key(char k[3],char prefix,int i){k[0]=prefix;k[1]='0'+i;k[2]=0;}
static int nvs_open_creds(int mode,nvs_handle_t *h){CHECK(mode==NVS_READWRITE);opens++;*h=1;return fail_open?ESP_FAIL:ESP_OK;}
static int read_slots(nvs_handle_t h){reads++;memcpy(s_saved,disk,sizeof(disk));return ESP_OK;}
static int write_slot(nvs_handle_t h,const char *ssid,const char *pass){writes++;snprintf(disk[0].ssid,33,"%s",ssid);disk[0].sequence++;commits++;return ESP_OK;}
static int erase_key(nvs_handle_t h,const char *k){writes++;if(!strcmp(k,"ssid"))legacy=0;else if(k[0]=='s')disk[k[1]-'0'].ssid[0]=0;return ESP_OK;}
static int nvs_get_str(nvs_handle_t h,const char *k,char *p,size_t *n){if(!legacy)return ESP_ERR_NVS_NOT_FOUND;snprintf(p,*n,"%s",!strcmp(k,"ssid")?"old":"password");return ESP_OK;}
static int nvs_commit(nvs_handle_t h){commits++;return ESP_OK;}
static void nvs_close(nvs_handle_t h){}
static int ls_nvs_run(int (*fn)(void*),void *p,unsigned stack){jobs++;return fn(p);}
''' + function(store_source, 'static esp_err_t store_job(void *ctx)') + '\n' + function(source, 'static esp_err_t store_run(int op,') + '\n' + function(source, 'static int saved_list_locked(') + r'''
static void pending_commit_locked(void){drains++;CHECK(store_run(1,"joined","secret",NULL)==ESP_OK);}
static int ap_start_locked(void){return 0;}
static int ap_stop_locked(void){return 0;}
static bool ls_wifi_ssid_valid(const char *s){return s && *s;}
static bool ls_wifi_pass_valid(const char *s){return true;}
static bool s_roam_used,s_roam_pending;
static char s_sta_ssid[33]="joined";
static int sta_join_locked(const char *s,const char *p){return 0;}
static int sta_leave_locked(void){return 0;}
static int sta_forget_locked(const char *s){return store_run(2,s,NULL,NULL);}
static int sta_autojoin_locked(void){return 0;}
static int sta_scan_locked(void *p,int cap,bool passive){return 0;}
static void sta_status_locked(void *p,int cap){store_run(0,NULL,NULL,NULL);}
static void status_locked(void *p,int cap){sta_status_locked(p,cap);}
typedef struct {char ssid[33];int rssi,authmode,primary;uint8_t bssid[6];} wifi_ap_record_t;
typedef struct {char ssid[33];int rssi,channel,auth;bool secure;uint8_t bssid[6];} ls_wifi_scan_ap_t;
#define WIFI_AUTH_OPEN 0
static int esp_wifi_sta_get_ap_info(wifi_ap_record_t *ap){memset(ap,0,sizeof(*ap));return 0;}
''' + source[enum_start:enum_end] + '\n' + source[request_start:request_end] + '\n' + function(source, 'static int dispatch_operation(') + r'''
int main(int argc,char **argv){
    bool migrating=!strcmp(argv[1],"migration");legacy=migrating;fail_open=!strcmp(argv[1],"failure");
    char names[8][33];char status[96];ls_wifi_scan_ap_t ap;
    wifi_operation_request_t r={.kind=WIFI_OP_LIST,.out=names,.capacity=8};
    int first=dispatch_operation(&r);CHECK(first==(fail_open?-1:migrating?1:0));
    int before_jobs=jobs,before_writes=writes,before_commits=commits;
    for(int i=0;i<2000;i++){
        r.kind=WIFI_OP_LIST;r.out=names;r.capacity=8;dispatch_operation(&r);
        r.kind=WIFI_OP_STA_STATUS;r.out=status;r.capacity=96;dispatch_operation(&r);
        r.kind=WIFI_OP_STATUS;dispatch_operation(&r);
        r.kind=WIFI_OP_STA_INFO;r.out=&ap;dispatch_operation(&r);
        r.kind=WIFI_OP_SURVEY;dispatch_operation(&r);
        r.kind=WIFI_OP_SURVEY_BEGIN;dispatch_operation(&r);
    }
    CHECK(jobs==before_jobs && writes==before_writes && commits==before_commits && drains==0);
    CHECK(opens==1);CHECK(!legacy);
    if(fail_open){fail_open=0;CHECK(store_run(1,"recovered","secret",NULL)==0);}
    else{
        r.kind=WIFI_OP_SAVE;r.ssid="new";r.password="secret";CHECK(dispatch_operation(&r)==0);CHECK(drains==1);
        CHECK(!strcmp(s_saved[0].ssid,"new"));
        before_jobs=jobs;CHECK(saved_list_locked(names,8)==1 && !strcmp(names[0],"new"));CHECK(jobs==before_jobs);
        CHECK(store_run(2,"new",NULL,NULL)==0);CHECK(saved_list_locked(names,8)==0);
    }
    return 0;
}
'''
        with tempfile.TemporaryDirectory(prefix='ls-flash-poll-') as tmp:
            c = pathlib.Path(tmp) / 'test.c'
            exe = pathlib.Path(tmp) / 'test.exe'
            c.write_text(code)
            subprocess.run([compiler, '-std=c11', '-Wall', '-Wno-unused-function', '-Wno-unused-variable', str(c), '-o', str(exe)], check=True)
            for scenario in ('empty', 'migration', 'failure'):
                with self.subTest(scenario=scenario):
                    subprocess.run([str(exe), scenario], check=True)


if __name__ == '__main__':
    unittest.main()
