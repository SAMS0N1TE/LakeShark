/* Shared TILES / MAP place picker. All I/O is mailbox work on carto-idle. */
#include "ls_cells.h"
#include "ls_keyboard.h"
#include "ls_map.h"
#include "ls_tui_ui.h"
#include "esp_attr.h"
#include <stdio.h>
#include <string.h>
static EXT_RAM_BSS_ATTR ls_cells_view view;
static EXT_RAM_BSS_ATTR char query[64],text[256];
static bool opened,jump,typing;
static unsigned page,selected,first_row,radius=25;
static tui_rect rows;
static const char *kind(unsigned k) { return k==1?"COUNTRY":k==2?"STATE":"CITY"; }
static void typed(const char *s) { snprintf(query,sizeof(query),"%s",s);ls_cells_query(query);typing=false; }
static void server(const char *s) { ls_cells_server(s); }
void ls_place_search_open(bool go) {
    opened=true;jump=go;page=selected=0;query[0]=0;typing=false;ls_cells_query("");
}
void ls_place_search_close(void) { opened=false;if(typing)ls_keyboard_close();typing=false; }
void ls_place_search_progress(void) { opened=true;jump=false;page=5; }
bool ls_place_search_active(void) { return opened; }
static void pick(void) {
    if(!ls_cells_snapshot(&view) || view.busy || strcmp(view.query,query) || selected>=view.count)return;
    if(jump) { ls_map_follow_set(false);ls_map_center(view.hits[selected].lat,view.hits[selected].lon);
        ls_map_zoom_by((view.hits[selected].kind==3?14:8)-ls_map_zoom());opened=false;return; }
    radius=25;if(ls_cells_pick(selected,radius))page=1;
}
static void button(unsigned b) {
    if(b==0) { if(page)page=0;else opened=false;return; }
    if(page==0) {
        if(b==1) { typing=true;ls_keyboard_open("SEARCH PLACES",query,63,typed); }
        if(b==2) { page=2;selected=0; }
        if(b==3)ls_keyboard_open("MAP SERVER",view.server,250,server);
        if(b==4)page=3;
        if(b==5)page=5;
    } else if(page==1) {
        if(b==1 && view.ready)ls_cells_action(1,0);
        if(b==2 && view.picked.kind==3 && radius>5) { radius-=5;ls_cells_pick(selected,radius); }
        if(b==3 && view.picked.kind==3 && radius<500) { radius+=5;ls_cells_pick(selected,radius); }
    } else if(page==2 && selected<view.library_count) {
        if(b==1)ls_cells_action(3,view.keys[selected]);
        if(b==2)page=4;
    } else if(page==4 && b==1 && selected<view.library_count) { ls_cells_action(2,view.keys[selected]);page=2; }
}
bool ls_place_search_key(ls_tk_t k,char ch) {
    if(!opened)return false;
    if(k==LS_TK_ESC) { button(0);return true; }
    if(k>=LS_TK_F1 && k<=LS_TK_F5) { button(1+k-LS_TK_F1);return true; }
    unsigned count=page==2?view.library_count:view.count;
    if(k==LS_TK_DOWN && selected+1<count)selected++;
    if(k==LS_TK_UP && selected)selected--;
    if(k==LS_TK_ENTER) { if(page==0)pick();else if(page==1)button(1); }
    if(page==1 && (k==LS_TK_LEFT || ch=='<'))button(2);
    if(page==1 && (k==LS_TK_RIGHT || ch=='>'))button(3);
    if(page==0) {
        size_t n=strlen(query);
        if(k==LS_TK_BACKSPACE && n) { query[n-1]=0;ls_cells_query(query);selected=0; }
        if(k==LS_TK_CHAR && (unsigned char)ch>=32 && n<63) { query[n]=ch;query[n+1]=0;ls_cells_query(query);selected=0; }
    }
    return true;
}
bool ls_place_search_touch(int x,int y) {
    if(!opened)return false;
    int b=ls_btn_hit(x,y);if(b>=0) { button((unsigned)b);return true; }
    if(x>=rows.x && x<rows.x+rows.w && y>=rows.y && y<rows.y+rows.h) {
        unsigned at=first_row+(unsigned)(y-rows.y)/3;
        if(page==0 && at<view.count) { selected=at;pick(); }
        if(page==2 && at<view.library_count)selected=at;
    }
    return true;
}
extern void ls_tiles_search_globe(tui_surface *,tui_rect,float,double,double);
void ls_place_search_draw(tui_surface *sf,tui_rect a) {
    ls_cells_snapshot(&view);
    if(typing) { const char *s=ls_keyboard_text();if(s && strcmp(s,query))typed(s);typing=ls_keyboard_active(); }
    ls_panel_box(sf,a,jump?"MAP / SEARCH PLACES":page==2?"TILES / INSTALLED":page==3?"TILES / INFO":page==5?"TILES / DOWNLOAD PROGRESS":"TILES / SEARCH PLACES",TUI_CYAN);
    const uint8_t ink=TUI_ATTR(TUI_YELLOW|TUI_BRIGHT,TUI_BLACK);
    ls_btn_t btn[6]={{.label="BACK"},{.label="KEYBOARD",.key='1'},{.label="INSTALLED",.key='2'},{.label="SERVER",.key='3'},{.label="INFO",.key='4'},{.label="PROGRESS",.key='5'}};
    int nb=6;
    if(page==1) { nb=4;btn[1]=(ls_btn_t){.label="DOWNLOAD",.dim=!view.ready};btn[2]=(ls_btn_t){.label="< RADIUS",.dim=view.picked.kind!=3};btn[3]=(ls_btn_t){.label="RADIUS >",.dim=view.picked.kind!=3}; }
    if(page==2) { nb=3;btn[1]=(ls_btn_t){.label="UPDATE"};btn[2]=(ls_btn_t){.label="DELETE"}; }
    if(page==3 || page==5)nb=1;
    if(page==4) { nb=2;btn[1]=(ls_btn_t){.label="CONFIRM DELETE"}; }
    int bh=ls_btn_raised_height(a,nb);
    tui_rect body=tui_rect_make(a.x+1,a.y+2,a.w-2,a.h-bh-4);
    ls_btn_bar_raised(sf,tui_rect_make(a.x+1,a.y+a.h-bh-1,a.w-2,bh),btn,nb,-1);
    int y=body.y;first_row=0;
#define LINE(s,at) ls_safe_line(sf,body,y++,(s),(at))
    if(page==0) {
        snprintf(text,sizeof(text),"> %s%s",query,view.busy?"  ...":"");LINE(text,ink);
        LINE(strlen(query)<2?"Type at least 2 letters. F1 opens keyboard.":view.message,LS_ATTR_DIM);y++;
        rows=tui_rect_make(body.x,y,body.w,body.h-4);
        for(unsigned i=0;i<view.count && y+2<body.y+body.h;i++) {
            snprintf(text,sizeof(text),"%c %s",i==selected?'>':' ',view.hits[i].name);LINE(text,ink);
            snprintf(text,sizeof(text),"  %s / %s",kind(view.hits[i].kind),view.hits[i].parent);LINE(text,LS_ATTR_DIM);y++;
        }
    } else if(page==1) {
        LINE(view.picked.name,ink);
        snprintf(text,sizeof(text),"%s / %s",kind(view.picked.kind),view.picked.parent);LINE(text,LS_ATTR_DIM);
        snprintf(text,sizeof(text),"%u cells / %u installed / %u unavailable",view.cells,view.installed,view.unavailable);LINE(text,ink);
        snprintf(text,sizeof(text),"Download %.2f MB / SD free %.2f GB",view.bytes/1e6,view.free/1e9);LINE(text,ink);
        if(view.picked.kind==3) { snprintf(text,sizeof(text),"City radius: %u km   < > to adjust",radius);LINE(text,LS_ATTR_DIM); }
        LINE(view.message,LS_ATTR_DIM);
        if(view.downloading) { snprintf(text,sizeof(text),"%.2f / %.2f MB - continues after BACK",view.done/1e6,view.total/1e6);LINE(text,ink); }
        for(unsigned i=0;i<view.preview_count && i<4;i++) { unsigned c=view.preview[i];
            snprintf(text,sizeof(text),"8/%u/%u  %s",c/256,c%256,view.present[i]==1?"INSTALLED":view.present[i]==2?"UNAVAILABLE":"DOWNLOAD");LINE(text,LS_ATTR_DIM); }
        ls_tiles_search_globe(sf,tui_rect_make(body.x,y,body.w,body.y+body.h-y),view.total?(float)view.done/view.total:0,view.picked.lat,view.picked.lon);
    } else if(page==5) {
        LINE(view.queue_name[0]?view.queue_name:"MAP DOWNLOADS",ink);
        LINE(view.message,LS_ATTR_DIM);
        snprintf(text,sizeof(text),"Current file: %.2f / %.2f MB",view.done/1e6,view.total/1e6);LINE(text,ink);
        LINE(view.queued?"Queue saved on SD; safe to leave TILES":"No queued places",LS_ATTR_DIM);
        ls_tiles_search_globe(sf,tui_rect_make(body.x,y,body.w,body.y+body.h-y),view.total?(float)view.done/view.total:0,view.queue_lat,view.queue_lon);
    } else if(page==2) {
        LINE(view.message,LS_ATTR_DIM);y++;rows=tui_rect_make(body.x,y,body.w,body.h-2);
        unsigned first=selected>5?selected-5:0;first_row=first;
        for(unsigned i=first;i<view.library_count && y+2<body.y+body.h;i++) {
            snprintf(text,sizeof(text),"%c %s",i==selected?'>':' ',view.library[i]);LINE(text,ink);y+=2;
        }
        if(!view.library_count) { LINE("No downloaded places yet",LS_ATTR_DIM);LINE("Download a region in TILES > SEARCH",LS_ATTR_DIM); }
    } else if(page==3) {
        LINE("GeoNames place names",ink);LINE("Creative Commons Attribution 4.0 (CC BY)",LS_ATTR_DIM);
        LINE("https://www.geonames.org/",LS_ATTR_DIM);y++;
        LINE("Map data: OpenStreetMap contributors",LS_ATTR_DIM);
        LINE("Downloads continue while you use other apps.",LS_ATTR_DIM);
        LINE("Shared cells stay until no place needs them.",LS_ATTR_DIM);
        LINE("Map server (editable with SERVER):",ink);LINE(view.server,LS_ATTR_DIM);
    } else {
        LINE("Delete this downloaded place?",ink);
        if(selected<view.library_count)LINE(view.library[selected],ink);
        LINE("Cells used by other places will be kept.",LS_ATTR_DIM);
    }
#undef LINE
}
