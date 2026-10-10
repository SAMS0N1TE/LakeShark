/* Included by ls_cartocore.c: single-owner SD/network service on carto-idle.
 * All large state and buffers are PSRAM. Public entry points only copy mail. */
#include "ls_cells.h"
#include "cartocore/places.h"
#include "mbedtls/sha256.h"
static FILE *cells_fopen(const char *path,const char *mode) {
    FILE *f=fopen(path,mode);if(f)setvbuf(f,NULL,_IONBF,0);return f;
}
#define CELLS_ROOT "/sdcard/maps/"
#define CELLS_BITS 8192
static EXT_RAM_BSS_ATTR struct {
    char query[64],url[256];unsigned serial,pick,radius,action;uint32_t key;
} cells_mail,cells_request;
static EXT_RAM_BSS_ATTR ls_cells_view cells_pub,cells_work;
static atomic_flag cells_guard=ATOMIC_FLAG_INIT;
static bool cells_take(void) { return !atomic_flag_test_and_set(&cells_guard); }
static void cells_give(void) { atomic_flag_clear(&cells_guard); }
bool ls_cells_snapshot(ls_cells_view *out) {
    if(!cells_take()) return false;
    memcpy(out,&cells_pub,sizeof(*out));cells_give();return true;
}
bool ls_cells_query(const char *s) {
    size_t n=strlen(s);if(n>=sizeof(cells_mail.query) || !cells_take()) return false;
    memcpy(cells_mail.query,s,n+1);cells_mail.serial++;cells_mail.action=0;cells_pub.ready=false;cells_pub.busy=n>=2;cells_pub.count=0;cells_give();return true;
}
bool ls_cells_pick(unsigned i,unsigned radius) {
    if(i>=LS_CELLS_HITS || radius<1 || radius>500 || !cells_take()) return false;
    if(cells_pub.busy || i>=cells_pub.count || strcmp(cells_pub.query,cells_mail.query)) { cells_give();return false; }
    cells_pub.ready=false;cells_pub.busy=true;
    cells_mail.pick=i;cells_mail.radius=radius;cells_mail.action=5;cells_mail.serial++;cells_give();return true;
}
bool ls_cells_action(unsigned action,uint32_t key) {
    if(action<1 || action>4 || !cells_take()) return false;
    if(action==1 && (!cells_pub.ready || cells_pub.busy)) { cells_give();return false; }
    cells_mail.action=action;cells_mail.key=key;cells_mail.serial++;cells_give();return true;
}
bool ls_cells_server(const char *url) {
    size_t n=strlen(url);if(n<8 || n>=sizeof(cells_mail.url) || !cells_take()) return false;
    memcpy(cells_mail.url,url,n+1);cells_mail.action=6;cells_mail.serial++;cells_give();return true;
}
static EXT_RAM_BSS_ATTR struct {
    cc_places places;cc_place_hit hits[LS_CELLS_HITS];
    cc_place_info info;cc_places_stats stats;cc_places_shard shard;
    struct { int fd;uint64_t size; } files[4];
    uint8_t cover[CELLS_BITS],queued[CELLS_BITS],scratch[CELLS_BITS];
    uint8_t buf[65536] __attribute__((aligned(128)));
    char server[256],country[3],credit[160],queue_name[64];
    uint32_t selected_key,queue_key,next_cell,serial,generation;
    unsigned picked,radius,hint_attempts;bool initialized,base_open,query_pending,queue_loaded,catalog_refresh,preview_active;
    int catalog;uint32_t entries,index_size,base_size;uint8_t index_sha[32],base_sha[32];
    char path[256],temp[272],backup[280];
} cw;
static uint32_t cells_u32(const uint8_t *p) { return p[0]|(uint32_t)p[1]<<8|(uint32_t)p[2]<<16|(uint32_t)p[3]<<24; }
static void cells_put32(uint8_t *p,uint32_t v) { for(unsigned i=0;i<4;i++)p[i]=(uint8_t)(v>>(8*i)); }
static uint32_t cells_crc(const void *data,size_t n) {
    const uint8_t *p=data;uint32_t c=~0u;while(n--) { c^=*p++;for(unsigned k=0;k<8;k++)c=(c>>1)^(0xedb88320u&-(c&1)); }return ~c;
}
static void cells_message(const char *s) { snprintf(cells_work.message,sizeof(cells_work.message),"%s",s); }
static void cells_publish(void) {
    cells_work.generation=cw.generation;cells_work.queued=cw.queue_loaded;
    snprintf(cells_work.queue_name,sizeof(cells_work.queue_name),"%s",cw.queue_loaded?cw.queue_name:"");
    while(!cells_take()) vTaskDelay(1);
    cells_pub=cells_work;
    if(cells_mail.serial!=cw.serial) { cells_pub.busy=true;cells_pub.ready=false;cells_pub.count=0; }
    cells_give();
}
static int cells_read(void *ctx,uint64_t off,void *dst,size_t n) {
    unsigned i=(unsigned)(uintptr_t)ctx;
    return carto_sd_read_at(cw.files[i].fd,cw.files[i].size,off,dst,n,NULL,0);
}
static bool cells_file(unsigned slot,const char *relative) {
    snprintf(cw.path,sizeof(cw.path),CELLS_ROOT "%s",relative);
    int fd=open(cw.path,O_RDONLY);struct stat st;
    if(fd<0)return false;
    if(fstat(fd,&st) || st.st_size<128) { close(fd);return false; }
    if(cw.files[slot].fd>=0) close(cw.files[slot].fd);
    cw.files[slot].fd=fd;cw.files[slot].size=st.st_size;return true;
}
static bool cells_safe_asset(const char *s) {
    if(!strcmp(s,"cells/index.cci") || !strcmp(s,"cells/catalog.ccm") || !strcmp(s,"cells/overview.ctile") ||
       !strcmp(s,"places/places_base.ccpl")) return true;
    unsigned x,y;int n=0;
    if(sscanf(s,"cells/8-%u-%u.ctile%n",&x,&y,&n)==2 && n && !s[n] && x<256 && y<256) {
        char canonical[32];snprintf(canonical,sizeof(canonical),"cells/8-%u-%u.ctile",x,y);return !strcmp(s,canonical);
    }
    return strlen(s)==21 && !strncmp(s,"places/places_",14) && s[14]>='A' && s[14]<='Z' &&
        s[15]>='A' && s[15]<='Z' && !strcmp(s+16,".ccpl");
}
static bool cells_hash(const char *path,const uint8_t sha[32],uint64_t size) {
    FILE *f=cells_fopen(path,"rb");if(!f)return false;setvbuf(f,NULL,_IONBF,0);
    mbedtls_sha256_context ctx;mbedtls_sha256_init(&ctx);bool ok=!mbedtls_sha256_starts(&ctx,0);
    uint64_t count=0;size_t n;while(ok && (n=fread(cw.buf,1,32768,f))) {
        count+=n;ok=count<=size && !mbedtls_sha256_update(&ctx,cw.buf,n);
        vTaskDelay(1); /* httpd may also verify; do not starve lower-priority owners. */
    }
    ok=ok && !ferror(f) && count==size;fclose(f);uint8_t digest[32];
    ok=ok && !mbedtls_sha256_finish(&ctx,digest) && !memcmp(digest,sha,32);mbedtls_sha256_free(&ctx);return ok;
}
static bool cells_release_source(void) {
    if(xSemaphoreTake(file_lock,pdMS_TO_TICKS(2000))!=pdTRUE)return false;
    if(xSemaphoreTake(view_lock,pdMS_TO_TICKS(2000))!=pdTRUE) { xSemaphoreGive(file_lock);return false; }
    if(!sd_map || !sd_map->set) {
        xSemaphoreGive(view_lock);xSemaphoreGive(file_lock);return true;
    }
    source_refreshing=true;map_prepared=false;map_owned=false;
    map_invalidate_frame();frames_clear();
    release(shown);shown=NULL;close_sd(sd_map);sd_map=NULL;sd_map_size=0;
    xSemaphoreGive(view_lock);xSemaphoreGive(file_lock);
    return true;
}
/* Replace only on the serialized owner. Old readers close before rename. */
static bool cells_install(const char *part,const char *path) {
    if(!strcmp(path,CELLS_ROOT "cells/catalog.ccm") && cw.catalog>=0) { close(cw.catalog);cw.catalog=-1; }
    if(strstr(path,"/places/")) {
        if(!strcmp(path,CELLS_ROOT "places/places_base.ccpl")) {
            for(unsigned i=0;i<4;i++) { if(cw.files[i].fd>=0)close(cw.files[i].fd);cw.files[i].fd=-1; }
            cw.base_open=false;
        } else for(unsigned i=1;i<4;i++) {
            char relative[96];snprintf(relative,sizeof(relative),CELLS_ROOT "places/places_%.2s.ccpl",cw.places.file[i].iso2);
            if(!strcmp(relative,path) && cw.files[i].fd>=0) { cc_places_detach(&cw.places,i);close(cw.files[i].fd);cw.files[i].fd=-1; }
        }
    }
    if(strstr(path,"/cells/")) {
        if(!cells_release_source())return false;
        cw.generation++;
    }
    snprintf(cw.backup,sizeof(cw.backup),"%s.bak",path);struct stat st;
    if(!carto_digest_remove(path))return false;
    bool exists=!stat(path,&st);
    if(exists)unlink(cw.backup);
    if(exists && rename(path,cw.backup)) return false;
    if(rename(part,path)) { if(exists)rename(cw.backup,path);return false; }
    if(exists)unlink(cw.backup);
    return true;
}
/* Header: CCMAPS1\0, count, index size/hash, base size/hash, edition, padding.
 * Sorted 40-byte entries: cell id (0 overview), size, SHA256. Bounded paging. */
