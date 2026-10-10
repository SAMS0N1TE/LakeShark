#include "ls_cartocore_map.h"
#include "ls_cartocore_cells.h"
#include "ls_tiles.h"
#include "cartocore/cache.h"
#include "cartocore/render.h"
#include "cartocore/profile.h"
#include "cartocore/arch.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "esp_attr.h"
#include <assert.h>
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
#include <direct.h>
#include <ctype.h>
#include <limits.h>
static bool carto_host_worker,carto_host_nvs;
static unsigned carto_host_locks;
typedef int SemaphoreHandle_t;
#define pdTRUE 1
#define portMAX_DELAY -1
#define pdMS_TO_TICKS(n) (n)
static int xSemaphoreTake(int lock,int ticks) {
    if(!ticks && (carto_host_locks&lock)) return 0;
    assert(!(carto_host_locks&lock));carto_host_locks|=lock;return pdTRUE;
}
static void xSemaphoreGive(int lock) { assert(carto_host_locks&lock);carto_host_locks&=~lock; }
static void vTaskDelay(int ticks) { (void)ticks; }
static double esp_clk_cpu_freq(void) { return 1000000.; }
static int esp_ptr_internal(const void *p) { (void)p;return 0; }
/* Model the MAP translation unit's PSRAM, including source and publication
 * allocations. Other simulator allocations stand in for the board's idle use. */
