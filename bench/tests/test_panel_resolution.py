"""Host-only tests of production terminal sizing and NVS count migration."""
from pathlib import Path
import subprocess
import sys

root = Path(__file__).resolve().parents[2]
src = (root / 'components/lakeshark/board/ls_panel_ws.c').read_text()
out = root / 'bench/build-panel-resolution'
out.mkdir(exist_ok=True)
settings = src[src.index('typedef struct {\n    bool write;'):src.index('/* --------------------------------------------------------------- bridge')]
fit = src[src.index('static void term_fit_pixels('):src.index('static esp_err_t virtual_up(')]
pixels = src[src.index('bool ls_panel_fb('):src.index('void ls_panel_fb_present(void)')]
resize = src[src.index('bool ls_panel_ws_term_resize('):src.index('bool ls_panel_ws_term_view(')]
caps = '\n'.join(s for s in src.splitlines() if s.startswith('#define TERM_'))
pixel = next(s for s in (root / 'components/lakeshark/board/ls_panel_ws.h').read_text().splitlines() if s.startswith('#define LS_TERM_PIXEL_MAX'))
code = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#define taskENTER_CRITICAL(x) ((void)(x))
#define taskEXIT_CRITICAL(x) ((void)(x))
typedef int esp_err_t;
typedef int nvs_handle_t;
#define NVS_NS "ls_panel"
#define ESP_OK 0
#define ESP_ERR_NVS_NOT_FOUND 1
#define NVS_READWRITE 1
#define NVS_READONLY 0
static uint16_t col_value;
static uint8_t row_value;
static int col_type=8, commits;
static int nvs_open(const char *ns,int mode,int *h) { (void)ns;(void)mode;*h=1;return 0; }
static void nvs_close(int h) { (void)h; }
static int nvs_commit(int h) { (void)h;commits++;return 0; }
static int nvs_erase_key(int h,const char *k) { (void)h;(void)k;return 0; }
static int nvs_set_str(int h,const char *k,const char *v) { (void)h;(void)k;(void)v;return 0; }
static int nvs_get_str(int h,const char *k,char *v,size_t *n) { (void)h;(void)k;(void)n;*v=0;return 1; }
static int nvs_set_u16(int h,const char *k,uint16_t v) { (void)h;if(!strcmp(k,"tcols")){col_value=v;col_type=16;}return 0; }
static int nvs_set_u8(int h,const char *k,uint8_t v) { (void)h;if(!strcmp(k,"trows"))row_value=v;return 0; }
static int nvs_get_u16(int h,const char *k,uint16_t *v) { (void)h;if(!strcmp(k,"tcols") && col_type==16){*v=col_value;return 0;}return 1; }
static int nvs_get_u8(int h,const char *k,uint8_t *v) {
    (void)h;if(!strcmp(k,"tcols") && col_type==8){*v=(uint8_t)col_value;return 0;}
    if(!strcmp(k,"trows")){*v=row_value;return 0;}return 1;
}
static int ls_nvs_run(int (*fn)(void *),void *arg,int timeout) { (void)timeout;return fn(arg); }
''' + caps + '\n' + pixel + '\n' + settings + r'''
static struct { uint16_t w,h; } VIRTUAL;
static void *s_model=&VIRTUAL;
static uint16_t s_term_land_w,s_term_land_h;
static uint32_t s_term_wh;
static int s_term_mux;
static uint16_t *s_frames[2];
static const unsigned s_back=1;
typedef struct { uint16_t *pixels; int width,height; } ls_panel_fb_t;
static void geometry(int *w,int *h) { *w=s_term_wh>>16;*h=s_term_wh&65535; }

static int s_term_cols=118,s_term_rows=28,s_term_cw=10,s_term_ch=17;
''' + fit + pixels + resize + r'''
int main(void) {
    saved_t s;
    col_value=200;row_value=60;saved_read(&s);assert(s.tcols==200 && s.trows==60);
    s.tcols=384;s.trows=112;assert(saved_write(&s)==0 && col_type==16);
    saved_read(&s);assert(s.tcols==384 && s.trows==112);
    assert(ls_panel_ws_term_resize(360,105,7,13));
    assert(VIRTUAL.w==2520 && VIRTUAL.h==1365 && col_value==360 && row_value==105);
    assert(s_term_wh==((uint32_t)2520<<16|1365));
    int before=commits;assert(!ls_panel_ws_term_resize(360,105,7,13));assert(commits==before);
    assert(ls_panel_ws_term_resize(384,112,7,13));
    assert(!ls_panel_ws_term_resize(385,112,7,13));
    assert(!ls_panel_ws_term_resize(384,113,7,13));
    assert(!ls_panel_ws_term_resize(384,112,10,17)); // bounded pixel allocation
    ls_panel_ws_term_refont(10,17);
    assert((size_t)VIRTUAL.w*VIRTUAL.h<=LS_TERM_PIXEL_MAX);
    assert(VIRTUAL.w==s_term_cols*10 && VIRTUAL.h==s_term_rows*17);
    assert(ls_panel_ws_term_resize(256,84,10,17)); // 2560x1440, every offered font
    assert(ls_panel_ws_term_resize(320,102,8,14));
    assert(ls_panel_ws_term_resize(365,110,7,13));
    // Identical physical size can still mean a different font and cell grid.
    assert(ls_panel_ws_term_resize(80,14,7,13));
    assert(ls_panel_ws_term_resize(70,13,8,14));assert(VIRTUAL.w==560 && VIRTUAL.h==182);
    // Geometry is available with no pixels; PNG lends an exact-size target.
    ls_panel_fb_t fb;assert(ls_panel_fb(&fb) && !fb.pixels);
    uint16_t target[560*182];
    assert(!ls_panel_ws_term_pixels(target,561,182));
    assert(ls_panel_ws_term_pixels(target,560,182));
    assert(ls_panel_fb(&fb) && fb.pixels==target && fb.width==560 && fb.height==182);
    assert(!ls_panel_ws_term_resize(360,105,7,13)); // cannot resize beneath blitter
    assert(ls_panel_ws_term_pixels(NULL,0,0));
    assert(ls_panel_fb(&fb) && !fb.pixels);
    assert(ls_panel_ws_term_resize(360,105,7,13));
    s_model=NULL;assert(!ls_panel_ws_term_resize(360,105,7,13));
    assert(!ls_panel_ws_term_pixels(target,2520,1365));
    puts("PASS: u16 columns/legacy NVS, exact font metrics, max grid, pixel budget, no-op/glass rejection");
}
'''
(out / 'test.c').write_text(code)
subprocess.run([sys.argv[1], '-std=gnu11', '-O2', '-Wall', '-Wextra', '-Werror',
                str(out / 'test.c'), '-o', str(out / 'test.exe')], check=True)
subprocess.run([str(out / 'test.exe')], check=True)