static bool cells_catalog(void) {
    if(cw.catalog>=0) { close(cw.catalog);cw.catalog=-1; }
    cw.catalog=open(CELLS_ROOT "cells/catalog.ccm",O_RDONLY);struct stat st;
    if(cw.catalog<0)return false;
    uint8_t h[128];
    if(fstat(cw.catalog,&st) || !carto_sd_read_at(cw.catalog,st.st_size,0,h,128,NULL,0) ||
       memcmp(h,"CCMAPS1",8) || (cw.entries=cells_u32(h+8))>65537 || (uint64_t)st.st_size!=128+(uint64_t)cw.entries*40) goto bad;
    cw.index_size=cells_u32(h+12);memcpy(cw.index_sha,h+16,32);
    cw.base_size=cells_u32(h+48);memcpy(cw.base_sha,h+52,32);
    if(cw.index_size<64 || cw.base_size<128 || cw.base_size>8*1024*1024) goto bad;
    uint32_t prior=0;
    for(uint32_t i=0;i<cw.entries;i++) {
        uint8_t r[40];if(!carto_sd_read_at(cw.catalog,st.st_size,128+(uint64_t)i*40,r,40,NULL,0))goto bad;
        uint32_t id=cells_u32(r);if(id>65536 || (i && id<=prior) || cells_u32(r+4)<64)goto bad;prior=id;
    }
    return true;
bad:close(cw.catalog);cw.catalog=-1;cells_message("Invalid cells catalog");return false;
}
static bool cells_entry(uint32_t id,uint32_t *size,uint8_t sha[32]) {
    if(cw.catalog<0)return false;
    uint32_t lo=0,hi=cw.entries;uint8_t r[40];
    while(lo<hi) {
        uint32_t mid=lo+(hi-lo)/2;
        if(!carto_sd_read_at(cw.catalog,128+(uint64_t)cw.entries*40,128+(uint64_t)mid*40,r,40,NULL,0))return false;
        uint32_t key=cells_u32(r);if(key<id)lo=mid+1;else if(key>id)hi=mid;else { *size=cells_u32(r+4);memcpy(sha,r+8,32);return true; }
    }
    return false;
}
static void cells_name(uint32_t id,char *out,size_t n,bool remote) {
    if(!id)snprintf(out,n,"cells/overview.ctile");
    else snprintf(out,n,remote?"cells/8/%u/%u.ctile":"cells/8-%u-%u.ctile",(id-1)/256,(id-1)%256);
}
static bool cells_matches(const char *,uint64_t,const uint8_t[32]);
static bool cells_present(uint32_t id,uint32_t size) {
    char name[64],path[96];cells_name(id,name,sizeof(name),false);snprintf(path,sizeof(path),CELLS_ROOT "%s",name);
    uint8_t sha[32];uint32_t expected;
    return cells_entry(id,&expected,sha) && expected==size && cells_matches(path,size,sha);
}
/* Persistent per-place bitsets are also the ownership/refcount source of truth.
 * Queued places own their cells, so deleting another place cannot break resume. */
