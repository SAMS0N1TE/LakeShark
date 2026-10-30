/* Included by ls_cartocore.c: single-owner SD/network service on carto-idle.
 * All large state and buffers are PSRAM. Public entry points only copy mail. */
#include "ls_cells.h"
#include "cartocore/places.h"
#include "mbedtls/sha256.h"
#include "ls_cells_json.h"
#include <time.h>
#include <errno.h>
static FILE *cells_fopen(const char *path,const char *mode) {
    FILE *f=fopen(path,mode);if(f)setvbuf(f,NULL,_IONBF,0);return f;
}
static void cells_pause(int64_t *since) {
    if(esp_timer_get_time()-*since<20000)return;
    /* command_lock can retain SDIO's inherited priority; leave a whole idle tick. */
    vTaskDelay(2);*since=esp_timer_get_time();
}
#define CELLS_ROOT "/sdcard/maps/"
#define CELLS_BITS 8192
static EXT_RAM_BSS_ATTR struct {
    char query[64],url[256];unsigned serial,pick,radius,action;uint32_t key;
} cells_mail,cells_request;
static EXT_RAM_BSS_ATTR ls_cells_view cells_pub,cells_work;
static bool cells_plus,cells_plus_work;
#define CELLS_ACTIONS 4
typedef struct { unsigned action;uint32_t key;char name[64];uint8_t cover[CELLS_BITS]; } cells_command;
static EXT_RAM_BSS_ATTR cells_command cells_actions[CELLS_ACTIONS],cells_command_work,cells_preview_mail;
static unsigned cells_action_head,cells_action_count;
static atomic_flag cells_guard=ATOMIC_FLAG_INIT;
static bool cells_take(void) { return !atomic_flag_test_and_set(&cells_guard); }
static void cells_give(void) { atomic_flag_clear(&cells_guard); }
/* Readers wait out a publish (a struct copy, microseconds) instead of
   failing. A failed read reached TILES as "no state list": the US map lost
   its colours for one frame every time the worker published, which is the
   flashing on GET MAPS. */
