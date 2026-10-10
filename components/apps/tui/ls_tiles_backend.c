#include "ls_board.h"
/* SD and network operations live off the render/input task. */
#include "sdkconfig.h"
#if defined(ESP_PLATFORM) && LS_HAS_CARTOCORE
#include "ls_tiles.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "esp_vfs_fat.h"
#include "esp_attr.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "ls_sdcard.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "freertos/idf_additions.h"
#include "cJSON.h"
#include "../../../main/ls_wifi.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>
#include <ctype.h>
#include <stdatomic.h>
#include <errno.h>
#include <math.h>
#include "ls_carto_digest.h"

#define BASE "https://cartocore-maps.pages.dev/"
/* ~24 KB of region metadata belongs in PSRAM, not the internal/DMA heap. */
static EXT_RAM_BSS_ATTR ls_tiles_catalog data;
static StaticSemaphore_t lock_storage;
static portMUX_TYPE init_mux=portMUX_INITIALIZER_UNLOCKED;
static SemaphoreHandle_t lock;
static atomic_bool busy;
/* Independent of the catalog mutex: diagnostics must survive a stuck worker.
 * The critical section only copies trace metadata, never performs I/O. */
static portMUX_TYPE stage_mux=portMUX_INITIALIZER_UNLOCKED;
static atomic_uint stage_id;
static int64_t stage_us;
static char stage_label[40]="idle",stage_file[272];
static atomic_uint snapshot_ok,snapshot_failed;
static void stage(unsigned id,const char *label,const char *file) {
    int64_t now=esp_timer_get_time();
    portENTER_CRITICAL(&stage_mux);
    stage_us=now;
    snprintf(stage_label,sizeof(stage_label),"%s",label);
    if(file) snprintf(stage_file,sizeof(stage_file),"%s",file);
    atomic_store_explicit(&stage_id,id,memory_order_relaxed);
    portEXIT_CRITICAL(&stage_mux);
}
enum { S_IDLE,S_LOCK,S_MOUNT,S_CAPACITY,S_FOPEN,S_FREAD,S_FCLOSE,
       S_OPENDIR,S_READDIR,S_STAT,S_CLOSEDIR,S_MKDIR,S_HTTP_INIT,
       S_HTTP_OPEN,S_HTTP_HEADERS,S_HTTP_READ,S_HTTP_CLOSE,S_HTTP_CLEANUP,
       S_FWRITE,S_UNLINK,S_RENAME,S_TRANSFER,S_REGISTER,S_FETCH,S_COMMAND,
       S_DELAY,S_PUBLISH,S_ALLOC,S_LOG,S_WIFI };
/* These wrappers mark every invocation, including each file/read iteration.
 * Function-like macros suppress their own expansion in the underlying call. */