typedef struct carto_host_alloc { void *ptr;size_t bytes;struct carto_host_alloc *next; } carto_host_alloc;
static carto_host_alloc *carto_host_allocs;
static size_t carto_host_psram_used;
static size_t carto_host_psram_free(unsigned caps) {
    if(!(caps&MALLOC_CAP_SPIRAM)) return heap_caps_get_free_size(caps);
    const char *limit=getenv("LSSIM_CARTO_PSRAM_FREE");
    size_t total=limit?(size_t)strtoull(limit,NULL,10):64u*1024u*1024u;
    return total>carto_host_psram_used?total-carto_host_psram_used:0;
}
static size_t carto_host_psram_largest(unsigned caps) {
    size_t bytes=carto_host_psram_free(caps);
    const char *limit=getenv("LSSIM_CARTO_PSRAM_LARGEST");
    if((caps&MALLOC_CAP_SPIRAM) && limit && bytes>(size_t)strtoull(limit,NULL,10)) bytes=(size_t)strtoull(limit,NULL,10);
    return bytes;
}
static void *carto_host_malloc(size_t bytes,unsigned caps) {
    if((caps&MALLOC_CAP_SPIRAM) && bytes>carto_host_psram_largest(caps)) return NULL;
    void *p=heap_caps_malloc(bytes,caps);
    if(p && (caps&MALLOC_CAP_SPIRAM)) {
        carto_host_alloc *a=malloc(sizeof(*a));assert(a);
        *a=(carto_host_alloc){p,bytes,carto_host_allocs};carto_host_allocs=a;carto_host_psram_used+=bytes;
    }
    return p;
}
static void *carto_host_calloc(size_t count,size_t bytes,unsigned caps) {
    if(bytes && count>SIZE_MAX/bytes) return NULL;
    void *p=carto_host_malloc(count*bytes,caps);if(p)memset(p,0,count*bytes);return p;
}
static void carto_host_free(void *p) {
    for(carto_host_alloc **a=&carto_host_allocs;*a;a=&(*a)->next) if((*a)->ptr==p) {
        carto_host_alloc *old=*a;carto_host_psram_used-=old->bytes;*a=old->next;free(old);break;
    }
    heap_caps_free(p);
}
#define heap_caps_malloc carto_host_malloc
#define heap_caps_calloc carto_host_calloc
#define heap_caps_free carto_host_free
#define heap_caps_get_free_size carto_host_psram_free
#define heap_caps_get_largest_free_block carto_host_psram_largest
static void *heap_caps_aligned_alloc(size_t align,size_t size,unsigned caps) {
    (void)align;
    if((caps&MALLOC_CAP_INTERNAL) && !(caps&MALLOC_CAP_DMA) && getenv("LSSIM_CARTO_FAIL_INTERNAL")) {
        printf("carto test: rejected internal allocation bytes=%zu\n",size);return NULL;
    }
    if(!(caps&MALLOC_CAP_DMA) && size>=1024*1024 && getenv("LSSIM_CARTO_FAIL_ARENA")) return NULL;
    return heap_caps_malloc(size,caps);
}
static const char *carto_host_virtual(const char *path) {
    static char out[256];const char *base=strrchr(path,'/');const char *back=strrchr(path,'\\');
    if(back && (!base || back>base)) base=back;
    snprintf(out,sizeof(out),"/sdcard/maps/%s",base?base+1:path);return out;
}
static const char *carto_host_path(const char *path) {
    static char out[2][1024];static unsigned turn;
    if(strncmp(path,"/sdcard",7)) return path;
    const char *cells=getenv("LSSIM_CELLS");
    if(cells && !strncmp(path,"/sdcard/maps/cells/",19)) {
        char *p=out[(turn++)&1];snprintf(p,1024,"%s/%s",cells,path+19);return p;
    }
    const char *override=getenv("LSSIM_CTILE");
    if(override && !strcmp(path,carto_host_virtual(override))) return override;
    const char *dir=getenv("LSSIM_CARTO_DIR");
    if(!dir) dir="bench/build/no-carto-sd";
    char *p=out[(turn++)&1];snprintf(p,1024,"%s%s",dir,!strncmp(path,"/sdcard/maps",12)?path+12:path+7);return p;
}
#define CARTO_IO_ASSERT() do { if(!carto_host_worker) { fputs("CartoCore file API on tui thread\n",stderr);exit(86); } } while(0)
static FILE *carto_fopen(const char *p,const char *m) { CARTO_IO_ASSERT();return fopen(carto_host_path(p),m); }
static DIR *carto_opendir(const char *p) { CARTO_IO_ASSERT();return opendir(carto_host_path(p)); }
static int carto_stat(const char *p,struct stat *st) { CARTO_IO_ASSERT();return stat(carto_host_path(p),st); }
static int carto_unlink(const char *p) { CARTO_IO_ASSERT();return unlink(carto_host_path(p)); }
static int carto_rename(const char *a,const char *b) { CARTO_IO_ASSERT();return rename(carto_host_path(a),carto_host_path(b)); }
static int carto_open(const char *p,int flags) { CARTO_IO_ASSERT();return open(carto_host_path(p),flags|O_BINARY); }
static ssize_t carto_read(int fd,void *p,size_t n) { CARTO_IO_ASSERT();return read(fd,p,n); }
static int carto_close(int fd) { CARTO_IO_ASSERT();return close(fd); }
static size_t carto_fread(void *p,size_t s,size_t n,FILE *f) { CARTO_IO_ASSERT();return fread(p,s,n,f); }
static int carto_fclose(FILE *f) { CARTO_IO_ASSERT();return fclose(f); }
static long carto_lseek(int fd,long o,int w) { CARTO_IO_ASSERT();return lseek(fd,o,w); }
static int carto_fstat(int fd,struct stat *s) { CARTO_IO_ASSERT();return fstat(fd,s); }
#define fread carto_fread
#define fclose carto_fclose
#define lseek carto_lseek
#define fstat carto_fstat
static int carto_mkdir(const char *p,int mode) { (void)mode;CARTO_IO_ASSERT();return _mkdir(carto_host_path(p)); }
#define mkdir(p,m) carto_mkdir(p,m)
#define fopen carto_fopen
#define opendir carto_opendir
#define stat(p,s) carto_stat(p,s)
#define unlink carto_unlink
#define rename carto_rename
#define open carto_open
#define read carto_read
#define close carto_close
/* Strict NVS context: only the dispatcher callback may touch flash. */
typedef int nvs_handle_t;
typedef int esp_err_t;
#define ESP_OK 0
#define NVS_READONLY 0
#define NVS_READWRITE 1
static int nvs_open(const char *ns,int mode,int *h) { (void)ns;(void)mode;if(!carto_host_nvs) { fputs("CartoCore NVS outside dispatcher\n",stderr);exit(87); }*h=1;return 0; }
static int nvs_get_blob(int h,const char *key,void *p,size_t *n) { (void)h;(void)key;(void)p;(void)n;if(!carto_host_nvs) { fputs("CartoCore NVS outside dispatcher\n",stderr);exit(87); }return -1; }
static int nvs_set_blob(int h,const char *key,const void *p,size_t n) { (void)h;(void)key;(void)p;(void)n;if(!carto_host_nvs) { fputs("CartoCore NVS outside dispatcher\n",stderr);exit(87); }return 0; }
static int nvs_commit(int h) { (void)h;if(!carto_host_nvs) { fputs("CartoCore NVS outside dispatcher\n",stderr);exit(87); }return 0; }
static void nvs_close(int h) { (void)h;if(!carto_host_nvs) { fputs("CartoCore NVS outside dispatcher\n",stderr);exit(87); } }
static const char *esp_err_to_name(int e) { (void)e;return "host error"; }
static int ls_nvs_call(int (*fn)(void *),void *ctx,unsigned size) {
    (void)size;assert(carto_host_worker);carto_host_nvs=true;int e=fn(ctx);carto_host_nvs=false;return e;
}
#define ls_nvs_run ls_nvs_call
/* Generated from the same embedded fixture, without linker/assembler tricks. */
#include "lssim_carto_mini.h"
static const uint8_t *mini_start=carto_mini,*mini_end=carto_mini+sizeof(carto_mini);