static bool cells_take_reader(void) {
    /* A pure spin: this runs on the UI's checked stack, which may not call
       out. The worker holds the guard for one struct copy on the other core;
       if it is somehow longer, the read fails and TILES keeps its last list. */
    for(unsigned i=0;i<20000;i++) { if(cells_take())return true; __asm__ volatile("" ::: "memory"); }
    return false;
}
bool ls_cells_snapshot(ls_cells_view *out) {
    if(!cells_take_reader()) return false;
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
    if(action<1 || (action>4 && action!=7 && action!=8) || !cells_take()) return false;
    if(action==1 && (!cells_pub.ready || cells_pub.busy)) { cells_give();return false; }
    if(cells_action_count==CELLS_ACTIONS) { cells_give();return false; }
    cells_command *q=&cells_actions[(cells_action_head+cells_action_count)%CELLS_ACTIONS];
    q->action=action;q->key=key;
    if(action==1) { q->key=cells_preview_mail.key;memcpy(q->name,cells_preview_mail.name,64);memcpy(q->cover,cells_preview_mail.cover,CELLS_BITS); }
    cells_action_count++;cells_give();return true;
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
    uint64_t queue_total,queue_done,queue_partial;bool progress_loaded,want_detail;
    uint32_t progress_revision;
    unsigned picked,radius,hint_attempts;bool initialized,base_open,query_pending,queue_loaded,catalog_refresh,states_refresh,preview_active;
    int catalog;bool direct;uint32_t stride,entries,index_size,base_size;uint8_t index_sha[32],base_sha[32];
    uint8_t *catalog_mem;uint64_t catalog_bytes;   /* the catalog in PSRAM, or NULL: read from the card */
    int64_t queue_wait_until,catalog_auto_at;      /* a stopped queue sleeps; a stale catalog refreshes once */
    char path[256],temp[272],backup[280];
} cw;
static EXT_RAM_BSS_ATTR uint8_t cells_credited[2*CELLS_BITS];
static void cells_progress_reset(void) {
    cw.progress_loaded=false;cw.progress_revision++;
    cw.queue_total=cw.queue_done=cw.queue_partial=0;cells_work.maps_progress=0;
}
static void cells_progress_credit(unsigned at,uint32_t size) {
    if(cw.progress_loaded && at<131072 && !(cells_credited[at/8]&(1u<<(at&7)))) {
        cells_credited[at/8]|=(uint8_t)(1u<<(at&7));cw.queue_done+=size;
    }
}
static uint32_t cells_u32(const uint8_t *p) { return p[0]|(uint32_t)p[1]<<8|(uint32_t)p[2]<<16|(uint32_t)p[3]<<24; }
static void cells_put32(uint8_t *p,uint32_t v) { for(unsigned i=0;i<4;i++)p[i]=(uint8_t)(v>>(8*i)); }
static uint32_t cells_crc(const void *data,size_t n) {
    const uint8_t *p=data;uint32_t c=~0u;while(n--) { c^=*p++;for(unsigned k=0;k<8;k++)c=(c>>1)^(0xedb88320u&-(c&1)); }return ~c;
}
static void cells_message(const char *s) { snprintf(cells_work.message,sizeof(cells_work.message),"%s",s); }
static void cells_publish(void) {
    cells_work.generation=cw.generation;cells_work.queued=cw.queue_loaded;
    snprintf(cells_work.queue_name,sizeof(cells_work.queue_name),"%s",cw.queue_loaded?cw.queue_name:"");
    while(!cells_take()) vTaskDelay(2);
    cells_pub=cells_work;cells_plus=cells_plus_work;
    if(!cw.progress_loaded)cells_pub.maps_progress=0;
    if(cells_work.ready) { cells_preview_mail.key=cw.selected_key;memcpy(cells_preview_mail.name,cells_work.picked.name,64);memcpy(cells_preview_mail.cover,cw.cover,CELLS_BITS); }
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
    if(!strcmp(s,"cells/states.json") || !strcmp(s,"cells/index.cci") || !strcmp(s,"cells/catalog.ccm") || !strcmp(s,"cells/overview.ctile") ||
       !strcmp(s,"places/places_base.ccpl") || !strcmp(s,"outlines.lso")) return true;
    unsigned x,y;int n=0;
    if(sscanf(s,"cells/8-%u-%u.detail.ctile%n",&x,&y,&n)==2 && n && !s[n] && x<256 && y<256) {
        char canonical[48];snprintf(canonical,sizeof(canonical),"cells/8-%u-%u.detail.ctile",x,y);return !strcmp(s,canonical);
    }
    n=0;
    if(sscanf(s,"cells/8-%u-%u.ctile%n",&x,&y,&n)==2 && n && !s[n] && x<256 && y<256) {
        char canonical[32];snprintf(canonical,sizeof(canonical),"cells/8-%u-%u.ctile",x,y);return !strcmp(s,canonical);
    }
    return strlen(s)==21 && !strncmp(s,"places/places_",14) && s[14]>='A' && s[14]<='Z' &&
        s[15]>='A' && s[15]<='Z' && !strcmp(s+16,".ccpl");
}
/* The SHA-256 of a file of exactly `size` bytes. */
static bool cells_digest(const char *path,uint64_t size,uint8_t digest[32]) {
    /* read() avoids newlib's unbuffered refill walking every FILE under sfp. */
    int fd=open(path,O_RDONLY);if(fd<0)return false;
    mbedtls_sha256_context ctx;mbedtls_sha256_init(&ctx);bool ok=!mbedtls_sha256_starts(&ctx,0);
    int64_t since=esp_timer_get_time();
    uint64_t count=0;int n=0;while(ok && (n=read(fd,cw.buf,32768))>0) {
        count+=n;ok=count<=size && !mbedtls_sha256_update(&ctx,cw.buf,n);
        cells_pause(&since);
    }
    ok=ok && n==0 && count==size;if(close(fd))ok=false;
    ok=ok && !mbedtls_sha256_finish(&ctx,digest);mbedtls_sha256_free(&ctx);return ok;
}
static bool cells_hash(const char *path,const uint8_t sha[32],uint64_t size) {
    uint8_t digest[32];return cells_digest(path,size,digest) && !memcmp(digest,sha,32);
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
static void cells_catalog_close(void) {
    if(cw.catalog>=0)close(cw.catalog);
    cw.catalog=-1;heap_caps_free(cw.catalog_mem);cw.catalog_mem=NULL;cw.catalog_bytes=0;
}
/* Catalog rows from PSRAM when it fits there. A state count looks up every
   cell of all 52 states twice; from the card each lookup was a dozen reads,
   about 30 ms a cell, and a count took over a minute of carto-idle. */
static bool cells_catalog_read(uint64_t offset,void *dst,size_t n) {
    if(cw.catalog_mem) {
        if(offset>cw.catalog_bytes || n>cw.catalog_bytes-offset)return false;
        memcpy(dst,cw.catalog_mem+offset,n);return true;
    }
    return carto_sd_read_at(cw.catalog,cw.catalog_bytes,offset,dst,n,NULL,0);
}
static bool cells_install(const char *part,const char *path) {
    if(!strcmp(path,CELLS_ROOT "cells/catalog.ccm"))cells_catalog_close();
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
static bool cells_asset_valid(const char *,const char *);
static bool cells_direct_active(void) {
    FILE *f=cells_fopen(CELLS_ROOT "cells/catalog.ccm","rb");if(!f)return false;char magic[8];bool ok=fread(magic,1,8,f)==8 && !memcmp(magic,"CCMAPS2",8);fclose(f);return ok;
}
static bool cells_catalog(void) {
    cells_progress_reset();cw.next_cell=0; /* Every catalog replacement revalidates queued ownership. */
    cells_catalog_close();
    if(!cells_asset_valid(CELLS_ROOT "cells/catalog.ccm","cells/catalog.ccm"))return false;
    cw.catalog=open(CELLS_ROOT "cells/catalog.ccm",O_RDONLY);struct stat st;
    if(cw.catalog<0)return false;
    uint8_t h[128];
    if(fstat(cw.catalog,&st) || !carto_sd_read_at(cw.catalog,st.st_size,0,h,128,NULL,0) ||
       (memcmp(h,"CCMAPS1",8) && memcmp(h,"CCMAPS2",8)) || (cw.entries=cells_u32(h+8))>131073 || (uint64_t)st.st_size!=128+(uint64_t)cw.entries*(!memcmp(h,"CCMAPS2",8)?48:40)) goto bad;
    cw.catalog_bytes=(uint64_t)st.st_size;
    /* 2281 cells today is 109 KB; past 2 MB the card keeps serving it */
    if(st.st_size<=2*1024*1024) {
        cw.catalog_mem=heap_caps_malloc((size_t)st.st_size,MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);
        if(cw.catalog_mem && !carto_sd_read_at(cw.catalog,st.st_size,0,cw.catalog_mem,(size_t)st.st_size,NULL,0)) {
            heap_caps_free(cw.catalog_mem);cw.catalog_mem=NULL;
        }
    }
    cw.direct=!memcmp(h,"CCMAPS2",8);cw.stride=cw.direct?48:40;
    cw.index_size=cells_u32(h+12);memcpy(cw.index_sha,h+16,32);
    cw.base_size=cells_u32(h+48);memcpy(cw.base_sha,h+52,32);
    if((!cw.direct && cw.index_size<64) || (cw.direct && cw.index_size) || cw.base_size<128 || cw.base_size>8*1024*1024) goto bad;
    uint32_t prior=0;int64_t since=esp_timer_get_time();
    for(uint32_t i=0;i<cw.entries;i++) {
        cells_pause(&since);
        uint8_t r[40];if(!cells_catalog_read(128+(uint64_t)i*cw.stride,r,40))goto bad;
        uint32_t id=cells_u32(r);if(id>131072 || (i && id<=prior) || cells_u32(r+4)<64)goto bad;prior=id;
    }
    return true;
bad:cells_catalog_close();cells_message("Invalid cells catalog");return false;
}
static bool cells_entry_checked(uint32_t id,uint32_t *size,uint8_t sha[32],bool *error) {
    if(cw.catalog<0) { if(error)*error=true;return false; }
    uint32_t lo=0,hi=cw.entries;uint8_t r[40];
    while(lo<hi) {
        uint32_t mid=lo+(hi-lo)/2;
        if(!cells_catalog_read(128+(uint64_t)mid*cw.stride,r,40)) { if(error)*error=true;return false; }
        uint32_t key=cells_u32(r);if(key<id)lo=mid+1;else if(key>id)hi=mid;else { *size=cells_u32(r+4);memcpy(sha,r+8,32);return true; }
    }
    return false;
}
static bool cells_entry(uint32_t id,uint32_t *size,uint8_t sha[32]) {
    return cells_entry_checked(id,size,sha,NULL);
}
/* CCMAPS2 has fixed 48-byte rows regardless of part count. uint32_t counts
 * support large cells without a per-part array; joined .part checkpoints resume
 * at any host part, restarting only that part if HTTP Range is refused. */
static bool cells_parts(uint32_t id,uint32_t *part,uint32_t *count) {
    *part=0;*count=1;if(!cw.direct)return true;
    uint32_t lo=0,hi=cw.entries;uint8_t row[48];
    while(lo<hi) {
        uint32_t mid=lo+(hi-lo)/2;
        if(!cells_catalog_read(128+(uint64_t)mid*48,row,48))return false;
        uint32_t key=cells_u32(row);if(key<id)lo=mid+1;else if(key>id)hi=mid;else {
            *part=cells_u32(row+40);*count=cells_u32(row+44);return true;
        }
    }return false;
}
static void cells_name(uint32_t id,char *out,size_t n,bool remote) {
    if(!id)snprintf(out,n,"cells/overview.ctile");
    else snprintf(out,n,remote?"cells/8/%u/%u%s.ctile":"cells/8-%u-%u%s.ctile",((id-1)%65536)/256,(id-1)%256,id>65536?".detail":"");
}
static bool cells_matches(const char *,uint64_t,const uint8_t[32]);
static bool cells_matches_checked(const char *,uint64_t,const uint8_t[32],bool *);
static bool cells_present(uint32_t id,uint32_t size) {
    char name[64],path[96];cells_name(id,name,sizeof(name),false);snprintf(path,sizeof(path),CELLS_ROOT "%s",name);
    uint8_t sha[32];uint32_t expected;
    return cells_entry(id,&expected,sha) && expected==size && cells_matches(path,size,sha);
}
#include "ls_cells_states_worker.h"
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
    bool ok=fread(h,1,80,f)==80 && fread(bits,1,CELLS_BITS,f)==CELLS_BITS && fgetc(f)==EOF && !ferror(f);
    fclose(f);if(!ok || memcmp(h,"CCPLACE2",8) || cells_u32(h+8)>1 || !memchr(h+16,0,64) || cells_crc(bits,CELLS_BITS)!=cells_u32(h+12))return false;
    memcpy(name,h+16,64);*queued=cells_u32(h+8)!=0;return true;
}
static void cells_library(void) {
    (void)cells_token_load(); /* keeps maps.plus current for the STATES page */
    static EXT_RAM_BSS_ATTR struct {
        char library[LS_CELLS_PLACES][64],name[64];uint32_t keys[LS_CELLS_PLACES],key;
        uint8_t cover[CELLS_BITS];unsigned count;bool queued;
    } next;
    DIR *d=opendir(CELLS_ROOT "installed");if(!d)return;struct dirent *e;
    next.count=0;next.queued=false;bool ok=true;int64_t since=esp_timer_get_time();
    for(;;) {
        cells_pause(&since);
        errno=0;e=readdir(d);if(!e) { if(errno)ok=false;break; }
        unsigned key;int n=0;bool queued;char name[64];
        if(sscanf(e->d_name,"%8x.ccpi%n",&key,&n)!=1 || !n || e->d_name[n])continue;
        snprintf(cw.path,sizeof(cw.path),CELLS_ROOT "installed/%s",e->d_name);
        if(!cells_record_read(cw.path,cw.scratch,name,&queued)) { ok=false;break; }
        if(next.count<LS_CELLS_PLACES) {
            unsigned at=next.count++;next.keys[at]=key;
            snprintf(next.library[at],64,"%s%s",queued?"[queued] ":"",name);
        }
        if(queued && !cw.queue_loaded && !next.queued) {
            memcpy(next.cover,cw.scratch,CELLS_BITS);next.key=key;memcpy(next.name,name,64);next.queued=true;
        }
    }
    if(closedir(d))ok=false;
    if(!ok)return;
    memcpy(cells_work.library,next.library,sizeof(next.library));memcpy(cells_work.keys,next.keys,sizeof(next.keys));cells_work.library_count=next.count;
    if(next.queued) {
        memcpy(cw.queued,next.cover,CELLS_BITS);cw.queue_key=next.key;memcpy(cw.queue_name,next.name,64);cw.queue_loaded=true;cw.next_cell=0;cells_progress_reset();
        double sx=0,sy=0,rows=0,count=0,pi=3.14159265358979323846;
        for(unsigned id=0;id<65536;id++)if(cw.queued[id/8]&(1u<<(id&7))) {
            double angle=((id/256+.5)/256.0*2-1)*pi;sx+=cos(angle);sy+=sin(angle);rows+=id%256+.5;count++;
        }
        if(count) { cells_work.queue_lon=atan2(sy,sx)*180/pi;cells_work.queue_lat=atan(sinh(pi*(1-2*rows/count/256)))*180/pi; }
    }
}
static bool cells_delete(uint32_t key) {
    uint8_t *owned=heap_caps_calloc(1,CELLS_BITS,MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);if(!owned)return false;
    char name[64];bool queued;snprintf(cw.path,sizeof(cw.path),CELLS_ROOT "installed/%08lx.ccpi",(unsigned long)key);
    bool ok=cells_record_read(cw.path,cw.cover,name,&queued);if(!ok) { heap_caps_free(owned);return false; }
    /* Remove ownership first: a reset can leak an unreferenced file, never delete a referenced one. */
    if(unlink(cw.path)) { heap_caps_free(owned);return false; }
    DIR *d=opendir(CELLS_ROOT "installed");struct dirent *e;
    if(!d) { heap_caps_free(owned);return false; }
    bool valid=true;int64_t since=esp_timer_get_time();
    if(d) { while((e=readdir(d))) {
        cells_pause(&since);
        if(!strstr(e->d_name,".ccpi"))continue;
        snprintf(cw.path,sizeof(cw.path),CELLS_ROOT "installed/%s",e->d_name);
        if(cells_record_read(cw.path,cw.scratch,name,&queued))for(unsigned i=0;i<CELLS_BITS;i++)owned[i]|=cw.scratch[i];
        else valid=false;
    }closedir(d); }
    if(!valid) { heap_caps_free(owned);cells_message("Unreadable ownership record; cells retained");return false; }
    if(!cells_release_source()) { heap_caps_free(owned);return false; }
    for(unsigned id=0;id<65536;id++)if((cw.cover[id/8]&(1u<<(id&7))) && !(owned[id/8]&(1u<<(id&7)))) {
        cells_pause(&since);
        for(unsigned layer=0;layer<2;layer++) {
            char relative[64];cells_name(id+1+layer*65536,relative,sizeof(relative),false);
            snprintf(cw.path,sizeof(cw.path),CELLS_ROOT "%s",relative);unlink(cw.path);
            snprintf(cw.temp,sizeof(cw.temp),"%s.part",cw.path);unlink(cw.temp);
            snprintf(cw.temp,sizeof(cw.temp),"%s.part.meta",cw.path);unlink(cw.temp);
        }
    }
    if(cw.queue_key==key) { cw.queue_loaded=false;cells_progress_reset(); }
    cw.generation++;heap_caps_free(owned);cells_library();return true;
}

typedef struct {
    int fd;uint64_t size;int64_t yielded;
    bool (*check)(void *);void *check_ctx;
} cells_reader;
static int cells_asset_read(void *ctx,uint64_t off,void *dst,size_t n) {
    cells_reader *r=ctx;
    cells_pause(&r->yielded);
    if(r->check && !r->check(r->check_ctx))return 0;
    return carto_sd_read_at(r->fd,r->size,off,dst,n,NULL,0);
}
static bool cells_asset_valid_checked(const char *path,const char *relative,bool (*check)(void *),void *ctx) {
    cells_reader r={.fd=open(path,O_RDONLY),.check=check,.check_ctx=ctx,.yielded=esp_timer_get_time()};struct stat st;if(r.fd<0)return false;
    bool ok=!fstat(r.fd,&st);r.size=ok?st.st_size:0;
    if(ok && !strcmp(relative,"cells/states.json"))ok=cells_states_parse(path,false);
    else if(ok && !strcmp(relative,"outlines.lso")) {
        uint8_t h[20];ok=r.size>=20 && cells_asset_read(&r,0,h,sizeof(h)) && !memcmp(h,"LSOUT1\0\0",8);
        if(ok)ok=r.size==20+(uint64_t)cells_u32(h+8)*58+(uint64_t)cells_u32(h+12)*8+(uint64_t)cells_u32(h+16)*4;
    }
    else if(ok && !strncmp(relative,"places/",7))ok=cc_places_verify(cells_asset_read,&r,r.size,cw.buf,sizeof(cw.buf));
    else if(ok && !strcmp(relative,"cells/catalog.ccm")) {
        uint8_t h[128];ok=r.size>=128 && cells_asset_read(&r,0,h,128) && (!memcmp(h,"CCMAPS1",8) || !memcmp(h,"CCMAPS2",8));
        uint32_t count=ok?cells_u32(h+8):0,prior=0;bool direct=ok && !memcmp(h,"CCMAPS2",8);unsigned stride=direct?48:40;
        ok=ok && count && count<=131073 && r.size==128+(uint64_t)count*stride && (direct?cells_u32(h+12)==0:cells_u32(h+12)>=64) && cells_u32(h+48)>=128;
        for(uint32_t i=0;ok && i<count;i++) {
            uint8_t row[48]={0};ok=cells_asset_read(&r,128+(uint64_t)i*stride,row,stride);
            uint32_t id=cells_u32(row);ok=ok && id<=131072 && (!i || id>prior) && cells_u32(row+4)>=64;
            if(direct) { uint32_t part=cells_u32(row+40),n=cells_u32(row+44),bytes=cells_u32(row+4);
                ok=ok && ((part==0 && n==1 && bytes<=24u*1024u*1024u) || (part>0 && part<24u*1024u*1024u && n==1+(bytes-1)/part)); }
            prior=id;
        }
    } else if(ok) {
        cc_cellset *set=heap_caps_calloc(1,sizeof(*set),MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);
        uint8_t *dir=heap_caps_malloc(4096,MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);uint8_t dummy;
        if(!set || !dir)ok=false;
        else if(!strcmp(relative,"cells/index.cci"))
            ok=cc_cellset_open(set,cells_asset_read,&r,r.size,(cc_cellset_io){NULL,cell_open,cell_read,cell_close},dir,4096,&dummy,1);
        else ok=cc_ctile_open_source(&set->map,&set->source,cells_asset_read,&r,r.size,dir,4096,&dummy,1) && set->source.header[8]==7 && set->map.zmax<=16;
        heap_caps_free(set);heap_caps_free(dir);
    }
    close(r.fd);return ok;
}
static bool cells_asset_valid(const char *path,const char *relative) {
    return cells_asset_valid_checked(path,relative,NULL,NULL);
}
static bool cells_cached_digest(const char *path,const struct stat *st,const uint8_t sha[32],bool *known,bool *error) {
    char side[272],hex[65],expected[65];long long size,mtime;*known=false;
    snprintf(side,sizeof(side),"%s.sha",path);FILE *f=fopen(side,"r");
    if(!f) { if(error && errno!=ENOENT && errno!=ENOTDIR)*error=true;return false; }
    int n=carto_digest_scan(f,&size,&mtime,hex);bool failed=ferror(f)!=0;
    if(fclose(f))failed=true;
    if(failed) { if(error)*error=true;return false; }
    *known=n==3 && size==(long long)st->st_size && mtime==(long long)st->st_mtime && strlen(hex)==64;
    for(unsigned i=0;i<32;i++)snprintf(expected+2*i,3,"%02x",sha[i]);
    return *known && !strcasecmp(hex,expected);
}
static bool cells_matches_checked(const char *path,uint64_t size,const uint8_t sha[32],bool *error) {
    struct stat st;if(stat(path,&st)) { if(error && errno!=ENOENT && errno!=ENOTDIR)*error=true;return false; }
    if(st.st_size!=(int64_t)size)return false;
    bool known;
    if(cells_cached_digest(path,&st,sha,&known,error))return true;
    if(error && *error)return false;
    /* The record says what this exact file hashed to last time. Kept only
       when it matched, every out-of-date cell was hashed again on every
       state count: minutes of SD reads after each boot or refresh, with
       TILES showing no states meanwhile. */
    if(known)return false;
    uint8_t got[32];if(!cells_digest(path,size,got)) { if(error)*error=true;return false; }
    struct stat after;
    if(stat(path,&after) || st.st_size!=after.st_size || st.st_mtime!=after.st_mtime) { if(error)*error=true;return false; }
    char ghex[65];for(unsigned i=0;i<32;i++)snprintf(ghex+2*i,3,"%02x",got[i]);
    /* FatFs rename cannot replace a stale (e.g. old mtime) sidecar. */
    if(carto_digest_remove(path))(void)carto_digest_write(path,&st,ghex);
    return !memcmp(got,sha,32);
}
static bool cells_matches(const char *path,uint64_t size,const uint8_t sha[32]) {
    return cells_matches_checked(path,size,sha,NULL);
}
/* Direct cells carry their own offsets; keep old roads until UPDATE replaces
 * them. A new index must never address offsets in an older cell revision. */
static bool cells_file_current(uint32_t id,const char *path) {
    if(!cw.initialized || cw.catalog<0)return !cells_direct_active(); /* locally copied indexed legacy set */
    if((!cw.direct && !cells_matches(CELLS_ROOT "cells/index.cci",cw.index_size,cw.index_sha)))return false;
    uint32_t size;uint8_t sha[32];if(!cells_entry(id,&size,sha))return false;
    if(cw.direct) { struct stat st;return !stat(path,&st) && S_ISREG(st.st_mode) && st.st_size>=64; }
    return cells_matches(path,size,sha);
}
/* Failure records survive dl's per-attempt reset, including joined .pN parts.
 * Only failed assets allocate PSRAM; never evict a permanent stop automatically. */
typedef struct cells_retry {
    struct cells_retry *next;
    char relative[64];unsigned failures;int status;int64_t at;
} cells_retry;
static cells_retry *cells_retries;
static bool cells_retry_oom;
static cells_retry *cells_retry_find(const char *relative) {
    for(cells_retry *r=cells_retries;r;r=r->next)if(!strcmp(r->relative,relative))return r;
    return NULL;
}
static void cells_retry_reset(const char *relative) {
    for(cells_retry **p=&cells_retries;*p;) {
        cells_retry *r=*p;
        if(!relative || !strcmp(r->relative,relative)) { *p=r->next;heap_caps_free(r); }
        else p=&r->next;
    }
    if(!relative)cells_retry_oom=false;
    cw.queue_wait_until=0;
}
static void cells_retry_message(const cells_retry *r) {
    char why[128];
    if(r->status)snprintf(why,sizeof(why),"Map server %s %s; tap REFRESH to try again",
        r->status==404 || r->status==410?"missing":"rejected",r->relative);
    else snprintf(why,sizeof(why),"Invalid map %s (SHA256 / format / size); tap REFRESH",r->relative);
    cells_message(why);
}
static bool cells_retry_ready(const char *relative) {
    if(cells_retry_oom) { cells_work.downloading=false;cells_message("Map retry memory unavailable; tap REFRESH to try again");return false; }
    cells_retry *r=cells_retry_find(relative);
    if(r && r->at==INT64_MAX) { cells_retry_message(r);cells_work.downloading=false;return false; }
    if(r && esp_timer_get_time()<r->at) { cells_work.downloading=false;return false; }
    return true;
}
/* A hard 15 minute cap after +/-20% jitter. Server minimums may exceed it. */
static int64_t cells_retry_delay(unsigned failures,uint32_t random) {
    int64_t base=5000000;
    while(failures>1 && base<900000000) { base*=2;failures--; }
    if(base>900000000)base=900000000;
    int64_t delay=base-base/5+(int64_t)((uint64_t)random*(uint64_t)(2*(base/5))/UINT32_MAX);
    return delay>900000000?900000000:delay;
}
#ifndef LS_CARTOCORE_HOST
/* Retry-After exposes both delta-seconds and IMF-fixdate through ON_HEADER. */
static int64_t cells_retry_after(const char *value) {
    while(*value==' ' || *value=='\t')value++;
    if(*value>='0' && *value<='9') {
        uint64_t seconds=0;
        do { unsigned digit=(unsigned)(*value++-'0');
            if(seconds>((uint64_t)INT64_MAX/1000000-digit)/10)return INT64_MAX;
            seconds=seconds*10+digit;
        } while(*value>='0' && *value<='9');
        while(*value==' ' || *value=='\t')value++;
        return *value?0:(int64_t)seconds*1000000;
    }
    int day,year,hour,minute,second,end=0;char month[4];
    if(sscanf(value,"%*3s, %2d %3s %4d %2d:%2d:%2d GMT%n",&day,month,&year,&hour,&minute,&second,&end)!=6 || !end)return 0;
    while(value[end]==' ' || value[end]=='\t')end++;
    if(value[end] || year<1970 || year>9999 || hour<0 || hour>23 || minute<0 || minute>59 || second<0 || second>59)return 0;
    static const char months[]="JanFebMarAprMayJunJulAugSepOctNovDec";
    static const unsigned days[]={31,28,31,30,31,30,31,31,30,31,30,31};
    unsigned m=0;while(m<12 && memcmp(month,months+3*m,3))m++;
    bool leap=year%4==0 && (year%100!=0 || year%400==0);
    if(m==12 || day<1 || (unsigned)day>days[m]+(m==1 && leap))return 0;
    int64_t count=(int64_t)(year-1970)*365+(year-1)/4-1969/4-(year-1)/100+1969/100+(year-1)/400-1969/400;
    for(unsigned i=0;i<m;i++)count+=days[i]+(i==1 && leap);
    int64_t seconds=(count+day-1)*86400+hour*3600+minute*60+second-(int64_t)time(NULL);
    return seconds>0?seconds*1000000:0;
}
#endif
static EXT_RAM_BSS_ATTR struct {
    char relative[64],remote[96],path[128],part[144],meta[160],url[384],range[80];
    uint64_t size,offset;uint8_t sha[32];unsigned kind;bool active;uint32_t part_bytes,part_count;uint64_t segment_start,segment_end;
    FILE *out,*in;int64_t retry_after;
#ifndef LS_CARTOCORE_HOST
    esp_http_client_handle_t http;
#endif
    uint32_t cell_id;
} dl;
#ifndef LS_CARTOCORE_HOST
static esp_err_t cells_http_event(esp_http_client_event_t *event) {
    if(event->event_id==HTTP_EVENT_ON_HEADER && !strcasecmp(event->header_key,"Content-Range"))
        snprintf(dl.range,sizeof(dl.range),"%s",event->header_value);
    if(event->event_id==HTTP_EVENT_ON_HEADER && !strcasecmp(event->header_key,"Retry-After"))
        dl.retry_after=cells_retry_after(event->header_value);
    return ESP_OK;
}
/* The map server's connection, kept between files and between the 2 MB parts
   of one file: a new TLS session took 1.5 s a part on the bench board, about
   a quarter of each part's time. Only a response read to its end is kept;
   it is dropped after 10 s unused and on any error. */
static esp_http_client_handle_t cells_conn;
static int64_t cells_conn_at;
static void cells_conn_drop(void) {
    if(cells_conn) { esp_http_client_close(cells_conn);esp_http_client_cleanup(cells_conn);cells_conn=NULL; }
}
static void cells_conn_park(void) {
    if(!dl.http)return;
    cells_conn_drop();
    if(esp_http_client_is_complete_data_received(dl.http)) { cells_conn=dl.http;cells_conn_at=esp_timer_get_time(); }
    else { esp_http_client_close(dl.http);esp_http_client_cleanup(dl.http); }
    dl.http=NULL;
}
static void cells_conn_expire(void) {
    if(cells_conn && esp_timer_get_time()-cells_conn_at>10000000)cells_conn_drop();
}
#else
static void cells_conn_park(void) {}
static void cells_conn_expire(void) {}
#endif
static void cells_download_close(void) {
    if(dl.out)fclose(dl.out);
    if(dl.in)fclose(dl.in);
    dl.out=dl.in=NULL;
#ifndef LS_CARTOCORE_HOST
    if(dl.http) { esp_http_client_close(dl.http);esp_http_client_cleanup(dl.http);dl.http=NULL; }
#endif
}
static void cells_download_failed(const char *why,int status,bool permanent) {
    /* A cell that is not what the catalog says (size, hash, gone from the
       server) usually means the card's catalog is older than the server's:
       2026-10-30 a state stopped at 98% for hours on "tap REFRESH". Fetch
       the new catalog, at most once in 30 minutes, before stopping on it. */
    const int64_t failed_at=esp_timer_get_time();
    const bool stale=permanent && (dl.kind==5 || dl.kind==6) &&
        (!cw.catalog_auto_at || failed_at-cw.catalog_auto_at>1800000000LL);
    if(stale) { cw.catalog_auto_at=failed_at;cw.catalog_refresh=true;permanent=false; }
    cells_retry *r=cells_retry_find(dl.relative);
    if(!r) {
        r=heap_caps_calloc(1,sizeof(*r),MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);
        if(r) { snprintf(r->relative,sizeof(r->relative),"%s",dl.relative);r->next=cells_retries;cells_retries=r; }
        else cells_retry_oom=true; /* Fail closed if a stop cannot be remembered. */
    }
    if(r) {
        if(r->failures<9)r->failures++;
        r->status=status;
        if(permanent)r->at=INT64_MAX;
        else {
#ifdef LS_CARTOCORE_HOST
            uint32_t random=(uint32_t)rand();random=random*65537u+(uint32_t)rand();
#else
            uint32_t random=esp_random();
#endif
            int64_t delay=cells_retry_delay(r->failures,random),now=esp_timer_get_time();
            /* Honour Retry-After, but at most an hour: a date against an unset clock reads as decades. */
            int64_t asked=dl.retry_after>3600000000LL?3600000000LL:dl.retry_after;
            if((status==429 || status==503) && delay<asked)delay=asked;
            if(status==429 && delay<60000000)delay=60000000;
            r->at=delay>INT64_MAX-now?INT64_MAX:now+delay;
        }
    }
    cells_download_close();dl.active=false;cells_work.downloading=false;
    cells_message(stale?"Maps changed on the server; loading the new catalog":why);
    if(permanent && r)cells_retry_message(r);
    if(dl.kind==8)snprintf(cells_work.outlines,sizeof(cells_work.outlines),"%s",cells_work.message);
}
static void cells_download_fail(const char *why) { cells_download_failed(why,0,false); }
#ifndef LS_CARTOCORE_HOST
static void cells_download_http_fail(int status) {
    cells_download_failed("HTTP rejected download; partial retained",status,status>=400 && status<500 && status!=408 && status!=429);
}
#endif
static bool cells_download(unsigned kind,const char *remote,const char *relative,uint64_t size,const uint8_t *sha) {
    if(dl.active || !cells_safe_asset(relative) || !cells_retry_ready(relative))return false;
    uint32_t asset_id=0;
    if(kind==6) { unsigned x,y;if(sscanf(remote,"cells/8/%u/%u.ctile",&x,&y)!=2 || x>255 || y>255)return false;asset_id=1+x*256+y+(strstr(remote,".detail.ctile")?65536:0);
        if(!cells_id_allowed(asset_id)) { cells_message("LakeShark Plus token required");return false; } }
    memset(&dl,0,sizeof(dl));dl.kind=kind;dl.size=size;dl.segment_end=size;dl.cell_id=asset_id;
    if((kind==5 || kind==6) && !cells_parts(asset_id,&dl.part_bytes,&dl.part_count))return false;
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
    if(kind==6)cw.queue_partial=dl.offset;
    dl.out=cells_fopen(dl.part,dl.offset?"ab":"wb");if(!dl.out)return false;setvbuf(dl.out,NULL,_IONBF,0);
    if(size && dl.offset==size) { dl.active=true;return true; }
    uint64_t local_offset=dl.offset;
    dl.segment_end=size;
    if(dl.part_bytes) {
        unsigned part=(unsigned)(dl.offset/dl.part_bytes);dl.segment_start=(uint64_t)part*dl.part_bytes;
        dl.segment_end=dl.segment_start+dl.part_bytes;if(dl.segment_end>size)dl.segment_end=size;
        local_offset=dl.offset-dl.segment_start;
        snprintf(dl.remote,sizeof(dl.remote),"%s.p%u",remote,part);
    }
    snprintf(dl.url,sizeof(dl.url),"%s%s%s",cw.server,cw.server[strlen(cw.server)-1]=='/'?"":"/",dl.remote);
#ifdef LS_CARTOCORE_HOST
    if(strncmp(dl.url,"file://",7)) { cells_download_fail("Host server must use file://");return false; }
    dl.in=cells_fopen(dl.url+7,"rb");if(!dl.in || fseek(dl.in,(long)local_offset,SEEK_SET)) { cells_download_fail("Server file unavailable; will retry");return false; }
#else
    dl.range[0]=0;
    /* 4 s: a TLS handshake over a hosted C5/C6 link can outlast 1.5 s on its own. */
    /* 16 KB reads (PSRAM, as carto fetch has): 1 KB ones walked TLS and the
       parser 64 times for each 64 KB the worker asked for. */
    esp_http_client_config_t cfg={.url=dl.url,.timeout_ms=4000,.crt_bundle_attach=esp_crt_bundle_attach,
        .buffer_size=16384,.buffer_size_tx=1024,.event_handler=cells_http_event,.keep_alive_enable=true};
    bool reused=cells_conn && esp_timer_get_time()-cells_conn_at<10000000 && esp_http_client_set_url(cells_conn,dl.url)==ESP_OK;
    if(reused) { dl.http=cells_conn;cells_conn=NULL; }
    else { cells_conn_drop();dl.http=esp_http_client_init(&cfg); }
    if(!dl.http) { cells_download_fail("HTTP allocation failed");return false; }
    esp_err_t opened;int64_t length;
    for(;;) {
        esp_http_client_delete_header(dl.http,"Range");esp_http_client_delete_header(dl.http,"X-LakeShark-Token");
        if(local_offset) { char range[64];snprintf(range,sizeof(range),"bytes=%llu-",(unsigned long long)local_offset);esp_http_client_set_header(dl.http,"Range",range); }
        cells_token_load();if(cells_token[0])esp_http_client_set_header(dl.http,"X-LakeShark-Token",cells_token);
        opened=esp_http_client_open(dl.http,0);
        length=opened==ESP_OK?esp_http_client_fetch_headers(dl.http):-1;
        if((opened==ESP_OK && length>=0) || !reused)break;
        /* the server closed the kept connection: once more on a new one */
        esp_http_client_close(dl.http);esp_http_client_cleanup(dl.http);reused=false;
        dl.http=esp_http_client_init(&cfg);if(!dl.http) { cells_download_fail("HTTP allocation failed");return false; }
    }
    if(opened!=ESP_OK || length<0) {
        char why[80];snprintf(why,sizeof(why),"Connect failed (%s); partial retained",opened!=ESP_OK?esp_err_to_name(opened):"no reply");
        printf("cells: %s %s, internal free %u\n",why,dl.url,(unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
        cells_download_fail(why);return false;
    }
    int status=esp_http_client_get_status_code(dl.http);
    if(status==200 && local_offset) {
        fclose(dl.out);dl.out=cells_fopen(dl.part,"r+b");dl.offset=dl.segment_start;
        if(dl.out && (ftruncate(fileno(dl.out),(off_t)dl.segment_start) || fseek(dl.out,0,SEEK_END))) { fclose(dl.out);dl.out=NULL; }if(!dl.out) { cells_download_fail("Cannot restart partial");return false; }setvbuf(dl.out,NULL,_IONBF,0);
    } else if(status==206) {
        unsigned long long first,last,total;
        if(sscanf(dl.range,"bytes %llu-%llu/%llu",&first,&last,&total)!=3 || first!=local_offset ||
           last<first || !size || total!=dl.segment_end-dl.segment_start || last>=total) { cells_download_failed("Invalid HTTP Range response",0,true);return false; }
    } else if(status!=200) { cells_download_http_fail(status);return false; }
    if(size && length>0 && (uint64_t)length!=dl.segment_end-dl.offset) { cells_download_failed("Size mismatch; old asset retained",0,true);return false; }
#endif
    dl.active=true;cells_work.downloading=true;cells_work.done=dl.offset;cells_work.total=size;
    cells_message("Downloading; safe to leave TILES");return true;
}
static unsigned cells_download_step(void) {
    if(!dl.active)return 0;
    if(dl.size && dl.offset==dl.size && !dl.out) {
        dl.active=false;cells_retry_reset(dl.relative);
        if(dl.kind==6) { cells_progress_credit(dl.cell_id-1,(uint32_t)dl.size);cw.queue_partial=0; }
        return dl.kind;
    }
    size_t wanted=sizeof(cw.buf);if(dl.size && wanted>dl.segment_end-dl.offset)wanted=(size_t)(dl.segment_end-dl.offset);
    int got=0;
    if(wanted) {
#ifdef LS_CARTOCORE_HOST
        got=(int)fread(cw.buf,1,wanted,dl.in);if(!got && ferror(dl.in))got=-1;
#else
        got=esp_http_client_read(dl.http,(char*)cw.buf,(int)wanted);
#endif
    }
    if(got<0 || (got==0 && dl.size && dl.offset<dl.size)) {
#ifdef LS_CARTOCORE_HOST
        bool complete=got==0 && feof(dl.in);
#else
        bool complete=got==0 && esp_http_client_is_complete_data_received(dl.http);
#endif
        cells_download_failed(complete?"Size mismatch; old asset retained":"Interrupted; partial retained for resume",0,complete);return 0;
    }
    if(got && fwrite(cw.buf,1,(size_t)got,dl.out)!=(size_t)got) { cells_download_fail("SD write failed; partial retained");return 0; }
    dl.offset+=(unsigned)got;if(dl.kind==6)cw.queue_partial=dl.offset;cells_work.done=dl.offset;cells_work.total=dl.size;
    if(!dl.size && dl.offset>(dl.kind==1?8u:1u)*1024u*1024u) { cells_download_failed("Metadata exceeds size limit",0,true);return 0; }
    if(dl.part_bytes && dl.offset==dl.segment_end && dl.offset<dl.size) {
        bool saved=!fflush(dl.out);cells_conn_park();cells_download_close();dl.active=false;
        if(!saved)cells_download_fail("SD write failed; partial retained");
        return 0; /* next queue tick resumes joined .part at the next host part */
    }
    if(got && (!dl.size || dl.offset<dl.size))return 0;
    bool ok=!fflush(dl.out);cells_conn_park();cells_download_close();
    if(!ok) { cells_download_fail("SD write failed; partial retained");return 0; }
    if(dl.size)ok=ok && cells_hash(dl.part,dl.sha,dl.size);
    ok=ok && cells_asset_valid(dl.part,dl.relative);
    if(!ok) { unlink(dl.part);cells_download_failed("SHA256 / format check failed; old asset retained",0,true);return 0; }
    if(!cells_install(dl.part,dl.path)) { cells_download_fail("Atomic install failed; partial retained");return 0; }
    unlink(dl.meta);dl.active=false;cells_work.downloading=false;cells_retry_reset(dl.relative);
    if(dl.kind==6) { cells_progress_credit(dl.cell_id-1,(uint32_t)dl.size);cw.queue_partial=0; }
    /* Rename preserves the verified bytes; bind their digest to the installed file. */
    if(dl.size) {
        struct stat st;char hex[65];for(unsigned i=0;i<32;i++)snprintf(hex+2*i,3,"%02x",dl.sha[i]);
        if(!stat(dl.path,&st) && st.st_size==(off_t)dl.size)(void)carto_digest_write(dl.path,&st,hex);
    }
    cells_message("Installed and verified");return dl.kind;
}
static bool cells_receiving(void) { return dl.active && dl.out; }
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
    DIR *dir=opendir(folder);if(!dir)return;struct dirent *e;int64_t since=esp_timer_get_time();
    while((e=readdir(dir))) {
        cells_pause(&since);
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
    snprintf(cells_work.outlines,sizeof(cells_work.outlines),"missing");cells_catalog();if(!cells_states_parse(CELLS_ROOT "cells/states.json",true) && cw.direct)cw.catalog_refresh=true;cells_library();if(!cells_work.message[0])cells_message(cw.catalog>=0?"Maps ready":"Refresh maps to load states");cells_states_publish();cells_publish();
}
static bool cells_direct_ready(void) { cells_init();return cw.initialized && cells_catalog() && cw.direct; }
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
    bool detail=cells_token_load();int64_t since=esp_timer_get_time();
    if(!cc_places_cells_open(&cw.places,&cw.hits[cw.picked],cw.radius,&it)) { cells_message("Cannot read place shape");return; }
    while((got=cc_places_cells_next(&it,&x,&y))>0) {
        cells_pause(&since);
        if(x>=256 || y>=256) { cells_message("Place grid must be z8");return; }
        uint32_t id=x*256+y;cw.cover[id/8]|=(uint8_t)(1u<<(id&7));cells_work.cells++;
        uint32_t size;uint8_t sha[32];bool known=cells_entry(id+1,&size,sha);bool present=known && cells_present(id+1,size);
        if(!known)cells_work.unavailable++;else if(present)cells_work.installed++;else cells_work.bytes+=size;
        if(detail && cells_entry(id+65537,&size,sha) && !cells_present(id+65537,size))cells_work.bytes+=size;
        if(cells_work.preview_count<LS_CELLS_PREVIEW) {
            unsigned at=cells_work.preview_count++;cells_work.preview[at]=(uint16_t)id;cells_work.present[at]=present?1:known?0:2;
        }
    }
    if(got<0) { cells_message("Place cover read failed");return; }
    if(cw.catalog>=0) {
        if((!cw.direct && !cells_matches(CELLS_ROOT "cells/index.cci",cw.index_size,cw.index_sha)))cells_work.bytes+=cw.index_size;
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
/* A queue that cannot start its next file sleeps until that file's retry
   time (or a TILES command, or a new catalog) instead of reading the catalog
   and the card again every 10 ms: 37 ms of SD work a tick, a quarter of
   core 0, for as long as a state sat stopped. */
static void cells_queue_wait(const char *relative,int64_t otherwise) {
    if(dl.active)return;
    const int64_t now=esp_timer_get_time();const cells_retry *r=relative?cells_retry_find(relative):NULL;
    cw.queue_wait_until=r && r->at>now?r->at:now+otherwise;
}
static void cells_queue_step(void) {
    if(!cw.queue_loaded)return;
    if(esp_timer_get_time()<cw.queue_wait_until) { cells_work.downloading=false;return; }
    bool state_queue=(cw.queue_key&0xffff0000u)==0x53540000u;
    bool want_detail=cells_token_load();
    if(want_detail!=cw.want_detail) { cw.want_detail=want_detail;cw.next_cell=0;cells_progress_reset(); }
    if(state_queue && !cells_states_count) { cw.catalog_refresh=true;cells_message("State metadata required before resuming");return; }
    cells_work.downloading=true;
    if(cw.catalog<0) { if(!cells_download(1,"cells/catalog.ccm","cells/catalog.ccm",0,NULL)) { cells_queue_wait("cells/catalog.ccm",1000000); } return; }
    if((!cw.direct && !cells_matches(CELLS_ROOT "cells/index.cci",cw.index_size,cw.index_sha))) {
        if(!cells_download(4,"cells/index.cci","cells/index.cci",cw.index_size,cw.index_sha)) { cells_queue_wait("cells/index.cci",1000000); } return;
    }
    uint32_t size;uint8_t sha[32];
    if(!cw.progress_loaded) {
        /* A thousand-cell queue's stat() calls alone held the worker for
           seconds: after 200 ms this walk too resumes next turn. It starts
           over when the queue, its detail level or the catalog changes. */
        static unsigned walk_at;static uint32_t walk_revision;
        static uint64_t walk_total,walk_done;
        const unsigned walk_end=cw.want_detail?131072u:65536u;
        if(!walk_at || walk_revision!=cw.progress_revision) {
            walk_at=0;walk_revision=cw.progress_revision;walk_total=walk_done=0;
            memset(cells_credited,0,sizeof(cells_credited));
        }
        cells_message("Checking queued maps");
        const int64_t walk_began=esp_timer_get_time();
        for(;walk_at<walk_end;walk_at++)if(cw.queued[(walk_at%65536)/8]&(1u<<(walk_at&7))) {
            /* Credit cached digests here; the walk below hashes unknown cells. */
            bool error=false;
            if(cells_entry_checked(walk_at+1,&size,sha,&error)) { struct stat st;char name[64],path[96];bool known;
                cells_name(walk_at+1,name,sizeof(name),false);snprintf(path,sizeof(path),CELLS_ROOT "%s",name);
                walk_total+=size;
                if(!stat(path,&st) && st.st_size==(off_t)size && cells_cached_digest(path,&st,sha,&known,NULL)) {
                    walk_done+=size;cells_credited[walk_at/8]|=(uint8_t)(1u<<(walk_at&7));
                }
            } else if(error || walk_at<65536) {
                walk_at=0;
                if(error)cells_message("Cannot count queued maps; retrying");
                else { cells_work.downloading=false;cells_message("Blocked: queued cell absent from catalog");cells_queue_wait(NULL,5000000); }
                return;
            }
            if(esp_timer_get_time()-walk_began>200000 && walk_at+1<walk_end) { walk_at++;return; }
        }
        walk_at=0;cw.queue_total=walk_total;cw.queue_done=walk_done;cw.queue_partial=0;cw.progress_loaded=true;
    }
    if(!cells_entry(0,&size,sha)) { cells_message("Catalog has no overview");cells_queue_wait(NULL,5000000);return; }
    if(!cells_present(0,size)) { if(!cells_download(5,"cells/overview.ctile","cells/overview.ctile",size,sha)) { cells_queue_wait("cells/overview.ctile",1000000); } return; }
    const int64_t began=esp_timer_get_time();
    while(cw.next_cell<(cw.want_detail?131072u:65536u)) {
        unsigned at=cw.next_cell;
        if(!(cw.queued[(at%65536)/8]&(1u<<(at&7)))) { cw.next_cell++;continue; }
        if(at>=65536 && !cells_entry(at+1,&size,sha)) { cw.next_cell++;continue; }
        if(!cells_entry(at+1,&size,sha)) { cells_work.downloading=false;cells_message("Blocked: queued cell absent from catalog");cells_queue_wait(NULL,5000000);return; }
        if(cells_present(at+1,size)) {
            cells_progress_credit(at,size);cw.queue_partial=0;
            /* A cell not hashed since the last catalog can take seconds; after
               200 ms the walk resumes next turn so MAP gets the worker. */
            cw.next_cell++;if(esp_timer_get_time()-began>200000)return;continue;
        }
        char remote[64],local[64];cells_name(at+1,remote,sizeof(remote),true);cells_name(at+1,local,sizeof(local),false);
        cw.next_cell=at;if(!cells_download(6,remote,local,size,sha)) { cells_queue_wait(local,1000000); } return;
    }
    /* Recheck the entire ownership set before making the completion durable. */
    int64_t since=esp_timer_get_time();
    for(unsigned at=0;at<(cw.want_detail?131072u:65536u);at++)if(cw.queued[(at%65536)/8]&(1u<<(at&7))) {
        cells_pause(&since);
        if(at>=65536 && !cells_entry(at+1,&size,sha))continue;
        if(!cells_entry(at+1,&size,sha) || !cells_present(at+1,size)) { cw.next_cell=at;cells_work.downloading=false;cells_message("Blocked: queued cells need revalidation");cells_queue_wait(NULL,5000000);return; }
    }
    if(!cells_record(cw.queue_key,cw.queue_name,cw.queued,false)) { cells_message("Cannot save place installation");return; }
    cw.queue_loaded=false;cells_work.downloading=false;cells_library();
    if(!sd_map || !sd_map->set)select_map("cells");
    cells_states_publish();
    snprintf(saved.path,sizeof(saved.path),CELLS_ROOT "cells/%s",cw.direct?"catalog.ccm":"index.cci");save_view();
    cells_message(state_queue?"State installed":"Place installed");if(cw.preview_active)cells_preview();
}

static void cells_metadata_activate(const char *relative) {
    if(!strcmp(relative,"cells/states.json")) {
        cw.states_refresh=!cells_states_parse(CELLS_ROOT "cells/states.json",true);
        cells_states_publish();
    }
    if(!strcmp(relative,"outlines.lso")) {
        cells_outlines_pending=false;snprintf(cells_work.outlines,sizeof(cells_work.outlines),"ready");
    }
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
        cells_metadata_activate(upload.relative);cells_states_publish();cells_publish();
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
    cells_conn_expire();
    if(upload.active && esp_timer_get_time()-upload.last_us>60000000)upload.active=false;
    if(!cw.initialized || upload.active)return;
    bool command=false;
    if(cells_take()) {
        cells_request=cells_mail;
        if(cells_action_count && !((cells_actions[cells_action_head].action==2 || cells_actions[cells_action_head].action==8) && dl.active)) { cells_command_work=cells_actions[cells_action_head];cells_action_head=(cells_action_head+1)%CELLS_ACTIONS;cells_action_count--;command=true; }
        cells_give();
    }
    if(command) {
        cells_command *q=&cells_command_work;
        if(q->action==1 || q->action==3 || q->action==4 || q->action==7)cells_retry_reset(NULL);
        if(q->action==1) {
            bool known=false;for(unsigned i=0;i<cells_work.library_count;i++)if(cells_work.keys[i]==q->key)known=true;
            if(!known && cells_work.library_count>=LS_CELLS_PLACES)cells_message("32 places limit; delete a place first");
            else if(cells_record(q->key,q->name,q->cover,true)) {
                if(cw.queue_loaded && cw.queue_key==q->key) { memcpy(cw.queued,q->cover,CELLS_BITS);memcpy(cw.queue_name,q->name,64);cw.next_cell=0;cells_progress_reset(); }
                cells_library();cells_message("Queued; safe to leave TILES");
            }
            else cells_message("Cannot save download queue; retry");
        } else if(q->action==2) { cells_work.ready=false;cw.preview_active=false;if(!dl.active) { if(!cells_delete(q->key))cells_message("Delete failed; retry"); }else cells_message("Wait for the current cell before deleting"); }
        else if(q->action==3) {
            char name[64];bool queued;snprintf(cw.path,sizeof(cw.path),CELLS_ROOT "installed/%08lx.ccpi",(unsigned long)q->key);
            if(!cells_record_read(cw.path,cw.scratch,name,&queued) || !cells_record(q->key,name,cw.scratch,true))cells_message("Cannot save update queue; retry");
            else { cells_library();cw.catalog_refresh=true; }
        } else if(q->action==4) { cw.catalog_refresh=true;cells_message("Refreshing map catalog"); }
        else if(q->action==7 || q->action==8)cells_state_command(q->action,q->key);
        cells_work.action_done++;cells_publish();return; /* Publish the acknowledgement before consuming queries. */
    }
    if(cells_request.serial!=cw.serial) {
        cw.serial=cells_request.serial;
        if(cells_request.action==6 && cells_url_valid(cells_request.url)) {
            cells_retry_reset(NULL);
            snprintf(cw.server,sizeof(cw.server),"%s",cells_request.url);snprintf(cells_work.server,sizeof(cells_work.server),"%s",cw.server);cells_config_save();cw.catalog_refresh=true;
        } else if(cells_request.action==0) {
            cw.preview_active=false;cells_work.picked.name[0]=0;
            snprintf(cells_work.query,sizeof(cells_work.query),"%s",cells_request.query);
            cells_work.ready=false;cw.hint_attempts=0;cw.query_pending=strlen(cells_work.query)>=2;cells_work.busy=cw.query_pending;cells_work.count=0;
        } else if(cells_request.action==5) { cw.preview_active=true;cw.picked=cells_request.pick;cw.radius=cells_request.radius;cells_preview(); }
    }
    if(dl.active) {
        if(dl.kind==6 && strstr(dl.relative,".detail.ctile") && !cells_token_load()) {
            cells_download_fail("LakeShark Plus token required");cells_publish();return;
        }
        /* Keep reading for up to 100 ms of a turn while the file streams:
           one 64 KB read a turn left the link idle between turns. */
        unsigned done;const int64_t turn=esp_timer_get_time();
        do done=cells_download_step();
        while(!done && dl.active && dl.out && esp_timer_get_time()-turn<100000);
        if(done==1) { cw.catalog_refresh=false;cells_retry_reset(NULL);cells_catalog();if(cw.direct) { cw.states_refresh=true;cells_download(7,"cells/states.json","cells/states.json",0,NULL); }if(cw.preview_active)cells_preview(); }
        if(done==7 || done==8)cells_metadata_activate(dl.relative);
        if(done==2) { cw.base_open=false;cw.query_pending=true; }
        if(done==3) { cells_attach(cw.shard.iso2);if(cw.preview_active)cells_preview();else cw.query_pending=true; }
    } else if(cw.catalog_refresh)cells_download(1,"cells/catalog.ccm","cells/catalog.ccm",0,NULL);
    else if(cw.states_refresh)cells_download(7,"cells/states.json","cells/states.json",0,NULL);
    else if(cells_outlines_pending && cells_retry_ready("outlines.lso")) {
        if(!cells_outlines_needed()) {
            cells_outlines_pending=false;
            snprintf(cells_work.outlines,sizeof(cells_work.outlines),"%s",cells_outlines.bytes?"ready":"missing");
        } else if(cells_download(8,"places/outlines.lso","outlines.lso",cells_outlines.bytes,cells_outlines.sha))
            snprintf(cells_work.outlines,sizeof(cells_work.outlines),"downloading");
        else if(cells_retry_ready("outlines.lso"))cells_download_fail("error: cannot start outline download");
    }
    else if(cw.query_pending)cells_search();
    else if(cw.preview_active && cw.catalog<0)cells_preview();
    else if(cw.queue_loaded)cells_queue_step();
    cells_states_publish();cells_publish();
}
static void cells_service(void) {
    if(xSemaphoreTake(command_lock,0)!=pdTRUE)return;
    cells_step();
    if(source_refreshing && atomic_load(&view_active) && !atomic_load(&dismiss_pending)) {
        if(!select_map_mode("cells",false))show(saved.z);
    }
    xSemaphoreGive(command_lock);
}