static bool cells_record(uint32_t key,const char *name,const uint8_t *bits,bool queued) {
    uint8_t h[80]={0};memcpy(h,"CCPLACE2",8);cells_put32(h+8,queued);cells_put32(h+12,cells_crc(bits,CELLS_BITS));
    snprintf((char*)h+16,64,"%s",name);
    snprintf(cw.path,sizeof(cw.path),CELLS_ROOT "installed/%08lx.ccpi",(unsigned long)key);
    snprintf(cw.temp,sizeof(cw.temp),"%s.part",cw.path);FILE *f=cells_fopen(cw.temp,"wb");if(!f)return false;
    bool ok=fwrite(h,1,80,f)==80 && fwrite(bits,1,CELLS_BITS,f)==CELLS_BITS && !fflush(f);if(fclose(f))ok=false;
    return ok && cells_install(cw.temp,cw.path);
}
static bool cells_record_read(const char *path,uint8_t *bits,char *name,bool *queued) {
    FILE *f=cells_fopen(path,"rb");if(!f)return false;uint8_t h[80];
    bool ok=fread(h,1,80,f)==80 && fread(bits,1,CELLS_BITS,f)==CELLS_BITS && fgetc(f)==EOF;
    fclose(f);if(!ok || memcmp(h,"CCPLACE2",8) || cells_u32(h+8)>1 || !memchr(h+16,0,64) || cells_crc(bits,CELLS_BITS)!=cells_u32(h+12))return false;
    memcpy(name,h+16,64);*queued=cells_u32(h+8)!=0;return true;
}
static void cells_library(void) {
    cells_work.library_count=0;DIR *d=opendir(CELLS_ROOT "installed");if(!d)return;struct dirent *e;
    while((e=readdir(d))) {
        unsigned key;int n=0;bool queued;char name[64];
        if(sscanf(e->d_name,"%8x.ccpi%n",&key,&n)!=1 || !n || e->d_name[n])continue;
        snprintf(cw.path,sizeof(cw.path),CELLS_ROOT "installed/%s",e->d_name);
        if(!cells_record_read(cw.path,cw.scratch,name,&queued))continue;
        if(cells_work.library_count<LS_CELLS_PLACES) {
            unsigned at=cells_work.library_count++;cells_work.keys[at]=key;
            snprintf(cells_work.library[at],64,"%s%s",queued?"[queued] ":"",name);
        }
        if(queued && !cw.queue_loaded) {
            memcpy(cw.queued,cw.scratch,CELLS_BITS);cw.queue_key=key;snprintf(cw.queue_name,64,"%s",name);cw.queue_loaded=true;cw.next_cell=0;
            double sx=0,sy=0,rows=0,count=0,pi=3.14159265358979323846;
            for(unsigned id=0;id<65536;id++)if(cw.queued[id/8]&(1u<<(id&7))) {
                double angle=((id/256+.5)/256.0*2-1)*pi;sx+=cos(angle);sy+=sin(angle);rows+=id%256+.5;count++;
            }
            if(count) { cells_work.queue_lon=atan2(sy,sx)*180/pi;cells_work.queue_lat=atan(sinh(pi*(1-2*rows/count/256)))*180/pi; }
        }
    }
    closedir(d);
}
static bool cells_delete(uint32_t key) {
    uint8_t *owned=heap_caps_calloc(1,CELLS_BITS,MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);if(!owned)return false;
    char name[64];bool queued;snprintf(cw.path,sizeof(cw.path),CELLS_ROOT "installed/%08lx.ccpi",(unsigned long)key);
    bool ok=cells_record_read(cw.path,cw.cover,name,&queued);if(!ok) { heap_caps_free(owned);return false; }
    /* Remove ownership first: a reset can leak an unreferenced file, never delete a referenced one. */
    if(unlink(cw.path)) { heap_caps_free(owned);return false; }
    DIR *d=opendir(CELLS_ROOT "installed");struct dirent *e;
    if(!d) { heap_caps_free(owned);return false; }
    bool valid=true;
    if(d) { while((e=readdir(d))) {
        if(!strstr(e->d_name,".ccpi"))continue;
        snprintf(cw.path,sizeof(cw.path),CELLS_ROOT "installed/%s",e->d_name);
        if(cells_record_read(cw.path,cw.scratch,name,&queued))for(unsigned i=0;i<CELLS_BITS;i++)owned[i]|=cw.scratch[i];
        else valid=false;
    }closedir(d); }
    if(!valid) { heap_caps_free(owned);cells_message("Unreadable ownership record; cells retained");return false; }
    if(!cells_release_source()) { heap_caps_free(owned);return false; }
    for(unsigned id=0;id<65536;id++)if((cw.cover[id/8]&(1u<<(id&7))) && !(owned[id/8]&(1u<<(id&7)))) {
        char relative[64];cells_name(id+1,relative,sizeof(relative),false);snprintf(cw.path,sizeof(cw.path),CELLS_ROOT "%s",relative);unlink(cw.path);
        snprintf(cw.temp,sizeof(cw.temp),"%s.part",cw.path);unlink(cw.temp);
    }
    if(cw.queue_key==key)cw.queue_loaded=false;
    cw.generation++;heap_caps_free(owned);cells_library();return true;
}

