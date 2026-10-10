/* Shared CartoCore source, console overlay and MAP rendering. */
#ifdef LS_CARTOCORE_HOST
#include "lssim_cartocore_platform.h"
#else
#include "sdkconfig.h"
#include "ls_tui.h"
#include "ls_tiles.h"
#include "nvs.h"
#include "core/ls_nvs_safe.h"
#include "ls_options.h"
#include "ls_picker.h"
#include "ls_numpad.h"
#include "ls_keyboard.h"
#include "mbedtls/sha256.h"
#include "esp_vfs_fat.h"
#include "ls_sdcard.h"
#include "ls_cartocore_cells.h"
#include "cartocore/cache.h"
#include "cartocore/render.h"
#include "cartocore/profile.h"
#include "cartocore/arch.h"
#include "esp_console.h"
#include "esp_heap_caps.h"
#include "esp_memory_utils.h"
#include "esp_timer.h"
#include "esp_random.h"
#include "lwip/sockets.h"
#include "lwip/inet.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "esp_http_server.h"
#include "../../../main/ls_wifi.h"
#include "esp_private/esp_clk.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "freertos/idf_additions.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdatomic.h>
#include <dirent.h>
#include <sys/stat.h>
#include <strings.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <ctype.h>
#include <limits.h>
#endif
#include "ls_map_marks.h"
#include "ls_cartocore_map.h"
#include "ls_carto_sd_io.h"
#include "ls_carto_digest.h"
#include "ls_cartocore_select.h"
#include "cartocore/cellset.h"

static SemaphoreHandle_t view_lock, command_lock, file_lock;
static tui_cell *view;
static int view_cols, view_rows;
static atomic_bool view_active, dismiss_pending;
/* Serialize console jobs across terminals; idle/draw borrow under view_lock. */
typedef struct {
    int fd;
    size_t file_size,bounce_capacity;
    void *bounce;
    char path[512];
    cc_ctile map;
    cc_ctile_source source;
    cc_cellset *set;
    void *directory,*staging;
    size_t directory_bytes,staging_bytes;
} sd_source;
static sd_source *sd_map;
/* Bench may select the measured fastest fd path for this boot. */
static size_t sd_bounce_bytes=32u*1024u;
/* Source allocation high waters, serialized with view_lock like rendering. */
static size_t source_handles_live,source_handles_hw,source_directory_hw,source_staging_hw;
static size_t sd_map_size;
static char active_file[128];
void ls_carto_active_file(char *out,size_t size) {
    if(!view_lock || xSemaphoreTake(view_lock,0)!=pdTRUE) return;
    snprintf(out,size,"%s",active_file);
    xSemaphoreGive(view_lock);
}
/* FAT cannot safely delete/replace the file behind a live callback source. */
int ls_carto_unlink(const char *path) {
    xSemaphoreTake(file_lock,portMAX_DELAY);
    xSemaphoreTake(view_lock,portMAX_DELAY);
    int result;
    if(sd_map) { errno=EBUSY; result=-1; }
    else result=carto_digest_remove(path)?unlink(path):-1;
    xSemaphoreGive(view_lock);
    xSemaphoreGive(file_lock);
    return result;
}
static int sd_read(void *ctx,uint64_t offset,void *buffer,size_t length) {
    sd_source *s=ctx;
    if(buffer==s->directory && length>source_directory_hw)source_directory_hw=length;
    if(buffer==s->staging && length>source_staging_hw)source_staging_hw=length;
    return carto_sd_read_at(s->fd,s->file_size,offset,buffer,length,s->bounce,s->bounce_capacity);
}
static void close_sd(sd_source *s) {
    if(!s) return;
    if(s->set) { cc_cellset_close(s->set);source_handles_live-=sizeof(*s->set);heap_caps_free(s->set); }
    if(s->fd>=0) close(s->fd);
    heap_caps_free(s->bounce);
    heap_caps_free(s->directory); heap_caps_free(s->staging); heap_caps_free(s);
}
/* Four file descriptors share the existing SD bounce buffer; all callbacks
 * run on the same serialized worker. No per-cell internal/DMA allocations. */
typedef struct { int fd; uint64_t size; sd_source *owner; } sd_cell_file;
static int cell_read(void *ctx,uint64_t offset,void *dst,size_t bytes) {
    sd_cell_file *f=ctx;
    return carto_sd_read_at(f->fd,f->size,offset,dst,bytes,f->owner->bounce,f->owner->bounce_capacity);
}
static void cell_close(void *ctx) { sd_cell_file *f=ctx;close(f->fd);source_handles_live-=sizeof(*f);heap_caps_free(f); }
static bool cells_file_current(uint32_t,const char *);
static int cell_open(void *ctx,uint32_t id,void **handle,uint64_t *bytes) {
    sd_source *owner=ctx;char path[256];
    const char *slash=strrchr(owner->path,'/');if(!slash) return -1;
    int prefix=(int)(slash-owner->path);
    if(id) snprintf(path,sizeof(path),"%.*s/8-%u-%u.ctile",prefix,owner->path,(id-1)/256,(id-1)%256);
    else snprintf(path,sizeof(path),"%.*s/overview.ctile",prefix,owner->path);
    if(!cells_file_current(id,path))return 0;
    int fd=open(path,O_RDONLY);if(fd<0) return errno==ENOENT?0:-1;
    struct stat st;if(fstat(fd,&st) || !S_ISREG(st.st_mode) || st.st_size<64) { close(fd);return -1; }
    sd_cell_file *f=heap_caps_malloc(sizeof(*f),MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);
    if(!f) { close(fd);return -1; }
    source_handles_live+=sizeof(*f);
    if(source_handles_live>source_handles_hw)source_handles_hw=source_handles_live;
    *f=(sd_cell_file){fd,st.st_size,owner};*handle=f;*bytes=st.st_size;return 1;
}
static double centre_lat=43.4445, centre_lon=-71.6473;

/* One versioned NVS record keeps source selection and view together. Hiding
   releases the source without changing this selection. */
typedef struct {
    uint32_t version; unsigned z;
    double lat,lon;
    int terrain_on,terrain_strength;
    char path[512];
} carto_saved;
static carto_saved saved={.version=1,.z=14,.lat=43.4445,.lon=-71.6473,.terrain_on=1,.terrain_strength=80};
static bool saved_loaded, saved_valid;
static atomic_bool saved_dirty, terrain_changed;
/* These two callbacks run only through the existing DRAM-stack dispatcher. */
static esp_err_t load_saved_job(void *ctx) {
    (void)ctx;
    nvs_handle_t h;
    esp_err_t e=nvs_open("cartocore",NVS_READONLY,&h);
    if(e!=ESP_OK) return e;
    carto_saved next={0}; size_t n=sizeof(next);
    e=nvs_get_blob(h,"view",&next,&n); nvs_close(h);
    if(e==ESP_OK && n==sizeof(next) && next.version==1 && next.z<=22 &&
       isfinite(next.lat) && fabs(next.lat)<=85.05112878 &&
       isfinite(next.lon) && next.lon>=-180 && next.lon<180 &&
       (next.terrain_on==0 || next.terrain_on==1) && next.terrain_strength>=1 && next.terrain_strength<=100 &&
       memchr(next.path,0,sizeof(next.path))) {
        saved=next; saved_valid=true; centre_lat=saved.lat; centre_lon=saved.lon;
    }
    return e;
}
void ls_cartocore_boot_init(void) {
    if(saved_loaded) return;
    /* Boot calls this before console/UI/idle tasks exist. Never lazy-load NVS. */
    saved_loaded=true;
    (void)ls_nvs_run(load_saved_job,NULL,0);
    ls_marks_defer_io();
}
static esp_err_t save_saved_job(void *ctx) {
    const carto_saved *record=ctx;
    nvs_handle_t h;
    esp_err_t e=nvs_open("cartocore",NVS_READWRITE,&h);
    if(e!=ESP_OK) return e;
    e=nvs_set_blob(h,"view",record,sizeof(*record));
    if(e==ESP_OK) e=nvs_commit(h);
    nvs_close(h); return e;
}
/* Called under view_lock. UI/console/idle only update RAM and mark a save. */
static void save_view(void) {
    saved.lat=centre_lat; saved.lon=centre_lon; saved_valid=true;
    atomic_store(&saved_dirty,true);
}
/* Idle takes a snapshot, drops view_lock, then hands flash work to DRAM.
 * A concurrent edit leaves dirty set for the following turn. */
static void flush_saved(void) {
    static int64_t retry_after;
    if(!atomic_load(&saved_dirty) || esp_timer_get_time()<retry_after) return;
    carto_saved record;
    if(xSemaphoreTake(view_lock,0)!=pdTRUE) return;
    bool pending=atomic_exchange(&saved_dirty,false);
    record=saved;
    xSemaphoreGive(view_lock);
    if(!pending) return;
    esp_err_t e=ls_nvs_call(save_saved_job,&record,0);
    if(e!=ESP_OK) {
        atomic_store(&saved_dirty,true); retry_after=esp_timer_get_time()+5000000;
        printf("carto: deferred NVS save failed: %s\n",esp_err_to_name(e));
    }
}
static int restore_map(void);
static int prepare_source(double,double);
static int select_map(const char *);
static bool recover_maps(void);
static bool source_refreshing; /* worker-owned: retain publication across cell replacement */
static void close_hidden(void);

static size_t internal_free(void) { return heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT); }
static size_t dma_free(void) { return heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA); }
static void heaps(const char *where)
{
    printf("carto heap %s internal=%zu DMA=%zu largest_DMA=%zu\n", where,
           internal_free(), dma_free(), heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA));
}
static int open_map(cc_ctile *map)
{
    if(sd_map) { *map=sd_map->map; return 1; }
#ifdef LS_CARTOCORE_HOST
    /* Host-only fixture; the disabled path exercises the firmware behavior. */
    if(!getenv("LSSIM_CARTO_NO_EMBEDDED"))
        return cc_ctile_open((cc_str){mini_start,(size_t)(mini_end-mini_start)},map);
#endif
    memset(map,0,sizeof(*map));
    return 0;
}
static void origin(int cols, int rows, unsigned z, int64_t *left, int64_t *top)
{
    const double pi = 3.14159265358979323846, scale = 256.0 * (1u << z);
    *left = (int64_t)llround((centre_lon+180.0)/360.0*scale)-cols;
    *top = (int64_t)llround((1.0-log(tan(pi/4.0+centre_lat*pi/360.0))/pi)/2.0*scale)-rows*2;
}
static int cmp_double(const void *a, const void *b)
{
    double x=*(const double *)a, y=*(const double *)b;
    return (x>y)-(x<y);
}
static void stats(const char *stage, double *times, int n)
{
    qsort(times, n, sizeof(*times), cmp_double);
    printf("  %-8s median=%.2f p90=%.2f us\n", stage,
           (times[(n-1)/2]+times[n/2])/2, times[(9*n+9)/10-1]);
}

