"""Compile production NVS record load/save with a bounded in-memory NVS adapter."""
from pathlib import Path
import subprocess
root=Path(__file__).resolve().parents[2]
out=root/'bench/build/carto-saved';out.mkdir(parents=True,exist_ok=True)
src=(root/'components/apps/tui/ls_cartocore.c').read_text()
record=src[src.index('typedef struct {\n    uint32_t version;'):src.index('static int restore_map(void);')]
from carto_nvs_shim import NVS_SHIM
prefix=r"""
#include <assert.h>
#include <stdint.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <math.h>
static void ls_marks_defer_io(void) {}
static double centre_lat=43.4445,centre_lon=-71.6473;
#define pdTRUE 1
static int view_lock=1;
static int xSemaphoreTake(int lock,int ticks) { (void)lock;(void)ticks;return pdTRUE; }
static void xSemaphoreGive(int lock) { (void)lock; }
static int64_t clock_us;
static int64_t esp_timer_get_time(void) { return clock_us; }
"""+NVS_SHIM
main=r"""
int main(void) {
    ls_cartocore_boot_init();assert(saved_loaded && !saved.path[0]);
    strcpy(saved.path,"/sdcard/maps/terrain.ctile");saved.z=12;
    centre_lat=43.5;centre_lon=-71.5;
    unsigned before=nvs_calls;save_view();
    assert(nvs_calls==before && !persisted_size); /* PSRAM carto path is RAM-only. */
    flush_saved();assert(nvs_calls>before && dispatches && !allow_nvs);
    assert(persisted_size==sizeof(carto_saved));
    carto_saved expected=saved;
    memset(&saved,0,sizeof(saved));saved_loaded=false;centre_lat=centre_lon=0;
    ls_cartocore_boot_init();assert(!memcmp(&saved,&expected,sizeof(saved)));
    assert(centre_lat==43.5 && centre_lon==-71.5);
    /* Invalid persisted records must never overwrite defaults/current selection. */
    carto_saved bad=expected;
    for(int kind=0;kind<8;kind++) {
        bad=expected;
        if(kind==0) bad.version=99;
        if(kind==1) bad.z=23;
        if(kind==2) bad.lat=NAN;
        if(kind==3) bad.lon=180;
        if(kind==4) memset(bad.path,'x',sizeof(bad.path));
        if(kind==5) bad.terrain_strength=101;
        if(kind==6) bad.terrain_on=3;
        memcpy(persisted,&bad,sizeof(bad));persisted_size=kind==7?sizeof(bad)-1:sizeof(bad);
        saved=expected;saved_loaded=false;ls_cartocore_boot_init();assert(!memcmp(&saved,&expected,sizeof(saved)));
    }
    memcpy(persisted,&expected,sizeof(expected));persisted_size=sizeof(expected);
    commit_fail=true;strcpy(saved.path,"/sdcard/maps/failed.ctile");save_view();flush_saved();
    assert(!memcmp(persisted,&expected,sizeof(expected)));
    open_fail=true;save_view();clock_us+=5000000;flush_saved();
    assert(atomic_load(&saved_dirty));
    open_fail=commit_fail=false;clock_us+=5000000;flush_saved();assert(!atomic_load(&saved_dirty));
    puts("PASS: no NVS on PSRAM caller; DRAM dispatch/retry, production NVS reboot roundtrip, version/size/coordinate/path/terrain validation, failed commit retention");
}
"""
file=out/'test.c';file.write_text(prefix+record+main)
exe=out/'test.exe'
subprocess.run(['gcc','-std=gnu11','-O2','-Wall','-Wextra','-Werror','-Wno-unused-variable',str(file),'-o',str(exe)],check=True)
subprocess.run([str(exe)],check=True)

# The MAP entry/draw getter must not acquire NVS, even via a safe synchronous call.
settings=(root/'components/lakeshark/core/settings.c').read_text()
map_api=settings[settings.index('uint32_t settings_get_map_layers'):settings.index('/* ADS-B:',settings.index('uint32_t settings_get_map_layers'))]
map_test=r"""
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <stdio.h>
#define ESP_LOGW(...) ((void)0)
static bool s_map_layers_valid,s_nvs_ok=true,s_wq=true;
static uint32_t s_map_layers;
static unsigned queued;
static void sput_u32(const char *key,uint32_t value) {
    assert(!strcmp(key,"map_layers") && value==s_map_layers);queued++;
}
"""+map_api+r"""
int main(void) {
    assert(settings_get_map_layers(42)==42);
    settings_set_map_layers(0x40007ff7);assert(queued==1);
    for(int i=0;i<100;i++) assert(settings_get_map_layers(42)==0x40007ff7);
    assert(queued==1);
    s_wq=false;settings_set_map_layers(0x30007ff7);
    assert(queued==1 && settings_get_map_layers(42)==0x30007ff7);
    puts("PASS: MAP settings entry/draw RAM reads and queued-only style saves");
}
"""
file=out/'map-settings.c';file.write_text(map_test);exe=out/'map-settings.exe'
subprocess.run(['gcc','-std=gnu11','-Wall','-Wextra','-Werror',str(file),'-o',str(exe)],check=True)
subprocess.run([str(exe)],check=True)
