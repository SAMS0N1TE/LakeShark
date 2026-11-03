"""Compile/test/time the production cells encoder, framing and streaming deflate.

No board or serial port. Requires a host C compiler and public-domain miniz.c
v1.14 (https://github.com/richgel999/miniz/blob/v114/miniz.c). The firmware uses
the compatible ESP ROM tdefl API; these are host timings, not MCU timings.

python bench/tools/bench_rterm_cells.py gcc bench/build/miniz.c bench/build-desk/res.cells
"""
from pathlib import Path
import json
import subprocess
import sys
import zlib

root = Path(__file__).resolve().parents[2]
cc, miniz, snapshot = sys.argv[1:4]
out = root / 'bench/build-rterm-resolution'
out.mkdir(exist_ok=True)
src = (root / 'components/apps/tui/ls_rterm.c').read_text()
session = src[src.index('typedef struct {'):src.index('} session_t;') + len('} session_t;')]
framing = src[src.index('static uint16_t crc16('):src.index('static void net_close(')]
cells = src[src.index('typedef struct { uint8_t b[512]'):src.index('/* --- input ')]
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
#define MINIZ_NO_STDIO
#define MINIZ_NO_ARCHIVE_APIS
#define MINIZ_NO_ZLIB_APIS
''' + '#include "' + Path(miniz).resolve().as_posix() + '"\n' + defines + '\n#include "ls_rterm_visual.h"\n' + session + r'''
static int64_t now = 10000000;
static int64_t esp_timer_get_time(void) { return now; }
#define MALLOC_CAP_SPIRAM 1
static void *heap_caps_malloc(size_t n, int caps) { (void)caps; return malloc(n); }
static void *heap_caps_calloc(size_t n,size_t size,int caps) { (void)caps; return calloc(n,size); }
static void heap_caps_free(void *p) { free(p); }
static session_t s_ser;
static int s_now_cols=360, s_now_rows=105, s_font_i=4, s_cell_w=7, s_cell_h=13;
static tui_cell *s_snap;
static uint8_t *s_glass;
static size_t s_glass_n;
static uint32_t *s_top, *s_bot;
static ls_vt_cell *s_now;
static tui_rect s_img;
static uint16_t s_pal[16];
static uint32_t s_snap_seq=123, s_snap_epoch=1;
static char *s_frame;
static void (*s_sink)(const char *, size_t);
static tdefl_compressor *s_z;
static char *s_zpay;
static size_t s_zpay_n, s_zin;
static uint8_t wire[43008*64+16384];
static size_t wire_n;
static void sink(const char *p, size_t n) { assert(n <= sizeof(wire)-wire_n); memcpy(wire+wire_n,p,n); wire_n+=n; }
void ls_vt_screen_init(ls_vt_screen *s,int c,int r,ls_vt_cell *shown) { (void)c;(void)r;(void)shown; memset(s,0,sizeof(*s)); s->full=true; }
bool ls_vt_screen_resize(ls_vt_screen *s,int c,int r) { (void)s;(void)c;(void)r; return false; }
size_t ls_vt_enter(ls_vt_out_fn f,void *p,int c,int r) { (void)f;(void)p;(void)c;(void)r; assert(false); return 0; }
size_t ls_vt_render(ls_vt_screen *s,const ls_vt_cell *c,ls_vt_out_fn f,void *p) { (void)s;(void)c;(void)f;(void)p; assert(false); return 0; }
''' + framing + cells + r'''
static void reset(bool z) {
    wire_n=0; s_ser=(session_t){.on=true,.cells=true,.z=z,.acks=true,.cfont=-1,
        .shown=calloc(43008,sizeof(ls_vt_cell)),.write=sink,.input_epoch=1};
    assert(s_ser.shown);
}
static void save(const char *dir,const char *name) {
    char path[2048]; snprintf(path,sizeof(path),"%s/%s",dir,name);
    FILE *f=fopen(path,"wb"); assert(f); assert(fwrite(wire,1,wire_n,f)==wire_n); fclose(f);
}
int main(int argc,char **argv) {
    assert(argc==3); assert(sizeof(tui_cell)==4 && sizeof(ls_vt_cell)==12);
    const size_t max=43008;
    s_snap=calloc(max,sizeof(tui_cell));s_top=calloc(max,4);s_bot=calloc(max,4);
    s_glass=calloc((max+7)/8,1);s_frame=malloc(4112);s_now=calloc(max,sizeof(ls_vt_cell));
    assert(s_snap && s_top && s_bot && s_glass && s_frame && s_now);
    FILE *f=fopen(argv[1],"rb");assert(f);assert(fread(s_snap,4,360*105,f)==360*105);fclose(f);
    LARGE_INTEGER freq,a,b;QueryPerformanceFrequency(&freq);
    for(int z=0;z<2;z++) {
        reset(z); render(&s_ser,frames_out); save(argv[2],z?"desk.zframes":"desk.frames");
        size_t bytes=wire_n; QueryPerformanceCounter(&a);
        for(int k=0;k<200;k++) { wire_n=0;s_ser.scr.full=true;s_ser.unacked=0;render(&s_ser,frames_out); }
        QueryPerformanceCounter(&b);
        printf("{\"case\":\"desk\",\"deflate\":%d,\"framed_bytes\":%zu,\"us\":%.3f}\n",z,bytes,(b.QuadPart-a.QuadPart)*1e6/freq.QuadPart/200);
        /* Same cell counts, another font: G clears the page's arrays, so all
           cells must follow it. A palette-only change must finish as well. */
        wire_n=0;s_ser.unacked=0;s_ser.scr.full=false;s_font_i=3;
        s_cell_w=8;s_cell_h=14;render(&s_ser,frames_out);
        assert(wire_n && s_ser.unacked==1);
        save(argv[2],z?"refont.zframes":"refont.frames");
        wire_n=0;s_ser.unacked=0;s_pal[1]^=1;render(&s_ser,frames_out);
        assert(wire_n && s_ser.unacked==1);
        save(argv[2],z?"palette.zframes":"palette.frames");s_pal[1]^=1;
        s_font_i=4;s_cell_w=7;s_cell_h=13;
        /* Flow control holds at least five seconds, then recovers a lost ack. */
        wire_n=0;s_ser.unacked=TERM_AHEAD;s_ser.ack_wait_us=now;
        now+=TERM_ACK_US-1;s_ser.scr.full=true;render(&s_ser,frames_out);assert(!wire_n);
        now++;render(&s_ser,frames_out);assert(wire_n && s_ser.unacked==1);
        free(s_ser.shown);
    }
    /* Max-capacity, incompressible X cells exercise u16 offsets, the serial
       staging bound, >4000-byte chunks, and compressor output callbacks. */
    s_now_cols=384;s_now_rows=112;s_img=tui_rect_make(0,0,384,112);
    uint32_t rng=1;
    for(size_t i=0;i<max;i++) {
        rng=rng*1664525u+1013904223u;s_top[i]=rng&0xffffff;
        rng=rng*1664525u+1013904223u;s_bot[i]=rng&0xffffff;
        s_snap[i]=(tui_cell){0xB0,(uint8_t)rng};
    }
    reset(false);render(&s_ser,frames_out);save(argv[2],"max.frames");size_t raw_n=wire_n;free(s_ser.shown);
    reset(true);render(&s_ser,frames_out);save(argv[2],"max.zframes");
    printf("{\"case\":\"max-X\",\"raw_framed_bytes\":%zu,\"z_framed_bytes\":%zu}\n",raw_n,wire_n);
    return 0;
}
'''
(out / 'bench.c').write_text(code)
subprocess.run([cc, '-std=gnu11', '-O2', '-Wall', '-Wextra',
                '-I' + str(root / 'components/apps/tui'), str(out / 'bench.c'),
                str(root / 'components/apps/tui/tuilib/tui_core.c'),
                '-Wno-misleading-indentation', '-Wno-attributes', '-o', str(out / 'bench.exe')], check=True)
result = subprocess.run([str(out / 'bench.exe'), str(Path(snapshot).resolve()), str(out)],
                        check=True, capture_output=True, text=True)

def payload(name, compressed):
    wire = (out / name).read_bytes()
    parts, at, frames = [], 0, 0
    while at < len(wire):
        start = wire[at]
        n = int(wire[at+1:at+5], 16)
        assert n <= 4000 and wire[at+5+n] == 0x1f
        data = wire[at+5:at+5+n]
        import binascii
        assert binascii.crc_hqx(data, 0xffff) == int(wire[at+6+n:at+10+n], 16)
        assert start == (0x1c if compressed and n else 0x1e)
        if n:
            parts.append(data)
            frames += 1
        else:
            assert at+10 == len(wire)
        at += n+10
    raw = b''.join(parts)
    return zlib.decompress(raw, -15) if compressed else raw, sum(map(len, parts)), frames

for prefix in ['desk', 'max', 'refont', 'palette']:
    raw, _, _ = payload(prefix + '.frames', False)
    decoded, compressed, frames = payload(prefix + '.zframes', True)
    assert raw == decoded
    if prefix in ['desk', 'max']:
        assert raw[0] == ord('G') and raw[8] == ord('P') and raw[-5] == ord('L')
    elif prefix == 'refont':
        assert raw[0] == ord('G') and raw[5:8] == bytes([3,8,14])
        assert raw[41] == ord('C') and int.from_bytes(raw[44:46], 'little') == 360*105
        assert len(raw) == 8+33+5+360*105*3
    else:
        assert raw[0] == ord('P') and len(raw) == 33
    print(json.dumps({'case': prefix, 'raw_bytes': len(raw), 'compressed_bytes': compressed, 'z_frames': frames}))
print(result.stdout, end='')
print('PASS: production C/X/G/P/L encoder, deflate/frame/CRC roundtrip, max grid, ack timeout')