#include "ls_cartocore_render.h"
static render_state *shown;
/* Retained across close/rotation; values are bytes, idle may alias cold. */
void ls_carto_hw_status(bool reset) {
    if(!view_lock) return;
    xSemaphoreTake(view_lock,portMAX_DELAY);
    carto_hw_sample(shown);
    if(reset) {
        carto_hw=(carto_arena_hw){0};
        source_handles_hw=source_handles_live;
        source_directory_hw=source_staging_hw=carto_sd_bounce_peak=0;
        if(shown) {
            shown->arena.peak=shown->arena.used;
            shown->hot_arena.peak=shown->hot_arena.used;
            shown->decoded.peak=shown->decoded.used;
            shown->idle_arena.peak=shown->idle_arena.used;
            shown->coarse_frames=0;
        }
        carto_hw_sample(shown);
    }
    printf("carto hw cold=%zu decoded=%zu idle=%zu hot=%zu cache=%zu cell_handles=%zu directory=%zu staging=%zu bounce=%zu cold_cap=%zu decoded_cap=%zu idle_alloc=%zu coarse_frames=%u%s\n",
        carto_hw.cold,carto_hw.decoded,carto_hw.idle,carto_hw.hot,carto_hw.cache,
        source_handles_hw,source_directory_hw,source_staging_hw,carto_sd_bounce_peak,shown?shown->arena.size:0,
        shown?shown->decoded.size:0,shown && shown->idle_memory?shown->idle_arena.size:0,
        shown?shown->coarse_frames:0,reset?" reset":"");
    xSemaphoreGive(view_lock);
}
static atomic_bool map_owned;
static atomic_bool map_prepared;
static void map_service(void);
static void map_invalidate_frame(void);
static int bench_one(int cols,int rows,unsigned z,cc_mode mode,cc_edges edges,int n,bool external,bool tilecache)
{
    cc_ctile map;
    if(!open_map(&map) && select_map("/sdcard/maps/franklin_mini.ctile")) {
        puts("carto bench: missing /sdcard/maps/franklin_mini.ctile; install it on SD or open a map first");
        return 0;
    }
    int ok=0; size_t before=internal_free(),dma_before=dma_free();
    printf("carto bench %dx%d z%u %s %s N=%d arena=%s decoded=%u cell_cache=%u PSRAM\n",
           cols,rows,z,mode==CC_BRAILLE?"braille":"quadrant",edges==CC_EDGES_CRISP?"crisp":"smooth",
           n,"split (HOT internal preferred, COLD PSRAM)",DECODED_BYTES,tilecache?CELL_BYTES:0);
    heaps("before");
    render_state *s=create(cols,rows,z,external,tilecache);
    double *times=heap_caps_malloc((size_t)n*(CC_STAGES+1)*sizeof(*times),MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);
    if (!s || !times) { puts("  unavailable: allocation/map/planes failed"); goto done; }
    const double ticks_per_us=esp_clk_cpu_freq()/1000000.0;
    static const char *const workload[]={"cold-first-frame","warm-same-view","1-cell-pan","zoom-change"};
    static const char *const names[]={"lookup","decode","lines","fill","coverage","labels","encode"};
    for (int w=0;w<4;w++) {
        if (w) {
            cc_renderer_invalidate(&s->renderer);
            xSemaphoreTake(view_lock,portMAX_DELAY);
            int rendered=render_frame(s,mode,edges,s->left,s->top);
            xSemaphoreGive(view_lock);
            if (!rendered) goto done;
        }
        if(w && tilecache) {
            /* Warm paths are measured after idle filling, outside timed frames. */
            while(s->tiles.pending) {
                xSemaphoreTake(view_lock,portMAX_DELAY);
                size_t filled=cc_cell_cache_step(&s->tiles,512,&s->idle_arena);
                xSemaphoreGive(view_lock);
                if(!filled) break;
                vTaskDelay(1);
            }
        }
        for (int i=0;i<n;i++) {
            if (!w) { cc_cell_cache_clear(&s->tiles); cc_decoded_cache_reset(&s->decoded); }
            if(w==3) {
                s->z=(i%2)?z:(z==15?14:z+1);
                cc_renderer_invalidate(&s->renderer);
            }
            /* Every sample rasterizes/encodes. Warm retains the prepared scene;
               cold/pan/zoom release it and rebuild from decoded tiles. */
            if(w==1 && !tilecache) cc_renderer_reraster(&s->renderer);
            else cc_renderer_invalidate(&s->renderer);
            /* The profiler pointer is global; exclude shown-view idle work. */
            xSemaphoreTake(view_lock,portMAX_DELAY);
            cc_profile prof={{0}}; cc_profiling=&prof;
            int64_t start=esp_timer_get_time();
            int64_t left=s->left+(w==2?(i+1)*2:0),top=s->top;
            if(w==3) {
                int64_t cx=s->left+s->cols,cy=s->top+s->rows*2;
                if(s->z>z) { cx*=2; cy*=2; } else if(s->z<z) { cx/=2; cy/=2; }
                left=cx-s->cols; top=cy-s->rows*2; left-=left%2; top-=top%4;
                cc_cell_cache_clear(&s->tiles);
            }
            int rendered=render_frame(s,mode,edges,left,top);
            times[i]=(double)(esp_timer_get_time()-start); cc_profiling=NULL;
            xSemaphoreGive(view_lock);
            if (!rendered) { printf("  render failed arena=%zu peak=%zu\n",s->arena.size,s->arena.peak); goto done; }
            for (int k=0;k<CC_STAGES;k++) times[(k+1)*n+i]=prof.ticks[k]/ticks_per_us;
            if(w==2 && tilecache) {
                while(s->tiles.pending) {
                    xSemaphoreTake(view_lock,portMAX_DELAY);
                    size_t filled=cc_cell_cache_step(&s->tiles,512,&s->idle_arena);
                    xSemaphoreGive(view_lock);
                    if(!filled) break;
                    vTaskDelay(1);
                }
            }
            vTaskDelay(1);
        }
        printf("  %s arena capacity=%zu peak=%zu used=%zu decoded used=%zu peak=%zu hits=%zu misses=%zu evictions=%zu bypasses=%zu cell hits=%zu misses=%zu evictions=%zu fallbacks=%zu frame=%zu\n",
               workload[w],s->arena.size,s->arena.peak,s->arena.used,s->decoded.used,s->decoded.peak,
               s->decoded.hits,s->decoded.misses,s->decoded.evictions,s->decoded.bypasses,
               s->tiles.hits,s->tiles.misses,s->tiles.evictions,s->tile_fallbacks,(size_t)cols*rows*sizeof(cc_cell));
        stats("total",times,n);
        s->z=z;
        for (int k=0;k<CC_STAGES;k++) stats(names[k],times+(k+1)*n,n);
    }
    ok=1;
done:
    cc_profiling=NULL; heap_caps_free(times); release(s); heaps("after");
    printf("  heap delta internal=%ld DMA=%ld (after-before)\n",(long)internal_free()-(long)before,(long)dma_free()-(long)dma_before);
    return ok;
}

/* Double-buffered PSRAM publication, independent of the render/source mutex.
 * Only the tiny pointer swap waits for a reader; rendering never owns this lock. */
typedef struct {
    tui_cell *cells; size_t capacity,count;
    int cols,rows; double lat,lon; unsigned z,generation;
    bool console,braille,labels;
    ls_carto_label names[LS_CARTO_LABEL_MAX];
} carto_frame;
static EXT_RAM_BSS_ATTR carto_frame frames[2];
static unsigned frame_front,frame_generation;
static atomic_flag frame_guard=ATOMIC_FLAG_INIT;
static atomic_bool frame_valid;
static bool frame_take(void) { return !atomic_flag_test_and_set(&frame_guard); }
static void frame_give(void) { atomic_flag_clear(&frame_guard); }
static void frames_clear(void) {
    while(!frame_take()) vTaskDelay(1);
    atomic_store(&frame_valid,false);
    for(unsigned i=0;i<2;i++) { heap_caps_free(frames[i].cells);frames[i].cells=NULL;frames[i].capacity=0; }
    view=NULL;frame_give();
}
static bool frame_commit(render_state *s,bool console,double lat,double lon,bool braille,bool labels,
                         const ls_carto_label *names,size_t count) {
    carto_frame *f=&frames[frame_front^1];size_t n=(size_t)s->cols*s->rows;
#ifdef LS_CARTOCORE_HOST
    if(getenv("LSSIM_CARTO_CHECK_COMPLETE")) {
        /* Independent, cache-free render rejects partial cache/decode output,
         * even when enough nonblank cells would fool a visual density check. */
        void *memory=malloc(32u*1024u*1024u);cc_cell *reference=malloc(n*sizeof(*reference));
        assert(memory && reference);cc_arena arena;cc_renderer renderer;
        cc_arena_init(&arena,memory,32u*1024u*1024u);
        assert(cc_renderer_init(&renderer,&arena,s->cols,s->rows,reference));
        renderer.colors16=1;renderer.terrain_strength=s->renderer.terrain_strength;
        renderer.edges_mode=console?CC_EDGES_CRISP:CC_EDGES_SMOOTH;
        assert(cc_render(&renderer,s->coarse?&s->coarse_map:&s->map,s->z,s->left,s->top,braille?CC_BRAILLE:CC_QUADRANT,
                         console?s->labels:0,reference));
        assert(!memcmp(s->cells,reference,n*sizeof(*reference)));
        free(reference);free(memory);
    }
#endif
    if(f->capacity<n) {
        tui_cell *p=heap_caps_malloc(n*sizeof(*p),MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);
        if(!p) { carto_memory_failed=true;return false; }
        heap_caps_free(f->cells);f->cells=p;f->capacity=n;
    }
    for(size_t i=0;i<n;i++) f->cells[i]=(tui_cell){glyph(s->cells[i].codepoint),carto_attr(s->cells[i])};
    f->cols=s->cols;f->rows=s->rows;f->lat=lat;f->lon=lon;f->z=s->z;
    f->console=console;f->braille=braille;f->labels=labels;f->count=count;
    if(count) memcpy(f->names,names,count*sizeof(*names));
#ifdef LS_CARTOCORE_HOST
    if(getenv("LSSIM_CELLS_MEMORY"))printf("cells render: arena peak=%zu/%zu idle=%zu hot=%zu decoded=%zu frame=%zu free_psram=%zu coarse=%d detail=%u\n",s->arena.peak,s->arena.size,s->idle_memory?s->idle_arena.size:0,s->hot_arena.size,s->decoded.peak,n*sizeof(tui_cell)*2,heap_caps_get_free_size(MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT),s->coarse,s->coarse?s->coarse_map.zmax:s->map.zmax);
#endif
    f->generation=++frame_generation;
    while(!frame_take()) vTaskDelay(1);
    frame_front^=1;view=f->cells;view_cols=f->cols;view_rows=f->rows;
    atomic_store(&frame_valid,true);frame_give();return true;
}

bool ls_cartocore_dismiss(void)
{
    /* Input never waits for a raster/decode step. Cleanup belongs to idle. */
    bool active=atomic_exchange(&view_active,false);
    if(active) atomic_store(&dismiss_pending,true);
    return active;
}
bool ls_cartocore_draw(tui_surface *sf) {
    if(!atomic_load(&view_active) || !atomic_load(&frame_valid)) return false;
    if(!frame_take()) { memcpy(sf->back,sf->front,(size_t)sf->w*sf->h*sizeof(*sf->back));return true; }
    const carto_frame *f=&frames[frame_front];bool ok=f->console && f->cells;
    if(ok) for(int y=0;y<sf->h;y++) for(int x=0;x<sf->w;x++)
        sf->back[y*sf->w+x]=f->cells[(y*f->rows/sf->h)*f->cols+x*f->cols/sf->w];
    frame_give();return ok;
}
/* Called with view_lock held; publication and retained state change together. */
static int queue_hint(void *ctx,unsigned z,uint32_t x,uint32_t y,uint64_t offset,uint32_t length);
static int redraw(render_state *s,const char *action)
{
    int64_t start=esp_timer_get_time();
    if (!render_frame(s,CC_QUADRANT,CC_EDGES_CRISP,s->left,s->top)) {
        printf("carto: render failed z%u (map z%u..%u), arena peak=%zu\n",s->z,s->map.zmin,s->map.zmax,s->arena.peak); return 1;
    }
    if(!frame_commit(s,true,centre_lat,centre_lon,false,s->labels,NULL,0)) {
        puts("carto: display buffer allocation failed; previous frame retained");return 1;
    }
    if(!atomic_load(&dismiss_pending)) atomic_store(&view_active,true);
    if(s->map.source && strcmp(action,"later")) {
        s->hint_count=s->hint_next=0;
        cc_ctile_prefetch_hint(&s->map,s->z,s->left,s->top,s->cols,s->rows,0,0,queue_hint,s);
    }
    printf("carto %s z%u grid=%dx%d frame=%lld us cache_pending=%d arena_peak=%zu decoded=%zu/%u cell_cache=%u PSRAM\n",
           action,s->z,s->cols,s->rows,(long long)(esp_timer_get_time()-start),s->tiles.pending,s->arena.peak,s->decoded.used,(unsigned)s->decoded.size,s->cell_memory?CELL_BYTES:0);
    if(strcmp(action,"later")) {
        double scale=256.0*(1u<<s->z),pi=3.14159265358979323846;
        centre_lon=(s->left+s->cols)/scale*360.0-180.0;
        centre_lon=fmod(centre_lon+540.0,360.0)-180.0;
        centre_lat=atan(sinh(pi*(1.0-2.0*(s->top+s->rows*2)/scale)))*180.0/pi;
        if(centre_lat>85.05112878) centre_lat=85.05112878;
        if(centre_lat< -85.05112878) centre_lat=-85.05112878;
        saved.z=s->z; save_view();
    }
    return 0;
}
/* Idle cache work shares the decoded cache under the view mutex. One row per
 * turn bounds raster area; separate scratch preserves the direct saved frame.
 * Low priority and a yield every turn keep console/touch tasks schedulable. */
