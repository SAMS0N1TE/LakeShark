/* Explicit host fixtures exercise the same screen; no network or SD writes. */
#include "ls_tiles.h"
#include "esp_timer.h"
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
static ls_tiles_catalog catalog;
static ls_carto_transfer transfer;
static bool seeded;
bool ls_tiles_refresh(bool online) {
    (void)online; if(seeded) return true; seeded=true;
    catalog=(ls_tiles_catalog){.count=4,.sd=true,.total=32000000000ULL,.free=24300000000ULL,.maps=47000000};
    strcpy(catalog.message,"Catalog refreshed");
    const char *names[]={"Central New Hampshire","Cape Cod","White Mountains","Coastal Maine"};
    const int states[]={1,0,2,3};
    for(int i=0;i<4;i++) {
        ls_tile_region *r=&catalog.region[i]; snprintf(r->name,sizeof(r->name),"%s",names[i]);
        snprintf(r->file,sizeof(r->file),"region_%d.ctile",i); r->bytes=18000000+i*3500000;
        r->local_bytes=states[i]==1 || states[i]==3?r->bytes:0;
        r->status=states[i]; r->zmin=8; r->zmax=16; r->catalog=true;
        r->verified=i==0;
        r->bbox[0]=-72.1; r->bbox[1]=43.1; r->bbox[2]=-71.2; r->bbox[3]=44.2;
        strcpy(r->description,"Roads, water, trails and place labels for offline field navigation.");
        memset(r->sha256,'a'+i,64); r->sha256[64]=0;
    }
    const char *s=getenv("LSSIM_TILES");
    if(s && (!strncmp(s,"download",8) || !strcmp(s,"receive") || !strcmp(s,"verify"))) {
        int p=!strncmp(s,"download",8)?atoi(s+8):0;
        transfer=(ls_carto_transfer){.total=!strcmp(s,"receive")?0:18000000,.bytes=180000ULL*p,
            .state=p==100?2:1,.start_us=esp_timer_get_time()-5000000,.receiving=!strcmp(s,"receive"),.port=8080,.code="0123456789abcdef0123456789abcdef"};
        strcpy(transfer.file,!strcmp(s,"receive")?"Laptop push":"region_0.ctile");
        if(p==100) strcpy(transfer.message,"Saved and verified");
        if(!strcmp(s,"verify")) {
            transfer.verifying=true; transfer.total=23732294; transfer.bytes=12000000;
            strcpy(transfer.file,"region_0.ctile");
            strcpy(transfer.message,"Verifying SHA256; BACK to cancel");
        }
    }
    return true;
}
bool ls_tiles_snapshot(ls_tiles_catalog *out) { *out=catalog; return true; }
bool ls_tiles_action(int action,const ls_tile_region *r) {
    (void)r;
    if(action==0 || action==1 || action==4) { transfer.state=1; transfer.total=action==1?0:18000000; transfer.bytes=0; transfer.start_us=esp_timer_get_time(); transfer.verifying=action==4; }
    if(action==3) catalog.region[0].status=0;
    return true;
}
void ls_carto_transfer_snapshot(ls_carto_transfer *out) {
    static int snapshots;
    const char *state=getenv("LSSIM_TILES");
    if(state && !strncmp(state,"external-",9) && snapshots++==1) {
        transfer=(ls_carto_transfer){.state=1,.start_us=esp_timer_get_time()+1,.total=18000000,.bytes=6300000,
                                   .receiving=!strcmp(state,"external-receive"),.port=8080,.has_bbox=true,.code="0123456789abcdef0123456789abcdef"};
        memcpy(transfer.bbox,catalog.region[0].bbox,sizeof(transfer.bbox));
        strcpy(transfer.file,"console_nh.ctile");
    }
    *out=transfer;
}
void ls_carto_transfer_cancel(void) { transfer.state=3; strcpy(transfer.message,"Cancelled; partial retained"); }
void ls_tiles_address(char *out,size_t size) { snprintf(out,size,"192.168.1.42"); }
void ls_tiles_host_page(int *page) {
    const char *s=getenv("LSSIM_TILES");
    if(s && !strcmp(s,"info")) *page=1;
    else if(s && (!strncmp(s,"download",8) || !strcmp(s,"receive") || !strcmp(s,"verify"))) *page=2;
}

#ifndef LS_CARTOCORE_HOST
void ls_carto_active_file(char *out,size_t size) { snprintf(out,size,"region_0.ctile"); }
#endif
