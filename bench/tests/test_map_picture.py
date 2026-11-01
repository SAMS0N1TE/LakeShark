"""Production picture preparation: 50 ms preview deadline and exact shift geometry."""
from pathlib import Path
import subprocess
root=Path(__file__).resolve().parents[2]
out=root/'bench/build-mapfast';out.mkdir(exist_ok=True)
source=(root/'components/apps/tui/screens/ext/scr_map.c').read_text()
picture=source[source.index('static uint16_t *s_pic, *s_pic_view;'):source.index('/* The outlines under the map')]
code=r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "tuilib/tui_core.h"
#define MALLOC_CAP_SPIRAM 0
#define MALLOC_CAP_8BIT 0
#define LS_TUI_IMAGE_CELL 0
static void *heap_caps_malloc(size_t n,int caps){(void)caps;return malloc(n);}
#define heap_caps_free free
static int64_t now,ready_at;
static int64_t esp_timer_get_time(void){return now;}
static struct {uint32_t ground;} pal={0x123456};
static const void *palette_unused;
#define palette() (&pal)
static uint16_t carto_rgb565(uint32_t c){return ((c>>8)&0xf800)|((c>>5)&0x7e0)|((c>>3)&31);}
static double merc_y(double lat){double r=lat*3.141592653589793/180.;return (1-log(tan(r)+1/cos(r))/3.141592653589793)/2;}
static unsigned worker_serial=1,worker_z=14;
static double worker_lat=43.4445,worker_lon=-71.6473;
static bool ls_carto_map_pixels(double lat,double lon,unsigned z,int w,int h,int tw,int th,bool labels,
    uint16_t *dst,uint32_t *serial,double *flat,double *flon,unsigned *fz){
    (void)tw;(void)th;(void)labels;
    if(now>=ready_at && (worker_lat!=lat || worker_lon!=lon || worker_z!=z)){
        worker_lat=lat;worker_lon=lon;worker_z=z;worker_serial++;
    }
    if(*serial!=worker_serial){for(int i=0;i<w*h;i++)dst[i]=(uint16_t)(i*37+worker_serial);*serial=worker_serial;}
    *flat=worker_lat;*flon=worker_lon;*fz=worker_z;return true;
}
static void ls_tui_image(tui_rect r,const uint16_t *p,int w,int h,uint32_t s){(void)r;(void)p;(void)w;(void)h;(void)s;}
static void ls_tui_image_pan(tui_rect r,const uint16_t *p,int w,int h,uint32_t s,uint32_t e,int32_t x,int32_t y,uint16_t g){
    (void)e;(void)x;(void)y;(void)g;ls_tui_image(r,p,w,h,s);
}
''' + picture + r'''
int main(void){
    double lat=worker_lat,lon=worker_lon,world=256.*(1<<14);
    now=1000000;ready_at=0;assert(picture_prepare(lat,lon,14,690,240,115,27,true));
    uint32_t serial=s_pic_shown;int32_t left=s_pic_left;
    /* Fast completion: never publish the intermediate shifted image. */
    ready_at=now+30000;lon+=90.*360/world;
    assert(picture_prepare(lat,lon,14,690,240,115,27,true));assert(s_pic_shown==serial);
    now+=30000;assert(picture_prepare(lat,lon,14,690,240,115,27,true));
    assert(s_pic_shown==serial+1 && s_pic_left==left+90 && !s_pic_built_dx);
    serial=s_pic_shown;left=s_pic_left;
    /* Slow completion: keep old ground until the deadline, then shift it.
       Fractional world origins exercise round(new)-round(old). */
    ready_at=now+300000;lon-=18.4*360/world;double old_lat=lat;lat+=0.0003;
    assert(picture_prepare(lat,lon,14,690,240,115,27,true));assert(s_pic_shown==serial);
    now+=49000;assert(picture_prepare(lat,lon,14,690,240,115,27,true));assert(s_pic_shown==serial);
    now+=1000;assert(picture_prepare(lat,lon,14,690,240,115,27,true));assert(s_pic_shown==serial+1);
    assert(s_pic_left-left==s_pic_built_dx);
    int dy=(int)(llround(merc_y(lat)*world)-llround(merc_y(old_lat)*world));
    assert(s_pic_built_dy==dy);
    for(int y=0;y<240;y++)for(int x=0;x<690;x++){
        int sx=x+s_pic_built_dx,sy=y+dy;
        uint16_t want=sx>=0 && sx<690 && sy>=0 && sy<240?s_pic[sy*690+sx]:carto_rgb565(pal.ground);
        assert(s_pic_view[y*690+x]==want);
    }
    now=ready_at;assert(picture_prepare(lat,lon,14,690,240,115,27,true));assert(!s_pic_built_dx && !s_pic_built_dy);
    uint32_t epoch=s_pic_epoch;ready_at=now+100000;
    assert(!picture_prepare(lat,lon,15,690,240,115,27,true));
    now=ready_at;assert(picture_prepare(lat,lon,15,690,240,115,27,true));assert(s_pic_epoch!=epoch);
    epoch=s_pic_epoch;pal.ground^=0xffffff;
    assert(picture_prepare(lat,lon,15,690,240,115,27,true));assert(s_pic_epoch!=epoch);
    picture_release();return 0;
}
'''
(out/'test_map_picture.c').write_text(code)
subprocess.run(['gcc','-O2','-I'+str(root/'components/apps/tui'),str(out/'test_map_picture.c'),
                str(root/'components/apps/tui/tuilib/tui_core.c'),'-I'+str(root/'bench/shims'),
                '-I'+str(root/'components/ls_textpack/include'),'-lm','-o',str(out/'test_map_picture.exe')],check=True)
subprocess.run([str(out/'test_map_picture.exe')],check=True)
print('PASS production MAP fast-frame coalescing, 50 ms slow preview, signed pan, exact world rounding and ground fill')