static void cells_service(void);
static void cache_step(void) {
    static bool boot_recovered;
    static int64_t recovery_after;
    ls_marks_io_step();
    cells_service();
    map_service();
    bool closed=false;
    if(xSemaphoreTake(view_lock,0)==pdTRUE) {
        if(atomic_exchange(&dismiss_pending,false)) {
            closed=true;map_prepared=false;map_owned=false;
            atomic_store(&view_active,false);
            frames_clear(); release(shown); shown=NULL;
            close_sd(sd_map); sd_map=NULL; sd_map_size=0; active_file[0]=0;
        }
        if(atomic_exchange(&terrain_changed,false) && shown) {
            shown->renderer.terrain_strength=saved.terrain_on?saved.terrain_strength:0;
            shown->tiles.terrain_strength=shown->renderer.terrain_strength;
            cc_renderer_invalidate(&shown->renderer); cc_cell_cache_clear(&shown->tiles);
            if(map_owned) map_invalidate_frame();
            if(!map_owned && redraw(shown,"terrain")) puts("carto: terrain redraw failed");
        }
        if(shown && shown->tiles.pending) {
            int64_t start=esp_timer_get_time();
            size_t cells=cc_cell_cache_step(&shown->tiles,128,&shown->idle_arena);
            if(!cells && shown->tiles.pending) {
                shown->tiles.pending=0;
                puts("carto idle fill unavailable; direct rendering retained");
            } else if(!shown->tiles.pending) {
                printf("carto idle cache ready last_step=%lld us; later frame follows\n",(long long)(esp_timer_get_time()-start));
                if(!map_owned) redraw(shown,"later");
            }
        }
        if(shown && !shown->tiles.pending && shown->hint_next<shown->hint_count) {
            if(shown->map.cellset) {
                cc_renderer_invalidate(&shown->renderer);
                shown->arena.used=shown->renderer.mark;
            }
            unsigned i=shown->hint_next++;
            size_t mark=shown->idle_arena.used; cc_str tile;
            cc_ctile_source_tile(&shown->map,shown->hints[i].offset,shown->hints[i].length,
                                 &shown->idle_arena,&shown->decoded,&tile);
            shown->idle_arena.used=mark;
            if(shown->map.cellset)
                carto_hw_max(&shown->arena.peak,shown->renderer.mark+shown->idle_arena.peak);
            carto_hw_sample(shown);
            cc_renderer_invalidate(&shown->renderer);
        }
        xSemaphoreGive(view_lock);
    }
    if(closed) (void)recover_maps();
    /* SD mounting can finish after console registration. Try once when
       maps becomes available; an open source defers publication to close. */
    int64_t now=esp_timer_get_time();
    if(!boot_recovered && now>=recovery_after) {
        struct stat st;recovery_after=now+1000000;
        if(!stat("/sdcard/maps",&st) && S_ISDIR(st.st_mode)) boot_recovered=recover_maps();
    }
    flush_saved();
}
#ifndef LS_CARTOCORE_HOST
static void cache_idle(void *arg) {
    (void)arg;cc_arch_task_enable();
    for(;;) { cache_step();vTaskDelay(pdMS_TO_TICKS(10)); }
}
#endif
static int show(unsigned z)
{
    if(restore_map()) { puts("carto: no map installed; download a region in TILES"); return 1; }
    int cols,rows; ls_tui_geometry(&cols,&rows,NULL,NULL);
    if (cols<1 || rows<1 || cols>400 || rows>200) { puts("carto: TUI not ready or grid too large"); return 1; }
    render_state *s=create(cols,rows,z,true,true);
    if (!s) { puts(carto_memory_failed?"carto: not enough memory for map":"carto: map unavailable"); return 1; }
    xSemaphoreTake(view_lock,portMAX_DELAY);
    atomic_store(&dismiss_pending,false);
    map_owned=false;
    int result=redraw(s,"show");
    if (!result) { release(shown); shown=s; s=NULL; }
    xSemaphoreGive(view_lock); release(s);
    return result;
}
static int select_source(const char *path) { return select_map(path); }
#include "ls_cartocore_map_impl.h"
static int queue_hint(void *ctx,unsigned z,uint32_t x,uint32_t y,uint64_t offset,uint32_t length) {
    (void)z; (void)x; (void)y;
    render_state *s=ctx;
    if(s->hint_count==64) return 0;
    s->hints[s->hint_count].offset=offset; s->hints[s->hint_count++].length=length;
    return 1;
}
static int move_view(int dx,int dy,int z)
{
    xSemaphoreTake(view_lock,portMAX_DELAY);
    int result=1;
    if (!shown || !view || !atomic_load(&view_active)) puts("carto: show a view first");
    else {
        int64_t left=shown->left,top=shown->top; unsigned oldz=shown->z;
        if (z) {
            /* Keep the selected map centre when changing zoom. */
            int64_t cx=left+shown->cols,cy=top+shown->rows*2;
            if ((unsigned)z>oldz) { cx*=INT64_C(1)<<(z-oldz); cy*=INT64_C(1)<<(z-oldz); }
            else { cx/=INT64_C(1)<<(oldz-z); cy/=INT64_C(1)<<(oldz-z); }
            shown->left=cx-shown->cols; shown->top=cy-shown->rows*2; shown->z=z;
            shown->left-=shown->left%2; shown->top-=shown->top%4;
        } else { shown->left+=(int64_t)dx*2; shown->top+=(int64_t)dy*4; }
        shown->hint_count=shown->hint_next=0;
        result=redraw(shown,z?"zoom":"pan");
        if(!result && shown->map.source)
            cc_ctile_prefetch_hint(&shown->map,shown->z,shown->left,shown->top,
                                  shown->cols,shown->rows,dx*2,dy*4,queue_hint,shown);
        if (result) { shown->left=left; shown->top=top; shown->z=oldz; puts("carto: render failed; previous view retained"); }
    }
    xSemaphoreGive(view_lock); return result;
}

#define MAP_DIR "/sdcard/maps"
static bool ctile_name(const char *name)
{
    size_t n=strlen(name);
    return n>6 && !strcasecmp(name+n-6,".ctile");
}
/* List regular files; default open uses the first matching directory entry. */
static int map_directory(char *first,size_t capacity)
{
    DIR *dir=opendir(MAP_DIR);
    if (!dir) { printf("carto: %s: %s\n",MAP_DIR,strerror(errno)); return 1; }
    int found=0; struct dirent *entry;
    while ((entry=readdir(dir))) {
        char path[256]; struct stat st;
        if (!ctile_name(entry->d_name)) continue;
        int n=snprintf(path,sizeof(path),MAP_DIR "/%s",entry->d_name);
        if (n<0 || (size_t)n>=sizeof(path) || stat(path,&st) || !S_ISREG(st.st_mode)) continue;
        found++;
        if (first) {
            if ((size_t)n>=capacity) continue;
            memcpy(first,path,(size_t)n+1); break;
        }
        printf("%s  %lld bytes\n",path,(long long)st.st_size);
    }
    closedir(dir);
    if (!found) puts("carto: no .ctile files in " MAP_DIR);
    return first && !first[0] ? 1 : 0;
}
static double bbox_degrees(const uint8_t *p)
{
    uint32_t v=(uint32_t)p[0]|(uint32_t)p[1]<<8|(uint32_t)p[2]<<16|(uint32_t)p[3]<<24;
    int64_t signed_v=v;
    if (v&UINT32_C(0x80000000)) signed_v-=INT64_C(0x100000000);
    return signed_v/1e7;
}
static int select_map_locked(const char *requested,bool persist)
{
    sd_source *next=NULL; size_t size=0;
    double lat=43.4445,lon=-71.6473;
    static EXT_RAM_BSS_ATTR char path[256];path[0]=0; /* file_lock */
    int64_t start=esp_timer_get_time();
#ifndef LS_CARTOCORE_HOST
    /* Compatibility alias: the former embedded fixture is now an SD asset. */
    if(requested && !strcmp(requested,"embedded")) requested="/sdcard/maps/franklin_mini.ctile";
#endif
    if(requested && !strcmp(requested,"cells")) requested="/sdcard/maps/cells/index.cci";
    if (!requested || strcmp(requested,"embedded")) {
        if (!requested) { if (map_directory(path,sizeof(path))) return 1; }
        else {
            int n;
            if (!strncmp(requested,"/maps/",6)) n=snprintf(path,sizeof(path),"/sdcard%s",requested);
            else if (requested[0]=='/') n=snprintf(path,sizeof(path),"%s",requested);
            else n=snprintf(path,sizeof(path),MAP_DIR "/%s",requested);
            if (n<0 || (size_t)n>=sizeof(path)) { puts("carto: path too long"); return 1; }
        }
        if (strncmp(path,"/sdcard/",8) || strstr(path,"/../") ||
            (!ctile_name(path) && strcmp(path,"/sdcard/maps/cells/index.cci"))) { puts("carto: expected an SD .ctile path or cells"); return 1; }
        int fd=open(path,O_RDONLY); struct stat st;
        if (fd<0) { printf("carto: %s: %s\n",path,strerror(errno)); return 1; }
        if (fstat(fd,&st) || !S_ISREG(st.st_mode) || st.st_size<64 || (uint64_t)st.st_size>LONG_MAX) {
            close(fd); puts("carto: invalid file size"); return 1;
        }
        size=(size_t)st.st_size;
        next=heap_caps_calloc(1,sizeof(*next),MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);
        if(!next) { close(fd); return 1; }
        next->fd=fd; next->file_size=size;
        next->bounce_capacity=sd_bounce_bytes;
        if(next->bounce_capacity)
            next->bounce=heap_caps_aligned_alloc(64,next->bounce_capacity,MALLOC_CAP_INTERNAL|MALLOC_CAP_DMA|MALLOC_CAP_8BIT);
        if(sd_bounce_bytes && !next->bounce && sd_bounce_bytes>32768) {
            next->bounce_capacity=32768;
            next->bounce=heap_caps_aligned_alloc(64,next->bounce_capacity,MALLOC_CAP_INTERNAL|MALLOC_CAP_DMA|MALLOC_CAP_8BIT);
        }
        if(sd_bounce_bytes && !next->bounce) {
            next->bounce_capacity=4096;
            next->bounce=heap_caps_aligned_alloc(64,next->bounce_capacity,MALLOC_CAP_INTERNAL|MALLOC_CAP_DMA|MALLOC_CAP_8BIT);
        }
        printf("carto SD: read() %s chunk=%zu\n",next->bounce?"internal DMA -> PSRAM":"PSRAM",next->bounce?next->bounce_capacity:0);
        snprintf(next->path,sizeof(next->path),"%s",path);
        bool cells=!strcmp(path,"/sdcard/maps/cells/index.cci");
        uint8_t header_bytes[64];if(!sd_read(next,0,header_bytes,64)) { close_sd(next);return 1; }
        bool v7=header_bytes[8]==7;
        size_t dirbytes=cells?4096:256u*1024u,stagingbytes=v7?1:192u*1024u;
        next->directory_bytes=dirbytes;next->staging_bytes=stagingbytes;
        next->directory=heap_caps_aligned_alloc(16,dirbytes,MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);
        next->staging=heap_caps_aligned_alloc(16,stagingbytes,MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);
        bool ok=false;
        if(next->directory && next->staging) {
            if(cells) {
                next->set=heap_caps_calloc(1,sizeof(*next->set),MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);
                if(next->set) {
                    source_handles_live+=sizeof(*next->set);
                    if(source_handles_live>source_handles_hw)source_handles_hw=source_handles_live;
                }
                cc_cellset_io io={next,cell_open,cell_read,cell_close};
                ok=next->set && cc_cellset_open(next->set,sd_read,next,size,io,next->directory,dirbytes,next->staging,stagingbytes);
                if(ok) next->map=next->set->map;
            } else ok=cc_ctile_open_source(&next->map,&next->source,sd_read,next,size,
                                           next->directory,dirbytes,next->staging,stagingbytes);
        }
        if(!ok) { close_sd(next);puts("carto: streaming open failed; active map retained");return 1; }
        if(!v7 && next->source.max_blob>next->source.staging_capacity) {
            heap_caps_free(next->staging);
            next->staging=heap_caps_aligned_alloc(16,next->source.max_blob,MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);
            if(!next->staging) { close_sd(next);return 1; }
            next->staging_bytes=next->source.max_blob;
            next->source.staging=next->staging;next->source.staging_capacity=next->source.max_blob;
        }
        const uint8_t *header=next->map.source->header;
        double minlon=bbox_degrees(header+12),minlat=bbox_degrees(header+16);
        double maxlon=bbox_degrees(header+20),maxlat=bbox_degrees(header+24);
        if (minlon>maxlon || minlat>maxlat || minlon< -180 || maxlon>180 || minlat< -85.05112878 || maxlat>85.05112878) {
            close_sd(next); puts("carto: invalid geographic bbox; active map retained"); return 1;
        }
        lon=(minlon+maxlon)/2; lat=(minlat+maxlat)/2;
    }
    if(xSemaphoreTake(view_lock,pdMS_TO_TICKS(2000))!=pdTRUE) {
        close_sd(next); puts("carto open: map view lock timeout; active map retained"); return 1;
    }
    map_owned=false; map_prepared=false;
    if(!source_refreshing) { atomic_store(&view_active,false); atomic_store(&dismiss_pending,false);frames_clear(); }
    release(shown); shown=NULL;
    close_sd(sd_map); sd_map=next; sd_map_size=size;
    snprintf(active_file,sizeof(active_file),"%s",next?strrchr(path,'/')+1:"");
    if(!source_refreshing) { centre_lat=lat; centre_lon=lon; }
    if(persist) { snprintf(saved.path,sizeof(saved.path),"%s",next?path:""); save_view(); }
    else { centre_lat=saved.lat; centre_lon=saved.lon; }
    source_refreshing=false;
    xSemaphoreGive(view_lock);
    int64_t elapsed=esp_timer_get_time()-start;
    printf("carto open %s bytes=%zu load=%.3f ms %.2f MB/s centre=%.7f,%.7f\n",
           next?path:"embedded",size,elapsed/1000.0,elapsed>0?(double)size/elapsed:0.0,lat,lon);
    return 0;
}
/* Serialize open/validate/publish with all map mutations. Blocking mutations
 * while any SD source is retained also covers FAT case, path and 8.3 aliases. */