#define fopen(p,m) (stage(S_FOPEN,"fopen",p),fopen(p,m))
#define fread(b,s,n,f) (stage(S_FREAD,"fread",NULL),fread(b,s,n,f))
#define fclose(f) (stage(S_FCLOSE,"fclose",NULL),fclose(f))
#define fwrite(b,s,n,f) (stage(S_FWRITE,"fwrite",NULL),fwrite(b,s,n,f))
#define opendir(p) (stage(S_OPENDIR,"opendir",p),opendir(p))
#define readdir(d) (stage(S_READDIR,"readdir",NULL),readdir(d))
#define closedir(d) (stage(S_CLOSEDIR,"closedir",NULL),closedir(d))
#define stat(p,s) (stage(S_STAT,"stat",p),stat(p,s))
#define mkdir(p,m) (stage(S_MKDIR,"mkdir",p),mkdir(p,m))
#define unlink(p) (stage(S_UNLINK,"unlink",p),unlink(p))
#define rename(p,q) (stage(S_RENAME,"rename",p),rename(p,q))
#define ls_sdcard_mounted() (stage(S_MOUNT,"mount-state",NULL),ls_sdcard_mounted())
#define ls_sdcard_size(t,f) (stage(S_CAPACITY,"fat-capacity",NULL),ls_sdcard_size(t,f))
#define vTaskDelay(t) (stage(S_DELAY,"yield",NULL),vTaskDelay(t))
#define ls_wifi_sta_connected() (stage(S_WIFI,"wifi-state",""),ls_wifi_sta_connected())
#define esp_http_client_init(c) (stage(S_HTTP_INIT,"https-init",NULL),esp_http_client_init(c))
#define esp_http_client_open(c,n) (stage(S_HTTP_OPEN,"https-open",NULL),esp_http_client_open(c,n))
#define esp_http_client_fetch_headers(c) (stage(S_HTTP_HEADERS,"https-headers",NULL),esp_http_client_fetch_headers(c))
#define esp_http_client_read(c,b,n) (stage(S_HTTP_READ,"https-read",NULL),esp_http_client_read(c,b,n))
#define esp_http_client_close(c) (stage(S_HTTP_CLOSE,"https-close",NULL),esp_http_client_close(c))
#define esp_http_client_cleanup(c) (stage(S_HTTP_CLEANUP,"https-cleanup",NULL),esp_http_client_cleanup(c))
void ls_tiles_address(char *out,size_t size) { ls_wifi_sta_ip(out,size); if(!out[0]) snprintf(out,size,"no WiFi"); }
extern int ls_cartocore_command(int argc,char **argv);
extern int ls_cartocore_fetch(const char *,const char *,const char *,uint64_t);
extern void ls_cartocore_register_command(void);

