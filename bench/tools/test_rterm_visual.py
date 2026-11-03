"""Focused, hardware-free production I/O/V and legacy cells encoder fixture.

python bench/tools/test_rterm_visual.py [gcc] [bench/build/miniz.c]
Requires the host miniz v1.14 used by bench_rterm_cells.py. Builds only this
fixture and lssim; invokes no other tests. Results: bench/build-pixels/.
"""
from pathlib import Path
import binascii
import json
import os
import importlib.util
import struct
import subprocess
import sys
import zlib

root = Path(__file__).resolve().parents[2]
out = root / 'bench/build-pixels'
out.mkdir(exist_ok=True)
if not (root/'bench/build-mapfast/map.rgb565').exists():
    subprocess.run([sys.executable,str(root/'bench/tools/mapfast_bench.py')],check=True)
cc = sys.argv[1] if len(sys.argv) > 1 else 'gcc'
miniz = Path(sys.argv[2] if len(sys.argv) > 2 else root / 'bench/build/miniz.c').resolve()
src = (root / 'components/apps/tui/ls_rterm.c').read_text()
old = subprocess.check_output(['git', 'show', 'HEAD:components/apps/tui/ls_rterm.c'], cwd=root, text=True)
session = src[src.index('typedef struct {'):src.index('} session_t;') + len('} session_t;')]
framing = src[src.index('static uint16_t crc16('):src.index('static void net_close(')]
cells = src[src.index('typedef struct { uint8_t b[512]'):src.index('/* --- input ')]
baseline = old[old.index('static size_t cells_render('):old.index('static void render(')].replace('cells_render(', 'legacy_cells_render(')
capture = src[src.index('static bool visual_stroke('):src.index('static bool grid_buffers(')]
axis = src[src.index('static int visual_axis('):src.index('static void update_want(')]
defines = '#define CONFIG_LS_PANEL_RUNTIME 1\n' + src[src.index('#if CONFIG_LS_PANEL_RUNTIME'):src.index('#define AUTH_US')]
code = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>
#include "ls_vt.h"
#include "ls_tui.h"
#include "ls_braille.h"
#include "ls_map_ink.h"
#define MINIZ_NO_STDIO
#define MINIZ_NO_ARCHIVE_APIS
#define MINIZ_NO_ZLIB_APIS
''' + '#include "' + miniz.as_posix() + '"\n' + defines + r'''
#include "ls_rterm_visual.h"
''' + session + r'''
static int64_t now=10000000;
static int64_t esp_timer_get_time(void) { return now; }
#define MALLOC_CAP_SPIRAM 1
static bool fail_allocation;
static void *heap_caps_malloc(size_t n,int caps) { (void)caps;if(fail_allocation){fail_allocation=false;return NULL;}return malloc(n); }
static void *heap_caps_calloc(size_t n,size_t size,int caps) { (void)caps;return calloc(n,size); }
static void heap_caps_free(void *p) { free(p); }
static session_t s_ser;
static int s_now_cols,s_now_rows,s_cols,s_rows,s_font_i,s_cell_w,s_cell_h;
static tui_cell *s_snap;
static uint8_t *s_glass;
static size_t s_glass_n;
static uint32_t *s_top,*s_bot;
static ls_vt_cell *s_now;
static tui_rect s_img;
static uint16_t s_pal[16];
static uint32_t s_snap_seq,s_snap_epoch=1;
static char *s_frame;
static void (*s_sink)(const char *,size_t);
static tdefl_compressor *s_z;
static char *s_zpay;
static size_t s_zpay_n,s_zin;
static uint8_t wire[4000000];static size_t wire_n;
static void sink(const char *p,size_t n) { assert(wire_n+n<=sizeof(wire));memcpy(wire+wire_n,p,n);wire_n+=n; }
static void raw_sink(void *ctx,const char *p,size_t n) { (void)ctx;sink(p,n); }
static tui_surface surface;
tui_surface *ls_tui_surface(void) { return &surface; }
void ls_tui_geometry(int *c,int *r,int *w,int *h) {if(c)*c=s_cols;if(r)*r=s_rows;if(w)*w=s_cell_w;if(h)*h=s_cell_h;}
void ls_tui_get_origin(int *x,int *y) {if(x)*x=0;if(y)*y=0;}
void ls_tui_put_glass(tui_surface *sf,tui_rect clip,int x,int y,int16_t glyph,uint8_t attr) {
    if(tui_rect_contains(tui_rect_intersect(clip,tui_surface_rect(sf)),x,y)) sf->back[y*sf->w+x]=(tui_cell){glyph,attr};
}
static uint8_t *fixture_masks;
static uint16_t *fixture_glyphs;
static bool *fixture_halos;
static unsigned fixture_count;
static bool fixture_ring,fixture_pan;
static uint32_t fixture_pan_epoch;
static int32_t fixture_pan_left,fixture_pan_top;
bool ls_tui_image_pan_state(uint32_t *epoch,int32_t *left,int32_t *top,uint16_t *ground) {
    *epoch=fixture_pan_epoch;*left=fixture_pan_left;*top=fixture_pan_top;*ground=0;return fixture_pan;
}
static uint32_t fixture_epoch,fixture_columns,fixture_rows;
bool ls_tui_image_ring_state(uint32_t *epoch,uint32_t *columns,uint32_t *rows) {
    *epoch=fixture_epoch;*columns=fixture_columns;*rows=fixture_rows;return fixture_ring;
}
bool ls_tui_glass_mask(uint16_t glyph,uint8_t *dst,bool *halo) {
    for(unsigned i=0;i<fixture_count;i++) if(fixture_glyphs[i]==glyph) {
        memcpy(dst,fixture_masks+(size_t)i*s_cell_w*s_cell_h,(size_t)s_cell_w*s_cell_h);
        *halo=fixture_halos[i];return true;
    }
    memset(dst,0,(size_t)s_cell_w*s_cell_h);*halo=false;return false;
}
''' + axis + capture + framing + cells + baseline + r'''
static void reset(bool pixels,bool vectors,bool z) {
    free(s_ser.shown);free(s_ser.visual.hash);free(s_ser.visual.valid);free(s_ser.visual.columns_valid);
    s_ser=(session_t){.on=true,.cells=true,.z=z,.acks=true,.cfont=-1,.input_epoch=1,.write=sink,
        .shown=calloc((size_t)s_cols*s_rows,sizeof(ls_vt_cell)),.visual={.pixels=pixels,.vectors=vectors}};
    wire_n=0;
}
static void save(const char *dir,const char *name) {
    char path[2048];snprintf(path,sizeof(path),"%s/%s",dir,name);
    FILE *f=fopen(path,"wb");assert(f);assert(fwrite(wire,1,wire_n,f)==wire_n);fclose(f);
}
static void read_fixture(const char *path) {
    FILE *f=fopen(path,"rb");assert(f);char magic[4];uint32_t m[15];
    assert(fread(magic,1,4,f)==4 && !memcmp(magic,"LSP2",4));assert(fread(m,4,15,f)==15);
    s_cols=s_now_cols=m[0];s_rows=s_now_rows=m[1];s_font_i=m[2];s_cell_w=m[3];s_cell_h=m[4];
    s_img=(tui_rect){m[5],m[6],m[7],m[8]};size_t count=(size_t)s_cols*s_rows;
    s_snap=calloc(count,sizeof(*s_snap));s_top=calloc(count,4);s_bot=calloc(count,4);s_now=calloc(count,sizeof(*s_now));
    s_glass_n=(count+7)/8;s_glass=malloc(s_glass_n);s_frame=malloc(4112);
    assert(fread(s_pal,2,16,f)==16);assert(fread(s_snap,sizeof(*s_snap),count,f)==count);assert(fread(s_glass,1,s_glass_n,f)==s_glass_n);
    uint16_t *image=malloc((size_t)m[9]*m[10]*2);assert(fread(image,2,(size_t)m[9]*m[10],f)==(size_t)m[9]*m[10]);
    fixture_count=m[14];fixture_glyphs=calloc(fixture_count,2);fixture_halos=calloc(fixture_count,sizeof(bool));
    fixture_masks=calloc(fixture_count,(size_t)s_cell_w*s_cell_h);
    for(unsigned i=0;i<fixture_count;i++) {
        uint16_t index;assert(fread(&index,2,1,f)==1);assert(fread(fixture_glyphs+i,2,1,f)==1);
        fixture_halos[i]=fgetc(f)!=0;assert(fread(fixture_masks+(size_t)i*s_cell_w*s_cell_h,1,(size_t)s_cell_w*s_cell_h,f)==(size_t)s_cell_w*s_cell_h);
    }
    uint32_t ring[4];assert(fread(ring,4,4,f)==4);
    fixture_ring=ring[0];fixture_epoch=ring[1];fixture_columns=ring[2];fixture_rows=ring[3];
    fclose(f);surface=(tui_surface){.w=s_cols,.h=s_rows,.front=s_snap,.back=s_snap};
    s_visual_pixels=true;
    visual_capture(s_img,image,m[9],m[10],m[11],m[12],m[13],now,false);
    ls_vt_sample_image(s_img,s_cols,image,m[9],m[10],m[11],m[12],s_top,s_bot);
    free(image);
}
int main(int argc,char **argv) {
    assert(argc==3);read_fixture(argv[1]);assert(s_picture.ready && s_picture.w && s_picture.h);
    visual_view options={0};
    assert(visual_options(&options,true,true,"1 1") && options.pixels && options.vectors);
    assert(!visual_options(&options,true,true,"1") && options.pixels && options.vectors);
    assert(visual_options(&options,false,true,"1 1") && !options.pixels && !options.vectors);
    assert(visual_options(&options,true,false,"1 1") && !options.pixels && !options.vectors);
    assert(visual_options(&options,true,true,"0 0") && !options.pixels && !options.vectors);
    uint16_t *input_source=malloc(s_picture.bytes);assert(input_source);memset(input_source,0,s_picture.bytes);
    uint32_t input_generation=s_picture.generation;
    visual_capture(s_img,input_source,s_picture.sw,s_picture.sh,0,0,s_picture.serial+1,now+PICTURE_US,true);
    assert(!s_picture.ready && s_picture.generation==input_generation);free(input_source);s_picture.ready=true;
    int saved_w=s_picture.sw,saved_h=s_picture.sh,saved_x=s_picture.x0,saved_y=s_picture.y0;uint32_t saved_serial=s_picture.serial;
    uint16_t *recovery=calloc((size_t)saved_w*(saved_h+1),2);assert(recovery);memcpy(recovery,s_picture.src,s_picture.bytes);
    fail_allocation=true;now+=PICTURE_US;
    visual_capture(s_img,recovery,saved_w,saved_h+1,saved_x,saved_y,saved_serial+1,now,false);
    assert(s_picture.unavailable && s_picture.ready && !s_picture.src && !s_picture.bytes);
    session_t fallback={.visual={.pixels=true}};size_t first=(size_t)s_img.y*s_cols+s_img.x;
    uint32_t fallback_top=s_top[first];s_top[first]=0xffffff;
    assert(cell_feed_key(&fallback,first,s_img.x,s_img.y).fg==0xffff);s_top[first]=fallback_top;
    now+=PICTURE_US;visual_capture(s_img,recovery,saved_w,saved_h,saved_x,saved_y,saved_serial,now,false);
    assert(!s_picture.unavailable && s_picture.ready && s_picture.src);
    /* Both toggles disabled: byte-for-byte compare with the pre-change
       production G/P/C/X/L encoder, including a delta and L record. */
    tui_cell first_cell=s_snap[0];
    uint8_t *baseline_bytes=malloc(sizeof(wire));
    reset(false,false,false);s_ser.cols=s_cols;s_ser.rows=s_rows;s_ser.scr.full=true;
    cells_render(&s_ser,raw_sink);size_t n=wire_n;memcpy(baseline_bytes,wire,n);
    reset(false,false,false);s_ser.cols=s_cols;s_ser.rows=s_rows;s_ser.scr.full=true;
    legacy_cells_render(&s_ser,raw_sink);assert(wire_n==n && !memcmp(wire,baseline_bytes,n));
    save(argv[2],"legacy.records");
    s_snap_seq=77;s_snap[0].ch='!';wire_n=0;cells_render(&s_ser,raw_sink);n=wire_n;memcpy(baseline_bytes,wire,n);
    s_ser.latency_seq=0;s_ser.shown[0].cp=0;wire_n=0;legacy_cells_render(&s_ser,raw_sink);
    assert(wire_n==n && !memcmp(wire,baseline_bytes,n));s_snap_seq=0;s_snap[0]=first_cell;free(baseline_bytes);
    /* Negotiate a picture, finish its bounded progressive refresh, and save
       the actual deflate/frame/CRC stream for the real page parser. */
    reset(true,false,true);LARGE_INTEGER freq,a,b;QueryPerformanceFrequency(&freq);double us=0;size_t total=0,max=0;unsigned updates=0;
    char path[2048];snprintf(path,sizeof(path),"%s/all.zframes",argv[2]);FILE *all=fopen(path,"wb");assert(all);
    do {
        wire_n=0;s_ser.unacked=0;QueryPerformanceCounter(&a);render(&s_ser,frames_out);QueryPerformanceCounter(&b);
        us+=(b.QuadPart-a.QuadPart)*1e6/freq.QuadPart;total+=wire_n;if(wire_n>max)max=wire_n;
        fwrite(wire,1,wire_n,all);updates++;now+=PICTURE_US;
        assert(updates<3000);
    } while(s_ser.visual.remaining || s_ser.visual.pending_columns || s_ser.visual.masks_hash!=s_picture.masks_hash);
    fclose(all);
    printf("{\"width\":%d,\"height\":%d,\"source_width\":%d,\"source_height\":%d,\"source_bytes\":%zu,\"updates\":%u,\"framed_bytes\":%zu,\"max_update_bytes\":%zu,\"encode_us_total\":%.1f,\"encode_us_mean\":%.1f}\n",s_picture.w,s_picture.h,s_picture.sw,s_picture.sh,s_picture.bytes,updates,total,max,us,us/updates);
    wire_n=0;s_ser.unacked=0;render(&s_ser,frames_out);assert(!wire_n);
    size_t pi=0;while(pi<(size_t)s_cols*s_rows && (cell_key(pi,pi%s_cols,pi/s_cols).cp>>24)!=1) pi++;
    assert(pi<(size_t)s_cols*s_rows);
    uint32_t old_top=s_top[pi],old_bot=s_bot[pi];s_top[pi]^=0xffffff;s_bot[pi]^=0xffffff;
    render(&s_ser,frames_out);assert(!wire_n); /* pixels own animation, not X */
    s_top[pi]=old_top;s_bot[pi]=old_bot;
    /* Scrolling waterfall: newest ring rows, one scroll record, no old-row
       payload. Delay all ACKs and assert new pixels are dropped first. */
    int new_y=(s_picture.y0+s_picture.sh-1)%s_picture.sh;
    for(int x=0;x<s_picture.sw;x++) recovery[(size_t)new_y*s_picture.sw+x]=(uint16_t)(x*319u);
    bool was_ring=fixture_ring;
    if(!fixture_ring) {fixture_ring=true;fixture_epoch++;fixture_columns=fixture_rows=0;}
    fixture_rows++;now+=PICTURE_US;
    visual_capture(s_img,recovery,s_picture.sw,s_picture.sh,s_picture.x0,new_y,s_picture.serial+1,now,false);
    if(was_ring) assert(s_picture.copy_bytes==(uint32_t)s_picture.sw*2);
    s_ser.unacked=1;wire_n=0;render(&s_ser,frames_out);assert(!wire_n);
    s_ser.unacked=0;QueryPerformanceCounter(&a);render(&s_ser,frames_out);QueryPerformanceCounter(&b);
    save(argv[2],"scroll.zframes");
    printf("{\"case\":\"one-row-ring-scroll\",\"framed_bytes\":%zu,\"capture_copy_bytes\":%u,\"encode_us\":%.1f}\n",wire_n,s_picture.copy_bytes,(b.QuadPart-a.QuadPart)*1e6/freq.QuadPart);
    wire_n=0;s_ser.unacked=0;render(&s_ser,frames_out);assert(!wire_n); /* rate cap */
    memcpy(recovery,s_picture.src,s_picture.bytes);
    tui_rect moved=s_img;moved.x++;uint16_t *staged=s_picture.src;now+=PICTURE_US;
    visual_capture(moved,recovery,s_picture.sw,s_picture.sh,s_picture.x0,s_picture.y0,s_picture.serial,now,true);
    assert(!s_picture.ready && s_picture.src==staged && s_picture.rect.x==moved.x);
    s_snap_seq=122;s_ser.unacked=0;wire_n=0;render(&s_ser,frames_out);save(argv[2],"move.zframes");
    assert(s_ser.visual.rect.x==moved.x);
    /* Input sequence is still sent while pixels yield their budget. */
    s_snap_seq=123;wire_n=0;s_ser.unacked=0;now+=PICTURE_US;render(&s_ser,frames_out);assert(wire_n);
    save(argv[2],"input.zframes");
    /* Exercise the actual braille observer and ownership mask, including the
       frame reset and hard 512 segment cap. */
    s_visual_vectors=true;tui_stroke_hook=visual_stroke;
    visual_stroke(0,&surface,tui_surface_rect(&surface),0,0,0,0,0,0,0);
    s_frame_dots=calloc(s_dot_count,sizeof(*s_frame_dots));s_frame_dot_count=s_dot_count;
    ls_braille_glyph_put(&surface,tui_surface_rect(&surface),1,1,0x28ff,15);
    assert(!s_line_count && s_snap[s_cols+1].ch==0x28ff); /* dot text/pictures opt out */
    assert(ls_ink_begin_braille((tui_rect){2,2,8,4}));ls_ink_line(0,0,15,15,15,1,0);
    ls_ink_flush(&surface,(tui_rect){0});assert(s_line_count==1);
    assert(s_lines[0].x0==2*s_cell_w*4+visual_axis(0,2,s_cell_w,true));
    /* Signed legacy sextants must keep the same ownership as their X/C byte. */
    assert(ls_ink_begin((tui_rect){2,2,8,4}));ls_ink_line(0,0,15,5,15,5,0);ls_ink_flush(&surface,(tui_rect){0});
    memcpy(s_frame_dots,s_dots,s_dot_count*sizeof(*s_dots));assert(visual_dot_visible((size_t)2*s_cols+2));
    ls_ink_ellipse(8,6,5,4,15,1,2);assert(s_line_count>2);
    visual_stroke(0,&surface,tui_surface_rect(&surface),0,0,0,0,0,0,0);
    ls_braille_put(&surface,tui_surface_rect(&surface),1,1,0x2847,15);
    assert(s_line_count==1 && s_dots[s_cols+1].bits==0x47);
    for(int i=0;i<600;i++) ls_braille_put(&surface,tui_surface_rect(&surface),1,1,0x2847,15);
    assert(s_line_count==VECTOR_CAP && s_line_dropped==89);
    uint16_t *replacement=malloc(s_picture.bytes);assert(replacement);memcpy(replacement,s_picture.src,s_picture.bytes);
    uint32_t generation=s_picture.generation;
    visual_capture(s_img,replacement,s_picture.sw,s_picture.sh,s_picture.x0,s_picture.y0,s_picture.serial,now,false);
    assert(s_picture.generation!=generation);
    reset(false,true,true);s_snap_seq=0;render(&s_ser,frames_out);save(argv[2],"vectors.zframes");
    for(size_t i=0;i<s_dot_count;i++) {
        s_snap[i]=(tui_cell){0x2847,15};s_frame_dots[i]=(visual_dot){0x2800,15,0x47};
    }
    s_vectors_hash++;now+=PICTURE_US;reset(true,true,true);render(&s_ser,frames_out);save(argv[2],"budget.zframes");
    /* A desk window may be dragged partly off-grid. Keep the original map
       scale/origin and clip only the destination, while legacy C stays C. */
    for(size_t i=0;i<s_dot_count;i++) s_snap[i]=(tui_cell){(int16_t)(char)0xb0,15};
    s_img.w=0;memset(s_glass,0,s_glass_n);s_picture.rect.x=-3;s_picture.rect.y=-2;
    s_picture.masks_n=3;s_picture.masks[0]='O';s_picture.masks[1]=s_picture.masks[2]=0;
    s_picture.masks_hash=visual_hash(s_picture.masks,3);s_picture.generation=++s_picture_generation;
    reset(true,false,true);now+=PICTURE_US;
    snprintf(path,sizeof(path),"%s/crop.zframes",argv[2]);all=fopen(path,"wb");assert(all);
    do {wire_n=0;s_ser.unacked=0;render(&s_ser,frames_out);fwrite(wire,1,wire_n,all);now+=PICTURE_US;}
    while(s_ser.visual.remaining || s_ser.visual.pending_columns);fclose(all);
    int old_x=s_picture.x0;
    for(int y=0;y<s_picture.sh;y++) replacement[(size_t)y*s_picture.sw+old_x]=(uint16_t)(y*115u);
    fixture_columns++;now+=PICTURE_US;
    visual_capture(s_picture.rect,replacement,s_picture.sw,s_picture.sh,(old_x+1)%s_picture.sw,s_picture.y0,s_picture.serial+1,now,false);
    assert(s_picture.copy_bytes==(uint32_t)s_picture.sh*2);
    s_ser.unacked=0;wire_n=0;QueryPerformanceCounter(&a);render(&s_ser,frames_out);QueryPerformanceCounter(&b);
    save(argv[2],"column.zframes");
    printf("{\"case\":\"one-column-ring-scroll\",\"framed_bytes\":%zu,\"capture_copy_bytes\":%u,\"encode_us\":%.1f}\n",wire_n,s_picture.copy_bytes,(b.QuadPart-a.QuadPart)*1e6/freq.QuadPart);
    /* A delayed viewer can miss a full ring revolution: equal origins must
       not make the old rows look current. Use real captures, no delta queue. */
    int origin=s_picture.y0;
    for(int step=0;step<2;step++) {
        int delta=step?s_picture.sh-s_picture.sh/2:s_picture.sh/2;
        int next=(s_picture.y0+s_picture.sh-delta)%s_picture.sh;
        for(int j=0;j<delta;j++) for(int x=0;x<s_picture.sw;x++)
            replacement[(size_t)((next+j)%s_picture.sh)*s_picture.sw+x]=(uint16_t)(x*71u+j*13u+step);
        fixture_rows+=delta;now+=PICTURE_US;
        visual_capture(s_picture.rect,replacement,s_picture.sw,s_picture.sh,s_picture.x0,next,s_picture.serial+1,now,false);
        wire_n=0;s_ser.unacked=1;render(&s_ser,frames_out);assert(!wire_n);
    }
    assert(s_picture.y0==origin);
    do {wire_n=0;s_ser.unacked=0;render(&s_ser,frames_out);now+=PICTURE_US;} while(s_ser.visual.remaining);
    for(int y=0;y<s_picture.sh;y++) assert(s_ser.visual.valid[y] && s_ser.visual.hash[y]==picture_row_hash(y));

    /* Real, contiguous MAP picture: immediate 2D shift, then edge completion.
       Exercise capture and the encoder, not hand-constructed I records. */
    if(s_cols==360) {
        const int pw=690,ph=240;size_t bytes=(size_t)pw*ph*2;
        uint16_t *picture=malloc(bytes),*old_picture=malloc(bytes);assert(picture && old_picture);
        FILE *map=fopen("bench/build-mapfast/map.rgb565","rb");assert(map);
        assert(fread(picture,1,bytes,map)==bytes);fclose(map);memcpy(old_picture,picture,bytes);
        s_img=tui_rect_make(0,1,115,27);
        fixture_ring=false;fixture_pan=true;fixture_pan_epoch=1;fixture_pan_left=1000;fixture_pan_top=2000;
        now+=PICTURE_US;visual_capture(s_img,picture,pw,ph,0,0,1,now,false);
        reset(true,false,true);wire_n=0;QueryPerformanceCounter(&a);render(&s_ser,frames_out);QueryPerformanceCounter(&b);save(argv[2],"pan-initial.zframes");
        printf("{\"case\":\"2d-full-indexed\",\"framed_bytes\":%zu,\"encode_us\":%.1f}\n",wire_n,(b.QuadPart-a.QuadPart)*1e6/freq.QuadPart);
        assert(!s_ser.visual.remaining);
        uint32_t pan_generation=s_picture.generation;tui_rect pan_moved=s_img;pan_moved.x++;
        now+=20000;visual_capture(pan_moved,picture,pw,ph,0,0,1,now,false);
        wire_n=0;s_ser.unacked=0;render(&s_ser,frames_out);save(argv[2],"pan-move.zframes");
        assert(s_ser.visual.picture_serial==pan_generation && !s_ser.visual.remaining);
        now+=20000;visual_capture(s_img,picture,pw,ph,0,0,1,now,false);
        wire_n=0;s_ser.unacked=0;render(&s_ser,frames_out);save(argv[2],"pan-move-back.zframes");
        fixture_pan_left+=90;fixture_pan_top+=16;
        for(int y=0;y<ph;y++)for(int x=0;x<pw;x++)picture[y*pw+x]=(x+90<pw && y+16<ph)?old_picture[(y+16)*pw+x+90]:0;
        now+=20000;visual_capture(s_img,picture,pw,ph,0,0,2,now,true);
        wire_n=0;s_ser.unacked=0;QueryPerformanceCounter(&a);render(&s_ser,frames_out);QueryPerformanceCounter(&b);
        save(argv[2],"pan-preview.zframes");
        printf("{\"case\":\"2d-shift-preview\",\"framed_bytes\":%zu,\"encode_us\":%.1f}\n",wire_n,(b.QuadPart-a.QuadPart)*1e6/freq.QuadPart);
        now+=20000;s_ser.unacked=0;wire_n=0;render(&s_ser,frames_out);assert(!s_ser.visual.remaining);
        for(int y=0;y<ph;y++)for(int x=0;x<pw;x++)if(x+90>=pw || y+16>=ph)picture[y*pw+x]=old_picture[((y+16)%ph)*pw+(x+90)%pw];
        now+=20000;visual_capture(s_img,picture,pw,ph,0,0,3,now,false);
        wire_n=0;s_ser.unacked=0;QueryPerformanceCounter(&a);render(&s_ser,frames_out);QueryPerformanceCounter(&b);
        save(argv[2],"pan-fine.zframes");assert(!s_ser.visual.remaining);
        printf("{\"case\":\"2d-new-strips\",\"framed_bytes\":%zu,\"capture_copy_bytes\":%u,\"encode_us\":%.1f}\n",wire_n,s_picture.copy_bytes,(b.QuadPart-a.QuadPart)*1e6/freq.QuadPart);
        snprintf(path,sizeof(path),"%s/pan-expected.rgb565",argv[2]);map=fopen(path,"wb");assert(map);fwrite(picture,1,bytes,map);fclose(map);
        memcpy(old_picture,picture,bytes);
        for(int y=0;y<ph;y++)for(int x=0;x<pw;x++)picture[y*pw+x]=(x>=12 && y>=8)?old_picture[(y-8)*pw+x-12]:0;
        fixture_pan_left-=12;fixture_pan_top-=8;now+=20000;
        visual_capture(s_img,picture,pw,ph,0,0,4,now,true);
        wire_n=0;s_ser.unacked=0;render(&s_ser,frames_out);save(argv[2],"pan-negative.zframes");
        assert(!s_ser.visual.remaining); /* no pixel payload behind an input scroll */
        /* A viewer missing two snapshots must recover, rather than applying a
           delta to an obsolete picture. Also change the epoch at equal size. */
        for(int j=0;j<2;j++) {
            fixture_pan_left-=12;fixture_pan_top-=8;picture[j]^=0xffff;now+=20000;
            visual_capture(s_img,picture,pw,ph,0,0,5+j,now,false);
        }
        wire_n=0;s_ser.unacked=0;render(&s_ser,frames_out);save(argv[2],"pan-lag.zframes");assert(s_ser.visual.pan_full);
        fixture_pan_epoch++;now+=20000;visual_capture(s_img,picture,pw,ph,0,0,7,now,false);
        wire_n=0;s_ser.unacked=0;render(&s_ser,frames_out);assert(s_ser.visual.pan_full);
        fixture_pan_epoch++;
        for(int y=0;y<ph;y++)for(int x=0;x<pw;x++)picture[y*pw+x]=(uint16_t)(1+((x+y)&1));
        now+=20000;visual_capture(s_img,picture,pw,ph,0,0,8,now,false);
        wire_n=0;s_ser.unacked=0;render(&s_ser,frames_out);save(argv[2],"pan-index4.zframes");
        assert(s_picture.palette_n==3 && s_picture.palette_generation);
        fixture_pan_epoch++;
        for(int y=0;y<ph;y++)for(int x=0;x<pw;x++)picture[y*pw+x]=(uint16_t)(x*239u+y*977u);
        now+=20000;visual_capture(s_img,picture,pw,ph,0,0,9,now,false);
        wire_n=0;s_ser.unacked=0;render(&s_ser,frames_out);save(argv[2],"pan-overflow.zframes");
        assert(s_picture.palette_overflow && !s_picture.palette_generation && !s_ser.visual.remaining);
        fixture_pan=false;s_picture.pan=false;free(picture);free(old_picture);
    }
    free(replacement);
    if(s_cols==360) {
        free(s_picture.src);s_picture.bytes=(size_t)2520*1365*2;s_picture.src=malloc(s_picture.bytes);assert(s_picture.src);
        uint32_t random=1;for(size_t i=0;i<(size_t)2520*1365;i++) {random=random*1664525u+1013904223u;s_picture.src[i]=(uint16_t)(random>>16);}
        s_img=s_picture.rect=(tui_rect){0,0,360,105};s_picture.w=s_picture.sw=2520;s_picture.h=s_picture.sh=1365;
        s_picture.x0=s_picture.y0=0;s_picture.generation=++s_picture_generation;
        s_picture.full_epoch=++s_picture_full_epoch;s_picture.partial_axes=0;s_picture.ring=false;
        reset(true,false,true);us=0;max=0;
        for(int i=0;i<10;i++) {
            now+=PICTURE_US;s_ser.unacked=0;wire_n=0;QueryPerformanceCounter(&a);render(&s_ser,frames_out);QueryPerformanceCounter(&b);
            us+=(b.QuadPart-a.QuadPart)*1e6/freq.QuadPart;if(wire_n>max)max=wire_n;
        }
        save(argv[2],"worst.zframes");
        printf("{\"case\":\"full-native-noise\",\"source_bytes\":%zu,\"max_update_bytes\":%zu,\"encode_us_mean\":%.1f}\n",s_picture.bytes,max,us/10);
    }
    puts("PASS: legacy bytes, bounded rows, unchanged serial, row/column rings including skipped revolution, ACK/input priority, actual braille stroke ownership/cap");
}
'''
(out / 'fixture.c').write_text(code)
subprocess.run([cc, '-std=gnu11', '-O2', '-Wall', '-Wextra', '-UNDEBUG',
                '-I' + str(root / 'components/apps/tui'), '-I' + str(root / 'components/ls_textpack/include'),
                str(out / 'fixture.c'), str(root / 'components/apps/tui/tuilib/tui_core.c'),
                str(root / 'components/apps/tui/ls_vt.c'), str(root / 'components/apps/tui/ls_map_ink.c'),
                '-I' + str(root / 'bench/shims'), '-Wno-misleading-indentation', '-Wno-attributes',
                '-o', str(out / 'fixture.exe')], check=True)
subprocess.run(['cmake', '--build', str(root / 'bench/build'), '--target', 'lssim', '-j', '8'], check=True)

def decode_frames(data):
    parts, records, at = [], [], 0
    while at < len(data):
        start = data[at]
        n = int(data[at+1:at+5], 16)
        assert n <= 4000 and data[at+5+n] == 0x1f
        body = data[at+5:at+5+n]
        assert binascii.crc_hqx(body, 0xffff) == int(data[at+6+n:at+10+n], 16)
        assert start == (0x1c if n else 0x1e)
        if n:
            parts.append(body)
        else:
            records.append(zlib.decompress(b''.join(parts), -15))
            parts = []
        at += n + 10
    assert not parts
    return records

def check_budget(raw):
    """Decode every production record, not a search for marker bytes in RGB."""
    at = extra = 0
    while at < len(raw):
        start = at
        tag = chr(raw[at]); at += 1
        if tag == 'G': at += 7
        elif tag == 'P': at += 32
        elif tag in 'CX':
            n = struct.unpack_from('<H', raw, at + 2)[0]
            at += 4 + n * (3 if tag == 'C' else 8)
        elif tag == 'L': at += 4
        elif tag == 'I':
            op = raw[at]
            n, sw, sh = struct.unpack_from('<HHH', raw, at + 11)
            at += 21 + (n * sw * 2 if op == 1 else n * sh * 2 if op == 4 else 2 if op == 5 else 0)
            if op == 7:
                count=struct.unpack_from('<H',raw,at)[0];assert 0<count<=256;at+=2+count*2
            if op == 6:
                left,width,codec=struct.unpack_from('<HHB',raw,at);at+=5
                assert n==1 and left+width<=sw
                if codec==0:at+=width*2
                elif codec in (2,3):at+=width if codec==2 else (width+1)//2
                else:
                    assert codec==1
                    runs=struct.unpack_from('<H',raw,at)[0];at+=2
                    assert sum(struct.unpack_from('<H',raw,at+i*4)[0] for i in range(runs))==width
                    at+=runs*4
        elif tag == 'O':
            n = struct.unpack_from('<H', raw, at + 1)[0]
            at += 3 + n * (5 + 46)  # fixture font 7x13, packed coverage
        elif tag == 'V':
            n = struct.unpack_from('<H', raw, at)[0]
            assert n <= 512
            at += 2 + n * 10
            count = struct.unpack_from('<H', raw, at)[0]; assert count <= 1024
            at += 2 + count * 6
        else: raise AssertionError((tag, at))
        assert at <= len(raw)
        if tag in 'IOV': extra += at - start
    assert extra <= 524288, extra

results = []
for name, app, grid in [('map', 'map', '160x45'), ('waterfall', 'fm', '160x45'), ('desktop-map', 'map', '360x105'), ('carto-portrait','map','52x70'), ('carto-landscape','map','120x40')]:
    dest = out / name
    dest.mkdir(exist_ok=True)
    env = {k: v for k, v in os.environ.items() if not k.startswith('LSSIM_')}
    env.update(LSSIM_GRID=grid, LSSIM_VISUAL_OUT=str(dest / 'snapshot.bin'))
    if app == 'map':
        env.update(LSSIM_MAP_VIEW='0')
        if name.startswith('carto-'):
            env.update(LSSIM_MAP_VIEW='5',LSSIM_CARTOCORE='picture',LSSIM_CARTO_ZOOM='14')
    else:
        env.update(LSSIM_FM_LR='1', LSSIM_LR_IQ='1')
    command = [str(root / 'bench/build/lssim.exe'), app, '-F', '4', '-f', '8', '-o', str(dest / 'glass.bmp')]
    if app == 'map':
        command += ['-m', str(root / 'bench/fixtures/carto/franklin_z12.pmtiles')]
    else:
        command += ['-k', '3']
    subprocess.run(command, env=env, cwd=root, check=True, capture_output=True)
    run = subprocess.run([str(out / 'fixture.exe'), str(dest / 'snapshot.bin'), str(dest)], check=True, capture_output=True, text=True)
    print(name + ': ' + run.stdout)
    results.append({'case': name, 'measurements': [json.loads(l) for l in run.stdout.splitlines() if l.startswith('{')]})
    records = decode_frames((dest / 'all.zframes').read_bytes())
    for record in records: check_budget(record)
    (dest / 'records.bin').write_bytes(b''.join(records))
    scroll = decode_frames((dest / 'scroll.zframes').read_bytes())
    assert len(scroll) == 1 and scroll[0][:2] == b'I\x02'
    assert b'I' not in decode_frames((dest / 'input.zframes').read_bytes())[0]  # L only
    column = decode_frames((dest / 'column.zframes').read_bytes())
    assert len(column) == 1 and column[0][:2] == b'I\x02' and column[0][22:24] == b'I\x04'
    assert len(column[0]) == 44 + results[-1]['measurements'][0]['source_height'] * 2
    for filename in ['scroll', 'move', 'vectors', 'budget', 'crop', 'column']:
        for record in decode_frames((dest / (filename + '.zframes')).read_bytes()): check_budget(record)
    if name == 'desktop-map':
        for filename in ['pan-initial','pan-move','pan-move-back','pan-preview','pan-fine','pan-negative','pan-lag','pan-index4','pan-overflow']:
            for record in decode_frames((dest/(filename+'.zframes')).read_bytes()):check_budget(record)
        for record in decode_frames((dest / 'worst.zframes').read_bytes()): check_budget(record)
(out / 'measurements.json').write_text(json.dumps(results, indent=2))
(out / 'legacy-screen.js').write_text(subprocess.check_output(
    ['git', 'show', 'HEAD:components/apps/tui/web/screen.js'], cwd=root, text=True))
subprocess.run(['node', str(root / 'bench/tests/test_screen_visual.js'), str(out)], check=True)

# The unchanged USB bridge passes extended capabilities and arbitrary framed
# records. Importing its definitions never creates a Board or opens a port.
spec = importlib.util.spec_from_file_location('lsweb_visual_fixture', root / 'host/lsterm/lsweb.py')
bridge = importlib.util.module_from_spec(spec); spec.loader.exec_module(bridge)
split = bridge.Split()
wire = (out / 'map/scroll.zframes').read_bytes()
got, lines = split.feed(b'term: caps ev1 pix1 vec1\r\n' + wire)
assert got == wire and lines == ['term: caps ev1 pix1 vec1']
assert bridge.BOARD is None
print('PASS USB bridge opaque records and extended capability forwarding; no serial instance')