static int publish_locked(const char *,const char *,bool,const char *);
static bool recover_maps(void) {
    if(!file_lock) return false;
    if(xSemaphoreTake(file_lock,pdMS_TO_TICKS(2000))!=pdTRUE) return false;
    if(xSemaphoreTake(view_lock,pdMS_TO_TICKS(2000))!=pdTRUE) { xSemaphoreGive(file_lock); return false; }
    if(!sd_map) {
        DIR *dir=opendir(MAP_DIR); struct dirent *e;
        if(dir) {
            while((e=readdir(dir))) {
                size_t n=strlen(e->d_name);
                if(n+sizeof(MAP_DIR)+1>256) continue; /* never truncate a recovery target */
                if(n>14 && !strcasecmp(e->d_name+n-14,".ctile.pending")) {
                    static EXT_RAM_BSS_ATTR char pending[272],path[256],side[280],hex[65]; struct stat st;
                    snprintf(pending,sizeof(pending),MAP_DIR "/%s",e->d_name);
                    snprintf(path,sizeof(path),MAP_DIR "/%.*s",(int)n-8,e->d_name);
                    snprintf(side,sizeof(side),"%s.sha",pending);
                    FILE *f=fopen(side,"r"); long long size,mtime; char extra;
                    int fields=f?fscanf(f,"%lld %lld %64s %c",&size,&mtime,hex,&extra):0;
                    if(f) fclose(f);
                    if(fields!=3 || stat(pending,&st) || !carto_digest_cached(pending,&st,hex)) {
                        printf("carto: pending verification metadata invalid: %s\n",pending); continue;
                    }
                    uint8_t header[64]; f=fopen(pending,"rb");
                    size_t got=f?fread(header,1,64,f):0; if(f) fclose(f);
                    if(!cc_ctile_header_check(header,got,st.st_size)) {
                        printf("carto: pending header invalid: %s\n",pending); continue;
                    }
                    if(!publish_locked(pending,path,true,hex)) {
                        unlink(side); printf("carto: installed pending %s\n",path);
                    } else printf("carto: pending swap failed: %s: %s\n",path,strerror(errno));
                    continue;
                }
                if(n<5 || strcasecmp(e->d_name+n-4,".bak")) continue;
                static EXT_RAM_BSS_ATTR char backup[272],path[256],side[272]; struct stat st;
                snprintf(backup,sizeof(backup),MAP_DIR "/%s",e->d_name);
                snprintf(path,sizeof(path),MAP_DIR "/%.*s",(int)n-4,e->d_name);
                snprintf(side,sizeof(side),"%s.sha",path); unlink(side);
                if(!stat(path,&st)) unlink(backup);
                else if(errno==ENOENT) rename(backup,path);
            }
            closedir(dir);
        }
    }
    xSemaphoreGive(view_lock); xSemaphoreGive(file_lock); return true;
}
void ls_carto_recover(void) { (void)recover_maps(); }
static int select_map_mode(const char *requested,bool persist) {
    /* Host fixture selection can release a retained source during recovery. */
    if(!requested || strcmp(requested,"embedded")) ls_carto_recover();
    if(xSemaphoreTake(file_lock,pdMS_TO_TICKS(2000))!=pdTRUE) { puts("carto open: map file lock timeout"); return 1; }
    int result=select_map_locked(requested,persist);
    xSemaphoreGive(file_lock);
    if(!result) (void)recover_maps();
    return result;
}
static int select_map(const char *requested) { return select_map_mode(requested,true); }
static int prepare_source(double lat,double lon) {
    (void)recover_maps();
    if(sd_map) return 0;
    if(saved.path[0] && !select_map_mode(saved.path,false)) return 0;
    struct stat st;
    if(!stat("/sdcard/maps/cells/index.cci",&st) && !select_map("cells")) return 0;
    char path[256];
    if(carto_choose_file(MAP_DIR,lat,lon,path,sizeof(path)) && !select_map(path)) return 0;
    cc_ctile map;
    return open_map(&map)?0:1; /* Only host tests have an embedded fixture. */
}
static int restore_map(void) { return prepare_source(centre_lat,centre_lon); }
static void close_hidden(void) {
    source_refreshing=false;
    xSemaphoreTake(file_lock,portMAX_DELAY);
    xSemaphoreTake(view_lock,portMAX_DELAY);
    map_owned=false; map_prepared=false;
    atomic_store(&view_active,false); atomic_store(&dismiss_pending,false);
    frames_clear(); release(shown); shown=NULL;
    close_sd(sd_map); sd_map=NULL; sd_map_size=0; active_file[0]=0;
    xSemaphoreGive(view_lock); xSemaphoreGive(file_lock);
    (void)recover_maps();
}
/* file_lock + view_lock held. Rename transaction retains the installed file
   on failure; boot recovery handles the backup if rollback is interrupted. */
static int publish_locked(const char *part,const char *path,bool force,const char *digest) {
    static EXT_RAM_BSS_ATTR char backup[272],side[272]; struct stat st; int result=-1;
    snprintf(backup,sizeof(backup),"%s.bak",path);
    snprintf(side,sizeof(side),"%s.sha",path);
    bool exists=stat(path,&st)==0;
    if(!exists && errno!=ENOENT) goto done;
    if(exists && (!force || !S_ISREG(st.st_mode))) { errno=EEXIST; goto done; }
    /* A failed invalidation must prevent publication. */
    if(unlink(side) && errno!=ENOENT) goto done;
    if(exists && rename(path,backup)) goto done;
    if(rename(part,path)) {
        int saved=errno;
        if(exists) rename(backup,path); /* If rollback fails, recovery retains .bak. */
        errno=saved; goto done;
    }
    if(exists) unlink(backup);
    if(!stat(path,&st) && !carto_digest_write(path,&st,digest))
        puts("carto: saved map; digest cache unavailable (VERIFY to retry)");
    result=0;
done:
    return result;
}
/* Serialize with source selection as well as render readers. FAT aliases mean
 * even a differently named target may refer to the retained source. Release
 * every borrowed map/cache before closing it, then reopen the selected path. */
static int publish_map(const char *part,const char *path,bool force,const char *digest) {
    int result=-1;
    if(xSemaphoreTake(command_lock,pdMS_TO_TICKS(2000))!=pdTRUE) { errno=ETIMEDOUT; return -1; }
    if(xSemaphoreTake(file_lock,pdMS_TO_TICKS(2000))!=pdTRUE) { xSemaphoreGive(command_lock); errno=ETIMEDOUT; return -1; }
    if(xSemaphoreTake(view_lock,pdMS_TO_TICKS(2000))!=pdTRUE) { xSemaphoreGive(file_lock); xSemaphoreGive(command_lock); errno=ETIMEDOUT; return -1; }
    static EXT_RAM_BSS_ATTR char reopen[sizeof(sd_map->path)]; /* command_lock */
    static EXT_RAM_BSS_ATTR char pending[272];
    struct stat st;
    double lat=centre_lat,lon=centre_lon;
    reopen[0]=0;
    if(stat(part,&st) || !S_ISREG(st.st_mode)) goto unlock;
    if(!stat(path,&st) && (!force || !S_ISREG(st.st_mode))) { errno=EEXIST; goto unlock; }
    /* A previous firmware's deferred update must not overwrite this fresh
     * download on a later close/reboot. Retire it before the new transaction. */
    snprintf(pending,sizeof(pending),"%s.pending",path);
    if(!stat(pending,&st)) {
        if(!carto_digest_remove(pending) || unlink(pending)) goto unlock;
    } else if(errno!=ENOENT) goto unlock;
    if(sd_map) {
        strcpy(reopen,sd_map->path);
        map_owned=false; map_prepared=false; map_invalidate_frame();
        atomic_store(&view_active,false); atomic_store(&dismiss_pending,false);
        frames_clear(); release(shown); shown=NULL;
        close_sd(sd_map); sd_map=NULL; sd_map_size=0; active_file[0]=0;
    }
    result=publish_locked(part,path,force,digest);
    int publish_errno=errno;
    if(reopen[0]) source_refreshing=true;
    xSemaphoreGive(view_lock);
    if(reopen[0]) {
        /* Reopen the original on rollback too. MAP rebuilds on its owner task;
         * allocation failure leaves saved selection available for a retry. */
        int failed=select_map_locked(reopen,false);
        xSemaphoreTake(view_lock,portMAX_DELAY);
        source_refreshing=false; centre_lat=lat; centre_lon=lon;
        xSemaphoreGive(view_lock);
        if(failed) puts("carto: source reopen failed; MAP will retry saved selection");
    }
    xSemaphoreGive(file_lock); xSemaphoreGive(command_lock);
    errno=publish_errno; return result;
unlock:
    xSemaphoreGive(view_lock); xSemaphoreGive(file_lock); xSemaphoreGive(command_lock);
    return -1;
}
#include "ls_cells_worker.h"

#ifndef LS_CARTOCORE_HOST
#define TRANSFER_CAP (512ULL*1024*1024)
#define TRANSFER_MARGIN (1024ULL*1024)
#define TRANSFER_US (INT64_C(900)*1000000)
static uint64_t transfer_limit(void) {
    uint64_t total=0,available=0;
    if(esp_vfs_fat_info("/sdcard",&total,&available)!=ESP_OK || available<=TRANSFER_MARGIN) return 0;
    available-=TRANSFER_MARGIN;
    return available<TRANSFER_CAP?available:TRANSFER_CAP;
}
static int goto_view(double lat,double lon)
{
    xSemaphoreTake(view_lock,portMAX_DELAY);
    double oldlat=centre_lat,oldlon=centre_lon;
    centre_lat=lat; centre_lon=lon;
    int result=0;
    if (shown && view && atomic_load(&view_active)) {
        int64_t left=shown->left,top=shown->top;
        origin(shown->cols,shown->rows,shown->z,&shown->left,&shown->top);
        shown->left-=shown->left%2; shown->top-=shown->top%4;
        result=redraw(shown,"goto");
        if (result) { shown->left=left; shown->top=top; centre_lat=oldlat; centre_lon=oldlon; }
    }
    if (!result) { save_view(); printf("carto centre=%.7f,%.7f\n",lat,lon); }
    xSemaphoreGive(view_lock); return result;
}