static void ensure_lock(void) {
    portENTER_CRITICAL(&init_mux);
    if(!lock) lock=xSemaphoreCreateMutexStatic(&lock_storage);
    portEXIT_CRITICAL(&init_mux);
}
static void error(ls_tiles_catalog *d,int code,const char *message) {
    d->last_error=code;
    snprintf(d->message,sizeof(d->message),"%s",message);
}
static void publish_error(int code,const char *message) {
    stage(S_LOCK,"catalog-lock",""); xSemaphoreTake(lock,portMAX_DELAY);
    error(&data,code,message);
    xSemaphoreGive(lock);
    stage(S_LOG,"error-log","");
    ESP_LOGE("tiles","%s (%s)",message,esp_err_to_name(code));
}
static bool safe_name(const char *s) {
    size_t n=strlen(s); if(n<7 || n>=128 || s[0]=='.' || strcmp(s+n-6,".ctile")) return false;
    for(;*s;s++) if(!isalnum((unsigned char)*s) && *s!='_' && *s!='-' && *s!='.') return false;
    return true;
}
static double degree(const uint8_t *p) {
    uint32_t u=(uint32_t)p[0]|(uint32_t)p[1]<<8|(uint32_t)p[2]<<16|(uint32_t)p[3]<<24;
    return (int32_t)u/1e7;
}
static void scan(ls_tiles_catalog *d) {
    ls_cartocore_register_command(); ls_carto_recover();
    /* Mount presence and capacity query are separate facts. */
    d->sd=ls_sdcard_mounted(); d->maps=0; d->total=d->free=0;
    if(d->sd && !ls_sdcard_size(&d->total,&d->free))
        error(d,ESP_FAIL,"SD mounted; capacity query failed");
    for(int i=0;i<d->count;i++) { d->region[i].status=0; d->region[i].verified=false; d->region[i].local_bytes=0; d->region[i].partial_bytes=0; }
    if(!d->sd) { error(d,ESP_ERR_NOT_FOUND,"No SD; insert a card"); return; }
    DIR *dir=opendir("/sdcard/maps");
    if(!dir) { if(errno!=ENOENT) error(d,ESP_FAIL,"Cannot scan SD maps directory"); return; }
    struct dirent *e;
    while((e=readdir(dir))) {
        char name[128],path[272]; struct stat st;
        size_t n=strlen(e->d_name); bool partial=n>5 && !strcmp(e->d_name+n-5,".part");
        if(n>=sizeof(name)) continue;
        strcpy(name,e->d_name); if(partial) name[n-5]=0;
        if(!safe_name(name)) continue;
        snprintf(path,sizeof(path),"/sdcard/maps/%s",e->d_name);
        if(stat(path,&st) || !S_ISREG(st.st_mode)) continue;
        d->maps+=st.st_size;
        int i; for(i=0;i<d->count;i++) if(!strcmp(d->region[i].file,name)) break;
        if(i==d->count) {
            if(i==LS_TILES_MAX) { snprintf(d->message,sizeof(d->message),"Region limit: first 48 shown"); continue; }
            d->count++; memset(&d->region[i],0,sizeof(d->region[i]));
            snprintf(d->region[i].file,sizeof(d->region[i].file),"%s",name);
            snprintf(d->region[i].name,sizeof(d->region[i].name),"%.*s",(int)strlen(name)-6,name);
        }
        ls_tile_region *r=&d->region[i];
        if(partial) {
            r->partial_bytes=st.st_size; if(!r->local_bytes) r->status=2;
            if(!r->catalog) {
                r->bytes=st.st_size; FILE *f=fopen(path,"rb"); uint8_t h[64];
                if(f) { if(fread(h,1,64,f)==64 && !memcmp(h,"CTILE1\0\0",8)) {
                    for(int j=0;j<4;j++) r->bbox[j]=degree(h+12+4*j);
                    r->zmin=h[28]; r->zmax=h[29]; r->bytes=0;
                    for(int j=0;j<8;j++) r->bytes|=(uint64_t)h[48+j]<<(8*j);
                } fclose(f); }
            }
            continue;
        }
        r->local_bytes=st.st_size; r->status=1;
        if(!r->catalog) {
            r->bytes=st.st_size; FILE *f=fopen(path,"rb"); uint8_t h[64];
            if(f) { if(fread(h,1,64,f)==64 && !memcmp(h,"CTILE1\0\0",8)) {
                for(int j=0;j<4;j++) r->bbox[j]=degree(h+12+4*j);
                r->zmin=h[28]; r->zmax=h[29];
            } else r->status=2; fclose(f); }
        } else if(r->bytes!=r->local_bytes) r->status=3;
        else r->verified=carto_digest_cached(path,&st,r->sha256);
    }
    closedir(dir);
}
static const char *str(cJSON *obj,const char *key) {
    cJSON *v=cJSON_GetObjectItemCaseSensitive(obj,key); return cJSON_IsString(v)?v->valuestring:"";
}
/* Bound cJSON recursion and allocations before it sees any untrusted bytes.
 * At most 4096 nodes/strings of <=64 KiB total input, depth <=8. */
