"""Exercise the firmware SD loader against the real CartoCore reader on Windows.

Run: python bench/tools/test_carto_sd.py (requires gcc on PATH).
Only ESP heap/task/display calls and the SD mount path are substituted.
"""
from pathlib import Path
import subprocess
import os
import shutil
import struct
import math
import sys
from carto_nvs_shim import NVS_SHIM

root = Path(__file__).resolve().parents[2]
engine = Path(os.getenv("CARTOCORE_DIR") or root / "components/cartocore/upstream")
library = Path(os.getenv("CARTOCORE_LIBRARY", engine / "build/libcartocore.a"))
out = root / "bench/build/carto-sd-test"
maps = out / "maps"
maps.mkdir(parents=True, exist_ok=True)
for name in ("fetch.ctile", "fetch.ctile.part"):
    (maps / name).unlink(missing_ok=True)
data = (root / "data/franklin_mini.ctile").read_bytes()
(maps / "valid.ctile").write_bytes(data)
(maps / "bad.ctile").write_bytes(b"invalid!" + data[8:])
(maps / "truncated.ctile").write_bytes(data[:-1])
bad_bbox = bytearray(data)
bad_bbox[16:20] = (900000000).to_bytes(4, "little", signed=True)
(maps / "bbox.ctile").write_bytes(bad_bbox)
(maps / "ignore.txt").write_bytes(b"ignored")
region = Path(os.getenv("CARTOCORE_REGION", engine / "data/central_nh_compact.ctile"))
if not region.exists() and not os.getenv("CARTOCORE_REGION"):
    region = engine / "data/franklin_compact.ctile"
if not region.exists() and not os.getenv("CARTOCORE_REGION"):
    # Standalone public bench: real v7 decoding at every streaming-test zoom,
    # using upstream's deterministic geometry/label/terrain fixture, no map download.
    fixture = out / "region-fixture"
    subprocess.run([sys.executable, str(root / "bench/tools/carto_cells_fixture.py"),
                    str(engine / "tests/cells_fixture.py"), str(fixture)], check=True)
    tile = (fixture / "tile.cz7").read_bytes()
    header = bytearray((fixture / "mono.ctile").read_bytes()[:64])
    header[28:30] = bytes((10, 15))
    entries = []
    for zoom in range(10, 16):
        # Cover the host viewport and make this larger than the embedded map,
        # exercising default SD selection and bounded streaming residency.
        scale = 1 << zoom
        cx = int((-71.6473 + 180) / 360 * scale)
        cy = int((1 - math.asinh(math.tan(math.radians(43.4445))) / math.pi) / 2 * scale)
        entries.extend((zoom, x, y) for x in range(cx - 12, cx + 13)
                       for y in range(cy - 12, cy + 13))
    struct.pack_into('<I', header, 32, len(entries))
    offset = 64 + 24 * len(entries)
    directory = bytearray()
    for zoom, x, y in entries:
        directory += struct.pack('<B3xIIQI', zoom, x, y, offset, len(tile))
        offset += len(tile)
    struct.pack_into('<Q', header, 48, offset)
    region = fixture / "streaming.ctile"
    region.write_bytes(header + directory + tile * len(entries))
    print("Using deterministic v7 streaming fixture (zooms 10..15)")
