"""Compile the production catalog and ls_panel_start against fake I2C/NVS/DSI.
No device access. Run: python bench/tests/test_nano_panel.py
"""
from pathlib import Path
import subprocess, tempfile
root = Path(__file__).resolve().parents[2]
s = (root/'components/lakeshark/board/ls_panel_ws.c').read_text()
def function(signature):
    start=s.index(signature); brace=s.index('{',start); depth=1; i=brace+1
    while depth:
        depth += (s[i]=='{')-(s[i]=='}'); i+=1
    return s[start:i]
prefix = r'''
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <assert.h>
#define ESP_OK 0
#define ESP_ERR_NOT_FOUND 1
#define ESP_ERR_INVALID_STATE 2
#define ESP_ERR_NO_MEM 3
#define MALLOC_CAP_SPIRAM 0
#include <stdlib.h>
static void *heap_caps_calloc(size_t n, size_t sz, int caps) { (void)caps; return calloc(n, sz); }
#define ESP_LOGI(...) ((void)0)
#define ESP_LOGW(...) ((void)0)
#define ESP_LOGE(...) ((void)0)
#define TMAP_SET 0x80
#define TAG "panel"
typedef int esp_err_t;
typedef struct { bool swap_xy, mirror_x, mirror_y; } ls_panel_ws_touch_map_t;
'''
catalog=s[s.index('typedef enum { CTL_NONE'):s.index('static const ws_panel_t *find_panel')]
stubs=r'''
typedef struct { bool write; char model[12]; uint8_t tmap; uint16_t mbps; } saved_t;
typedef struct { void *on_refresh_done; } esp_lcd_dpi_panel_event_callbacks_t;
static saved_t saved;
static int bridge_error, touch_error, init_error, fb_error, saves, probes;
static uint16_t rx, ry;
static uint16_t *s_frames[2], pixels[2];
static unsigned s_back=1;
static bool s_tried;
static int s_result;
static void *s_panel;
static void *refresh_done;
static const ws_panel_t *s_model;
static ls_panel_ws_touch_map_t s_tmap;
static void saved_read(saved_t *sv) { *sv=saved; }
static int saved_write(saved_t *sv) { saved=*sv; saves++; return 0; }
static int bridge_open(void) { probes++; return bridge_error; }
static int bridge_power_up(const ws_panel_t *p) { assert(p); return 0; }
static int ls_touch_gt911_info(uint8_t *a,char *id,uint16_t *x,uint16_t *y) { *x=rx; *y=ry; return touch_error; }
static int bring_up(const ws_panel_t *p, unsigned mbps) { assert(p); return init_error; }
static int esp_lcd_dpi_panel_get_frame_buffer(void *p,int n,void **a,void **b) { *a=pixels; *b=pixels+1; return fb_error; }
static int esp_lcd_dpi_panel_register_event_callbacks(void *p,const void *cb,void *ctx) { return 0; }
static void release_all(void) {}
static const char *esp_err_to_name(int e) { return "mock"; }
static void reset(void) { memset(&saved,0,sizeof(saved)); bridge_error=touch_error=init_error=fb_error=saves=probes=0; rx=480; ry=800; s_tried=false; s_result=1; s_frames[0]=s_frames[1]=NULL; s_model=NULL; }
'''
tests=r'''
int main(void) {
 reset(); assert(ls_panel_start()==0); assert(strcmp(saved.model,"4a")==0); assert(saves==1); assert(s_model->ctl==CTL_ST7701); assert(s_model->bl_reg==0x96); assert(s_model->mbps==500); assert(ls_panel_start()==0 && saves==1);
 reset(); rx=800; ry=480; assert(ls_panel_start()==0 && saves==1);
 reset(); bridge_error=1; assert(ls_panel_start()==0 && saves==0 && s_model==&VIRTUAL); assert(ls_panel_start()==0 && probes==1);
 reset(); touch_error=1; assert(ls_panel_start()==0 && saves==0 && s_model==&VIRTUAL);
 reset(); rx=123; ry=456; assert(ls_panel_start()==0 && saves==0 && s_model==&VIRTUAL);
 reset(); rx=720; ry=1280; assert(ls_panel_start()==0 && saves==0 && s_model==&VIRTUAL);
 reset(); init_error=2; assert(ls_panel_start()==0 && saves==0 && s_model==&VIRTUAL);
 reset(); strcpy(saved.model,"term"); assert(ls_panel_start()==0 && probes==0 && saves==0 && s_model==&VIRTUAL && s_frames[1]);
 reset(); fb_error=2; assert(ls_panel_start()==2 && saves==0 && !s_frames[1]);
 reset(); strcpy(saved.model,"none"); assert(ls_panel_start()!=0 && probes==0 && saves==0);
 reset(); strcpy(saved.model,"4a"); touch_error=1; assert(ls_panel_start()==0 && probes==0 && saves==0);
 reset(); rx=480; ry=640;
#if CONFIG_ESP32P4_REV_MIN_FULL >= 300
 assert(ls_panel_start()==0 && strcmp(saved.model,"2.8")==0);
#else
 assert(ls_panel_start()==0 && saves==0 && s_model==&VIRTUAL);
#endif
 puts("PASS production panel startup: target, rotated touch, absent, ambiguous, init failure, NVS, headless, remote terminal fallback");
}
'''
with tempfile.TemporaryDirectory(prefix='nano-panel-') as tmp:
    # The table generator and its Waveshare inputs live outside the repo (tools/ is an allowlist);
    # check the tables against them when they are here, and say so when they are not.
    gen=root/'tools/gen_ws_panels.py'
    if gen.exists() and (gen.parent/'vendor').exists():
        regenerated=Path(tmp)/'panels.h'
        subprocess.run(['python',str(gen),str(root.parent/'vendor-ref/ws-components'),str(regenerated)],check=True)
        assert regenerated.read_bytes()==(root/'components/lakeshark/board/ls_panel_ws_tables.h').read_bytes(), 'Panel tables are stale'
    else:
        print('SKIP stale-table check: tools/gen_ws_panels.py and its vendor folder are not in this tree')
    source=Path(tmp)/'test.c'; exe=Path(tmp)/'test.exe'
    source.write_text(prefix+'\n#include "ls_panel_ws_tables.h"\n'+catalog+function('static const ws_panel_t *find_panel')+stubs+function('static esp_err_t virtual_up(void)')+function('esp_err_t ls_panel_start(void)')+tests)
    for revision in [1,300]:
        subprocess.run(['gcc','-std=c11',f'-DCONFIG_ESP32P4_REV_MIN_FULL={revision}','-I'+str(root/'components/lakeshark/board'),str(source),'-o',str(exe)],check=True)
        subprocess.run([str(exe)],check=True)
