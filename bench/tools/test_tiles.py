"""Exercise production catalog/SD merge/SHA verification and render TILES."""
from pathlib import Path
import hashlib
import importlib.util
import json
import os
import shutil
import subprocess
import struct
from PIL import Image

ROOT=Path(__file__).resolve().parents[2]
ENGINE=Path(os.getenv("CARTOCORE_DIR",ROOT.parent/"CartoCore-wip"))
OUT=ROOT/'bench/out/tiles'
OUT.mkdir(parents=True,exist_ok=True)
BUILD=ROOT/'bench/build/tiles'
BUILD.mkdir(parents=True,exist_ok=True)
MAPS=BUILD/'test-maps'
MAPS.mkdir(exist_ok=True)
spec=importlib.util.spec_from_file_location('catalog',ROOT/'tools/carto_catalog.py')
module=importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)
blob=(ROOT/'data/franklin_mini.ctile').read_bytes()
(MAPS/'installed.ctile').write_bytes(blob)
row=module.region(MAPS/'installed.ctile')
assert row['sha256']==hashlib.sha256(blob).hexdigest()
assert row['bytes']==len(blob) and row['bbox']==[v/1e7 for v in struct.unpack_from('<4i',blob,12)]
for name,body in [('short.ctile',blob[:40]),('wrong-size.ctile',blob[:-1]),('bad-magic.ctile',b'INVALID!'+blob[8:])]:
    p=MAPS/name
    p.write_bytes(body)
    try: module.region(p)
    except ValueError: pass
    else: raise AssertionError(name)
    p.unlink()
missing={**row,'file':'absent.ctile'}
partial={**row,'file':'partial.ctile'}
changed={**row,'file':'changed.ctile','sha256':'0'*64}
(MAPS/'partial.ctile.part').write_bytes(blob[:100])
(MAPS/'changed.ctile').write_bytes(blob)
catalog=dict(version=1,regions=[row,missing,partial,changed])
def cstring(value): return json.dumps(json.dumps(value,separators=(',',':')))
backend=(ROOT/'components/apps/tui/ls_tiles_backend.c').read_text()
merge=backend[backend.index('static bool safe_name('):backend.index('static bool catalog_download(')]
merge=merge.replace('/sdcard/maps',MAPS.as_posix())
carto=(ROOT/'components/apps/tui/ls_cartocore.c').read_text()
verify=carto[carto.index('typedef struct {\n    mbedtls_sha256_context ctx;'):carto.index('typedef struct { char url[1024]')]
verify+=carto[carto.index('int ls_carto_verify('):carto.index('#define FETCH_CHUNK')]
idf=Path(os.getenv('IDF_PATH','C:/esp/v5.5.4/esp-idf'))
mbed=idf/'components/mbedtls/mbedtls'
cjson=idf/'components/json/cJSON'
(BUILD/'tiles_mbedtls_config.h').write_text('#define MBEDTLS_SHA256_C\n#define MBEDTLS_PLATFORM_C\n')
bad_catalogs=[{'regions':[{**row,'file':'../escape.ctile'}]}, {'regions':[{**row,'sha256':'oops'}]},
              {'regions':[{**row,'bbox':[2,3,1,4]}]}, {'regions':[{**row,'bytes':64.5}]},
              {'regions':[{**row,'zmin':23}]}, {'regions':[row,row]}, {'regions':[row]*49}]