typedef struct {
    int fd;uint64_t size;int64_t yielded;
    bool (*check)(void *);void *check_ctx;
} cells_reader;
static int cells_asset_read(void *ctx,uint64_t off,void *dst,size_t n) {
    cells_reader *r=ctx;
    int64_t now=esp_timer_get_time();
    if(now-r->yielded>=20000) { vTaskDelay(1);r->yielded=now; }
    if(r->check && !r->check(r->check_ctx))return 0;
    return carto_sd_read_at(r->fd,r->size,off,dst,n,NULL,0);
}
static bool cells_asset_valid_checked(const char *path,const char *relative,bool (*check)(void *),void *ctx) {
    cells_reader r={.fd=open(path,O_RDONLY),.check=check,.check_ctx=ctx,.yielded=esp_timer_get_time()};struct stat st;if(r.fd<0)return false;
    bool ok=!fstat(r.fd,&st);r.size=ok?st.st_size:0;
    if(ok && !strncmp(relative,"places/",7))ok=cc_places_verify(cells_asset_read,&r,r.size,cw.buf,sizeof(cw.buf));
    else if(ok && !strcmp(relative,"cells/catalog.ccm")) {
        uint8_t h[128];ok=r.size>=128 && cells_asset_read(&r,0,h,128) && !memcmp(h,"CCMAPS1",8);
        uint32_t count=ok?cells_u32(h+8):0,prior=0;
        ok=ok && count && count<=65537 && r.size==128+(uint64_t)count*40 && cells_u32(h+12)>=64 && cells_u32(h+48)>=128;
        for(uint32_t i=0;ok && i<count;i++) {
            uint8_t row[40]={0};ok=cells_asset_read(&r,128+(uint64_t)i*40,row,40);
            uint32_t id=cells_u32(row);ok=ok && id<=65536 && (!i || id>prior) && cells_u32(row+4)>=64;prior=id;
        }
    } else if(ok) {
        cc_cellset *set=heap_caps_calloc(1,sizeof(*set),MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);
        uint8_t *dir=heap_caps_malloc(4096,MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);uint8_t dummy;
        if(!set || !dir)ok=false;
        else if(!strcmp(relative,"cells/index.cci"))
            ok=cc_cellset_open(set,cells_asset_read,&r,r.size,(cc_cellset_io){NULL,cell_open,cell_read,cell_close},dir,4096,&dummy,1);
        else ok=cc_ctile_open_source(&set->map,&set->source,cells_asset_read,&r,r.size,dir,4096,&dummy,1) && set->source.header[8]==7 && set->map.zmax<=14;
        heap_caps_free(set);heap_caps_free(dir);
    }
    close(r.fd);return ok;
}
static bool cells_asset_valid(const char *path,const char *relative) {
    return cells_asset_valid_checked(path,relative,NULL,NULL);
}
static bool cells_matches(const char *path,uint64_t size,const uint8_t sha[32]) {
    struct stat st;if(stat(path,&st) || st.st_size!=(int64_t)size)return false;
    char hex[65];for(unsigned i=0;i<32;i++)snprintf(hex+2*i,3,"%02x",sha[i]);
    if(carto_digest_cached(path,&st,hex))return true;
    if(!cells_hash(path,sha,size))return false;
    return carto_digest_write(path,&st,hex);
}
/* A new index must never address offsets in an older cell revision. Catalog
 * hashes make stale cells behave exactly like missing cells until replaced. */