/* Detached downloads own their arguments; console argv is only borrowed. */
static portMUX_TYPE transfer_mux=portMUX_INITIALIZER_UNLOCKED;
static ls_carto_transfer transfer;
static void transfer_stage(const char *stage) {
    portENTER_CRITICAL(&transfer_mux);
    snprintf(transfer.stage,sizeof(transfer.stage),"%s",stage);
    transfer.stage_us=esp_timer_get_time();
    portEXIT_CRITICAL(&transfer_mux);
}
static atomic_bool transfer_cancel;
void ls_carto_transfer_snapshot(ls_carto_transfer *out) {
    portENTER_CRITICAL(&transfer_mux); *out=transfer; portEXIT_CRITICAL(&transfer_mux);
}
void ls_carto_transfer_cancel(void) {
    atomic_store(&transfer_cancel,true);
    portENTER_CRITICAL(&transfer_mux);
    snprintf(transfer.message,sizeof(transfer.message),"Stopping transfer...");
    portEXIT_CRITICAL(&transfer_mux);
}
static void transfer_begin(const char *file,uint64_t total) {
    atomic_store(&transfer_cancel,false);
    portENTER_CRITICAL(&transfer_mux);
    memset(&transfer,0,sizeof(transfer)); transfer.state=1;
    transfer.total=total; transfer.start_us=esp_timer_get_time();
    snprintf(transfer.file,sizeof(transfer.file),"%s",file);
    portEXIT_CRITICAL(&transfer_mux);
    transfer_stage("queued");
}
static void transfer_progress(uint64_t bytes,uint64_t total) {
    portENTER_CRITICAL(&transfer_mux); transfer.bytes=bytes;
    if(total) transfer.total=total;
    portEXIT_CRITICAL(&transfer_mux);
}
static void transfer_end(bool ok,const char *why) {
    portENTER_CRITICAL(&transfer_mux);
    snprintf(transfer.message,sizeof(transfer.message),"%s",why);
    transfer.state=ok?2:3;
    snprintf(transfer.last_error,sizeof(transfer.last_error),"%s",ok?"":why);
    portEXIT_CRITICAL(&transfer_mux);
    transfer_stage("finished");
}
void ls_carto_transfer_clear(void) {
    portENTER_CRITICAL(&transfer_mux);
    if(transfer.state!=1) memset(&transfer,0,sizeof(transfer));
    portEXIT_CRITICAL(&transfer_mux);
}
void ls_carto_transfer_fail(const char *why) { transfer_end(false,why); }
static void transfer_header(const uint8_t header[64]) {
    if(memcmp(header,"CTILE1\0\0",8)) return;
    double bbox[4]; for(int i=0;i<4;i++) bbox[i]=bbox_degrees(header+12+i*4);
    if(bbox[0]>=bbox[2] || bbox[1]>=bbox[3] || bbox[0]<-180 || bbox[2]>180 || bbox[1]<-85.051129 || bbox[3]>85.051129) return;
    portENTER_CRITICAL(&transfer_mux); memcpy(transfer.bbox,bbox,sizeof(bbox)); transfer.has_bbox=true; portEXIT_CRITICAL(&transfer_mux);
}
/* Hash exactly the bytes being written, without a second SD pass. */
typedef struct {
    mbedtls_sha256_context ctx;
    uint8_t header[64]; size_t header_used;
} stream_digest;
static bool stream_begin(stream_digest *s) {
    memset(s,0,sizeof(*s)); mbedtls_sha256_init(&s->ctx);
    return mbedtls_sha256_starts(&s->ctx,0)==0;
}
static bool stream_update(stream_digest *s,const uint8_t *p,size_t n) {
    size_t take=64-s->header_used; if(take>n) take=n;
    memcpy(s->header+s->header_used,p,take); s->header_used+=take;
    if(take && s->header_used==64) transfer_header(s->header);
    return mbedtls_sha256_update(&s->ctx,p,n)==0;
}
static bool verify_transfer(stream_digest *s,const char *sha,uint64_t expected,uint64_t bytes,int64_t deadline,char hex[65]) {
    if(atomic_load(&transfer_cancel) || esp_timer_get_time()>=deadline || (expected && bytes!=expected)) return false;
    bool valid=cc_ctile_header_check(s->header,s->header_used,bytes);
    uint8_t digest[32]; if(mbedtls_sha256_finish(&s->ctx,digest)) return false;
    for(int i=0;i<32;i++) snprintf(hex+i*2,3,"%02x",digest[i]);
    return valid && (!sha[0] || !strcasecmp(hex,sha));
}
typedef struct { char url[1024], path[256],sha[65]; uint64_t expected; bool force; } fetch_job;
static atomic_bool fetch_running;
/* Runs on the TILES worker; shares transfer progress/cancel and writer guard. */
int ls_carto_verify(const ls_tile_region *r) {
    if(atomic_exchange(&fetch_running,true)) return 1;
    transfer_begin(r->file,r->local_bytes);
    transfer_stage("verify");
    portENTER_CRITICAL(&transfer_mux); transfer.verifying=true;
    snprintf(transfer.message,sizeof(transfer.message),"Verifying SHA256; BACK to cancel");
    portEXIT_CRITICAL(&transfer_mux);
    char path[272],hex[65]; snprintf(path,sizeof(path),MAP_DIR "/%s",r->file);
    const char *why="Cannot verify SD file"; bool ok=false;
    int fd=-1; uint8_t *buf=NULL; void *bounce=NULL; struct stat st,after;
    mbedtls_sha256_context ctx; mbedtls_sha256_init(&ctx);
    bool locked=xSemaphoreTake(file_lock,pdMS_TO_TICKS(2000))==pdTRUE;
    if(!locked) { why="Map file lock timeout"; goto done; }
    if(!carto_digest_remove(path)) { why="Cannot invalidate digest cache"; goto done; }
    fd=open(path,O_RDONLY);
    if(fd<0 || fstat(fd,&st) || st.st_size<64 || (uint64_t)st.st_size>LONG_MAX) goto done;
    size_t chunk=sd_bounce_bytes?sd_bounce_bytes:32768;
    /* Bounded aligned internal allocation, with smaller fallback chunks. */
    while(chunk>=4096) {
        bounce=heap_caps_aligned_alloc(64,chunk,MALLOC_CAP_INTERNAL|MALLOC_CAP_DMA|MALLOC_CAP_8BIT);
        if(bounce) break;
        chunk/=2;
    }
    buf=heap_caps_aligned_alloc(64,chunk,MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);
    if(!bounce || !buf) { why="Verification buffer allocation failed"; goto done; }
    if(mbedtls_sha256_starts(&ctx,0)) goto done;
    uint64_t bytes=0; int64_t last_yield=esp_timer_get_time();
    while(bytes<(uint64_t)st.st_size) {
        if(atomic_load(&transfer_cancel)) { why="Verification cancelled"; goto done; }
        size_t n=(uint64_t)st.st_size-bytes; if(n>chunk) n=chunk;
        if(!carto_sd_read_at(fd,st.st_size,bytes,buf,n,bounce,chunk) || mbedtls_sha256_update(&ctx,buf,n)) goto done;
        bytes+=n; transfer_progress(bytes,st.st_size);
        int64_t now=esp_timer_get_time();
        if(now-last_yield>=20000) { vTaskDelay(1); last_yield=now; }
    }
    if(atomic_load(&transfer_cancel)) { why="Verification cancelled"; goto done; }
    if(mbedtls_sha256_finish(&ctx,(uint8_t *)buf)) goto done;
    for(int i=0;i<32;i++) snprintf(hex+i*2,3,"%02x",buf[i]);
    if(stat(path,&after) || st.st_size!=after.st_size || st.st_mtime!=after.st_mtime) { why="Map changed during verification"; goto done; }
    if(!r->catalog || !r->sha256[0]) { why="Digest computed; no catalog SHA256 supplied"; goto done; }
    if((uint64_t)st.st_size!=r->bytes || strcasecmp(hex,r->sha256)) { why="SHA256 / catalog size mismatch"; goto done; }
    ok=carto_digest_write(path,&after,hex); why=ok?"SHA256 verified":"Verified; digest cache write failed";
done:
    if(fd>=0) close(fd);
    heap_caps_free(buf); heap_caps_free(bounce); mbedtls_sha256_free(&ctx);
    if(locked) xSemaphoreGive(file_lock);
    transfer_end(ok,why); atomic_store(&fetch_running,false); return ok?0:1;
}
#define FETCH_CHUNK (32*1024)
static bool fetch_wifi(void)
{
    char ip[20]; ls_wifi_sta_ip(ip,sizeof(ip));
    if (!ls_wifi_sta_connected() || !ip[0]) {
        puts("carto fetch: WiFi is not connected (station IP required)"); return false;
    }
    printf("carto fetch: board IP %s\n",ip); return true;
}
static void fetch_worker(void *arg)
{
    fetch_job *j=arg;
    char part[sizeof(j->path)+6]; snprintf(part,sizeof(part),"%s.part",j->path);
    int fd=-1; uint8_t *buf=NULL; esp_http_client_handle_t client=NULL;
    size_t buffered=0;
    int64_t read_us=0,write_us=0,hash_us=0,download_start=0,finish_start=0;
    unsigned read_calls=0,write_calls=0;
    bool own_part=false, success=false;
    stream_digest stream; char digest[65]; bool hash_ok=stream_begin(&stream);
    const char *why="download failed";
    /* f_getfree may scan the whole FAT for minutes. Enforce the cap here;
     * short SD writes/flush failures remove the partial and retain the target. */
    uint64_t limit=TRANSFER_CAP;
    uint64_t bytes=0; int64_t start=esp_timer_get_time(),last_data=start,last_report=start;
    struct stat st;
    printf("carto fetch: starting %s -> %s\n",j->url,j->path); fflush(stdout);
    transfer_stage("recovery");
    if(!recover_maps()) { why="Map lock timeout during recovery"; goto done; }
    if(atomic_load(&transfer_cancel)) { why="Cancelled before download"; goto done; }
    if(!hash_ok) { why="SHA256 init failed"; goto done; }
    if(!limit || j->expected>limit) { why="Insufficient space or map exceeds upload cap"; goto done; }
    if (!fetch_wifi()) { why="No WiFi; connect in SYSTEM > WIRELESS"; goto done; }
    transfer_stage("prepare SD");
    if (mkdir(MAP_DIR,0777) && errno!=EEXIST) { why="cannot create " MAP_DIR; goto done; }
    if (!j->force && !stat(j->path,&st)) { why="destination exists; use -f"; goto done; }
    /* Never truncate another writer's or an interrupted download's .part. */
    fd=open(part,O_WRONLY|O_CREAT|O_EXCL,0666);
    /* Only one transfer runs at a time, so an existing .part is a leftover from a reset. */
    if (fd<0 && errno==EEXIST && unlink(part)==0) fd=open(part,O_WRONLY|O_CREAT|O_EXCL,0666);
    if (fd<0) { why="cannot create .part (check SD or remove stale .part)"; goto done; }
    own_part=true;
    /* P4 SDMMC can DMA aligned PSRAM directly. Avoid stdio's small buffer
     * and the driver's single-sector bounce fallback; no new internal pool.
     * Coalesce short HTTP reads so every bulk write starts at a sector boundary. */
    buf=heap_caps_aligned_alloc(128,FETCH_CHUNK,MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);
    if (!buf) { why="32 KB PSRAM allocation failed"; goto done; }
    /* The board's ALWAYSINTERNAL=0 keeps the larger HTTP buffer in PSRAM.
     * Match a TLS record; leave TLS allocation/window/transport pools alone. */
    esp_http_client_config_t cfg={.url=j->url,.timeout_ms=8000,.crt_bundle_attach=esp_crt_bundle_attach,
        .buffer_size=16384,.buffer_size_tx=1024,.disable_auto_redirect=true};
    client=esp_http_client_init(&cfg);
    transfer_stage("HTTP open");
    if (!client || esp_http_client_open(client,0)!=ESP_OK) { why="HTTP open failed"; goto done; }
    transfer_stage("HTTP headers");
    int64_t length=esp_http_client_fetch_headers(client);
    int status=esp_http_client_get_status_code(client);
    if (status!=200 || (length<0 && !esp_http_client_is_chunked_response(client))) {
        printf("carto fetch: HTTP status %d\n",status); why="HTTP headers/status rejected"; goto done;
    }
    if(length>0 && ((uint64_t)length>limit || (j->expected && (uint64_t)length!=j->expected))) { why="HTTP length / catalog size rejected"; goto done; }
    printf("carto fetch: length=%lld (0 means unknown)\n",(long long)length);
    last_data=esp_timer_get_time();
    download_start=last_data;
    transfer_stage("download");
    for (;;) {
        if(atomic_load(&transfer_cancel)) { why="Cancelled; partial removed"; goto done; }
        if(esp_timer_get_time()-start>=TRANSFER_US) { why="Download deadline exceeded"; goto done; }
        int64_t tick=esp_timer_get_time();
        int n=esp_http_client_read(client,(char *)buf+buffered,FETCH_CHUNK-buffered);
        int64_t now=esp_timer_get_time();
        read_us+=now-tick; read_calls++;
        if(now-start>=TRANSFER_US) { why="Download deadline exceeded"; goto done; }
        if (n>0) {
            if((uint64_t)n>limit-bytes || (j->expected && (bytes>j->expected || (uint64_t)n>j->expected-bytes)) || (length>0 && (bytes>(uint64_t)length || (uint64_t)n>(uint64_t)length-bytes))) { why="Download size exceeded"; goto done; }
            buffered+=(size_t)n;
            bytes+=(unsigned)n; last_data=now;
            transfer_progress(bytes,length>0?(uint64_t)length:j->expected);
        }
        if(buffered && (buffered==FETCH_CHUNK || n==0)) {
            tick=esp_timer_get_time();
            ssize_t written=write(fd,buf,buffered);
            write_us+=esp_timer_get_time()-tick; write_calls++;
            if(written!=(ssize_t)buffered) { why="SD write failed"; goto done; }
            tick=esp_timer_get_time();
            bool hashed=stream_update(&stream,buf,buffered);
            hash_us+=esp_timer_get_time()-tick;
            if(!hashed) { why="SHA256 update failed"; goto done; }
            buffered=0;
        }
        now=esp_timer_get_time();
        if (length>0 && bytes>(uint64_t)length) { why="HTTP length exceeded"; goto done; }
        int percent=length>0?(int)(bytes*100/(uint64_t)length):-1;
        if (now-last_report>=5000000) {
            printf("carto fetch: %llu bytes",(unsigned long long)bytes);
            if (percent>=0) printf(" %d%%",percent);
            printf(" %.1f s\n",(now-start)/1e6); fflush(stdout);
            last_report=now;
        }
        if (n==0) {
            if (!esp_http_client_is_complete_data_received(client) ||
                (length>0 && bytes!=(uint64_t)length)) { why="incomplete HTTP body"; goto done; }
            break;
        }
        if (n<0 && n!=-ESP_ERR_HTTP_EAGAIN) { why="HTTP read failed"; goto done; }
        if (now-last_data>30000000) { why="download stalled for 30 seconds"; goto done; }
        /* Blocking network/SD calls already yield to higher-priority tasks.
         * Only back off when there was no data, rather than every 32 KB. */
        if(n<0) vTaskDelay(pdMS_TO_TICKS(1));
    }
    finish_start=esp_timer_get_time();
    /* FAT close performs f_sync once, before verification/publication. */
    int close_error=close(fd); fd=-1;
    if (close_error) { why="SD flush/close failed"; goto done; }
    transfer_stage("verify");
    if (!verify_transfer(&stream,j->sha,j->expected,bytes,start+TRANSFER_US,digest)) { why=atomic_load(&transfer_cancel)?"Cancelled; partial removed":"SHA / size / CTILE mismatch; partial removed"; goto done; }
    if(atomic_load(&transfer_cancel)) { why="Cancelled; partial removed"; goto done; }
    transfer_stage("publish");
    int published=publish_map(part,j->path,j->force,digest);
    if(published<0) { why="SD publication failed; installed map retained"; goto done; }
    own_part=false; success=true;
    why=published?"Verified; pending until map closes":j->sha[0]?"Saved; SHA256 verified":"Saved; CTILE header checked";
done:
    mbedtls_sha256_free(&stream.ctx);
    if (fd>=0) close(fd);
    if (client) { esp_http_client_close(client); esp_http_client_cleanup(client); }
    heap_caps_free(buf);
    /* Failed transfers release their disk usage. */
    if(own_part) unlink(part);
    transfer_end(success,why);
    double seconds=(esp_timer_get_time()-start)/1e6;
    char ip[20]; ls_wifi_sta_ip(ip,sizeof(ip));
    printf("carto fetch: %s %s bytes=%llu seconds=%.3f MB/s=%.2f IP=%s\n",
           success?"saved":why,j->path,(unsigned long long)bytes,seconds,
           seconds>0?bytes/seconds/1e6:0.0,ip[0]?ip:"disconnected");
    printf("carto fetch timing: setup=%.3f HTTP-read=%.3f (%u calls) SD-write=%.3f (%u calls) SHA=%.3f finish=%.3f s\n",
           download_start?(download_start-start)/1e6:seconds,read_us/1e6,read_calls,
           write_us/1e6,write_calls,hash_us/1e6,
           finish_start?(esp_timer_get_time()-finish_start)/1e6:0.0);
    fflush(stdout);
    free(j); atomic_store(&fetch_running,false); vTaskDeleteWithCaps(NULL);
}
static int fetch_start_expected(int argc,char **argv,const char *sha,uint64_t expected)
{
    const char *url=NULL,*name=NULL; bool force=false;
    for (int i=2;i<argc;i++) {
        if (!strcmp(argv[i],"-f")) force=true;
        else if (!url) url=argv[i];
        else if (!name) name=argv[i];
        else goto bad;
    }
    if (!url || (strncmp(url,"http://",7) && strncmp(url,"https://",8)) || strlen(url)<9 || strlen(url)>=1024) goto bad;
    const size_t scheme=strncmp(url,"https://",8)?7:8;
    const char *path=strchr(url+scheme,'/');
    if (!path || path==url+scheme) goto bad;
    char basename[240];
    if (!name) {
        const char *end=path+strcspn(path,"?#"),*base=end;
        while (base>path && base[-1]!='/') base--;
        size_t n=(size_t)(end-base);
        if (!n || n>=sizeof(basename)) goto bad;
        memcpy(basename,base,n); basename[n]=0; name=basename;
    }
    if (!*name || strlen(name)>239 || name[0]=='.' || name[strlen(name)-1]=='.') goto bad;
    for (const unsigned char *p=(const unsigned char *)name;*p;p++)
        if (!isalnum(*p) && *p!='.' && *p!='_' && *p!='-') goto bad;
    if (!fetch_wifi()) return 1;
    if (atomic_exchange(&fetch_running,true)) { puts("carto fetch: download already running"); return 1; }
    fetch_job *j=heap_caps_calloc(1,sizeof(*j),MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);
    if (j) {
        strcpy(j->url,url); snprintf(j->path,sizeof(j->path),MAP_DIR "/%s",name); j->force=force; j->expected=expected;
        snprintf(j->sha,sizeof(j->sha),"%s",sha?sha:"");
        transfer_begin(name,expected);
        /* HTTPS plus FAT/stdio verification needs the same headroom as TILES.
         * Keep this larger stack out of scarce internal/DMA RAM. */
        if (xTaskCreateWithCaps(fetch_worker,"carto-fetch",24576,j,tskIDLE_PRIORITY+1,NULL,
                                MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT)==pdPASS) {
            puts("carto fetch: queued; console remains available"); return 0;
        }
    }
    free(j); atomic_store(&fetch_running,false); transfer_end(false,"Worker allocation failed"); puts("carto fetch: worker allocation failed"); return 1;
bad:
    puts("carto fetch URL [NAME] [-f] (http:// or https://; NAME must be a plain filename)"); return 1;
}
int ls_cartocore_fetch(const char *url,const char *name,const char *sha,uint64_t bytes) {
    char *args[]={"carto","fetch",(char *)url,(char *)name,"-f"};
    return fetch_start_expected(5,args,sha,bytes);
}
typedef struct {
    int cols,rows,z,n,result,dx,dy,move,operation; cc_mode mode; cc_edges edges;
    const char *path; double lat,lon; int argc;char **argv;
    bool display,defaults; SemaphoreHandle_t done;
} job;
#include "ls_carto_sdbench.h"
static void worker(void *arg)
{
    job *j=arg;
    cc_arch_task_enable();
    if(j->operation==6) j->result=cells_upload_command(j->argc,j->argv);
    else if (j->operation==1) j->result=select_map(j->path);
    else if (j->operation==5) {
        close_hidden(); j->result=0;
        puts("carto hidden; SD source released; selection saved");
    }
    else if (j->operation==2) j->result=map_directory(NULL,0);
    else if (j->operation==3) j->result=goto_view(j->lat,j->lon);
    else if (j->operation==4) j->result=sdbench(j->n);
    else if (j->move) j->result=move_view(j->dx,j->dy,j->move==2?j->z:0);
    else if (j->display) j->result=show(j->z);
    else if (j->defaults) {
        int successes=0;
        for (int config=0;config<3;config++) for (int ram=0;ram<2;ram++) for (int cache=0;cache<2;cache++)
            successes+=bench_one(config==1?52:120,config==1?70:40,14,config==2?CC_BRAILLE:CC_QUADRANT,
                                 config==2?CC_EDGES_SMOOTH:CC_EDGES_CRISP,50,ram,cache);
        j->result=successes?0:1;
    } else {
        int successes=0;
        for (int ram=0;ram<2;ram++) for (int cache=0;cache<2;cache++)
            successes+=bench_one(j->cols,j->rows,j->z,j->mode,j->edges,j->n,ram,cache);
        j->result=successes?0:1;
    }
    if(j->operation!=6)printf("carto worker stack unused=%u bytes\n",(unsigned)uxTaskGetStackHighWaterMark(NULL));
    xSemaphoreGive(j->done);
    vTaskSuspend(NULL);
}
static int number(const char *s, int lo, int hi, int *out)
{
    char *end; long v=strtol(s,&end,10);
    if (!*s || *end || v<lo || v>hi) return 0;
    *out=(int)v; return 1;
}
/* The supervisor owns this independent server; never stop httpd in its handler. */
typedef struct {
    httpd_handle_t server;
    uint8_t *buf;
    int64_t deadline;
    atomic_bool done;
    bool claimed;
    char code[33],ip[20];
    uint64_t limit;
    size_t internal_before,dma_before;
} recv_job;
static esp_err_t recv_reject(httpd_req_t *req,httpd_err_code_t code,const char *message)
{
    printf("carto recv: rejected: %s\n",message);
    httpd_resp_set_hdr(req,"Connection","close");
    httpd_resp_send_err(req,code,message);
    return ESP_FAIL; /* Do not drain an untrusted body after rejecting its URI. */
}
static bool recv_station_socket(httpd_req_t *req,const char *ip)
{
    /* httpd listens on AF_INET6 when IPv6 is enabled. lwIP then reports
     * IPv4 peers as ::ffff:a.b.c.d, even for an IPv4 laptop connection. */
    union {
        struct sockaddr_in v4;
#if CONFIG_LWIP_IPV6
        struct sockaddr_in6 v6;
#endif
    } local={0};
    socklen_t len=sizeof(local);
    if(getsockname(httpd_req_to_sockfd(req),(struct sockaddr *)&local,&len)) return false;
    uint32_t station=inet_addr(ip);
    if(local.v4.sin_family==AF_INET)
        return local.v4.sin_addr.s_addr==station;
#if CONFIG_LWIP_IPV6
    static const uint8_t mapped[12]={0,0,0,0,0,0,0,0,0,0,0xff,0xff};
    const uint8_t *addr=(const uint8_t *)&local.v6.sin6_addr;
    if(local.v6.sin6_family==AF_INET6 && len>=(socklen_t)sizeof(local.v6))
        return !memcmp(addr,mapped,sizeof(mapped)) && !memcmp(addr+12,&station,4);
#endif
    return false;
}
typedef struct { httpd_req_t *req;recv_job *job; } recv_asset_check;
static bool recv_asset_alive(void *ctx) {
    recv_asset_check *c=ctx;
    if(atomic_load(&transfer_cancel) || atomic_load(&c->job->done) || esp_timer_get_time()>=c->job->deadline)return false;
    /* Once the body is drained, FIN/reset means there is no client to ACK.
     * A nonblocking peek keeps directory verification cancellable too. */
    char byte;int n=recv(httpd_req_to_sockfd(c->req),&byte,1,MSG_PEEK|MSG_DONTWAIT);
    return n>0 || (n<0 && (errno==EAGAIN || errno==EWOULDBLOCK || errno==EINTR));
}
/* command_lock owns cells state. No queue or worker waits on this handler.
 * Keep one fd and coalesce TCP fragments into aligned sector-sized writes. */
