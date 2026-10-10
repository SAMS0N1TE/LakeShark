#include "../../ls_cells.h"
/* TILES: a region library and live transfers, with no I/O in draw. */
#include "../../ls_tiles.h"
#include "../../ls_tui_ui.h"
#include "../../ls_options.h"
#include "../../ls_picker.h"
#include "../../ls_glyph.h"
#include "../../ls_text.h"
#include "../../ls_map.h"
#include "esp_timer.h"
#include "esp_attr.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

static EXT_RAM_BSS_ATTR ls_tiles_catalog catalog;
static ls_carto_transfer transfer;
static int selected, first, page, focus=-1, slot, confirm, speed=4;
static int info_scroll,info_max;
static bool refreshed;
static int64_t turn_start,seen_transfer;
static char active_file[128];
static tui_rect list_area,content_area;
static int visible;
static const char *const STATES[]={"NOT INSTALLED","INSTALLED","PARTIAL","UPDATE AVAILABLE"};
#define INK TUI_ATTR(TUI_CYAN|TUI_BRIGHT,TUI_BLACK)
#define GOOD TUI_ATTR(TUI_GREEN|TUI_BRIGHT,TUI_BLACK)

/* 5-degree land cells, a 324-byte mask built from coarse original polygons. */
#include "../../ls_tiles_world.h"
typedef struct { int16_t lon,lat; uint8_t shade,valid; } sphere_cell;
static EXT_RAM_BSS_ATTR sphere_cell sphere[60*24];
static int sphere_w,sphere_h;
static void sphere_prepare(int w,int h) {
    if(w==sphere_w && h==sphere_h) return;
    sphere_w=w; sphere_h=h;
    for(int y=0;y<h;y++) for(int x=0;x<w;x++) {
        sphere_cell *p=&sphere[y*w+x]; float nx=(x+.5f-w*.5f)/(w*.5f),ny=(h*.5f-y-.5f)/(h*.5f);
        float r=nx*nx+ny*ny; p->valid=r<1;
        if(!p->valid) continue;
        float z=sqrtf(1-r);
        p->lat=(int16_t)lroundf(asinf(ny)*180/3.14159265f);
        p->lon=(int16_t)lroundf(atan2f(nx,z)*180/3.14159265f);
        p->shade=(uint8_t)(1+4*fmaxf(0,(z*.8f-nx*.4f+ny*.2f)));
        if(p->shade>5) p->shade=5;
    }
}
static bool search_globe;
static double search_lat,search_lon;
static void globe(tui_surface *sf,tui_rect a,float progress) {
    int h=a.h<24?a.h:24,w=h*2; if(w>a.w-2) { w=a.w-2; h=w/2; }
    if(h<4) return;
    sphere_prepare(w,h); int left=a.x+(a.w-w)/2,top=a.y+(a.h-h)/2;
    ls_tile_region incoming={0};
    ls_tile_region *r=catalog.count?&catalog.region[selected]:NULL;
    if(page==2) {
        r=NULL;
        if(!transfer.receiving) for(int i=0;i<catalog.count;i++)
            if(!strcmp(catalog.region[i].file,transfer.file)) r=&catalog.region[i];
    }
    if(page==2 && transfer.has_bbox) {
        memcpy(incoming.bbox,transfer.bbox,sizeof(incoming.bbox)); r=&incoming;
    }
    if(search_globe) { incoming.bbox[0]=search_lon-2;incoming.bbox[2]=search_lon+2;incoming.bbox[1]=search_lat-2;incoming.bbox[3]=search_lat+2;r=&incoming; }
    int centre=r?(int)((r->bbox[0]+r->bbox[2])*.5):0;
    int offset=centre+(int)(((esp_timer_get_time()-turn_start)/80000*speed)%360);
    static const char ramp[]=" .:-=+#";
    for(int y=0;y<h;y++) for(int x=0;x<w;x++) {
        sphere_cell *p=&sphere[y*w+x]; if(!p->valid) continue;
        int lon=(p->lon+offset+540)%360-180,lat=p->lat;
        int mx=(lon+180)/5,my=(90-lat)/5; if(my>35) my=35;
        bool land=(tiles_world[my][mx/8]>>(mx%8))&1;
        bool region=r && lon>=r->bbox[0]-3 && lon<=r->bbox[2]+3 && lat>=r->bbox[1]-3 && lat<=r->bbox[3]+3;
        char glyph=land?ramp[p->shade+1]:ramp[p->shade>3?2:1];
        uint8_t at=land?INK:LS_ATTR_FAINT;
        if(region) { glyph=".*+#@"[(int)(progress*4)]; at=progress>0?GOOD:LS_ATTR_DIM; }
        tui_put_char(sf,a,left+x,top+y,glyph,at);
    }
}
void ls_tiles_search_globe(tui_surface *sf,tui_rect a,float progress,double lat,double lon) {
    search_globe=true;search_lat=lat;search_lon=lon;globe(sf,a,progress);search_globe=false;
}
static void home(void) {
    const ls_app_t *a=ls_app_by_id("home"); if(a) ls_tui_screen_show(ls_tui_screen_index_of(a->screen));
}
static void back(void) {
    if(page==2 && transfer.state==1) { confirm=1; page=3; }
    else if(page==3) { page=confirm==1?2:0; confirm=0; }
    else if(page) page=0;
    else home();
}
static void map_open(void) {
    if(!catalog.count) return;
    if(ls_tiles_action(2,&catalog.region[selected])) {
        const ls_app_t *a=ls_app_by_id("map"); if(a) ls_tui_screen_show(ls_tui_screen_index_of(a->screen));
    }
}
static void action(const ls_opt_t *o) {
    if(catalog.busy) return;
    switch(o->arg) {
    case 0: if(catalog.count && ls_tiles_action(0,&catalog.region[selected])) { page=2; turn_start=esp_timer_get_time(); refreshed=false; transfer=(ls_carto_transfer){0}; } break;
    case 1: if(ls_tiles_action(1,NULL)) { page=2; turn_start=esp_timer_get_time(); refreshed=false; transfer=(ls_carto_transfer){0}; } break;
    case 2: map_open(); break;
    case 3: if(catalog.count) { page=3; confirm=2; } break;
    case 4: page=1; info_scroll=0; break;
    case 5: ls_tiles_refresh(true); break;
    case 6: if(catalog.count) {
        ls_tile_region *r=&catalog.region[selected];
        ls_map_center((r->bbox[1]+r->bbox[3])*.5,(r->bbox[0]+r->bbox[2])*.5);
        const ls_app_t *a=ls_app_by_id("map"); if(a) ls_tui_screen_show(ls_tui_screen_index_of(a->screen));
    } break;
    case 7: if(catalog.count && ls_tiles_action(4,&catalog.region[selected])) {
        page=2; turn_start=esp_timer_get_time(); refreshed=false; transfer=(ls_carto_transfer){0};
    } break;
    }
}
static const char *available(const ls_opt_t *o) {
    if(catalog.busy) return "Working";
    if(transfer.state==1 && (o->arg==0 || o->arg==1 || o->arg==3 || o->arg==7)) return "Transfer active";
    if(o->arg==1 || o->arg==5) return NULL;
    if(!catalog.count) return "No region selected";
    if(o->arg==0 && !catalog.region[selected].catalog) return "Refresh catalog to download";
    if(o->arg==2 && !catalog.region[selected].local_bytes) return "Install first";
    if(o->arg==3 && !catalog.region[selected].status) return "Not installed";
    if(o->arg==7 && !catalog.region[selected].local_bytes) return "Install first";
    if(o->arg==7 && !catalog.region[selected].sha256[0]) return "Refresh catalog for SHA256";
    return NULL;
}
static double speed_get(const ls_opt_t *o) { (void)o; return speed; }
static void speed_set(const ls_opt_t *o,double v) { (void)o; speed=(int)v; }
static const ls_opt_t region_opts[]={
    {.label="OPEN SELECTED MAP",.kind=LS_OPT_ACTION,.arg=2,.act=action,.why_not=available,.leaves=true},
    {.label="SHOW REGION LOCATION",.kind=LS_OPT_ACTION,.arg=6,.act=action,.why_not=available,.leaves=true},
    {.label="DOWNLOAD / RE-DOWNLOAD",.kind=LS_OPT_ACTION,.arg=0,.act=action,.why_not=available,.leaves=true},
    {.label="VERIFY",.kind=LS_OPT_ACTION,.arg=7,.act=action,.why_not=available,.leaves=true},
    {.label="DELETE",.kind=LS_OPT_ACTION,.arg=3,.act=action,.why_not=available,.leaves=true},
    {.label="INFO",.kind=LS_OPT_ACTION,.arg=4,.act=action,.why_not=available,.leaves=true}
};
static const ls_opt_ctx_t region_ctx={.name="REGION",.job=-1,.radio=LS_RSEL_NONE,LS_OPT_ROWS(region_opts)};
static void search_action(const ls_opt_t *arg) { if(arg->arg)ls_place_search_progress();else ls_place_search_open(false); }
static const ls_opt_t options[]={
    {.label="SELECTED REGION",.kind=LS_OPT_MENU,.sub=&region_ctx},
    {.label="REFRESH CATALOG",.kind=LS_OPT_ACTION,.arg=5,.act=action,.why_not=available,.leaves=true},
    {.label="RECEIVE FROM LAPTOP",.kind=LS_OPT_ACTION,.arg=1,.act=action,.why_not=available,.leaves=true},
    {.label="GLOBE TURN SPEED",.kind=LS_OPT_LEVEL,.num=speed_get,.set_num=speed_set,.lo=1,.hi=6,.step=1,.unit="degrees per frame"},
    {.label="SEARCH PLACES",.kind=LS_OPT_ACTION,.act=search_action,.leaves=true},
    {.label="DOWNLOAD PROGRESS",.kind=LS_OPT_ACTION,.act=search_action,.arg=1,.leaves=true},
};
static const ls_opt_ctx_t ctx={.name="TILES",.job=-1,.radio=LS_RSEL_NONE,.back_to_screen=true,LS_OPT_ROWS(options)};
static void enter(void) {
    focus=-1; page=0; refreshed=false; turn_start=esp_timer_get_time();
    ls_tiles_refresh(true);
    ls_carto_transfer_snapshot(&transfer); seen_transfer=transfer.start_us; if(transfer.state==1) page=2;
#ifdef LS_TILES_HOST
    extern void ls_tiles_host_page(int *); ls_tiles_host_page(&page);
#endif
}
static void line(tui_surface *sf,tui_rect a,int y,const char *s,uint8_t at) { ls_safe_line(sf,a,y,s,at); }
static void draw(tui_surface *sf,tui_rect a) {
    if(ls_place_search_active()) { ls_place_search_draw(sf,a);return; }
    bool had_bbox=transfer.has_bbox;
    ls_tiles_snapshot(&catalog); ls_carto_transfer_snapshot(&transfer);
    ls_carto_active_file(active_file,sizeof(active_file));
    if(transfer.state==1 && transfer.start_us!=seen_transfer) {
        seen_transfer=transfer.start_us; page=2; focus=-1; refreshed=false; turn_start=esp_timer_get_time();
    }
    if(transfer.receiving && transfer.has_bbox && !had_bbox) turn_start=esp_timer_get_time();
    if(selected>=catalog.count) selected=catalog.count?catalog.count-1:0;
    if(page==2 && transfer.state>1 && !catalog.busy && !refreshed) { refreshed=ls_tiles_refresh(false); }
    if(page==2 && transfer.state==2 && refreshed && !catalog.busy) {
        for(int i=0;i<catalog.count;i++) if(!strcmp(catalog.region[i].file,transfer.file)) selected=i;
    }
    if(page==2 && transfer.state==0 && !catalog.busy) page=0;
    ls_panel_box(sf,a,page==1?"TILES / REGION INFO":page==2?transfer.verifying?"TILES / VERIFY":transfer.receiving?"TILES / RECEIVE":"TILES / TRANSFER":page==3?"TILES / CONFIRM":"TILES / MAP REGIONS",TUI_CYAN);
    ls_btn_t btn[4]={{.label="BACK",.key='b'},{.label="OPTIONS",.value="REGIONS",.key='o'},
        {.label="OPEN",.value="SELECTED MAP",.key='a',.dim=catalog.busy || !catalog.count || !catalog.region[selected].local_bytes},{.label="SEARCH",.value="PLACES",.key='s'}};
    int nb=4;
    if(page==1) { btn[1]=(ls_btn_t){.label="OPEN",.value="SELECTED MAP",.key='a',.dim=!catalog.count || !catalog.region[selected].local_bytes}; btn[2]=(ls_btn_t){.label="OPTIONS",.value="REGION",.key='o'}; nb=3; }
    if(page==2) { nb=1; btn[0].label="BACK";
        if(transfer.state==2) { btn[1]=(ls_btn_t){.label="OPEN",.value="SELECTED MAP",.key='a',.dim=catalog.busy}; nb=2; } }
    if(page==3) { btn[1]=(ls_btn_t){.label="CONFIRM",.value=confirm==1?"CANCEL":"DELETE",.key='y'}; nb=2; }
    int bh=ls_btn_raised_height(a,nb); tui_rect bar=tui_rect_make(a.x+1,a.y+a.h-bh-1,a.w-2,bh);
    tui_rect body=tui_rect_make(a.x+1,a.y+2,a.w-2,a.h-bh-4);
    content_area=body;
    if(page==0) {
        line(sf,body,body.y,catalog.busy?"Loading catalog / checking SD...":catalog.message,LS_ATTR_DIM);
        tui_rect storage=tui_rect_make(body.x,body.y+body.h-5,body.w,5);
        list_area=tui_rect_make(body.x,body.y+2,body.w,body.h-8); visible=list_area.h/4;
        if(body.w>90) list_area.w=body.w/2;
        if(body.w<=90 && catalog.count*4+10<list_area.h) list_area.h=catalog.count*4;
        visible=list_area.h/4;
        if(visible<1) visible=1;
        if(selected<first) first=selected;
        if(selected>=first+visible) first=selected-visible+1;
        char paging[96]; snprintf(paging,sizeof(paging),"< PREV   %d / %d REGIONS   NEXT >",catalog.count?selected+1:0,catalog.count);
        line(sf,body,body.y+1,paging,INK);
        for(int i=first;i<catalog.count && i<first+visible;i++) {
            ls_tile_region *r=&catalog.region[i]; int y=list_area.y+(i-first)*4;
            tui_rect row=tui_rect_make(list_area.x,y,list_area.w,4);
            if(i==selected) ls_fill_dither(sf,row,LS_DITHER_LIGHT,TUI_CYAN);
            char text[200]; snprintf(text,sizeof(text),"%s%s%s",i==selected?"> ":"  ",r->name,!strcmp(r->file,active_file)?" [ACTIVE]":""); line(sf,row,y,text,INK);
            snprintf(text,sizeof(text),"  %s%s  |  %.2f MB  z%d-%d",STATES[r->status],r->verified?" verified":"",r->bytes/1e6,r->zmin,r->zmax); line(sf,row,y+1,text,r->status==1?GOOD:LS_ATTR_DIM);
            snprintf(text,sizeof(text),"  %.2f,%.2f to %.2f,%.2f",r->bbox[0],r->bbox[1],r->bbox[2],r->bbox[3]); line(sf,row,y+2,text,LS_ATTR_DIM);
        }
        if(!catalog.count) line(sf,list_area,list_area.y+2,catalog.sd?"No regions. REFRESH or RECEIVE to add maps.":"No SD card. Insert a card to install maps.",LS_ATTR_DIM);
        if(catalog.count && (body.w>90 || storage.y-list_area.y-list_area.h>=8)) {
            tui_rect preview=body.w>90?
                tui_rect_make(body.x+body.w/2+2,list_area.y,body.w-body.w/2-3,list_area.h):
                tui_rect_make(body.x,list_area.y+list_area.h+1,body.w,storage.y-list_area.y-list_area.h-1);
            ls_panel_box(sf,preview,"SELECTED REGION",TUI_CYAN);
            line(sf,preview,preview.y+1,catalog.region[selected].name,INK);
            globe(sf,tui_rect_make(preview.x+1,preview.y+2,preview.w-2,preview.h-4),catalog.region[selected].status==1?1:0);
            line(sf,preview,preview.y+preview.h-1,STATES[catalog.region[selected].status],LS_ATTR_DIM);
        }
        char text[160]; snprintf(text,sizeof(text),"SD %.2f GB free / %.2f GB used",catalog.free/1e9,(catalog.total-catalog.free)/1e9);
        line(sf,storage,storage.y,catalog.sd?text:"SD unavailable",LS_ATTR_DIM);
        ls_bar(sf,storage,1,storage.x+1,storage.w-2,catalog.total?(float)(catalog.total-catalog.free)/catalog.total:0);
        snprintf(text,sizeof(text),"MAPS %.2f MB  |  %d regions",catalog.maps/1e6,catalog.count); line(sf,storage,storage.y+3,text,INK);
    } else if(page==1 && catalog.count) {
        ls_tile_region *r=&catalog.region[selected]; int y=body.y-info_scroll; char text[200];
        line(sf,body,y++,r->name,INK); line(sf,body,y++,STATES[r->status],r->status==1?GOOD:LS_ATTR_DIM); y++;
        line(sf,body,y++,r->verified?"SHA256 verified":"SHA256 unverified",r->verified?GOOD:LS_ATTR_DIM);
        line(sf,body,y++,r->file,LS_ATTR_DIM);
        snprintf(text,sizeof(text),"Size %.3f MB  |  on SD %.3f MB",r->bytes/1e6,r->local_bytes/1e6); line(sf,body,y++,text,LS_ATTR_DIM);
        if(r->partial_bytes) { snprintf(text,sizeof(text),"Partial download %.3f MB",r->partial_bytes/1e6); line(sf,body,y++,text,LS_ATTR_DIM); }
        snprintf(text,sizeof(text),"Zoom %d to %d",r->zmin,r->zmax); line(sf,body,y++,text,INK); y++;
        snprintf(text,sizeof(text),"West / East  %.5f / %.5f",r->bbox[0],r->bbox[2]); line(sf,body,y++,text,LS_ATTR_DIM);
        snprintf(text,sizeof(text),"South / North  %.5f / %.5f",r->bbox[1],r->bbox[3]); line(sf,body,y++,text,LS_ATTR_DIM); y++;
        char wrapped[6][128]; int n=ls_wrap_text(r->description,body.w-4,(char *)wrapped,sizeof(wrapped[0]),6);
        for(int i=0;i<n;i++) line(sf,body,y++,wrapped[i],LS_ATTR_DIM);
        y++;
        line(sf,body,y++,r->sha256[0]?"SHA256":"SHA256 not supplied",INK);
        int chunk=body.w-4; if(chunk>32) chunk=32; if(chunk<1) chunk=1;
        for(int i=0;r->sha256[i];i+=chunk) { snprintf(text,sizeof(text),"%.*s",chunk,r->sha256+i); line(sf,body,y++,text,LS_ATTR_DIM); }
        info_max=y+info_scroll-body.y-body.h; if(info_max<0) info_max=0;
    } else if(page==2) {
        float progress=transfer.total?(float)transfer.bytes/transfer.total:0;
        if(progress>1) progress=1;
        line(sf,body,body.y,transfer.file[0]?transfer.file:catalog.count?catalog.region[selected].name:"Starting transfer...",INK);
        tui_rect art=body,details=body;
        if(body.w>90) { art.w=body.w/2; art.y+=2; art.h-=3; details.x+=art.w; details.w-=art.w; details.y+=3; details.h-=3; }
        else { art.y+=2; art.h=body.h-16; if(art.h>25) art.h=25; details.y=art.y+art.h+1; details.h=body.y+body.h-details.y; }
        globe(sf,art,progress);
        char percent[8];
        if(transfer.total) snprintf(percent,sizeof(percent),"%d",(int)(progress*100));
        else snprintf(percent,sizeof(percent),"%s",transfer.state==3?"ERROR":transfer.receiving?"READY":"START");
        int len=strlen(percent),gx=details.x+(details.w-(len*6+3))/2,gy=details.y;
        for(int i=0;i<len;i++) ls_glyph_draw(sf,tui_rect_make(gx+i*6,gy,5,7),percent[i],transfer.state==3?TUI_ATTR(TUI_RED|TUI_BRIGHT,TUI_BLACK):GOOD);
        if(transfer.total) tui_put_char(sf,details,gx+len*6,gy+5,'%',GOOD);
        int y=gy+8; double seconds=(esp_timer_get_time()-transfer.start_us)/1e6;
        double rate=seconds>0?transfer.bytes/seconds:0; char text[160];
        if(transfer.total) snprintf(text,sizeof(text),"%.2f / %.2f MB",transfer.bytes/1e6,transfer.total/1e6);
        else snprintf(text,sizeof(text),"%s",transfer.receiving?"Waiting for laptop push":"Preparing transfer");
        line(sf,details,y++,text,LS_ATTR_DIM);
        if(transfer.total && rate>0) snprintf(text,sizeof(text),"%.2f MB/s  |  ETA %.0f s",rate/1e6,(transfer.total-transfer.bytes)/rate);
        else snprintf(text,sizeof(text),"Waiting for bytes...");
        line(sf,details,y++,text,INK);
        snprintf(text,sizeof(text),"%llu bytes",(unsigned long long)transfer.bytes); line(sf,details,y++,text,LS_ATTR_DIM);
        line(sf,details,y++,transfer.message[0]?transfer.message:"BACK to cancel (confirmation)",transfer.state==3?TUI_ATTR(TUI_RED|TUI_BRIGHT,TUI_BLACK):LS_ATTR_DIM);
        if(transfer.total && y<details.y+details.h) {
            int w=details.w-2,lit=(int)(progress*w);
            for(int i=0;i<w;i++) tui_put_char(sf,details,details.x+1+i,y,i<lit?'#':'.',i<lit?GOOD:LS_ATTR_FAINT);
            y++;
        }
        if(transfer.receiving) {
            extern void ls_tiles_address(char *,size_t); char ip[40]; ls_tiles_address(ip,sizeof(ip));
            snprintf(text,sizeof(text),"http://%s:%d/maps/NAME.ctile",ip,transfer.port); line(sf,details,y++,text,INK);
            snprintf(text,sizeof(text),"Code: %s",transfer.code); line(sf,details,y++,text,INK);
            line(sf,details,y++,"PUT one .ctile file; expires after 5 min",LS_ATTR_DIM);
        }
    } else if(page==3) {
        line(sf,body,body.y+3,confirm==1?"Cancel this transfer?":"Delete this region and its partial file?",INK);
        line(sf,body,body.y+5,confirm==1?"Cancelled transfers remove their partial file.":"This removes the files from the SD card.",LS_ATTR_DIM);
        if(catalog.count) line(sf,body,body.y+7,catalog.region[selected].name,LS_ATTR_DIM);
    }
    ls_btn_bar_raised(sf,bar,btn,nb,focus);
}
static bool key(ls_tk_t k,char ch) {
    if(ls_place_search_active())return ls_place_search_key(k,ch);
    if(k==LS_TK_CHAR && ch=='s') { ls_place_search_open(false);return true; }
    if(k==LS_TK_ESC || k==LS_TK_BACKSPACE || (k==LS_TK_CHAR && (ch=='b' || ch=='B'))) { back(); return true; }
    if(k==LS_TK_CHAR) {
        if(ch=='o' || ch=='O') { ls_opt_open(page==1?&region_ctx:&ctx); return true; }
        if(ch=='r' || ch=='R') { ls_tiles_refresh(true); return true; }
        if(ch=='v' || ch=='V') { action(&(ls_opt_t){.arg=1}); return true; }
        if(ch=='a' || ch=='A') { if(!catalog.busy && catalog.count && catalog.region[selected].local_bytes) map_open(); return true; }
        if((ch=='y' || ch=='Y') && page==3) {
            if(confirm==1) { ls_carto_transfer_cancel(); page=2; }
            else { ls_tiles_action(3,&catalog.region[selected]); page=0; }
            confirm=0; return true;
        }
    }
    if(page==0 && (k==LS_TK_UP || k==LS_TK_DOWN) && focus<0) {
        selected+=k==LS_TK_UP?-1:1; if(selected<0) selected=0; if(selected>=catalog.count) selected=catalog.count?catalog.count-1:0; return true;
    }
    if(page==1 && (k==LS_TK_UP || k==LS_TK_DOWN) && focus<0) {
        info_scroll+=k==LS_TK_UP?-1:1;
        if(info_scroll<0) info_scroll=0;
        if(info_scroll>info_max) info_scroll=info_max;
        return true;
    }
    if(k==LS_TK_TAB) { focus=focus<0?0:-1; return true; }
    if(focus>=0 && ls_btn_navigate(k,&slot,&focus,false)) return true;
    if(k==LS_TK_ENTER) {
        if(focus>=0) { const char *keys=page==0?"boas":page==1?"bao":page==3?"by":"ba"; if(focus<(int)strlen(keys)) return key(LS_TK_CHAR,keys[focus]); }
        else if(page==0 && catalog.count) { page=1; info_scroll=0; }
        return true;
    }
    return false;
}
static bool touch(int col,int row) {
    if(ls_place_search_active())return ls_place_search_touch(col,row);
    int b=ls_btn_hit(col,row);
    if(b>=0) { const char *keys=page==0?"boas":page==1?"bao":page==3?"by":"ba"; if(b<(int)strlen(keys)) key(LS_TK_CHAR,keys[b]); return true; }
    if(page==0 && row==list_area.y-1) {
        selected+=col<list_area.x+list_area.w/2?-1:1;
        if(selected<0) selected=0;
        if(selected>=catalog.count) selected=catalog.count?catalog.count-1:0;
        return true;
    }
    if(page==0 && tui_rect_contains(list_area,col,row)) {
        int i=first+(row-list_area.y)/4; if(i<catalog.count) { selected=i; page=1; info_scroll=0; } return true;
    }
    if(page==1 && info_max) return key(row<content_area.y+content_area.h/2?LS_TK_UP:LS_TK_DOWN,0);
    return true;
}
const ls_tui_screen_t ls_scr_tiles={.name="TILES",.hint="UP/DOWN region  ENTER info  O options  BACK",.enter=enter,.leave=ls_place_search_close,.draw=draw,.key=key,.touch=touch};