static bool cells_file_current(uint32_t id,const char *path) {
    if(!cw.initialized || cw.catalog<0)return true; /* locally copied legacy set */
    if(!cells_matches(CELLS_ROOT "cells/index.cci",cw.index_size,cw.index_sha))return false;
    uint32_t size;uint8_t sha[32];return cells_entry(id,&size,sha) && cells_matches(path,size,sha);
}
static EXT_RAM_BSS_ATTR struct {
    char relative[64],remote[96],path[128],part[144],meta[160],url[384],range[80];
    uint64_t size,offset;uint8_t sha[32];unsigned kind;bool active;
    FILE *out,*in;int64_t retry;
#ifndef LS_CARTOCORE_HOST
    esp_http_client_handle_t http;
#endif
} dl;
#ifndef LS_CARTOCORE_HOST
static esp_err_t cells_http_event(esp_http_client_event_t *event) {
    if(event->event_id==HTTP_EVENT_ON_HEADER && !strcasecmp(event->header_key,"Content-Range"))
        snprintf(dl.range,sizeof(dl.range),"%s",event->header_value);
    return ESP_OK;
}
#endif
static void cells_download_close(void) {
    if(dl.out)fclose(dl.out);
    if(dl.in)fclose(dl.in);
    dl.out=dl.in=NULL;
#ifndef LS_CARTOCORE_HOST
    if(dl.http) { esp_http_client_close(dl.http);esp_http_client_cleanup(dl.http);dl.http=NULL; }
#endif
}
static void cells_download_fail(const char *why) {
    cells_download_close();dl.active=false;cells_work.downloading=false;dl.retry=esp_timer_get_time()+5000000;cells_message(why);
}
static bool cells_download(unsigned kind,const char *remote,const char *relative,uint64_t size,const uint8_t *sha) {
    if(dl.active || esp_timer_get_time()<dl.retry || !cells_safe_asset(relative))return false;
    memset(&dl,0,sizeof(dl));dl.kind=kind;dl.size=size;
    snprintf(dl.relative,sizeof(dl.relative),"%s",relative);snprintf(dl.remote,sizeof(dl.remote),"%s",remote);
    snprintf(dl.path,sizeof(dl.path),CELLS_ROOT "%s",relative);snprintf(dl.part,sizeof(dl.part),"%s.part",dl.path);
    snprintf(dl.meta,sizeof(dl.meta),"%s.meta",dl.part);if(sha)memcpy(dl.sha,sha,32);
    if(size && cells_matches(dl.path,size,dl.sha)) { dl.offset=size;dl.active=true;return true; }
    /* Bind partial bytes to this exact edition/hash, never append a different revision. */
    uint8_t meta[40],expected[40]={0};memcpy(expected,dl.sha,32);
    for(unsigned i=0;i<8;i++)expected[32+i]=(uint8_t)(size>>(8*i));
    FILE *m=cells_fopen(dl.meta,"rb");bool same=m && fread(meta,1,40,m)==40 && !memcmp(meta,expected,40);if(m)fclose(m);
    struct stat st;
    if(size && same && !stat(dl.part,&st) && (uint64_t)st.st_size<=size)dl.offset=st.st_size;
    else { unlink(dl.part);m=cells_fopen(dl.meta,"wb");if(!m)return false;bool ok=fwrite(expected,1,40,m)==40 && !fflush(m);if(fclose(m))ok=false;if(!ok)return false; }
    dl.out=cells_fopen(dl.part,dl.offset?"ab":"wb");if(!dl.out)return false;setvbuf(dl.out,NULL,_IONBF,0);
    if(size && dl.offset==size) { dl.active=true;return true; }
    snprintf(dl.url,sizeof(dl.url),"%s%s%s",cw.server,cw.server[strlen(cw.server)-1]=='/'?"":"/",remote);
#ifdef LS_CARTOCORE_HOST
    if(strncmp(dl.url,"file://",7)) { cells_download_fail("Host server must use file://");return false; }
    dl.in=cells_fopen(dl.url+7,"rb");if(!dl.in || fseek(dl.in,(long)dl.offset,SEEK_SET)) { cells_download_fail("Server file unavailable; will retry");return false; }
#else
    dl.range[0]=0;
    esp_http_client_config_t cfg={.url=dl.url,.timeout_ms=1500,.crt_bundle_attach=esp_crt_bundle_attach,
        .buffer_size=1024,.event_handler=cells_http_event,.keep_alive_enable=true};
    dl.http=esp_http_client_init(&cfg);if(!dl.http) { cells_download_fail("HTTP allocation failed");return false; }
    if(dl.offset) { char range[64];snprintf(range,sizeof(range),"bytes=%llu-",(unsigned long long)dl.offset);esp_http_client_set_header(dl.http,"Range",range); }
    if(esp_http_client_open(dl.http,0)!=ESP_OK || esp_http_client_fetch_headers(dl.http)<0) { cells_download_fail("Connect failed; partial retained");return false; }
    int status=esp_http_client_get_status_code(dl.http);
    if(status==200 && dl.offset) {
        fclose(dl.out);dl.out=cells_fopen(dl.part,"wb");dl.offset=0;if(!dl.out) { cells_download_fail("Cannot restart partial");return false; }setvbuf(dl.out,NULL,_IONBF,0);
    } else if(status==206) {
        unsigned long long first,last,total;
        if(sscanf(dl.range,"bytes %llu-%llu/%llu",&first,&last,&total)!=3 || first!=dl.offset ||
           last<first || !size || total!=size || last>=total) { cells_download_fail("Invalid HTTP Range response");return false; }
    } else if(status!=200) { cells_download_fail("HTTP rejected download; partial retained");return false; }
#endif
    dl.active=true;cells_work.downloading=true;cells_work.done=dl.offset;cells_work.total=size;
    cells_message("Downloading; safe to leave TILES");return true;
}
static unsigned cells_download_step(void) {
    if(!dl.active)return 0;
    if(dl.size && dl.offset==dl.size && !dl.out) { dl.active=false;return dl.kind; }
    size_t wanted=sizeof(cw.buf);if(dl.size && wanted>dl.size-dl.offset)wanted=(size_t)(dl.size-dl.offset);
    int got=0;
    if(wanted) {
#ifdef LS_CARTOCORE_HOST
        got=(int)fread(cw.buf,1,wanted,dl.in);if(!got && ferror(dl.in))got=-1;
#else
        got=esp_http_client_read(dl.http,(char*)cw.buf,(int)wanted);
#endif
    }
    if(got<0 || (got==0 && dl.size && dl.offset<dl.size)) { cells_download_fail("Interrupted; partial retained for resume");return 0; }
    if(got && fwrite(cw.buf,1,(size_t)got,dl.out)!=(size_t)got) { cells_download_fail("SD write failed; partial retained");return 0; }
    dl.offset+=(unsigned)got;cells_work.done=dl.offset;cells_work.total=dl.size;
    if(!dl.size && dl.offset>4u*1024u*1024u) { cells_download_fail("Catalog exceeds 4 MiB");return 0; }
    if(got && (!dl.size || dl.offset<dl.size))return 0;
    bool ok=!fflush(dl.out);cells_download_close();
    if(dl.size)ok=ok && cells_hash(dl.part,dl.sha,dl.size);
    ok=ok && cells_asset_valid(dl.part,dl.relative);
    if(!ok) { unlink(dl.part);cells_download_fail("SHA256 / format check failed; old asset retained");return 0; }
    if(!cells_install(dl.part,dl.path)) { cells_download_fail("Atomic install failed; partial retained");return 0; }
    unlink(dl.meta);dl.active=false;cells_work.downloading=false;
    if(dl.size)(void)cells_matches(dl.path,dl.size,dl.sha);
    cells_message("Installed and verified");return dl.kind;
}
static void cells_config_save(void) {
    uint8_t data[264]={0};memcpy(data,cw.server,256);memcpy(data+256,cw.country,3);cells_put32(data+260,cells_crc(data,260));
    FILE *f=cells_fopen(CELLS_ROOT "cells.settings.part","wb");if(!f)return;
    bool ok=fwrite(data,1,sizeof(data),f)==sizeof(data) && !fflush(f);if(fclose(f))ok=false;
    if(ok)(void)cells_install(CELLS_ROOT "cells.settings.part",CELLS_ROOT "cells.settings");
}
static bool cells_url_valid(const char *s) {
    if(strlen(s)>250 || strstr(s,"..") || strchr(s,'\n') || strchr(s,'\r'))return false;
#ifdef LS_CARTOCORE_HOST
    if(!strncmp(s,"file://",7))return true;
#endif
    return !strncmp(s,"https://",8) || !strncmp(s,"http://",7);
}
static void cells_recover(const char *folder) {
    DIR *dir=opendir(folder);if(!dir)return;struct dirent *e;
    while((e=readdir(dir))) {
        size_t n=strlen(e->d_name);if(n<5 || n>80 || strcmp(e->d_name+n-4,".bak"))continue;
        snprintf(cw.backup,sizeof(cw.backup),"%s/%s",folder,e->d_name);
        snprintf(cw.path,sizeof(cw.path),"%s/%.*s",folder,(int)(n-4),e->d_name);struct stat st;
        if(!stat(cw.path,&st))unlink(cw.backup);else rename(cw.backup,cw.path);
    }
    closedir(dir);
}
static void cells_init(void) {
    if(cw.initialized)return;
    struct stat mounted;if(stat("/sdcard/maps",&mounted)) { mkdir("/sdcard/maps",0777);if(stat("/sdcard/maps",&mounted))return; }
    cw.initialized=true;cw.catalog=-1;cw.radius=25;
    for(unsigned i=0;i<4;i++)cw.files[i].fd=-1;
    snprintf(cw.server,sizeof(cw.server),"https://cartocore-maps.pages.dev/");
    mkdir(CELLS_ROOT,0777);mkdir(CELLS_ROOT "cells",0777);mkdir(CELLS_ROOT "places",0777);mkdir(CELLS_ROOT "installed",0777);
    cells_recover(CELLS_ROOT "cells");cells_recover(CELLS_ROOT "places");cells_recover(CELLS_ROOT "installed");cells_recover(CELLS_ROOT);
    uint8_t config[264];FILE *f=cells_fopen(CELLS_ROOT "cells.settings","rb");
    if(f) { if(fread(config,1,sizeof(config),f)==sizeof(config) && memchr(config,0,256) &&
        cells_crc(config,260)==cells_u32(config+260) && cells_url_valid((char*)config)) {
        memcpy(cw.server,config,256);
        if(config[258]==0 && config[256]>='A' && config[256]<='Z' && config[257]>='A' && config[257]<='Z')memcpy(cw.country,config+256,3);
    }fclose(f); }
#ifdef LS_CARTOCORE_HOST
    const char *server=getenv("LSSIM_CELLS_SERVER");if(server && cells_url_valid(server))snprintf(cw.server,sizeof(cw.server),"%s",server);
#endif
    snprintf(cells_work.server,sizeof(cells_work.server),"%s",cw.server);cells_work.radius=25;
    cells_catalog();cells_library();cells_publish();
}
static bool cells_attach(const char *iso) {
    if(cc_places_shard_attached(&cw.places,iso))return true;
    unsigned slot=1;for(;slot<4;slot++)if(cw.files[slot].fd<0)break;
    if(slot==4) { slot=strcmp(cw.places.file[1].iso2,cw.country)?1:2;cc_places_detach(&cw.places,slot);close(cw.files[slot].fd);cw.files[slot].fd=-1; }
    char relative[32];snprintf(relative,sizeof(relative),"places/places_%.2s.ccpl",iso);
    if(!cells_file(slot,relative))return false;
    int attached=cc_places_attach(&cw.places,cells_read,(void*)(uintptr_t)slot,cw.files[slot].size);
    if(!attached) { close(cw.files[slot].fd);cw.files[slot].fd=-1;return false; }return true;
}
static void cells_search(void) {
    if(!cw.base_open) {
        if(!cells_file(0,"places/places_base.ccpl") || !cc_places_open(&cw.places,cells_read,0,cw.files[0].size)) {
            cells_message("Place base missing: connecting to map server");
            if(cw.catalog<0)cells_download(1,"cells/catalog.ccm","cells/catalog.ccm",0,NULL);
            else cells_download(2,"places/places_base.ccpl","places/places_base.ccpl",cw.base_size,cw.base_sha);
            return;
        }
        cw.base_open=true;cc_places_credit(&cw.places,cells_work.credit,sizeof(cells_work.credit));
        if(cw.country[0])cells_attach(cw.country);
    }
    int n=cc_places_search(&cw.places,cells_work.query,cw.hits,LS_CELLS_HITS,&cw.stats);
    if(n<0) { cells_message("Place index read failed");return; }
    cells_work.count=(unsigned)n;
    for(int i=0;i<n;i++)if(cc_places_info(&cw.places,&cw.hits[i],&cw.info)==0) {
        ls_place_row *r=&cells_work.hits[i];snprintf(r->name,64,"%s",cw.info.name);snprintf(r->parent,64,"%s",cw.info.parent);
        r->lat=cw.info.lat_e5/1e5;r->lon=cw.info.lon_e5/1e5;r->kind=cw.info.kind;
    }
    for(unsigned i=0;cw.hint_attempts<2 && i<cw.stats.nhints;i++)if(cc_places_shard_info(&cw.places,cw.stats.shard_area[i],&cw.shard)==1) {
        cw.hint_attempts++;
        if(cells_attach(cw.shard.iso2)) { cw.query_pending=true;return; }
        char relative[32];snprintf(relative,sizeof(relative),"places/places_%.2s.ccpl",cw.shard.iso2);
        cells_download(3,relative,relative,cw.shard.size,cw.shard.sha256);return;
    }
    cw.query_pending=false;cells_work.busy=false;cells_message(n?"Pick a place":"No matches; try a country qualifier");
}
static void cells_preview(void) {
    cells_work.busy=false;cells_work.ready=false;
    if(cw.picked>=cells_work.count || cc_places_info(&cw.places,&cw.hits[cw.picked],&cw.info)<0)return;
    cells_work.picked=cells_work.hits[cw.picked];cells_work.radius=cw.radius;
    if(cw.catalog<0) { cells_work.busy=true;cells_message("Loading cell sizes from server");cells_download(1,"cells/catalog.ccm","cells/catalog.ccm",0,NULL);return; }
    memset(cw.cover,0,CELLS_BITS);cells_work.cells=cells_work.installed=cells_work.unavailable=cells_work.preview_count=0;cells_work.bytes=0;
    cc_places_cells it;uint32_t x,y;int got;
    if(!cc_places_cells_open(&cw.places,&cw.hits[cw.picked],cw.radius,&it)) { cells_message("Cannot read place shape");return; }
    while((got=cc_places_cells_next(&it,&x,&y))>0) {
        if(x>=256 || y>=256) { cells_message("Place grid must be z8");return; }
        uint32_t id=x*256+y;cw.cover[id/8]|=(uint8_t)(1u<<(id&7));cells_work.cells++;
        uint32_t size;uint8_t sha[32];bool known=cells_entry(id+1,&size,sha);bool present=known && cells_present(id+1,size);
        if(!known)cells_work.unavailable++;else if(present)cells_work.installed++;else cells_work.bytes+=size;
        if(cells_work.preview_count<LS_CELLS_PREVIEW) {
            unsigned at=cells_work.preview_count++;cells_work.preview[at]=(uint16_t)id;cells_work.present[at]=present?1:known?0:2;
        }
    }
    if(got<0) { cells_message("Place cover read failed");return; }
    if(cw.catalog>=0) {
        if(!cells_matches(CELLS_ROOT "cells/index.cci",cw.index_size,cw.index_sha))cells_work.bytes+=cw.index_size;
        uint32_t size;uint8_t sha[32];if(cells_entry(0,&size,sha) && !cells_present(0,size))cells_work.bytes+=size;
    }
    uint32_t key=2166136261u;for(const char *p=cw.info.name;*p;p++)key=(key^(uint8_t)*p)*16777619u;
    key=(key^(uint32_t)cw.info.lat_e5)*16777619u;key=(key^(uint32_t)cw.info.lon_e5)*16777619u;cw.selected_key=key;
#ifdef LS_CARTOCORE_HOST
    cells_work.free=4ULL*1024*1024*1024;
#else
    uint64_t total=0;cells_work.free=0;ls_sdcard_size(&total,&cells_work.free);
#endif
    cells_work.ready=!cells_work.unavailable && cells_work.bytes<cells_work.free;
    cells_message(cells_work.unavailable?"Some cells unavailable from this server":cells_work.ready?"Ready to download":"Not enough SD space");
    /* Remember the selected country and keep its city shard attached. */
    cc_place_hit country[1];cc_places_stats stats;
    const char *name=cw.info.kind==CC_PLACE_COUNTRY?cw.info.name:cw.info.country;
    if(cc_places_search(&cw.places,name,country,1,&stats)==1 && country[0].kind==CC_PLACE_COUNTRY &&
       cc_places_shard_info(&cw.places,country[0].id,&cw.shard)==1) {
        memcpy(cw.country,cw.shard.iso2,3);cells_config_save();
        if(!cells_attach(cw.country) && !dl.active) {
            char relative[32];snprintf(relative,sizeof(relative),"places/places_%.2s.ccpl",cw.country);
            cells_download(3,relative,relative,cw.shard.size,cw.shard.sha256);
        }
    }
}
static void cells_queue_step(void) {
    if(!cw.queue_loaded)return;
    cells_work.downloading=true;
    if(cw.catalog<0) { cells_download(1,"cells/catalog.ccm","cells/catalog.ccm",0,NULL);return; }
    if(!cells_matches(CELLS_ROOT "cells/index.cci",cw.index_size,cw.index_sha)) {
        cells_download(4,"cells/index.cci","cells/index.cci",cw.index_size,cw.index_sha);return;
    }
    uint32_t size;uint8_t sha[32];
    if(!cells_entry(0,&size,sha)) { cells_message("Catalog has no overview");return; }
    if(!cells_present(0,size)) { cells_download(5,"cells/overview.ctile","cells/overview.ctile",size,sha);return; }
    while(cw.next_cell<65536) {
        unsigned at=cw.next_cell++;
        if(!(cw.queued[at/8]&(1u<<(at&7))))continue;
        if(!cells_entry(at+1,&size,sha)) { cells_message("Queued cell absent from catalog");return; }
        if(cells_present(at+1,size))continue;
        char remote[64],local[64];cells_name(at+1,remote,sizeof(remote),true);cells_name(at+1,local,sizeof(local),false);
        cw.next_cell=at;cells_download(6,remote,local,size,sha);return;
    }
    if(!cells_record(cw.queue_key,cw.queue_name,cw.queued,false)) { cells_message("Cannot save place installation");return; }
    cw.queue_loaded=false;cells_work.downloading=false;cells_library();
    if(!sd_map || !sd_map->set)select_map("cells");
    snprintf(saved.path,sizeof(saved.path),CELLS_ROOT "cells/index.cci");save_view();
    cells_message("Place installed; MAP uses the cell set");if(cw.preview_active)cells_preview();
}

