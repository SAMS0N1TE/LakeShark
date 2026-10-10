"""Host fault-injection tests of the production TILES queue/snapshot paths."""
from pathlib import Path
import shutil
import subprocess

root = Path(__file__).resolve().parents[2]
out = root / 'bench/build/tiles-state'
out.mkdir(parents=True, exist_ok=True)
source = (root / 'components/apps/tui/ls_tiles_backend.c').read_text()
worker = source[source.index('static void work('):source.index('static bool queue(')]
queue = source[source.index('static bool queue('):source.index('void ls_tiles_diagnostics(')]
init = source[source.index('static void ensure_lock('):source.index('static bool safe_name(')]
code = r"""
#include "ls_tiles.h"
#include <assert.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <stdatomic.h>
#include <errno.h>
#include <sys/stat.h>
#include <unistd.h>
typedef int StaticSemaphore_t;
typedef int portMUX_TYPE;
typedef int *SemaphoreHandle_t;
#define portENTER_CRITICAL(m) ((void)(m))
#define portEXIT_CRITICAL(m) ((void)(m))
#define pdTRUE 1
#define pdPASS 1
#define portMAX_DELAY 100
#define pdMS_TO_TICKS(n) (n)
#define MALLOC_CAP_SPIRAM 1
#define MALLOC_CAP_8BIT 2
#define ESP_ERR_NO_MEM 257
#define ESP_LOGE(...) ((void)0)
#define ESP_LOGI(...) ((void)0)
#define ESP_OK 0
#define ESP_FAIL 1
#define ESP_ERR_INVALID_STATE 2
#define ESP_ERR_INVALID_RESPONSE 3
#define BASE "https://example.invalid/"
const char *esp_err_to_name(int n) { (void)n; return "error"; }
static ls_tiles_catalog data;
static atomic_bool busy;
static atomic_uint snapshot_ok,snapshot_failed;
static void stage(unsigned id,const char *label,const char *file) { (void)id; (void)label; (void)file; }
enum { S_IDLE,S_LOCK,S_MOUNT,S_CAPACITY,S_FOPEN,S_FREAD,S_FCLOSE,
       S_OPENDIR,S_READDIR,S_STAT,S_CLOSEDIR,S_MKDIR,S_HTTP_INIT,
       S_HTTP_OPEN,S_HTTP_HEADERS,S_HTTP_READ,S_HTTP_CLOSE,S_HTTP_CLEANUP,
       S_FWRITE,S_UNLINK,S_RENAME,S_TRANSFER,S_REGISTER,S_FETCH,S_COMMAND,
       S_DELAY,S_PUBLISH,S_ALLOC,S_LOG,S_WIFI };
static SemaphoreHandle_t lock;
static StaticSemaphore_t lock_storage;
static portMUX_TYPE init_mux;
static bool contended, alloc_fail, task_fail;
static int created, cleared;
static SemaphoreHandle_t xSemaphoreCreateMutexStatic(StaticSemaphore_t *s) { return s; }
static int xSemaphoreTake(SemaphoreHandle_t s,int ticks) {
    assert(s); return (!contended || ticks==portMAX_DELAY)?pdTRUE:0;
}
static void xSemaphoreGive(SemaphoreHandle_t s) { assert(s); }
static void *heap_caps_calloc(size_t n,size_t size,int caps) {
    (void)caps; return alloc_fail?NULL:calloc(n,size);
}
static ls_carto_transfer transfer;
void ls_carto_transfer_snapshot(ls_carto_transfer *t) { *t=transfer; }
void ls_carto_transfer_clear(void) { cleared++; }
typedef struct { int action; bool online; ls_tile_region r; } request;
static request *pending;
static void work(void *arg);
static void vTaskDeleteWithCaps(void *arg) { (void)arg; }
static bool ls_sdcard_mounted(void) { return true; }
static bool ls_sdcard_size(uint64_t *t,uint64_t *f) { *t=*f=1000000; return true; }
static bool parse(ls_tiles_catalog *d,const char *b) { (void)d; (void)b; return true; }
static void scan(ls_tiles_catalog *d) { d->sd=true; d->count=2; }
static bool catalog_download(ls_tiles_catalog *d) {
    /* Local state must already be published while network is pending. */
    assert(data.sd && data.count==2 && atomic_load(&busy));
    snprintf(d->message,sizeof(d->message),"Loaded"); return true;
}
void ls_cartocore_register_command(void) {}
void ls_carto_recover(void) {}
int ls_carto_unlink(const char *path) { return unlink(path); }
int ls_cartocore_command(int n,char **args) { (void)n; (void)args; return 0; }
int ls_cartocore_fetch(const char *u,const char *f,const char *s,uint64_t b) {
    (void)u; (void)f; (void)s; (void)b; return 0;
}
int ls_carto_verify(const ls_tile_region *r) { (void)r; return 0; }
void ls_carto_transfer_fail(const char *why) { (void)why; }
#define heap_caps_malloc(n,c) malloc(n)
#define mkdir(path,mode) ((void)(path),(void)(mode))
static int xTaskCreateWithCaps(void (*fn)(void *),const char *name,int stack,
                              void *arg,int priority,void *handle,int caps) {
    (void)fn; (void)name; (void)priority; (void)handle; (void)caps;
    assert(lock && atomic_load(&busy)); assert(stack>=12288);
    if(task_fail) return 0;
    pending=arg; created++; return pdPASS;
}
""" + init + worker + queue + r"""
static void complete(void) {
    work(pending); pending=NULL; assert(!atomic_load(&busy));
}
int main(void) {
    ls_tiles_catalog snapshot={0};
    /* Snapshot before first queue creates the lock and reports idle. */
    assert(ls_tiles_snapshot(&snapshot) && !snapshot.busy && lock);
    alloc_fail=true;
    assert(!ls_tiles_refresh(true) && !atomic_load(&busy));
    assert(ls_tiles_snapshot(&snapshot) && snapshot.last_error==ESP_ERR_NO_MEM);
    assert(strstr(snapshot.message,"request allocation"));
    alloc_fail=false; task_fail=true;
    assert(!ls_tiles_refresh(true) && !atomic_load(&busy));
    assert(ls_tiles_snapshot(&snapshot) && strstr(snapshot.message,"worker creation"));
    task_fail=false;
    assert(ls_tiles_refresh(true) && atomic_load(&busy) && created==1);
    assert(!ls_tiles_refresh(true) && created==1);
    assert(ls_tiles_snapshot(&snapshot) && snapshot.busy);
    complete(); contended=true;
    assert(!ls_tiles_snapshot(&snapshot) && !snapshot.busy);
    contended=false;
    assert(ls_tiles_snapshot(&snapshot) && snapshot.sd && snapshot.count==2);
    transfer.state=1;
    assert(!ls_tiles_action(0,NULL) && !atomic_load(&busy) && !cleared);
    transfer.state=0;
    assert(ls_tiles_action(1,NULL) && cleared==1); complete();
    assert(ls_tiles_refresh(false)); complete();
    assert(ls_tiles_refresh(true)); alloc_fail=true; complete(); alloc_fail=false;
    assert(ls_tiles_snapshot(&snapshot) && !snapshot.busy);
    assert(strstr(snapshot.message,"state allocation") && snapshot.last_error==ESP_ERR_NO_MEM);
    assert(ls_tiles_refresh(true)); complete();
    puts("PASS: pre-init snapshot, allocation/task failures, single worker, completion contention, transfer guard, retry");
}
"""
(out / 'test.c').write_text(code)
gcc = shutil.which('gcc') or 'C:/mingw64/bin/gcc.exe'
exe = out / 'test.exe'
subprocess.run([gcc, '-std=gnu11', '-Wall', '-Wextra', '-Werror',
                '-I', str(root / 'components/apps/tui'), str(out / 'test.c'),
                '-o', str(exe)], check=True)
subprocess.run([str(exe)], check=True)