static bool catalog_bounds(const char *json) {
    size_t depth=0,tokens=0,n=strnlen(json,65537);
    bool quoted=false,escape=false;
    if(n>65536) return false;
    for(size_t i=0;i<n;i++) {
        unsigned char c=json[i];
        if(quoted) {
            if(escape) escape=false;
            else if(c=='\\') escape=true;
            else if(c=='"') quoted=false;
            continue;
        }
        if(c=='"') { quoted=true; if(++tokens>4096) return false; }
        else if(c=='[' || c=='{') { if(++depth>8 || ++tokens>4096) return false; }
        else if(c==']' || c=='}') { if(!depth) return false; depth--; }
        else if(c==',' || c==':') { if(++tokens>4096) return false; }
    }
    return !quoted && !depth;
}
static bool parse(ls_tiles_catalog *d,const char *json) {
    if(!catalog_bounds(json)) return false;
    cJSON *root=cJSON_ParseWithOpts(json,NULL,true); if(!root) return false;
    cJSON *rows=cJSON_IsArray(root)?root:cJSON_GetObjectItemCaseSensitive(root,"regions");
    if(!cJSON_IsArray(rows) || cJSON_GetArraySize(rows)>LS_TILES_MAX) { cJSON_Delete(root); return false; }
    ls_tile_region *next=heap_caps_calloc(LS_TILES_MAX,sizeof(*next),MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);
    if(!next) { cJSON_Delete(root); return false; }
    int count=0; cJSON *o;
    cJSON_ArrayForEach(o,rows) {
        ls_tile_region r={0}; const char *file=str(o,"file"),*sha=str(o,"sha256");
        cJSON *bytes=cJSON_GetObjectItemCaseSensitive(o,"bytes"),*bbox=cJSON_GetObjectItemCaseSensitive(o,"bbox");
        cJSON *zmin=cJSON_GetObjectItemCaseSensitive(o,"zmin"),*zmax=cJSON_GetObjectItemCaseSensitive(o,"zmax");
        bool valid=safe_name(file) && cJSON_IsNumber(bytes) && bytes->valuedouble>=64 && bytes->valuedouble<1e12 && floor(bytes->valuedouble)==bytes->valuedouble &&
            cJSON_IsArray(bbox) && cJSON_GetArraySize(bbox)==4 && cJSON_IsNumber(zmin) && cJSON_IsNumber(zmax) &&
            zmin->valuedouble==zmin->valueint && zmax->valuedouble==zmax->valueint &&
            zmin->valueint>=0 && zmax->valueint<=22 && zmin->valueint<=zmax->valueint &&
            (!sha[0] || strlen(sha)==64) && strlen(str(o,"name"))>0;
        for(int i=0;i<4;i++) { cJSON *v=cJSON_GetArrayItem(bbox,i); if(!cJSON_IsNumber(v) || !isfinite(v->valuedouble)) valid=false; else r.bbox[i]=v->valuedouble; }
        for(const char *p=sha;*p;p++) if(!isxdigit((unsigned char)*p)) valid=false;
        if(r.bbox[0]<-180 || r.bbox[2]>180 || r.bbox[1]<-85.051129 || r.bbox[3]>85.051129 || r.bbox[0]>=r.bbox[2] || r.bbox[1]>=r.bbox[3]) valid=false;
        for(int i=0;i<count;i++) if(!strcmp(next[i].file,file)) valid=false;
        if(!valid) { free(next); cJSON_Delete(root); return false; }
        snprintf(r.file,sizeof(r.file),"%s",file); snprintf(r.name,sizeof(r.name),"%s",str(o,"name"));
        snprintf(r.description,sizeof(r.description),"%s",str(o,"description")); snprintf(r.sha256,sizeof(r.sha256),"%s",sha);
        r.bytes=(uint64_t)bytes->valuedouble; r.zmin=zmin->valueint; r.zmax=zmax->valueint; r.catalog=true;
        next[count++]=r;
    }
    memcpy(d->region,next,count*sizeof(*next)); d->count=count; free(next);
    cJSON_Delete(root); return true;
}
static bool catalog_download(ls_tiles_catalog *d) {
    if(!ls_wifi_sta_connected()) { error(d,ESP_ERR_INVALID_STATE,"No WiFi; showing saved regions"); return false; }
    char *buf=heap_caps_malloc(65537,MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);
    if(!buf) { error(d,ESP_ERR_NO_MEM,"Catalog buffer allocation failed"); return false; }
    esp_http_client_config_t cfg={.url=BASE "catalog.json",.crt_bundle_attach=esp_crt_bundle_attach,
        .timeout_ms=5000,.disable_auto_redirect=true,.buffer_size=2048};
    esp_http_client_handle_t c=esp_http_client_init(&cfg);
    bool ok=false; int used=0; esp_err_t err=ESP_ERR_NO_MEM;
    const char *why="HTTPS client allocation failed";
    if(c) {
        err=esp_http_client_open(c,0); why="Catalog HTTPS connection failed";
        if(err==ESP_OK) {
            int64_t headers=esp_http_client_fetch_headers(c);
            int status=esp_http_client_get_status_code(c);
            if(headers<0) { err=ESP_FAIL; why="Catalog HTTP headers failed"; }
            else if(status!=200) {
                err=ESP_FAIL; snprintf(d->message,sizeof(d->message),"Catalog HTTP %d; saved regions shown",status);
                why=NULL;
            } else {
                int n=0;
                while(used<65536) { n=esp_http_client_read(c,buf+used,65536-used); if(n<=0) break; used+=n; }
                err=ESP_FAIL; why=used==65536?"Catalog exceeds 64 KB":"Catalog read incomplete";
                if(used<65536 && n>=0 && esp_http_client_is_complete_data_received(c)) {
                    buf[used]=0; ok=!memchr(buf,0,used) && parse(d,buf);
                    err=ok?ESP_OK:ESP_ERR_INVALID_RESPONSE; why="Catalog JSON invalid or allocation failed";
                }
            }
        }
        esp_http_client_close(c); esp_http_client_cleanup(c);
    }
    if(ok) {
        error(d,ESP_OK,"Catalog refreshed");
        if(d->sd) {
            FILE *f=fopen("/sdcard/maps/catalog.json.part","wb");
            if(f) { bool written=fwrite(buf,1,used,f)==(size_t)used; int closed=fclose(f);
                if(written && !closed) {
                    unlink("/sdcard/maps/catalog.json");
                    if(rename("/sdcard/maps/catalog.json.part","/sdcard/maps/catalog.json"))
                        error(d,ESP_FAIL,"Catalog loaded; saving cache failed");
                } else error(d,ESP_FAIL,"Catalog loaded; writing cache failed");
            } else error(d,ESP_FAIL,"Catalog loaded; cannot write SD cache");
        }
    } else { d->last_error=err; if(why) snprintf(d->message,sizeof(d->message),"%s; saved regions shown",why); }
    stage(S_LOG,"catalog-log","");
    ESP_LOGI("tiles","catalog: %s (error=%s)",d->message,esp_err_to_name(d->last_error));
    free(buf); return ok;
}

