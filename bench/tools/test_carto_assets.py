"""Real cell upload hash/validation/install helpers with bounded owner stubs."""
from pathlib import Path
import os
import subprocess

root = Path(__file__).resolve().parents[2]
engine = Path(os.environ['CARTOCORE_DIR'])
library = Path(os.environ['CARTOCORE_LIBRARY'])
idf = Path(os.getenv('IDF_PATH', 'C:/esp/v5.5.4/esp-idf'))
mbed = idf / 'components/mbedtls/mbedtls'
out = root / 'bench/build/assets-test'
for directory in ('cells', 'places'):
    (out / 'maps' / directory).mkdir(parents=True, exist_ok=True)
worker = (root / 'components/apps/tui/ls_cells_worker.h').read_text()

def section(start, end):
    return worker[worker.index(start):worker.index(end)]

code = r'''
#define LS_CARTOCORE_HOST 1
#define EXT_RAM_BSS_ATTR
#define MALLOC_CAP_SPIRAM 1
#define MALLOC_CAP_8BIT 2
#define pdTRUE 1
#define pdMS_TO_TICKS(n) (n)
#define CELLS_ROOT "bench/build/assets-test/maps/"
#include <assert.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include "cartocore/cellset.h"
#include "cartocore/places.h"
#include "mbedtls/sha256.h"
#include "ls_carto_sd_io.h"
#include "ls_carto_digest.h"
static int worker_open(const char *path,int flags) { return open(path,flags|O_BINARY); }
#define open worker_open
static int file_lock=1,view_lock=2,held,yields,fail_lock,cleared,closed;
static int xSemaphoreTake(int lock,int ticks) { assert(ticks>0);if(fail_lock==lock)return 0;assert(!(held&lock));held|=lock;return 1; }
static void xSemaphoreGive(int lock) { assert(held&lock);held&=~lock; }
static int64_t esp_timer_get_time(void) { static int64_t now;return now+=10000; }
static void vTaskDelay(int ticks) { assert(ticks==1);yields++; }
static void *heap_caps_malloc(size_t n,int caps) { (void)caps;return malloc(n); }
static void *heap_caps_calloc(size_t n,size_t m,int caps) { (void)caps;return calloc(n,m); }
static void heap_caps_free(void *p) { free(p); }
static struct { cc_cellset *set; } owner,*sd_map;
static size_t sd_map_size;
static bool source_refreshing,map_prepared,map_owned;
static void *shown;
static void release(void *p) { free(p); }
static void map_invalidate_frame(void) { assert(held==3); }
static void frames_clear(void) { assert(held==3);cleared++; }
static void close_sd(void *p) { assert(p==sd_map && held==3);closed++; }
static int cell_open(void *ctx,uint32_t id,void **handle,uint64_t *bytes) { (void)ctx;(void)id;(void)handle;(void)bytes;assert(0);return 0; }
static int cell_read(void *ctx,uint64_t off,void *dst,size_t n) { (void)ctx;(void)off;(void)dst;(void)n;assert(0);return 0; }
static void cell_close(void *ctx) { (void)ctx;assert(0); }
static struct {
    unsigned char buf[65536] __attribute__((aligned(128)));
    char backup[280];int catalog;unsigned generation;
    struct { int fd;uint64_t size; } files[4];
    cc_places places;bool base_open,query_pending;
} cw;
static struct { char query[64]; } cells_work;
static struct { bool active; } dl;
static void cells_init(void) {}
static bool cells_catalog(void) { return true; }
'''
code += section('static FILE *cells_fopen', '#define CELLS_ROOT')
code += section('static uint32_t cells_u32', 'static void cells_message')
code += section('static bool cells_safe_asset', '/* Header: CCMAPS1')
code += section('typedef struct {\n    int fd;uint64_t size;int64_t yielded;', 'static bool cells_matches(const char *path')
code += section('/* Transport-neutral, resumable uploader.', 'static int cells_upload_command')
code += r'''
static bool cancelled(void *ctx) { int *remaining=ctx;return --*remaining>0; }
int main(void) {
    cw.catalog=-1;for(unsigned i=0;i<4;i++)cw.files[i].fd=-1;
    size_t size=8303308;unsigned char *data=calloc(1,size);assert(data);
    memcpy(data,"CTILE1\0\0",8);cells_put32(data+8,7);data[28]=data[29]=14;
    cells_put32(data+32,10000);cells_put32(data+36,CT_CLASS_COUNT-1);cells_put32(data+48,size);
    for(unsigned i=0;i<10000;i++) { unsigned char *p=data+64+i*24;p[0]=14;cells_put32(p+4,i);cells_put32(p+12,240064);cells_put32(p+20,1); }
    unsigned char sha[32];char hex[65];assert(!mbedtls_sha256(data,size,sha,0));
    for(unsigned i=0;i<32;i++)snprintf(hex+2*i,3,"%02x",sha[i]);
    const char *names[]={"cells/overview.ctile","cells/8-76-91.ctile","cells/8-76-92.ctile","cells/8-76-93.ctile"};
    for(unsigned asset=0;asset<4;asset++) {
        assert(cells_upload_begin(names[asset],size,hex));
        FILE *f=fopen(upload.part,"wb");assert(f && fwrite(data,1,size,f)==size && !fclose(f));upload.offset=size;
        owner.set=(cc_cellset*)1;sd_map=&owner;shown=malloc(1);int before=yields;
        assert(cells_upload_end());assert(!sd_map && !shown && !held && !upload.active && yields>before);
        assert(cells_hash(upload.path,sha,size));assert(cells_asset_valid(upload.path,names[asset]));
    }
    assert(cleared==4 && closed==4);
    /* Replacing a same-size file within FAT timestamp resolution cannot reuse its old SHA. */
    struct stat st;assert(!stat(upload.path,&st));assert(carto_digest_cached(upload.path,&st,hex));
    data[size-1]=1;assert(!mbedtls_sha256(data,size,sha,0));for(unsigned i=0;i<32;i++)snprintf(hex+2*i,3,"%02x",sha[i]);
    assert(cells_upload_begin(names[3],size,hex));FILE *f=fopen(upload.part,"wb");assert(f && fwrite(data,1,size,f)==size && !fclose(f));upload.offset=size;
    int remaining=3;assert(!cells_asset_valid_checked(upload.part,names[3],cancelled,&remaining));
    fail_lock=view_lock;assert(!cells_upload_finish(true) && !held && !upload.active);fail_lock=0;
    assert(!stat(upload.part,&st)); /* failed install retains staging and original */
    assert(cells_upload_begin(names[3],size,hex));upload.offset=size;assert(cells_upload_end());
    assert(!stat(upload.path,&st) && carto_digest_cached(upload.path,&st,hex));
    data[0]='X';assert(cells_upload_begin(names[3],size,hex));f=fopen(upload.part,"wb");assert(f && fwrite(data,1,size,f)==size && !fclose(f));upload.offset=size;
    assert(!cells_upload_end() && !upload.active && !held);assert(cells_hash(upload.path,sha,size));
    free(data);puts("PASS: real SHA/CTILE validation, four 8.3 MB assets, retained cell owner release, bounded lock failure/retry, cancellation, stale digest invalidation, bad SHA retains installed asset");
}
'''
(out / 'test.c').write_text(code)
(out / 'config.h').write_text('#define MBEDTLS_SHA256_C\n#define MBEDTLS_PLATFORM_C\n')
subprocess.run(['gcc', '-std=gnu11', '-O2', '-Wall', '-Wextra', '-Werror',
                '-Wno-misleading-indentation', '-Wno-unused-function',
                '-DMBEDTLS_CONFIG_FILE="config.h"', '-I', str(out),
                '-I', str(engine / 'include'), '-I', str(root / 'components/apps/tui'),
                '-I', str(mbed / 'include'), str(out / 'test.c'), str(library),
                str(mbed / 'library/sha256.c'), str(mbed / 'library/platform_util.c'),
                str(mbed / 'library/platform.c'), '-o', str(out / 'test.exe')], check=True)
subprocess.run([str(out / 'test.exe')], cwd=root, check=True)