shutil.copyfile(region, maps / "central_nh_compact.ctile")
source = (root / "components/apps/tui/ls_cartocore.c").read_text()
loader = source[source.index('#define MAP_DIR'):source.index('#include "ls_cells_worker.h"')] + source[source.index('static int goto_view('):source.index('/* Detached downloads')]
projection = source[source.index('static int open_map('):source.index('static int cmp_double(')]
prefix = r'''
#define LS_CARTOCORE_HOST 1
#include <assert.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <dirent.h>
#include <sys/stat.h>
#include <errno.h>
#include <math.h>
#include <limits.h>
#include <fcntl.h>
#include <unistd.h>
static void ls_marks_defer_io(void) {}
static int short_reads,interrupt_read,fail_read;
static ssize_t test_read(int fd,void *buf,size_t n) {
    if(interrupt_read) { interrupt_read=0; errno=EINTR; return -1; }
    if(fail_read) { errno=EIO; return -1; }
    if(short_reads && n>997) n=997;
    return read(fd,buf,n);
}
#define read test_read
#include "ls_carto_sd_io.h"
#include "ls_carto_budget.h"
#undef read
#include "cartocore/render.h"
#include "cartocore/ctile.h"
#include "cartocore/cellset.h"
#define EXT_RAM_BSS_ATTR
#define MALLOC_CAP_SPIRAM 1
#define MALLOC_CAP_8BIT 2
#define MALLOC_CAP_INTERNAL 4
#define MALLOC_CAP_DMA 8
#define portMAX_DELAY 0
#define pdMS_TO_TICKS(n) (n)
#define pdTRUE 1
static size_t live_allocations;
static int fail_alloc, render_failure;
static int view_lock, file_lock=1, command_lock=2;
static void map_invalidate_frame(void) {}
static bool retained_source(void);
static atomic_bool view_active,dismiss_pending;
static bool map_owned,map_prepared,source_refreshing;
static bool cells_file_current(uint32_t id,const char *p) { (void)id;(void)p;return true; }
static int publish_locked(const char *,const char *,bool,const char *);
static bool recover_maps(void);
static double centre_lat=43.4445,centre_lon=-71.6473;
static const uint8_t *mini_start,*mini_end;
typedef struct { int cols,rows; unsigned z; int64_t left,top; } render_state;
static render_state *shown;
static void *view;
static void *heap_caps_aligned_alloc(size_t align,size_t size,int caps) {
    assert((align==16 && caps==3) || (align==64 && (caps==14 || caps==3))); if(fail_alloc) return NULL;
    void *p=malloc(size); if(p) live_allocations++; return p;
}
static void *heap_caps_calloc(size_t n,size_t size,int caps) {
    (void)caps; if(fail_alloc) return NULL;
    void *p=calloc(n,size); if(p) live_allocations++; return p;
}
static void *heap_caps_malloc(size_t n,int caps) { return heap_caps_calloc(1,n,caps); }
static void heap_caps_free(void *p) { if(p) { assert(live_allocations); live_allocations--; free(p); } }
static void release(render_state *s) { free(s); }
static void frames_clear(void) { heap_caps_free(view);view=NULL; }
static unsigned held_locks;
static int xSemaphoreTake(int lock,int wait) {
    (void)wait; assert(lock>=0 && lock<=2);
    assert(!(held_locks & (1u<<lock))); /* Nonrecursive mutexes, like FreeRTOS. */
    held_locks |= 1u<<lock; return pdTRUE;
}
static void xSemaphoreGive(int lock) { assert(held_locks & (1u<<lock)); held_locks &= ~(1u<<lock); }
static int64_t esp_timer_get_time(void) { static int64_t t; return t+=1000; }
static int redraw(render_state *s,const char *action) { (void)s; (void)action; return render_failure; }
static const char *host_path(const char *path) {
    static char buf[1024]; assert(!strncmp(path,"/sdcard/",8));
    snprintf(buf,sizeof(buf),"bench/build/carto-sd-test/%s",path+8); return buf;
}
static int sd_open(const char *p,int flags) { return open(host_path(p),flags|O_BINARY); }
static int sd_stat(const char *p,struct stat *s) { return stat(host_path(p),s); }
static DIR *sd_opendir(const char *p) { return opendir(host_path(p)); }
static int fail_publish,fail_rollback;
static int sd_unlink(const char *p) { return unlink(host_path(p)); }
static int sd_rename(const char *a,const char *b) {
    /* Digest sidecars are never retained map sources. */
    assert(strstr(a,".sha") || !retained_source());
    if((fail_publish && strstr(a,".part")) || (fail_rollback && strstr(a,".bak"))) { errno=EIO; return -1; }
    char x[1024]; snprintf(x,sizeof(x),"%s",host_path(a)); return rename(x,host_path(b));
}
#define unlink sd_unlink
#define rename sd_rename
#define open sd_open
#define stat(p,s) sd_stat(p,s)
#define opendir sd_opendir
'''
prefix += r'''
static FILE *cache_fopen(const char *p,const char *m) { return fopen(host_path(p),m); }
#define fopen cache_fopen
#include "ls_carto_digest.h"
#undef fopen
'''
main = r'''
#undef open
#undef stat
#undef opendir
#undef unlink
#undef rename
int main(void) {
    FILE *f=fopen("data/franklin_mini.ctile","rb"); assert(f);
    fseek(f,0,SEEK_END); size_t size=ftell(f); rewind(f);
    uint8_t *embedded=malloc(size); assert(fread(embedded,1,size,f)==size); fclose(f);
    mini_start=embedded; mini_end=embedded+size;
    assert(!prepare_source(0,0)); /* Fresh NVS, outside mini: largest valid SD map. */
    assert(sd_map && strstr(saved.path,"central_nh_compact.ctile"));
    assert(!nvs_calls && atomic_load(&saved_dirty)); /* actual select_map -> save_view */
    flush_saved();assert(nvs_calls>0 && dispatches && !allow_nvs);
    close_hidden();saved.path[0]=0;saved_valid=false;

    cc_ctile map; assert(open_map(&map));
    assert(!select_map("valid.ctile")); assert(sd_map && sd_map_size==size);
    assert(open_map(&map) && map.source==&sd_map->source);
    uint8_t *copy=malloc(size); assert(copy);
    for(int direct=0;direct<2;direct++) {
        void *bounce=sd_map->bounce; if(direct) sd_map->bounce=NULL;
        for(int partial=0;partial<2;partial++) {
            short_reads=partial; interrupt_read=partial;
            assert(sd_read(sd_map,0,copy,size) && !memcmp(copy,embedded,size));
            const size_t offsets[]={1,511,512,513,size-1,size};
            for(unsigned i=0;i<sizeof(offsets)/sizeof(offsets[0]);i++) {
                size_t off=offsets[i],n=size-off; if(n>65539) n=65539;
                assert(sd_read(sd_map,off,copy,n) && !memcmp(copy,embedded+off,n));
            }
        }
        fail_read=1; assert(!sd_read(sd_map,0,copy,512)); fail_read=0;
        assert(!sd_read(sd_map,size,copy,1)); assert(!sd_read(sd_map,size+1,copy,0));
        sd_map->bounce=bounce;
    }
    short_reads=0; free(copy);
    puts("PASS: DMA bounce/direct byte equality, unaligned boundaries, EOF, short reads, EINTR, I/O failure");
    double lat=(bbox_degrees(sd_map->source.header+16)+bbox_degrees(sd_map->source.header+24))/2;
    double lon=(bbox_degrees(sd_map->source.header+12)+bbox_degrees(sd_map->source.header+20))/2;
    assert(centre_lat==lat && centre_lon==lon);
    assert(ls_carto_unlink("/sdcard/maps/valid.ctile")==-1 && errno==EBUSY);
    assert(ls_carto_unlink("/sdcard/maps/VALID~1.CTI")==-1 && errno==EBUSY);
    assert(publish_map("/sdcard/maps/other.ctile.part","/sdcard/maps/other.ctile",true,"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa")==-1 && errno==ENOENT);
    sd_source *active=sd_map;
    const char *bad[]={"bad.ctile","truncated.ctile","bbox.ctile","missing.ctile","/spiffs/valid.ctile","../valid.ctile","ignore.txt"};
    for(size_t i=0;i<sizeof(bad)/sizeof(*bad);i++) {
        assert(select_map(bad[i])); assert(sd_map==active && live_allocations==4);
    }
    assert(!select_map("/maps/valid.ctile")); assert(live_allocations==4);
    fail_alloc=1; active=sd_map; assert(select_map("valid.ctile")); assert(sd_map==active); fail_alloc=0;
    assert(!map_directory(NULL,0));
    /* Default open promises the first entry, which may itself be invalid. */
    remove("bench/build/carto-sd-test/maps/bad.ctile");
    remove("bench/build/carto-sd-test/maps/truncated.ctile");
    remove("bench/build/carto-sd-test/maps/bbox.ctile");
    assert(!select_map(NULL)); assert(live_allocations==4);
    assert(!goto_view(42,-72)); assert(centre_lat==42 && centre_lon== -72);
    shown=calloc(1,sizeof(*shown)); shown->cols=52; shown->rows=70; shown->z=14;
    view=heap_caps_aligned_alloc(16,16,3); atomic_store(&view_active,true);
    assert(!goto_view(43,-71));
    int64_t left,top; origin(52,70,14,&left,&top); left-=left%2; top-=top%4;
    assert(shown->left==left && shown->top==top);
    render_failure=1; assert(goto_view(40,-70));
    assert(centre_lat==43 && centre_lon== -71 && shown->left==left && shown->top==top);
    assert(!select_map("embedded")); assert(!sd_map && !shown && !view && !live_allocations);
    assert(!atomic_load(&view_active) && open_map(&map) && map.bytes.data==embedded);
    assert(centre_lat==43.4445 && centre_lon== -71.6473);
    assert(!select_map("central_nh_compact.ctile")); struct stat compact_st; assert(!stat(host_path("/sdcard/maps/central_nh_compact.ctile"),&compact_st));
    assert(sd_map_size==(size_t)compact_st.st_size);
    assert(sd_map->source.resident);
    /* v7 decodes directly from the reader; older formats stage whole blobs. */
    assert(sd_map->source.version==7 ? sd_map->source.staging_capacity==1 :
           sd_map->source.max_blob<=sd_map->source.staging_capacity);
    assert(open_map(&map));
    carto_budget planned;
    assert(carto_plan_budget(20890868,20890868,128u*1024u,false,false,true,true,&planned));
    assert(planned.cold==2u*CARTO_MIB && planned.decoded==2u*CARTO_MIB && planned.idle==2u*CARTO_MIB);
    size_t source_bytes=sizeof(*sd_map)+sd_map->directory_bytes+sd_map->staging_bytes;
    /* Actual grid/publication overhead is covered by test_carto_psram. */
    assert(planned.cold+planned.decoded+planned.idle+source_bytes<=13u*CARTO_MIB/2u);
    void *arena_storage=malloc(2*1024*1024),*cache_storage=malloc(2*1024*1024);
    cc_cell *cells=malloc(240*67*sizeof(*cells)); cc_arena arena; cc_renderer renderer; cc_decoded_cache cache;
    cc_arena_init(&arena,arena_storage,2*1024*1024);
    assert(cc_renderer_init(&renderer,&arena,240,67,cells));
    assert(cc_decoded_cache_init(&cache,cache_storage,2*1024*1024)); renderer.decoded_cache=&cache; renderer.colors16=1;
    centre_lat=43.4445; centre_lon=-71.6473;
    for(unsigned z=10;z<=15;z++) {
        cc_renderer_invalidate(&renderer); origin(240,67,z,&left,&top);
        assert(cc_render(&renderer,&map,z,left,top,CC_QUADRANT,1,cells));
        size_t reads=sd_map->source.blob_reads;
        cc_renderer_invalidate(&renderer);
        assert(cc_render(&renderer,&map,z,left,top,CC_QUADRANT,1,cells));
        assert(sd_map->source.blob_reads==reads);
        printf("PASS central NH z%u arena_peak=%zu decoded=%zu warm reads=0\n",z,arena.peak,cache.used);
    }
    free(cells); free(cache_storage); free(arena_storage);
    assert(!select_map("embedded")); assert(!live_allocations);
    /* Re-download closes borrowed state before rename and reopens selection. */
    assert(!select_map("valid.ctile"));
    FILE *pending=fopen(host_path("/sdcard/maps/valid.ctile.part"),"wb"); assert(pending);
    assert(fwrite(embedded,1,size,pending)==size); fclose(pending);
    assert(publish_map("/sdcard/maps/valid.ctile.part","/sdcard/maps/valid.ctile",false,"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa")==-1 && errno==EEXIST);
    FILE *legacy=fopen(host_path("/sdcard/maps/valid.ctile.pending"),"wb");assert(legacy);
    assert(fwrite(embedded,1,size,legacy)==size);fclose(legacy);
    struct stat legacy_st;assert(!stat(host_path("/sdcard/maps/valid.ctile.pending"),&legacy_st));
    assert(carto_digest_write("/sdcard/maps/valid.ctile.pending",&legacy_st,"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"));
    shown=calloc(1,sizeof(*shown)); map_owned=map_prepared=true;
    view=heap_caps_aligned_alloc(16,16,3);
    centre_lat=42;centre_lon=-72;
    assert(publish_map("/sdcard/maps/valid.ctile.part","/sdcard/maps/valid.ctile",true,"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa")==0);
    assert(sd_map && !shown && !view && !map_owned && !map_prepared);
    assert(live_allocations==4 && centre_lat==42 && centre_lon==-72);
    uint8_t reopened[64];assert(sd_read(sd_map,0,reopened,64) && !memcmp(reopened,embedded,64));
    assert(access(host_path("/sdcard/maps/valid.ctile.part"),F_OK));
    assert(access(host_path("/sdcard/maps/valid.ctile.pending"),F_OK));
    /* Failed swap restores and reopens the original; the next retry succeeds. */
    pending=fopen(host_path("/sdcard/maps/valid.ctile.part"),"wb");assert(pending);
    assert(fwrite(embedded,1,size,pending)==size);fclose(pending);
    fail_publish=1;
    assert(publish_map("/sdcard/maps/valid.ctile.part","/sdcard/maps/valid.ctile",true,"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa")==-1);
    assert(sd_map && live_allocations==4 && sd_read(sd_map,0,reopened,64));
    fail_publish=0;
    assert(!publish_map("/sdcard/maps/valid.ctile.part","/sdcard/maps/valid.ctile",true,"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"));
    close_hidden();
    assert(!saved.path[0] || strstr(saved.path,"valid.ctile"));
    assert(access(host_path("/sdcard/maps/valid.ctile.pending"),F_OK));
    assert(!restore_map() && sd_map && strstr(active_file,"valid.ctile"));
    assert(!select_map("embedded"));
    strcpy(saved.path,"/sdcard/maps/missing.ctile");
    assert(!restore_map() && sd_map && open_map(&map));
    close_hidden();
    saved.path[0]=0;
    puts("PASS: active re-download closes readers, frees borrowed state, swaps, reopens, preserves centre; rollback and retry");
    /* Production publication: force gate, rollback and reboot recovery. */
    const char *dest="/sdcard/maps/publish.ctile",*part="/sdcard/maps/publish.ctile.part";
    FILE *old=fopen(host_path(dest),"wb"); assert(old); fputs("old",old); fclose(old);
    FILE *next=fopen(host_path(part),"wb"); assert(next); fputs("new",next); fclose(next);
    assert(publish_map(part,dest,false,"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa")==-1 && errno==EEXIST);
    fail_publish=1; assert(publish_map(part,dest,true,"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa")==-1);
    old=fopen(host_path(dest),"rb"); assert(old && fgetc(old)=='o'); fclose(old);
    fail_rollback=1; assert(publish_map(part,dest,true,"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa")==-1);
    assert(access(host_path(dest),F_OK));
    fail_publish=fail_rollback=0; ls_carto_recover();
    old=fopen(host_path(dest),"rb"); assert(old && fgetc(old)=='o'); fclose(old);
    old=fopen(host_path("/sdcard/maps/publish.ctile.sha"),"wb"); fputs("stale",old); fclose(old);
    assert(!publish_map(part,dest,true,"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"));
    old=fopen(host_path(dest),"rb"); assert(old && fgetc(old)=='n'); fclose(old);
    assert(!access(host_path("/sdcard/maps/publish.ctile.sha"),F_OK));
    old=fopen(host_path("/sdcard/maps/publish.ctile.bak"),"wb"); fputs("old",old); fclose(old);
    ls_carto_recover(); assert(access(host_path("/sdcard/maps/publish.ctile.bak"),F_OK));
    remove(host_path(dest));
    puts("PASS: alias protection, force gate, publication failure rollback, failed rollback boot recovery, stale SHA invalidation");
    assert(!held_locks); free(embedded); puts("PASS: real-reader SD load, failure retention, streaming size independence, bbox, goto rollback, embedded cleanup");
    return 0;
}
'''
harness = out / "test.c"
storage=source[source.index('typedef struct {\n    int fd;'):source.index('static double centre_lat=')]
record=source[source.index('typedef struct {\n    uint32_t version;'):source.index('static int restore_map(void);')]
harness.write_text(prefix + NVS_SHIM + record + "#define fopen cache_fopen\n#include \"ls_cartocore_select.h\"\n" + storage + 'static bool retained_source(void) { return sd_map!=NULL; }\n' + projection + loader + "#undef fopen\n" + main)
exe = out / "test.exe"
subprocess.run(["gcc", "-std=gnu11", "-O2", "-Wall", "-Wextra", "-Werror", "-Wno-unused-function", "-Wno-unused-variable",
                "-ffunction-sections", "-fdata-sections", "-Wl,--gc-sections",
                "-I", str(engine / "include"), "-I", str(root / "components/apps/tui"), str(harness),
                str(library),
                "-lm", "-o", str(exe)], cwd=root, check=True)