checks='assert(!parse(&d,\"[[[[[[[[[0]]]]]]]]]\")); assert(!parse(&d,\"[] trailing\"));\n'+'\n'.join(f'assert(!parse(&d,{cstring(b)})); assert(d.count==4);' for b in bad_catalogs)
harness=f'''
#include "ls_tiles.h"
#include "cartocore/ctile.h"
#include "mbedtls/sha256.h"
#include "cJSON.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <dirent.h>
#include <sys/stat.h>
#include <math.h>
#include <ctype.h>
#include <stdatomic.h>
#include <fcntl.h>
#include <unistd.h>
#include <utime.h>
#define open(p,f) open(p,(f)|O_BINARY)
#include "ls_carto_digest.h"
#include "ls_carto_sd_io.h"
#define MALLOC_CAP_INTERNAL 4
#define MALLOC_CAP_DMA 8
#define portMAX_DELAY 0
#define pdMS_TO_TICKS(n) (n)
#define pdTRUE 1
#define MAP_DIR "{MAPS.as_posix()}"
static atomic_bool fetch_running;
static ls_carto_transfer transfer;
static int transfer_mux,file_lock;
static size_t sd_bounce_bytes=32768;
#define portENTER_CRITICAL(m) ((void)(m))
#define portEXIT_CRITICAL(m) ((void)(m))
static int xSemaphoreTake(int m,int t) {{ (void)m; (void)t; return pdTRUE; }}
static void xSemaphoreGive(int m) {{ (void)m; }}
static void *heap_caps_aligned_alloc(size_t a,size_t n,int c) {{ (void)a; (void)c; return malloc(n); }}
static void transfer_header(const uint8_t *h) {{ (void)h; }}
static void transfer_stage(const char *s) {{ (void)s; }}
static void transfer_begin(const char *f,uint64_t n) {{ (void)f; (void)n; }}
static void transfer_progress(uint64_t b,uint64_t n) {{ assert(b<=n); }}
static void transfer_end(bool ok,const char *why) {{ (void)ok; (void)why; }}
#define MALLOC_CAP_SPIRAM 1
#define MALLOC_CAP_8BIT 2
#define ESP_OK 0
#define heap_caps_calloc(n,s,c) calloc(n,s)
#define heap_caps_malloc(n,c) malloc(n)
static atomic_bool transfer_cancel;
static int64_t now_us;
static int cancel_after,hash_yields;
static int64_t esp_timer_get_time(void) {{ now_us+=10000; return now_us; }}
#define heap_caps_free(p) free(p)
void ls_cartocore_register_command(void) {{}}
void ls_carto_recover(void) {{}}
static bool no_sd;
static void vTaskDelay(int n) {{ (void)n; now_us++; hash_yields++; if(cancel_after && hash_yields>=cancel_after) atomic_store(&transfer_cancel,true); }}
#define ESP_FAIL 1
#define ESP_ERR_NOT_FOUND 2
static bool ls_sdcard_mounted(void) {{ return !no_sd; }}
static bool capacity_failed;
static bool ls_sdcard_size(uint64_t *total,uint64_t *free_b) {{
    *total=1000000000; *free_b=500000000; return !capacity_failed;
}}
static void error(ls_tiles_catalog *d,int code,const char *message) {{
    d->last_error=code; snprintf(d->message,sizeof(d->message),"%s",message);
}}
{merge}
{verify}
int main(void) {{
    ls_tiles_catalog d={{0}};
    assert(parse(&d,{cstring(catalog)})); assert(d.count==4);
    {checks}
    char heavy[10000]; strcpy(heavy,"[");
    for(int i=0;i<4500;i++) strcat(heavy,i?",0":"0");
    strcat(heavy,"]"); assert(!parse(&d,heavy));
    scan(&d); assert(d.sd && d.region[0].status==1 && d.region[1].status==0);
    assert(d.region[2].status==2 && d.region[3].status==1);
    assert(!d.region[0].verified && !d.region[3].verified);
    struct stat st; const char *path="{(MAPS/'installed.ctile').as_posix()}";
    assert(!stat(path,&st));
    /* Old size-only and malformed records cannot confer verified status. */
    FILE *side=fopen("{(MAPS/'installed.ctile.sha').as_posix()}","w");
    fprintf(side,"%lld %s\\n",(long long)st.st_size,"{row['sha256']}"); fclose(side);
    scan(&d); assert(!d.region[0].verified);
    assert(!ls_carto_verify(&d.region[0])); scan(&d); assert(d.region[0].verified);
    assert(d.maps=={len(blob)*2+100});
    /* Matching size with changed mtime is installed but unverified. */
    struct utimbuf times={{st.st_atime,st.st_mtime+10}}; assert(!utime(path,&times));
    scan(&d); assert(d.region[0].status==1 && !d.region[0].verified);
    assert(!ls_carto_verify(&d.region[0])); scan(&d); assert(d.region[0].verified);
    /* An on-demand verify must hash even with a matching cache record. */
    FILE *changed_file=fopen(path,"r+b"); assert(changed_file);
    fseek(changed_file,100,SEEK_SET); int byte=fgetc(changed_file);
    fseek(changed_file,100,SEEK_SET); fputc(byte^1,changed_file); fclose(changed_file);
    assert(ls_carto_verify(&d.region[0])); scan(&d); assert(!d.region[0].verified);
    changed_file=fopen(path,"r+b"); fseek(changed_file,100,SEEK_SET); fputc(byte,changed_file); fclose(changed_file);
    now_us=0; hash_yields=0; cancel_after=1;
    assert(ls_carto_verify(&d.region[0])); assert(hash_yields==1);
    scan(&d); assert(!d.region[0].verified); cancel_after=0; atomic_store(&transfer_cancel,false);
    assert(!ls_carto_verify(&d.region[0]));
    assert(ls_carto_verify(&d.region[3])); scan(&d); assert(!d.region[3].verified);
    /* Streaming completion checks digest, header, expected size and cancel. */
    FILE *input=fopen(path,"rb"); uint8_t *body=malloc({len(blob)});
    assert(fread(body,1,{len(blob)},input)=={len(blob)}); fclose(input);
    stream_digest stream; char hex[65]; assert(stream_begin(&stream));
    assert(stream_update(&stream,body,7)); assert(stream_update(&stream,body+7,{len(blob)}-7));
    assert(verify_transfer(&stream,"{row['sha256']}",{len(blob)},{len(blob)},now_us+100000,hex));
    assert(!strcmp(hex,"{row['sha256']}")); mbedtls_sha256_free(&stream.ctx);
    assert(stream_begin(&stream)); assert(stream_update(&stream,body,{len(blob)}));
    assert(!verify_transfer(&stream,"{'0'*64}",{len(blob)},{len(blob)},now_us+100000,hex));
    mbedtls_sha256_free(&stream.ctx); free(body);
    capacity_failed=true; scan(&d); assert(d.sd && d.maps=={len(blob)*2+100} && d.last_error==ESP_FAIL);
    capacity_failed=false;
    no_sd=true; scan(&d); assert(!d.sd && d.maps==0 && !d.total && !d.free);
    puts("PASS: catalog validation/rollback, SD merge, metadata-bound cache, streaming SHA256, fast VERIFY cancellation, no SD");
}}
'''
(BUILD/'test_tiles.c').write_text(harness)
gcc=shutil.which('gcc') or 'C:/mingw64/bin/gcc.exe'
exe=BUILD/'test_tiles.exe'
subprocess.run([gcc,'-std=gnu11','-O2','-Wall','-Wextra','-Werror',
               '-DMBEDTLS_CONFIG_FILE="tiles_mbedtls_config.h"',
               '-I',str(ENGINE/'include'),'-I',str(BUILD),'-I',str(ROOT/'components/apps/tui'),'-I',str(mbed/'include'),'-I',str(cjson),
               str(BUILD/'test_tiles.c'),str(ENGINE/'build/libcartocore.a'),str(cjson/'cJSON.c'),str(mbed/'library/sha256.c'),
               str(mbed/'library/platform_util.c'),str(mbed/'library/platform.c'),'-lm','-o',str(exe)],check=True)