static esp_err_t recv_asset(httpd_req_t *req,recv_job *j,const char *name) {
    char sha[65],offset_text[32];uint64_t offset=0;
    if(!cells_safe_asset(name) || !req->content_len || req->content_len>j->limit ||
       httpd_req_get_hdr_value_str(req,"X-Carto-SHA256",sha,sizeof(sha))!=ESP_OK)
        return recv_reject(req,HTTPD_400_BAD_REQUEST,"Invalid asset or missing SHA256");
    if(esp_timer_get_time()>=j->deadline || atomic_load(&j->done))return recv_reject(req,HTTPD_408_REQ_TIMEOUT,"Receiver expired");
    if(xSemaphoreTake(command_lock,pdMS_TO_TICKS(1500))!=pdTRUE)return recv_reject(req,HTTPD_500_INTERNAL_SERVER_ERROR,"Map worker busy");
    if(upload.active) { xSemaphoreGive(command_lock);return recv_reject(req,HTTPD_500_INTERNAL_SERVER_ERROR,"Asset writer busy"); }
    bool begun=cells_upload_begin(name,req->content_len,sha),ok=false;
    int fd=-1;size_t buffered=0;
    const char *why="Asset upload failed; partial retained",*status="500 Internal Server Error";
    stream_digest stream;bool hash_ok=stream_begin(&stream);
    recv_asset_check check={req,j};
    transfer_begin(name,req->content_len);
    if(!begun || !hash_ok)goto done;
    /* j->limit includes the session's free-space margin. Do not rescan the
     * entire FAT for every asset; short writes/close errors fail safely. */
    /* HTTP PUT always sends the whole file; console chunks remain resumable. */
    fd=open(upload.part,O_WRONLY|O_CREAT|O_TRUNC,0666);upload.offset=0;
    if(fd<0)goto done;
    int64_t last_data=esp_timer_get_time();
    transfer_stage("upload");
    while(offset<req->content_len) {
        if(atomic_load(&transfer_cancel) || esp_timer_get_time()>=j->deadline) { why="Upload cancelled or receiver expired";status="408 Request Timeout";goto done; }
        size_t want=req->content_len-offset;if(want>FETCH_CHUNK-buffered)want=FETCH_CHUNK-buffered;
        int n=httpd_req_recv(req,(char*)j->buf+buffered,want);
        int64_t now=esp_timer_get_time();
        if(n==HTTPD_SOCK_ERR_TIMEOUT) {
            if(now-last_data>=INT64_C(10000000)) { why="Upload idle timeout";status="408 Request Timeout";goto done; }
            vTaskDelay(1);continue; /* also yields if the socket fails immediately */
        }
        if(n<=0) { why="Incomplete HTTP body";status="400 Bad Request";goto done; }
        buffered+=(size_t)n;offset+=(unsigned)n;last_data=now;
        transfer_progress(offset,req->content_len);
        if(buffered==FETCH_CHUNK || offset==req->content_len) {
            if(write(fd,j->buf,buffered)!=(ssize_t)buffered || !stream_update(&stream,j->buf,buffered))goto done;
            upload.offset+=buffered;buffered=0;vTaskDelay(1);
        }
    }
    int closed=close(fd);fd=-1;if(closed)goto done;
    transfer_stage("verify");
    uint8_t digest[32];
    if(!recv_asset_alive(&check)) { why="Client disconnected, cancelled or receiver expired";status="408 Request Timeout";goto done; }
    if(mbedtls_sha256_finish(&stream.ctx,digest) || memcmp(digest,upload.sha,32) ||
       !cells_asset_valid_checked(upload.part,name,recv_asset_alive,&check)) {
        why="Asset SHA256 or format validation failed";status="400 Bad Request";cells_upload_finish(false);goto done;
    }
    if(!recv_asset_alive(&check)) { why="Client disconnected, cancelled or receiver expired";status="408 Request Timeout";goto done; }
    transfer_stage("install");
    ok=cells_upload_finish(true);if(ok) { why="Verified and installed";status="200 OK"; }
done:
    if(fd>=0)close(fd);
    mbedtls_sha256_free(&stream.ctx);
    if(begun)upload.active=false;
    transfer_end(ok,why);
    xSemaphoreGive(command_lock);
    snprintf(offset_text,sizeof(offset_text),"%llu",(unsigned long long)offset);httpd_resp_set_hdr(req,"X-Carto-Bytes",offset_text);
    httpd_resp_set_hdr(req,"Connection","close");httpd_resp_set_status(req,status);
    esp_err_t result=httpd_resp_sendstr(req,why);return ok?result:ESP_FAIL;
}
static esp_err_t recv_put(httpd_req_t *req)
{
    recv_job *j=req->user_ctx;
    char path[256],part[262];
    const char *name=req->uri+6,*query=strchr(name,'?');
    size_t len=query?(size_t)(query-name):strlen(name);
    char supplied[33]={0},value[8]={0},params[128]={0};
    bool force=false;
    if(query) {
        if(strlen(query+1)>=sizeof(params)) return recv_reject(req,HTTPD_400_BAD_REQUEST,"Query too long");
        strcpy(params,query+1);
        esp_err_t auth=httpd_query_key_value(params,"code",supplied,sizeof(supplied));
        if(auth!=ESP_OK && auth!=ESP_ERR_NOT_FOUND)
            return recv_reject(req,HTTPD_403_FORBIDDEN,"Invalid session code");
        force=httpd_query_key_value(params,"f",value,sizeof(value))==ESP_OK && !strcmp(value,"1");
    }
    if(!supplied[0]) {
        if(httpd_req_get_hdr_value_len(req,"X-Carto-Code")!=32 ||
           httpd_req_get_hdr_value_str(req,"X-Carto-Code",supplied,sizeof(supplied))!=ESP_OK)
            return recv_reject(req,HTTPD_403_FORBIDDEN,"Session code missing or invalid");
    }
    if(strcmp(supplied,j->code)) return recv_reject(req,HTTPD_403_FORBIDDEN,"Session code required");
    if(!recv_station_socket(req,j->ip))
        return recv_reject(req,HTTPD_403_FORBIDDEN,"Station interface required");
    if(!strncmp(req->uri,"/maps/",6) && !query && (strchr(name,'/')))return recv_asset(req,j,name);
    if (strncmp(req->uri,"/maps/",6) || len<7 || len>=128 || name[0]=='.' ||
        memcmp(name+len-6,".ctile",6))
        return recv_reject(req,HTTPD_400_BAD_REQUEST,"Expected /maps/NAME.ctile");
    for (size_t i=0;i<len;i++) {
        unsigned char c=name[i];
        if (!((c>='A' && c<='Z') || (c>='a' && c<='z') ||
              (c>='0' && c<='9') || c=='.' || c=='_' || c=='-'))
            return recv_reject(req,HTTPD_400_BAD_REQUEST,"Invalid filename");
    }
    if (j->claimed || atomic_load(&j->done)) {
        puts("carto recv: rejected: Receiver already used");
        httpd_resp_set_status(req,"503 Service Unavailable");
        httpd_resp_set_hdr(req,"Connection","close");
        httpd_resp_sendstr(req,"Receiver already used");
        return ESP_FAIL;
    }
    if (esp_timer_get_time()>=j->deadline)
        return recv_reject(req,HTTPD_408_REQ_TIMEOUT,"Receiver expired");
    j->claimed=true;
    transfer_stage("upload");
    transfer_progress(0,req->content_len);
    snprintf(path,sizeof(path),MAP_DIR "/%.*s",(int)len,name);
    snprintf(part,sizeof(part),"%s.part",path);
    portENTER_CRITICAL(&transfer_mux);
    snprintf(transfer.file,sizeof(transfer.file),"%.*s",(int)len,name);
    portEXIT_CRITICAL(&transfer_mux);
    FILE *file=NULL; bool own_part=false,success=false;
    stream_digest stream; char digest[65]; bool hash_ok=stream_begin(&stream);
    char expected_sha[65],filename[128]; uint64_t expected_bytes=0,sd_total=0,sd_free=0;
    snprintf(filename,sizeof(filename),"%.*s",(int)len,name);
    ls_tiles_expected(filename,expected_sha,&expected_bytes);
    const char *why="upload failed",*status="500 Internal Server Error";
    struct stat st; size_t bytes=0; int next_percent=10;
    int64_t start=esp_timer_get_time(),last_data=start;
    if(!hash_ok) { why="SHA256 init failed"; goto done; }
    if (!req->content_len) { why="Empty body"; status="400 Bad Request"; goto done; }
    if(expected_bytes && expected_bytes!=req->content_len) { why="Catalog size mismatch"; status="400 Bad Request"; goto done; }
    if(esp_vfs_fat_info("/sdcard",&sd_total,&sd_free)!=ESP_OK) { why="No SD card"; goto done; }
    if(req->content_len>j->limit || sd_free<=TRANSFER_MARGIN || req->content_len>sd_free-TRANSFER_MARGIN) { why="Disk full"; status="507 Insufficient Storage"; goto done; }
    if (mkdir(MAP_DIR,0777) && errno!=EEXIST) { why="Cannot create maps directory"; goto done; }
    if (!force && !stat(path,&st)) { why="Destination exists; use ?f=1"; status="409 Conflict"; goto done; }
    int fd=open(part,O_WRONLY|O_CREAT|O_EXCL,0666);
    /* Only one transfer runs at a time, so an existing .part is a leftover from a reset. */
    if (fd<0 && errno==EEXIST && unlink(part)==0) fd=open(part,O_WRONLY|O_CREAT|O_EXCL,0666);
    if (fd<0) { why="Cannot create .part; check SD or remove stale .part"; goto done; }
    own_part=true; file=fdopen(fd,"wb");
    if (!file) { close(fd); why="Cannot open .part stream"; goto done; }
    while (bytes<req->content_len) {
        if(atomic_load(&transfer_cancel)) { why="Cancelled; partial removed"; goto done; }
        if (esp_timer_get_time()>=j->deadline) { why="Receiver timeout"; status="408 Request Timeout"; goto done; }
        size_t remaining=req->content_len-bytes;
        int n=httpd_req_recv(req,(char *)j->buf,remaining>FETCH_CHUNK?FETCH_CHUNK:remaining);
        if(esp_timer_get_time()>=j->deadline) { why="Receiver timeout"; status="408 Request Timeout"; goto done; }
        if(n>0) last_data=esp_timer_get_time();
        if (n==HTTPD_SOCK_ERR_TIMEOUT) {
            if(esp_timer_get_time()-last_data>=INT64_C(10000000)) { why="Upload idle timeout"; status="408 Request Timeout"; goto done; }
            continue;
        }
        if (n<=0) { why="Incomplete HTTP body"; status="400 Bad Request"; goto done; }
        if (fwrite(j->buf,1,(size_t)n,file)!=(size_t)n) { why="SD write failed"; goto done; }
        if(!stream_update(&stream,j->buf,(size_t)n)) { why="SHA256 update failed"; goto done; }
        bytes+=(size_t)n; transfer_progress(bytes,req->content_len);
        int percent=(int)((uint64_t)bytes*100/req->content_len);
        if (percent>=next_percent) {
            printf("carto recv: %zu bytes %d%% %.1f s\n",bytes,percent,(esp_timer_get_time()-start)/1e6);
            fflush(stdout); next_percent=(percent/10+1)*10;
        }
    }
    int write_error=fflush(file),close_error=fclose(file); file=NULL;
    if (write_error || close_error) { why="SD flush/close failed"; goto done; }
    if (esp_timer_get_time()>=j->deadline) { why="Receiver timeout"; status="408 Request Timeout"; goto done; }
    transfer_stage("verify");
    if (!verify_transfer(&stream,expected_sha,req->content_len,bytes,j->deadline,digest)) { why=atomic_load(&transfer_cancel)?"Cancelled; partial removed":"SHA / size / CTILE mismatch; partial removed"; goto done; }
    if(atomic_load(&transfer_cancel)) { why="Cancelled; partial removed"; goto done; }
    transfer_stage("publish");
    int published=publish_map(part,path,force,digest);
    if(published<0) { why="SD publication failed; installed map retained"; goto done; }
    own_part=false; success=true;
    why=published?"Verified; pending until map closes":expected_sha[0]?"Saved; SHA256 verified":"Saved; CTILE header checked"; status=published?"202 Accepted":"201 Created";
done:
    mbedtls_sha256_free(&stream.ctx);
    if (file) fclose(file);
    if(own_part) unlink(part);
    transfer_end(success,why);
    double seconds=(esp_timer_get_time()-start)/1e6;
    printf("carto recv: %s %s bytes=%zu seconds=%.3f MB/s=%.2f\n",
           why,path,bytes,seconds,seconds>0?bytes/seconds/1e6:0.0);
    httpd_resp_set_status(req,status);
    httpd_resp_set_type(req,"text/plain");
    httpd_resp_set_hdr(req,"Connection","close");
    esp_err_t result=httpd_resp_sendstr(req,why);
    printf("carto recv stack unused=%u bytes\n",(unsigned)uxTaskGetStackHighWaterMark(NULL));
    atomic_store(&j->done,true);
    return success?result:ESP_FAIL; /* Close rejected/incomplete request bodies. */
}
static void recv_supervisor(void *arg)
{
    recv_job *j=arg;
    while (!atomic_load(&j->done) && !atomic_load(&transfer_cancel) && esp_timer_get_time()<j->deadline)
        vTaskDelay(pdMS_TO_TICKS(100));
    if (!atomic_load(&j->done)) puts("carto recv: receiver timeout; stopping");
    while(httpd_stop(j->server)!=ESP_OK) {
        puts("carto recv: stop failed; retaining handler context and retrying");
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    if (!atomic_load(&j->done)) transfer_end(false,atomic_load(&transfer_cancel)?"Cancelled":"Receiver timeout"); /* Joins the handler before freeing its context. */
    heap_caps_free(j->buf);
    heaps("recv-after");
    printf("carto recv heap delta internal=%ld DMA=%ld (after-before; supervisor still allocated)\n",
           (long)internal_free()-(long)j->internal_before,(long)dma_free()-(long)j->dma_before);
    free(j); atomic_store(&fetch_running,false);
    vTaskDelete(NULL);
}
static int recv_start(int argc,char **argv)
{
    int port=8080,seconds=300;
    if (argc>4 || (argc>=3 && !number(argv[2],1,65535,&port)) ||
        (argc==4 && !number(argv[3],1,86400,&seconds))) {
        puts("carto recv [PORT] [SECONDS] (defaults 8080, 300)"); return 1;
    }
    char ip[20]; ls_wifi_sta_ip(ip,sizeof(ip));
    if (!ls_wifi_sta_connected() || !ip[0]) { puts("carto recv: WiFi is down (station IP required)"); return 1; }
    /* Share the download guard so these two SD writers cannot collide. */
    if (atomic_exchange(&fetch_running,true)) { puts("carto recv: transfer already running"); return 1; }
    transfer_begin("Laptop push",0);
    transfer_stage("receiver start");
    portENTER_CRITICAL(&transfer_mux); transfer.receiving=true; transfer.port=port; portEXIT_CRITICAL(&transfer_mux);
    size_t before=internal_free(),dma_before=dma_free(); heaps("recv-before");
    recv_job *j=calloc(1,sizeof(*j));
    esp_err_t err=ESP_ERR_NO_MEM;
    if (!j) goto fail;
    ls_carto_recover();
    j->limit=transfer_limit();
    if(!j->limit) { err=ESP_ERR_INVALID_STATE; goto fail; }
    snprintf(j->ip,sizeof(j->ip),"%s",ip);
    uint8_t random[16]; esp_fill_random(random,sizeof(random));
    for(int i=0;i<16;i++) snprintf(j->code+i*2,3,"%02x",random[i]);
    portENTER_CRITICAL(&transfer_mux); snprintf(transfer.code,sizeof(transfer.code),"%s",j->code); portEXIT_CRITICAL(&transfer_mux);
    j->internal_before=before; j->dma_before=dma_before;
    atomic_init(&j->done,false);
    j->buf=heap_caps_aligned_alloc(128,FETCH_CHUNK,MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);
    if (!j->buf) goto fail;
    httpd_config_t cfg=HTTPD_DEFAULT_CONFIG();
    cfg.server_port=port; cfg.ctrl_port=ESP_HTTPD_DEF_CTRL_PORT+1;
    cfg.max_open_sockets=1; cfg.max_uri_handlers=1;
    cfg.recv_wait_timeout=1; cfg.send_wait_timeout=1;
    cfg.uri_match_fn=httpd_uri_match_wildcard;
    cfg.stack_size=8192; /* Heap hash scratch; log high-water for hardware sizing. */
#if CONFIG_SPIRAM_ALLOW_STACK_EXTERNAL_MEMORY
    cfg.task_caps=MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT;
    cfg.stack_size=8192;
#endif
    j->deadline=esp_timer_get_time()+(int64_t)seconds*1000000;
    err=httpd_start(&j->server,&cfg);
    if (err!=ESP_OK) goto fail;
    httpd_uri_t uri={.uri="/maps/*",.method=HTTP_PUT,.handler=recv_put,.user_ctx=j};
    err=httpd_register_uri_handler(j->server,&uri);
    if (err!=ESP_OK) goto fail;
    /* Print before starting the supervisor, which can finish and free j. */
    printf("carto recv: board IP %s port=%d timeout=%d s stack=%s\n",ip,port,seconds,
           (cfg.task_caps&MALLOC_CAP_SPIRAM)?"PSRAM":"internal");
    printf("curl --fail --upload-file FILE.ctile \"http://%s:%d/maps/FILE.ctile?code=%s\"\n",ip,port,j->code);
    printf("carto recv: code=%s max=%llu bytes; append &f=1 to overwrite; one map PUT or multiple asset PUTs, 10s idle timeout\n",j->code,(unsigned long long)j->limit);
    transfer_stage("waiting for upload");
    if (xTaskCreate(recv_supervisor,"carto-recv",3072,j,tskIDLE_PRIORITY+1,NULL)==pdPASS) return 0;
    err=ESP_ERR_NO_MEM;
fail:
    if (j) { if (j->server) while(httpd_stop(j->server)!=ESP_OK) vTaskDelay(pdMS_TO_TICKS(100)); heap_caps_free(j->buf); free(j); }
    atomic_store(&fetch_running,false);
    transfer_end(false,"Receiver start failed");
    printf("carto recv: start failed: %s\n",esp_err_to_name(err)); heaps("recv-after"); return 1;
}
int ls_cartocore_command(int argc, char **argv)
{
    if(argc==1 || (argc==2 && !strcmp(argv[1],"status")) ||
       (argc>=2 && !strcmp(argv[1],"hw"))) {
        if(argc>3 || (argc==3 && strcmp(argv[2],"reset"))) return 1;
        ls_carto_hw_status(argc==3);return 0;
    }
    if (argc==2 && !strcmp(argv[1],"pie")) {
        xSemaphoreTake(command_lock,portMAX_DELAY);
        int enabled=cc_arch_task_enable();
        printf("carto pie: IDF task ownership %s\n",enabled?"enabled":"unavailable");
        int ok=cc_arch_selftest(stdout);
        cc_arch_bench(stdout);
        xSemaphoreGive(command_lock);
        return enabled && ok?0:1;
    }
    if (argc==2 && !strcmp(argv[1],"xfer")) {
        ls_carto_transfer t; ls_carto_transfer_snapshot(&t);
        static const char *states[]={"idle","running","saved","failed"};
        int64_t now=esp_timer_get_time();
        printf("carto xfer: state=%s stage=%s bytes=%llu/%llu target=%s age=%.1fs stage_age=%.1fs writer=%d\nlast error: %s\n",
            t.state>=0 && t.state<=3?states[t.state]:"unknown",t.stage[0]?t.stage:"idle",
            (unsigned long long)t.bytes,(unsigned long long)t.total,t.file,
            t.start_us?(now-t.start_us)/1e6:0.0,t.stage_us?(now-t.stage_us)/1e6:0.0,
            atomic_load(&fetch_running),t.last_error[0]?t.last_error:"none"); return 0;
    }
    if (argc==2 && !strcmp(argv[1],"tiles")) { ls_tiles_diagnostics(); return 0; }
    if (argc>=2 && !strcmp(argv[1],"recv")) return recv_start(argc,argv);
    if (argc>=2 && !strcmp(argv[1],"fetch")) return fetch_start_expected(argc,argv,NULL,0);
    job j={.z=saved.z,.n=50,.mode=CC_QUADRANT,.edges=CC_EDGES_CRISP};
    if(argc>=2 && !strcmp(argv[1],"upload")) { j.operation=6;j.argc=argc;j.argv=argv;
    } else if (argc==2 && !strcmp(argv[1],"hide")) {
        ls_cartocore_dismiss(); j.operation=5;
    } else if (argc>=2 && !strcmp(argv[1],"sdbench")) {
        j.operation=4; j.n=8;
        if(argc>3 || (argc==3 && !number(argv[2],1,256,&j.n))) goto usage;
    } else if (argc>=2 && !strcmp(argv[1],"open")) {
        if (argc>3) goto usage;
        j.operation=1; j.path=argc==3?argv[2]:NULL;
    } else if (argc==2 && !strcmp(argv[1],"ls")) {
        j.operation=2;
    } else if (argc>=2 && !strcmp(argv[1],"goto")) {
        if (argc!=4) goto usage;
        char *end;
        errno=0; j.lat=strtod(argv[2],&end);
        if (!*argv[2] || *end || errno || !isfinite(j.lat) || fabs(j.lat)>85.05112878) goto usage;
        errno=0; j.lon=strtod(argv[3],&end);
        if (!*argv[3] || *end || errno || !isfinite(j.lon) || j.lon< -180 || j.lon>=180) goto usage;
        j.operation=3;
    } else if (argc>=2 && !strcmp(argv[1],"pan")) {
        j.move=1;
        if (argc!=4 || !number(argv[2],-4096,4096,&j.dx) || !number(argv[3],-4096,4096,&j.dy)) goto usage;
    } else if (argc>=2 && !strcmp(argv[1],"zoom")) {
        j.move=2;
        if (argc!=3 || !number(argv[2],0,16,&j.z)) goto usage;
    } else if (argc>=2 && !strcmp(argv[1],"show")) {
        j.display=true;
        if (argc>3 || (argc==3 && !number(argv[2],0,22,&j.z))) goto usage;
    } else if (argc>=2 && !strcmp(argv[1],"bench")) {
        if (argc==2) j.defaults=true;
        else {
            if (argc!=8 || !number(argv[2],1,240,&j.cols) || !number(argv[3],1,100,&j.rows) ||
                !number(argv[4],13,15,&j.z) || !number(argv[7],1,500,&j.n)) goto usage;
            if (!strcmp(argv[5],"braille")) j.mode=CC_BRAILLE;
            else if (strcmp(argv[5],"quadrant")) goto usage;
            if (!strcmp(argv[6],"smooth")) j.edges=CC_EDGES_SMOOTH;
            else if (strcmp(argv[6],"crisp")) goto usage;
        }
    } else goto usage;
    xSemaphoreTake(command_lock,portMAX_DELAY);
    size_t before=internal_free(),dma_before=dma_free();
    if(j.operation!=6)heaps("command-before");
    StaticSemaphore_t done_storage;
    j.done=xSemaphoreCreateBinaryStatic(&done_storage);
    TaskHandle_t task=NULL;
    /* Pin beside the waiting caller so completion means it has suspended
     * before the caller deletes it. Large stack belongs to PSRAM, not DMA. */
    if (xTaskCreatePinnedToCoreWithCaps(worker,"carto",16384,&j,tskIDLE_PRIORITY+1,&task,
                                     xPortGetCoreID(),MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT)!=pdPASS) {
        vSemaphoreDelete(j.done); xSemaphoreGive(command_lock);
        puts("carto: worker allocation failed"); return 1;
    }
    xSemaphoreTake(j.done,portMAX_DELAY);
    vTaskSuspend(task);
    vTaskDeleteWithCaps(task);
    vSemaphoreDelete(j.done);
    if(j.operation!=6)heaps("command-after");
    if(j.operation!=6)printf("carto command heap delta internal=%ld DMA=%ld (after-before; concurrent system activity may vary)\n",
           (long)internal_free()-(long)before,(long)dma_free()-(long)dma_before);
    xSemaphoreGive(command_lock); return j.result;
usage:
    if(argc>=2 && !strcmp(argv[1],"show")) puts("carto show: invalid arguments; expected optional zoom 0..22");
    puts("carto sdbench [MB] | carto pie | carto recv [PORT] [SECONDS] | carto fetch URL [NAME] [-f] | carto open [PATH|cells] | carto ls | carto tiles | carto xfer | carto goto LAT LON | carto bench [cols rows z braille|quadrant smooth|crisp N] | carto show [0..22] | carto pan DX DY | carto zoom 0..16 | carto hide");
    return 1;
}
void ls_cartocore_register_command(void)
{
    if (view_lock) return; /* console-start retries must not leak a mutex */
    view_lock=xSemaphoreCreateMutex();
    if (!view_lock) { puts("carto: view mutex unavailable"); return; }
    command_lock=xSemaphoreCreateMutex();
    if (!command_lock) { vSemaphoreDelete(view_lock); view_lock=NULL; puts("carto: command mutex unavailable"); return; }
    file_lock=xSemaphoreCreateMutex();
    if(!file_lock) { vSemaphoreDelete(command_lock); command_lock=NULL; vSemaphoreDelete(view_lock); view_lock=NULL; return; }
    TaskHandle_t idle=NULL;
    if(xTaskCreatePinnedToCoreWithCaps(cache_idle,"carto-idle",16384,NULL,tskIDLE_PRIORITY,&idle,
                                     xPortGetCoreID(),MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT)!=pdPASS) {
        vSemaphoreDelete(file_lock); file_lock=NULL;
        vSemaphoreDelete(command_lock); command_lock=NULL;
        vSemaphoreDelete(view_lock); view_lock=NULL; puts("carto: idle worker allocation failed"); return;
    }
    const esp_console_cmd_t command={.command="carto",.help="SD CartoCore bench and quadrant display",
                                   .hint="sdbench|pie|tiles|xfer|recv|fetch|open|ls|goto|bench|show|pan|zoom|hide",.func=ls_cartocore_command};
    esp_err_t err=esp_console_cmd_register(&command);
    if (err!=ESP_OK) {
        vTaskDeleteWithCaps(idle); vSemaphoreDelete(view_lock); view_lock=NULL;
        vSemaphoreDelete(file_lock); file_lock=NULL;
        vSemaphoreDelete(command_lock); command_lock=NULL;
        printf("carto: command registration failed: %s\n",esp_err_to_name(err));
    }
}


#else
#include "lssim_cells_tests.h"
static void lssim_carto_boundary_test(void) {
    unsigned lo=0,hi=0,z=14;double lat=0,lon=0;
    tui_cell front[4]={{0}},back[4]={{0}};
    tui_surface sf={.w=2,.h=2,.front=front,.back=back};tui_rect area={0,0,2,2};
    carto_host_worker=false;
    xSemaphoreTake(view_lock,0);
    assert(!ls_carto_map_prepare(&lat,&lon,&z));
    assert(!ls_carto_map_limits(&lo,&hi));
    assert(!ls_carto_map_draw(&sf,area,lat,lon,z,false,true));
    ls_carto_map_leave();ls_cartocore_dismiss();assert(!ls_cartocore_draw(&sf));
    xSemaphoreGive(view_lock);
    assert(!ls_carto_map_prepare(&lat,&lon,&z));
    assert(!ls_carto_map_limits(&lo,&hi));
    assert(!ls_carto_map_draw(&sf,area,lat,lon,z,false,true));
    ls_carto_map_leave();assert(!sd_map && !shown && !view);
    carto_host_worker=true;
    puts("PASS: busy view lock, asynchronous request and cancelled startup are RAM-only");
}
static void lssim_carto_console_test(void) {
    tui_surface *sf=ls_tui_surface();size_t bytes=(size_t)sf->w*sf->h*sizeof(tui_cell);
    tui_cell *previous=malloc(bytes);assert(previous);
    assert(!show(getenv("LSSIM_CTILE")?11:14));
    for(int step=0;step<14;step++) {
        unsigned generation=frame_generation;
        memcpy(previous,frames[frame_front].cells,bytes);
        int result=step<10?move_view(0,0,step<5?12+step:20-step):move_view(step&1?4:-4,2,0);
        if(result) {
            assert(frame_generation==generation);
            assert(!memcmp(previous,frames[frame_front].cells,bytes));
        }
        /* The renderer's scratch is deliberately garbage during a busy worker. */
        memset(shown->cells,0xde,(size_t)shown->cols*shown->rows*sizeof(cc_cell));
        xSemaphoreTake(view_lock,0);carto_host_worker=false;
        assert(ls_cartocore_draw(sf));
        assert(!memcmp(sf->back,frames[frame_front].cells,bytes));
        memcpy(sf->front,sf->back,bytes);
        assert(frame_take());assert(ls_cartocore_draw(sf));frame_give();
        assert(!memcmp(sf->back,sf->front,bytes));
        carto_host_worker=true;xSemaphoreGive(view_lock);
        cc_renderer_invalidate(&shown->renderer);
    }
    ls_carto_hw_status(false);ls_carto_hw_status(true);
    assert(carto_hw.cold==shown->arena.used && carto_hw.decoded==shown->decoded.used);
    free(previous);ls_cartocore_dismiss();cache_step();
    puts("PASS: carto show zoom/pan, busy render/publication locks and poisoned failed frame retain complete cells");
}
bool lssim_carto_frame_info(tui_rect *area,unsigned *z,unsigned *generation,bool *fresh) {
    *area=ui_map_area;*z=frames[frame_front].z;*generation=frames[frame_front].generation;
    *fresh=ui_map_fresh;return ui_map_drawn;
}
void lssim_cartocore_pump(void) {
    const char *probe=getenv("LSSIM_CARTO_GUARD_PROBE");
    if(probe) {
        if(!strcmp(probe,"nvs")) { int h;nvs_open("probe",0,&h); }
        else if(!strcmp(probe,"stat")) { struct stat st;stat("guard-probe",&st); }
        else if(!strcmp(probe,"opendir")) opendir("guard-probe");
        else if(!strcmp(probe,"rename")) rename("guard-probe","guard-probe2");
        else if(!strcmp(probe,"unlink")) unlink("guard-probe");
        else fopen("guard-probe","rb");
        abort();
    }
    carto_host_worker=true;
    if(!view_lock) {
        view_lock=1;command_lock=2;file_lock=4;ls_marks_defer_io();
        const char *path=getenv("LSSIM_CTILE");
        if(path) snprintf(saved.path,sizeof(saved.path),"%s",carto_host_virtual(path));
        saved_valid=path!=NULL;
        if(getenv("LSSIM_CARTO_BOUNDARY_TEST")) lssim_carto_boundary_test();
        if(getenv("LSSIM_CELLS_TEST"))lssim_cells_transactions();
        if(getenv("LSSIM_CARTO_NO_MAP_TEST")) {
            cc_ctile map;
            assert(getenv("LSSIM_CARTO_NO_EMBEDDED"));
            assert(!open_map(&map) && !map.bytes.data);
            assert(show(14) && !shown && !atomic_load(&view_active));
            assert(!bench_one(20,10,14,CC_QUADRANT,CC_EDGES_SMOOTH,1,true,false));
            assert(!sd_map && !shown);
            puts("PASS: no-map console and missing SD bench fixture");
        }
        if(getenv("LSSIM_CARTO_SD_BENCH_TEST")) {
            assert(bench_one(20,10,14,CC_QUADRANT,CC_EDGES_SMOOTH,1,true,false));
            assert(sd_map && strstr(sd_map->path,"franklin_mini.ctile"));
            puts("PASS: bench loads SD fixture without embedded map");
        }
    }
    static bool console_tested;
    if(!console_tested && getenv("LSSIM_CARTO_CONSOLE_TEST")) {
        console_tested=true;lssim_carto_console_test();
    }
    static unsigned delay;
    if(getenv("LSSIM_CARTO_SLOW_WORKER")) {
        carto_host_locks &= ~view_lock;
        if((++delay%4)!=0 && atomic_load(&frame_valid)) {
            carto_host_locks |= view_lock;carto_host_worker=false;return;
        }
    }
    static bool owned_last_turn;
    bool wanted=atomic_load(&map_wanted);
    cache_step();
    if(owned_last_turn && !wanted && !atomic_load(&view_active)) {
        assert(!sd_map && !shown && !view);puts("PASS: MAP leave released source and arenas on worker");
    }
    owned_last_turn=wanted;
    carto_host_worker=false;
}
#endif