subprocess.run([str(exe)], cwd=root, check=True)

# Exercise benchmark checksums and the failure guard with the production reader.
bench_prefix = prefix + NVS_SHIM + record + '#define fopen cache_fopen\n#include "ls_cartocore_select.h"\n#undef fopen\n' + r"""

static void vTaskDelay(int ticks) { (void)ticks; }
static FILE *bench_fopen(const char *p,const char *mode) { return fopen(host_path(p),mode); }
#define fopen bench_fopen
"""
bench_main = r"""
#undef fopen
#undef open
#undef stat
#undef opendir
int main(void) {
    assert(!sdbench(1)); assert(!live_allocations);
    size_t selected=sd_bounce_bytes;
    fail_read=1; assert(sdbench(1)); assert(!live_allocations);
    assert(sd_bounce_bytes==selected);
    puts("PASS: sdbench verifies every path, rejects I/O errors, keeps selection on failure, frees buffers");
    return 0;
}
"""
bench_harness=out / "sdbench.c"
bench_harness.write_text(bench_prefix + storage + 'static bool retained_source(void) { return sd_map!=NULL; }\n' + projection + loader +
                        (root / "components/apps/tui/ls_carto_sdbench.h").read_text() + bench_main)
bench_exe=out / "sdbench.exe"
subprocess.run(["gcc", "-std=gnu11", "-O2", "-Wall", "-Wextra", "-Werror", "-Wno-unused-function", "-Wno-unused-variable",
                "-ffunction-sections", "-fdata-sections", "-Wl,--gc-sections",
                "-I", str(engine / "include"), "-I", str(root / "components/apps/tui"),
                str(bench_harness), str(library), "-lm", "-o", str(bench_exe)],cwd=root,check=True)