/* Transport-neutral, resumable uploader. Commands run only on the worker. */
static EXT_RAM_BSS_ATTR struct {
    char relative[64],path[128],part[144],meta[160];
    uint8_t sha[32],chunk[256];uint64_t size,offset;int64_t last_us;bool active;
} upload;
static bool cells_hex(const char *s,uint8_t *out,size_t n) {
    if(strlen(s)!=n*2)return false;
    for(size_t i=0;i<n;i++) {
        unsigned v=0;for(unsigned j=0;j<2;j++) { unsigned char c=s[i*2+j];unsigned d=c>='0'&&c<='9'?c-'0':c>='a'&&c<='f'?c-'a'+10:c>='A'&&c<='F'?c-'A'+10:16;
            if(d>15)return false;
            v=v*16+d; }out[i]=(uint8_t)v;
    }return true;
}
static bool cells_upload_begin(const char *relative,uint64_t size,const char *sha) {
    cells_init();if(dl.active || !cells_safe_asset(relative) || !size || size>256u*1024u*1024u || !cells_hex(sha,upload.sha,32))return false;
    upload.active=false;upload.size=size;upload.offset=0;snprintf(upload.relative,sizeof(upload.relative),"%s",relative);
    snprintf(upload.path,sizeof(upload.path),CELLS_ROOT "%s",relative);snprintf(upload.part,sizeof(upload.part),"%s.part",upload.path);
    snprintf(upload.meta,sizeof(upload.meta),"%s.meta",upload.part);
    uint8_t expected[40],prior[40];memcpy(expected,upload.sha,32);for(unsigned i=0;i<8;i++)expected[32+i]=(uint8_t)(size>>(8*i));
    FILE *f=cells_fopen(upload.meta,"rb");bool same=f && fread(prior,1,40,f)==40 && !memcmp(prior,expected,40);if(f)fclose(f);
    struct stat st;if(same && !stat(upload.part,&st) && (uint64_t)st.st_size<=size)upload.offset=st.st_size;
    else { f=cells_fopen(upload.part,"wb");if(!f)return false;fclose(f);
        f=cells_fopen(upload.meta,"wb");if(!f)return false;bool ok=fwrite(expected,1,40,f)==40 && !fflush(f);if(fclose(f))ok=false;if(!ok)return false; }
    upload.last_us=esp_timer_get_time();upload.active=true;return true;
}
static bool cells_upload_data(uint64_t offset,const void *data,size_t n,uint32_t crc) {
    if(!upload.active || !n || n>sizeof(cw.buf) || offset>upload.size || n>upload.size-offset || cells_crc(data,n)!=crc)return false;
    if(offset<upload.offset) { /* Lost ACK: acknowledge only identical bytes. */
        if(offset+n>upload.offset)return false;
        int fd=open(upload.part,O_RDONLY);if(fd<0)return false;
        uint8_t *copy=heap_caps_malloc(n,MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);
        bool ok=copy && carto_sd_read_at(fd,upload.offset,offset,copy,n,NULL,0) && !memcmp(copy,data,n);heap_caps_free(copy);close(fd);return ok;
    }
    if(offset!=upload.offset)return false;
    FILE *f=cells_fopen(upload.part,"ab");if(!f)return false;
    bool ok=fwrite(data,1,n,f)==n && !fflush(f);if(fclose(f))ok=false;
    if(ok) { upload.offset+=n;upload.last_us=esp_timer_get_time(); }
    return ok;
}
/* HTTP already hashed the persisted stream. Console resume hashes the file. */
static bool cells_upload_finish(bool verified) {
    if(!upload.active || upload.offset!=upload.size)return false;
    bool ok=verified;
    if(!ok) { unlink(upload.part);unlink(upload.meta); }
    if(ok)ok=cells_install(upload.part,upload.path);
    if(ok) { unlink(upload.meta);
        struct stat st;char hex[65];for(unsigned i=0;i<32;i++)snprintf(hex+2*i,3,"%02x",upload.sha[i]);
        if(!stat(upload.path,&st))carto_digest_write(upload.path,&st,hex);
        if(!strcmp(upload.relative,"cells/catalog.ccm"))cells_catalog();
        if(!strcmp(upload.relative,"places/places_base.ccpl")) { cw.base_open=false;cw.query_pending=strlen(cells_work.query)>=2; }
    }
    upload.active=false;return ok;
}
static bool cells_upload_end(void) {
    if(!upload.active || upload.offset!=upload.size)return false;
    return cells_upload_finish(cells_hash(upload.part,upload.sha,upload.size) && cells_asset_valid(upload.part,upload.relative));
}
static int cells_upload_command(int argc,char **argv) {
    bool ok=false;
    if(argc==6 && !strcmp(argv[2],"begin")) {
        char *end;unsigned long long size=strtoull(argv[4],&end,10);
        ok=*argv[4] && !*end && cells_upload_begin(argv[3],size,argv[5]);
        if(ok)printf("CARTO-UP READY %llu\n",(unsigned long long)upload.offset);
    } else if(argc==6 && !strcmp(argv[2],"data")) {
        char *end,*crc_end;uint64_t offset=strtoull(argv[3],&end,10);unsigned long crc=strtoul(argv[4],&crc_end,16);
        size_t n=strlen(argv[5])/2;
        ok=*argv[3] && !*end && strlen(argv[4])==8 && !*crc_end && n<=sizeof(upload.chunk) && cells_hex(argv[5],upload.chunk,n) && cells_upload_data(offset,upload.chunk,n,(uint32_t)crc);
        if(ok)printf("CARTO-UP ACK %llu\n",(unsigned long long)upload.offset);
    } else if(argc==3 && !strcmp(argv[2],"end")) { ok=cells_upload_end();if(ok)puts("CARTO-UP SAVED"); }
    else if(argc==3 && !strcmp(argv[2],"cancel")) { upload.active=false;ok=true;puts("CARTO-UP PAUSED"); }
    if(!ok)puts("CARTO-UP ERROR");
    return ok?0:1;
}
static void cells_step(void) {
    cells_init();
    if(upload.active && esp_timer_get_time()-upload.last_us>60000000)upload.active=false;
    if(!cw.initialized || upload.active)return;
    if(cells_take()) { cells_request=cells_mail;cells_give(); }
    if(cells_request.serial!=cw.serial) {
        cw.serial=cells_request.serial;
        if(cells_request.action==6 && cells_url_valid(cells_request.url)) {
            snprintf(cw.server,sizeof(cw.server),"%s",cells_request.url);snprintf(cells_work.server,sizeof(cells_work.server),"%s",cw.server);cells_config_save();cw.catalog_refresh=true;
        } else if(cells_request.action==0) {
            cw.preview_active=false;cells_work.picked.name[0]=0;
            snprintf(cells_work.query,sizeof(cells_work.query),"%s",cells_request.query);
            cells_work.ready=false;cw.hint_attempts=0;cw.query_pending=strlen(cells_work.query)>=2;cells_work.busy=cw.query_pending;cells_work.count=0;
        } else if(cells_request.action==5) { cw.preview_active=true;cw.picked=cells_request.pick;cw.radius=cells_request.radius;cells_preview(); }
        else if(cells_request.action==1 && cells_work.ready) {
            bool known=false;for(unsigned i=0;i<cells_work.library_count;i++)if(cells_work.keys[i]==cw.selected_key)known=true;
            if(!known && cells_work.library_count>=LS_CELLS_PLACES)cells_message("32 places limit; delete a place first");
            else if(cells_record(cw.selected_key,cells_work.picked.name,cw.cover,true)) { cells_library();cells_message("Queued; safe to leave TILES"); }
        } else if(cells_request.action==2) { if(!dl.active)cells_delete(cells_request.key);else cells_message("Wait for the current cell before deleting"); }
        else if(cells_request.action==3) {
            char name[64];bool queued;snprintf(cw.path,sizeof(cw.path),CELLS_ROOT "installed/%08lx.ccpi",(unsigned long)cells_request.key);
            if(cells_record_read(cw.path,cw.cover,name,&queued))cells_record(cells_request.key,name,cw.cover,true);
            cells_library();cw.catalog_refresh=true;
        } else if(cells_request.action==4) {
            cw.catalog_refresh=true;
        }
    }
    if(dl.active) {
        unsigned done=cells_download_step();
        if(done==1) { cw.catalog_refresh=false;cells_catalog();if(cw.preview_active)cells_preview(); }
        if(done==2) { cw.base_open=false;cw.query_pending=true; }
        if(done==3) { cells_attach(cw.shard.iso2);if(cw.preview_active)cells_preview();else cw.query_pending=true; }
    } else if(cw.catalog_refresh)cells_download(1,"cells/catalog.ccm","cells/catalog.ccm",0,NULL);
    else if(cw.query_pending)cells_search();
    else if(cw.preview_active && cw.catalog<0)cells_preview();
    else if(cw.queue_loaded)cells_queue_step();
    cells_publish();
}
static void cells_service(void) {
    if(xSemaphoreTake(command_lock,0)!=pdTRUE)return;
    cells_step();
    if(source_refreshing && atomic_load(&view_active) && !atomic_load(&dismiss_pending)) {
        if(!select_map_mode("cells",false))show(saved.z);
    }
    xSemaphoreGive(command_lock);
}