typedef struct { int action; bool online; ls_tile_region r; } request;
static void work(void *arg) {
    stage(S_ALLOC,"state-alloc","");
    request *q=arg; ls_tiles_catalog *d=heap_caps_calloc(1,sizeof(*d),MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);
    if(!d) { publish_error(ESP_ERR_NO_MEM,"Tiles state allocation failed"); goto done; }
    stage(S_LOCK,"catalog-lock",""); xSemaphoreTake(lock,portMAX_DELAY); *d=data; xSemaphoreGive(lock);
    d->message[0]=0; d->last_error=ESP_OK;
    ls_carto_transfer t; stage(S_TRANSFER,"transfer-snapshot",""); ls_carto_transfer_snapshot(&t);
    if(q->action<0) {
        if(t.state==1) { error(d,ESP_ERR_INVALID_STATE,"Transfer active; refresh after it finishes"); goto publish; }
        d->count=0;
        /* Publish presence before any FAT/VFS operation (including cache open). */
        d->sd=ls_sdcard_mounted(); d->maps=0; d->total=d->free=0;
        stage(S_PUBLISH,"publish-mount","");
        xSemaphoreTake(lock,portMAX_DELAY); data=*d; xSemaphoreGive(lock);
        FILE *f=d->sd?fopen("/sdcard/maps/catalog.json","rb"):NULL;
        if(f) { char *b=heap_caps_malloc(65537,MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);
            if(b) {
                size_t n=fread(b,1,65536,f); b[n]=0;
                if(ferror(f) || n==65536 || memchr(b,0,n) || !parse(d,b))
                    error(d,ESP_ERR_INVALID_RESPONSE,"Saved catalog unreadable or invalid");
                free(b);
            } else error(d,ESP_ERR_NO_MEM,"Saved catalog buffer allocation failed");
            fclose(f);
        }
        if(ls_sdcard_mounted()) mkdir("/sdcard/maps",0777);
        scan(d);
        if(!d->message[0]) snprintf(d->message,sizeof(d->message),"Saved regions loaded");
        /* Publish local maps before potentially slow HTTPS. */
        stage(S_PUBLISH,"publish",""); xSemaphoreTake(lock,portMAX_DELAY); data=*d; xSemaphoreGive(lock);
        if(q->online) { catalog_download(d); scan(d); }
    } else {
        char path[272]; snprintf(path,sizeof(path),"/sdcard/maps/%s",q->r.file);
        stage(S_REGISTER,"carto-register",""); ls_cartocore_register_command();
        if(q->action==0) {
            uint64_t total,free_b; char url[256]; snprintf(url,sizeof(url),BASE "%s",q->r.file);
            if(!ls_sdcard_size(&total,&free_b)) error(d,ESP_FAIL,
                ls_sdcard_mounted()?"Cannot query SD free space":"No SD; insert a card");
            else if(free_b<q->r.bytes+65536) error(d,ESP_ERR_NO_MEM,"Disk full; delete a region first");
            else if((stage(S_FETCH,"carto-fetch",q->r.file),ls_cartocore_fetch(url,q->r.file,q->r.sha256,q->r.bytes))) error(d,ESP_FAIL,"Cannot download; check WiFi / active transfer");
        } else if(q->action==1) {
            uint64_t total,free_b; char *args[]={"carto","recv"};
            if(!ls_sdcard_size(&total,&free_b)) error(d,ESP_FAIL,
                ls_sdcard_mounted()?"Cannot query SD free space":"No SD; insert a card");
            else if((stage(S_COMMAND,"carto-recv",""),ls_cartocore_command(2,args))) error(d,ESP_FAIL,"Cannot receive; check WiFi / active transfer");
        } else if(q->action==2) {
            char *open[]={"carto","open",path}; char z[8]; int zoom=14;
            if(zoom<q->r.zmin) zoom=q->r.zmin;
            if(zoom>q->r.zmax) zoom=q->r.zmax;
            snprintf(z,sizeof(z),"%d",zoom); char *show[]={"carto","show",z};
            if((stage(S_COMMAND,"carto-open",path),ls_cartocore_command(3,open)) || (stage(S_COMMAND,"carto-show",path),ls_cartocore_command(3,show))) error(d,ESP_FAIL,"Cannot open: invalid map / insufficient PSRAM");
            else snprintf(d->message,sizeof(d->message),"Map opened");
        } else if(q->action==3) {
            if(t.state==1) error(d,ESP_ERR_INVALID_STATE,"Transfer active; finish or cancel first");
            else {
                bool ok=ls_carto_unlink(path)==0 || errno==ENOENT;
                if(!ok && errno==EBUSY) error(d,ESP_ERR_INVALID_STATE,"Active map; open embedded before deleting");
                else {
                    char side[288]; snprintf(side,sizeof(side),"%s.sha",path); unlink(side);
                    strcat(path,".part"); ok=(unlink(path)==0 || errno==ENOENT) && ok;
                    error(d,ok?ESP_OK:ESP_FAIL,ok?"Region deleted":"Delete failed; check SD"); scan(d);
                }
            }
        } else if(q->action==4) {
            if(ls_carto_verify(&q->r)) error(d,ESP_FAIL,"Verification failed or cancelled");
            else snprintf(d->message,sizeof(d->message),"SHA256 verified");
            scan(d);
        }
        if(q->action<=1 && d->message[0]) {
            stage(S_TRANSFER,"transfer-snapshot",""); ls_carto_transfer_snapshot(&t);
            if(t.state==0) ls_carto_transfer_fail(d->message);
        }
    }
publish:
    stage(S_PUBLISH,"publish",""); xSemaphoreTake(lock,portMAX_DELAY); data=*d; xSemaphoreGive(lock); free(d);
done:
    stage(S_LOG,"completion-log","");
    ESP_LOGI("tiles","worker done; stack unused=%u bytes",(unsigned)uxTaskGetStackHighWaterMark(NULL));
    free(q); stage(S_IDLE,"idle",""); atomic_store(&busy,false); vTaskDeleteWithCaps(NULL);
}
static bool queue(int action,bool online,const ls_tile_region *r) {
    ensure_lock();
    if(atomic_exchange(&busy,true)) return false;
    if(action==0 || action==1 || action==4) {
        ls_carto_transfer t; ls_carto_transfer_snapshot(&t);
        if(t.state==1) { publish_error(ESP_ERR_INVALID_STATE,"Transfer active; finish or cancel first"); atomic_store(&busy,false); return false; }
        ls_carto_transfer_clear();
    }
    request *q=heap_caps_calloc(1,sizeof(*q),MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT); if(q) { q->action=action; q->online=online; if(r) q->r=*r;
        if(xTaskCreateWithCaps(work,"tiles",24576,q,1,NULL,MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT)==pdPASS) return true; }
    publish_error(ESP_ERR_NO_MEM,q?"Tiles worker creation failed":"Tiles request allocation failed");
    free(q); atomic_store(&busy,false); return false;
}
bool ls_tiles_refresh(bool online) { return queue(-1,online,NULL); }
bool ls_tiles_action(int action,const ls_tile_region *r) { return queue(action,false,r); }
bool ls_tiles_snapshot(ls_tiles_catalog *out) {
    ensure_lock();
    /* Contention must never preserve a stale busy=true. */
    bool copied=xSemaphoreTake(lock,pdMS_TO_TICKS(10))==pdTRUE;
    if(copied) { *out=data; xSemaphoreGive(lock); }
    atomic_fetch_add(copied?&snapshot_ok:&snapshot_failed,1);
    out->busy=atomic_load(&busy);
    return copied;
}
void ls_tiles_diagnostics(void) {
    ensure_lock();
    char label[40],file[272]; int64_t since; unsigned id;
    portENTER_CRITICAL(&stage_mux);
    id=atomic_load_explicit(&stage_id,memory_order_relaxed); since=stage_us;
    memcpy(label,stage_label,sizeof(label)); memcpy(file,stage_file,sizeof(file));
    portEXIT_CRITICAL(&stage_mux);
    printf("carto tiles: stage=%s for %lld ms id=%u file=%s\n",label,
        (long long)((esp_timer_get_time()-since)/1000),id,file);
    printf("screen snapshots: succeeded=%u failed=%u\n",
        atomic_load(&snapshot_ok),atomic_load(&snapshot_failed));
    /* No indefinite diagnostic wait and no console I/O under the state lock. */
    if(xSemaphoreTake(lock,pdMS_TO_TICKS(10))!=pdTRUE) {
        printf("carto tiles: busy=%d catalog lock unavailable\n",atomic_load(&busy)); return;
    }
    int count=data.count,last_error=data.last_error; bool sd=data.sd;
    uint64_t maps=data.maps,total=data.total,free_b=data.free; char message[96];
    memcpy(message,data.message,sizeof(message)); xSemaphoreGive(lock);
    printf("carto tiles: busy=%d sd=%d count=%d maps=%llu total=%llu free=%llu\n",
        atomic_load(&busy),sd,count,(unsigned long long)maps,
        (unsigned long long)total,(unsigned long long)free_b);
    printf("message: %s\nlast error: %s (0x%x)\n",message,
        esp_err_to_name(last_error),(unsigned)last_error);
}
void ls_tiles_expected(const char *file,char sha[65],uint64_t *bytes) {
    sha[0]=0; *bytes=0;
    if(!lock) return;
    xSemaphoreTake(lock,portMAX_DELAY);
    for(int i=0;i<data.count;i++) if(data.region[i].catalog && !strcmp(file,data.region[i].file)) {
        snprintf(sha,65,"%s",data.region[i].sha256); *bytes=data.region[i].bytes; break;
    }
    xSemaphoreGive(lock);
}
#endif