subprocess.run([str(bench_exe)],cwd=root,check=True)

# Exercise the actual asynchronous fetch code with bounded fake HTTP reads and
# real disk files. Network/RTOS calls are substituted; publication is not.
fetch = source[source.index('/* Detached downloads'):source.index('typedef struct {\n    int cols,rows,z,n,result')]
fetch = fetch[:fetch.index('/* Runs on the TILES worker')] + fetch[fetch.index('#define FETCH_CHUNK'):]
fetch = source[source.index('static double bbox_degrees('):source.index('static int select_map_locked(')] + fetch
fetch = fetch.replace('#ifndef LS_CARTOCORE_HOST\n', '').replace('#include "ls_cells_worker.h"', '')
fetch_prefix = r'''
#include "ls_tiles.h"
#include "cartocore/ctile.h"
#include "cartocore/cellset.h"
#include "mbedtls/sha256.h"
#include <assert.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <ctype.h>
#include <strings.h>
#define portMUX_TYPE int
#define portMUX_INITIALIZER_UNLOCKED 0
#define portENTER_CRITICAL(p) ((void)(p))
#define portEXIT_CRITICAL(p) ((void)(p))
#define esp_crt_bundle_attach NULL
#define MAP_DIR "/sdcard/maps"
#define EXT_RAM_BSS_ATTR
#define MALLOC_CAP_SPIRAM 1
#define MALLOC_CAP_8BIT 2
#define ESP_OK 0
#define ESP_ERR_HTTP_EAGAIN 0x7007
#define pdPASS 1
#define tskIDLE_PRIORITY 0
#define pdMS_TO_TICKS(n) (n)
static bool wifi=true, complete=true, allocation_fail, task_fail;
static int status=200, reads, writes, read_error;
static bool disk_full, short_write, close_fail;
static int64_t content_length=100000;
static size_t body_size=100000, position, live, max_read=32768;
static void (*queued_worker)(void *);
static void *queued_arg;
typedef void *esp_http_client_handle_t;
typedef struct { const char *url; void *crt_bundle_attach; int timeout_ms,buffer_size,buffer_size_tx; bool disable_auto_redirect; } esp_http_client_config_t;
static bool ls_wifi_sta_connected(void) { return wifi; }
static void ls_wifi_sta_ip(char *out,int n) { snprintf(out,n,"%s",wifi?"192.168.1.9":""); }
static int64_t time_step=100000;
static int64_t esp_timer_get_time(void) { static int64_t t; return t+=time_step; }
static void *heap_caps_malloc(size_t n,int caps) {
    assert((n==32768 && caps==3) || (n==4096 && caps==2)); if(allocation_fail) return NULL;
    void *p=malloc(n); if(p) live++; return p;
}
static void *heap_caps_aligned_alloc(size_t align,size_t n,int caps) {
    assert(align==128 && n==32768 && caps==3);
    return heap_caps_malloc(n,caps);
}
static void heap_caps_free(void *p) { if(p) { assert(live); live--; free(p); } }
static void vTaskDelay(int ticks) { (void)ticks; }
static void *heap_caps_calloc(size_t n,size_t bytes,int caps) {
    assert(n==1 && caps==3);return calloc(n,bytes);
}
static void vTaskDeleteWithCaps(void *task) { assert(!task); }
static int xTaskCreateWithCaps(void (*fn)(void *),const char *name,int stack,void *arg,int priority,void *handle,int caps) {
    assert(!strcmp(name,"carto-fetch") && stack==24576 && caps==3);
    (void)priority; (void)handle;
    if(task_fail) return 0; queued_worker=fn; queued_arg=arg; return pdPASS;
}
static esp_http_client_handle_t esp_http_client_init(const esp_http_client_config_t *cfg) {
    assert(cfg->timeout_ms==8000 && cfg->buffer_size==16384 && cfg->buffer_size_tx==1024 && cfg->disable_auto_redirect);
    assert(!strcmp(cfg->url,"http://192.168.1.2/fetch.ctile?version=1"));
    position=0; return (void *)1;
}
static int esp_http_client_open(void *c,int n) { (void)c; assert(!n); return ESP_OK; }
static int64_t esp_http_client_fetch_headers(void *c) { (void)c; return content_length; }
static int esp_http_client_get_status_code(void *c) { (void)c; return status; }
static int esp_http_client_read(void *c,char *out,int n) {
    (void)c; reads++; assert(n>0 && n<=32768);
    if(read_error) return read_error;
    size_t count=body_size-position; if(count>(size_t)n) count=(size_t)n;
    if(count>max_read) count=max_read;
    memset(out,'A',count);
    if(!position && count>=64) {
        memset(out,0,64); memcpy(out,"CTILE1\0\0",8); out[8]=5; out[28]=13; out[29]=15; out[36]=CT_CLASS_COUNT-1;
        for(int i=0;i<8;i++) out[48+i]=(char)((uint64_t)body_size>>(8*i));
    }
    position+=count; return (int)count;
}
static bool esp_http_client_is_chunked_response(void *c) { (void)c; return content_length<0; }
static bool esp_http_client_is_complete_data_received(void *c) { (void)c; return complete; }
static void esp_http_client_close(void *c) { (void)c; }
static void esp_http_client_cleanup(void *c) { (void)c; }
static const char *host_path(const char *p,char *buf) {
    assert(!strncmp(p,"/sdcard/",8)); snprintf(buf,1024,"bench/build/carto-sd-test/%s",p+8); return buf;
}
static int sd_mkdir(const char *p,int mode) { char b[1024]; (void)mode; return mkdir(host_path(p,b)); }
static int sd_open(const char *p,int flags,int mode) { char b[1024]; return open(host_path(p,b),flags|O_BINARY,mode); }
static int sd_stat(const char *p,struct stat *s) { char b[1024]; return stat(host_path(p,b),s); }
static int sd_unlink(const char *p) { char b[1024]; return unlink(host_path(p,b)); }
static int sd_rename(const char *a,const char *b) { char x[1024],y[1024]; return rename(host_path(a,x),host_path(b,y)); }
static FILE *sd_fopen(const char *p,const char *mode) { char b[1024]; return fopen(host_path(p,b),mode); }
static size_t sd_fwrite(const void *p,size_t s,size_t n,FILE *f) {
    writes++; assert(n<=32768);
    if(disk_full) { errno=ENOSPC; return 0; }
    return fwrite(p,s,n,f);
}
#define mkdir sd_mkdir
#define open sd_open
#define stat(p,s) sd_stat(p,s)
#define unlink sd_unlink
#define portMAX_DELAY 0
#define pdMS_TO_TICKS(n) (n)
static bool lock_timeout;
static int xSemaphoreTake(int lock,int ticks) { (void)lock; (void)ticks; return lock_timeout?0:1; }
#define pdTRUE 1
static void xSemaphoreGive(int lock) { (void)lock; }
static bool recover_maps(void) { return !lock_timeout; }
static uint64_t available=1024ULL*1024*1024;
static int capacity_queries;
static int esp_vfs_fat_info(const char *path,uint64_t *total,uint64_t *free_b) { (void)path; capacity_queries++; *total=*free_b=available; return ESP_OK; }
#define rename sd_rename
static ssize_t sd_write(int fd,const void *p,size_t n) {
    writes++; assert(n>0 && n<=32768);
    off_t offset=lseek(fd,0,SEEK_CUR);
    assert(offset>=0 && offset%32768==0);
    if(disk_full) { errno=ENOSPC; return -1; }
    if(short_write) n/=2;
    return write(fd,p,n);
}
static int sd_close(int fd) {
    int result=close(fd);
    if(close_fail) { errno=EIO; return -1; }
    return result;
}
#define write sd_write
#define close sd_close
#define fwrite sd_fwrite
#define fopen sd_fopen
'''
fetch_main = r'''
#undef fopen
#undef mkdir
#undef open
#undef stat
#undef unlink
#undef rename
#undef fwrite
#undef write
#undef close
static int fetch_start(int argc,char **argv) { return fetch_start_expected(argc,argv,NULL,0); }
static void run(void) {
    assert(queued_worker && atomic_load(&fetch_running));
    void (*fn)(void *)=queued_worker; queued_worker=NULL; fn(queued_arg);
    assert(!atomic_load(&fetch_running) && !live);
}
static size_t size(const char *name) {
    char p[512]; snprintf(p,sizeof(p),"bench/build/carto-sd-test/maps/%s",name);
    struct stat st; return stat(p,&st)?0:(size_t)st.st_size;
}
int main(void) {
    remove("bench/build/carto-sd-test/maps/fetch.ctile");
    remove("bench/build/carto-sd-test/maps/fetch.ctile.part");
    uint8_t header[64]; FILE *input=fopen("bench/build/carto-sd-test/maps/valid.ctile","rb");
    assert(input && fread(header,1,64,input)==64); fclose(input);
    transfer_header(header); assert(transfer.has_bbox);
    char url[]="http://192.168.1.2/fetch.ctile?version=1";
    char *args[]={"carto","fetch",url,"-f"};
    wifi=false; assert(fetch_start(3,args)); assert(!queued_worker); wifi=true;
    char *bad[]={"carto","fetch","https://192.168.1.2"}; assert(fetch_start(3,bad));
    char *traversal[]={"carto","fetch",url,"../oops"}; assert(fetch_start(4,traversal));
    assert(!fetch_start(3,args)); assert(!reads && !size("fetch.ctile"));
    assert(fetch_start(3,args)); /* only one worker */
    url[7]='X'; run(); url[7]='1'; /* task owns a copy */
    assert(size("fetch.ctile")==100000 && !size("fetch.ctile.part") && writes>=4);
    /* Short HTTP reads must coalesce into full blocks plus one tail. */
    max_read=997; writes=0;
    assert(!fetch_start(4,args)); run();
    assert(size("fetch.ctile")==100000 && writes==4 && !size("fetch.ctile.part"));
    max_read=32768;
    reads=0; assert(!fetch_start(3,args)); run(); assert(!reads && size("fetch.ctile")==100000);
    body_size=50000; content_length=100000;
    assert(!fetch_start(4,args)); run(); assert(size("fetch.ctile")==100000 && !size("fetch.ctile.part"));
    content_length=0; complete=false;
    assert(!fetch_start(4,args)); run(); assert(size("fetch.ctile")==100000 && !size("fetch.ctile.part"));
    complete=true; assert(!fetch_start(4,args)); run(); assert(size("fetch.ctile")==50000);
    status=404; assert(!fetch_start(4,args)); run(); assert(size("fetch.ctile")==50000 && !size("fetch.ctile.part")); status=200;
    allocation_fail=true; assert(!fetch_start(4,args)); run(); assert(!size("fetch.ctile.part")); allocation_fail=false;
    read_error=-1; assert(!fetch_start(4,args)); run(); assert(size("fetch.ctile")==50000 && !size("fetch.ctile.part")); read_error=0;
    FILE *f=fopen("bench/build/carto-sd-test/maps/fetch.ctile.part","wb"); assert(f); fputs("keep",f); fclose(f);
    assert(!fetch_start(4,args)); run(); assert(!size("fetch.ctile.part") && size("fetch.ctile")==50000);
    assert(!fetch_start(4,args)); ls_carto_transfer_cancel(); run();
    assert(transfer.state==3 && size("fetch.ctile")==50000);
    remove("bench/build/carto-sd-test/maps/fetch.ctile.part");
    content_length=600000000; assert(!fetch_start(4,args)); run(); assert(!size("fetch.ctile.part") && size("fetch.ctile")==50000);
    content_length=-1; body_size=100000; available=1024*1024+40000;
    assert(!fetch_start(4,args)); run(); assert(!size("fetch.ctile.part") && size("fetch.ctile")==100000);
    assert(!capacity_queries); /* No cold FAT scan before download. */
    body_size=50000; content_length=50000;
    assert(!fetch_start(4,args)); run(); assert(size("fetch.ctile")==50000);
    available=1024ULL*1024*1024; content_length=100000;
    assert(!ls_cartocore_fetch(url,"fetch.ctile","",50000)); run(); assert(!size("fetch.ctile.part") && size("fetch.ctile")==50000);
    time_step=500000000; assert(!fetch_start(4,args)); run(); assert(!size("fetch.ctile.part") && size("fetch.ctile")==50000); time_step=100000;
    /* The exact observed reset residue: installed archive plus empty .part. */
    f=fopen("bench/build/carto-sd-test/maps/fetch.ctile.part","wb");assert(f);fclose(f);
    body_size=content_length=50000; assert(!ls_cartocore_fetch(url,"fetch.ctile","",50000)); run();
    assert(transfer.state==2);
    assert(size("fetch.ctile")==50000 && !size("fetch.ctile.part"));
    /* Verification failure during re-download must retain the installed map. */
    assert(!ls_cartocore_fetch(url,"fetch.ctile","0000000000000000000000000000000000000000000000000000000000000000",50000));run();
    assert(transfer.state==3 && size("fetch.ctile")==50000 && !size("fetch.ctile.part"));
    assert(!fetch_start(4,args)); lock_timeout=true; run(); lock_timeout=false;
    assert(transfer.state==3 && strstr(transfer.last_error,"lock timeout"));
    /* Timeout/refusal/cancel must allow the next transfer. */
    body_size=content_length=50000; assert(!fetch_start(4,args)); run();
    assert(transfer.state==2 && !transfer.last_error[0]);
    disk_full=true; assert(!fetch_start(4,args)); run(); disk_full=false;
    assert(transfer.state==3 && strstr(transfer.last_error,"SD write failed"));
    assert(size("fetch.ctile")==50000 && !size("fetch.ctile.part"));
    assert(!fetch_start(4,args)); run(); assert(transfer.state==2);
    short_write=true; assert(!fetch_start(4,args)); run(); short_write=false;
    assert(transfer.state==3 && strstr(transfer.last_error,"SD write failed"));
    assert(size("fetch.ctile")==50000 && !size("fetch.ctile.part"));
    close_fail=true; assert(!fetch_start(4,args)); run(); close_fail=false;
    assert(transfer.state==3 && strstr(transfer.last_error,"SD flush/close failed"));
    assert(size("fetch.ctile")==50000 && !size("fetch.ctile.part"));
    task_fail=true; assert(fetch_start(4,args)); assert(!atomic_load(&fetch_running));
    puts("PASS: asynchronous fetch, bounded chunks, WiFi, names, overwrite, partial responses, errors, cleanup");
    return 0;
}
'''
fetch_harness = out / "fetch.c"
# Source ownership/reopen is tested above with the real reader. Here retain the
# production rename transaction while isolating HTTP/RTOS from map selection.
publication = source[source.index('static int publish_locked(const char *part'):source.index('/* Serialize with source selection')]
fetch_harness.write_text(fetch_prefix + '#include \"ls_carto_digest.h\"\n' + publication + r'''
static int publish_map(const char *part,const char *path,bool force,const char *digest) {
    return publish_locked(part,path,force,digest);
}
#define TRANSFER_CAP (512ULL*1024*1024)
#define TRANSFER_US (INT64_C(900)*1000000)
''' + fetch + fetch_main)
fetch_exe = out / "fetch.exe"
idf = Path(os.getenv("IDF_PATH", "C:/esp/v5.5.4/esp-idf"))
mbed = idf / "components/mbedtls/mbedtls"
(out / "tiles_mbedtls_config.h").write_text("#define MBEDTLS_SHA256_C\n#define MBEDTLS_PLATFORM_C\n")
subprocess.run(["gcc", "-std=gnu11", "-O2", "-Wall", "-Wextra", "-Werror",
                "-Wno-misleading-indentation", "-Wno-unused-function", '-DMBEDTLS_CONFIG_FILE="tiles_mbedtls_config.h"',
                "-I",str(engine / "include"),"-I",str(out),"-I",str(root / "components/apps/tui"),"-I",str(mbed / "include"),
                str(fetch_harness),str(library),str(mbed / "library/sha256.c"),str(mbed / "library/platform_util.c"),
                str(mbed / "library/platform.c"),"-o", str(fetch_exe)], cwd=root, check=True)
subprocess.run([str(fetch_exe)], cwd=root, check=True)

# Exercise the production HTTP receiver and laptop multi-asset sender too.
import sys
subprocess.run([sys.executable, str(root / "bench/tools/test_carto_assets.py")], cwd=root, check=True)
subprocess.run([sys.executable, str(root / "bench/tools/test_carto_recv.py")], cwd=root, check=True)