subprocess.run([str(exe)],check=True)

sim=ROOT/'bench/build/lssim.exe'
def render(state,orientation='portrait',extra=(),expected=None,name=None):
    env={**os.environ,'LSSIM_TILES':state}
    bmp=OUT/f'{name or state}-{orientation}.bmp'
    cmd=[str(sim),'tiles','-o',str(bmp),'-d',*extra]
    if orientation=='landscape': cmd.append('-l')
    result=subprocess.run(cmd,env=env,check=True,text=True,capture_output=True)
    if expected: assert expected in result.stdout,(state,expected,result.stdout)
    bmp.with_suffix('.txt').write_text(result.stdout)
    im=Image.open(bmp)
    if orientation=='landscape': im=im.rotate(-90,expand=True)
    im.save(bmp.with_suffix('.png'))
    return result.stdout
for orientation in ('portrait','landscape'):
    for state in ('list','info','download0','download35','download80','download100','receive','verify'):
        text=render(state,orientation,expected='TILES /')
        if state=='receive': assert 'Code: 0123456789abcdef0123456789abcdef' in text
render('list','landscape',extra=('-g',str(ROOT/'bench/fixtures/route.gpx')),expected='TILES /',name='tracks')
render('list',extra=('-x','10:14,~4'),expected='TILES / REGION INFO',name='touch')
render('external-fetch',expected='TILES / TRANSFER',name='console-fetch')
render('external-receive','landscape',expected='TILES / RECEIVE',name='console-receive')
render('list',extra=('-K','down,enter'),expected='Cape Cod',name='keyboard-info')
render('list',extra=('-x','10:14'),expected='Cape Cod',name='touch-info')
render('download35',extra=('-k','b'),expected='Cancel this transfer?',name='cancel-confirm')
render('download35',extra=('-k','by'),expected='Cancelled; partial retained',name='cancelled')
render('list',extra=('-k','o'),expected='TILES OPTIONS',name='options')
render('list',extra=('-k','o','-x','@enter'),expected='REGION',name='region-options')
render('info','landscape',extra=('-F','2','-K','down,down,down,down,down,down'),expected='SHA256',name='large-font-info')
render('download35','landscape',extra=('-T','500','-A','80'),name='performance')
print('PASS: keyboard, touch, OPTIONS, cancellation, portrait/landscape screenshots')
