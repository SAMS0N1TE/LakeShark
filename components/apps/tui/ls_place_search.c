/* Shared TILES / MAP place picker. All I/O is mailbox work on carto-idle. */
#include "ls_cells.h"
#include "ls_keyboard.h"
#include "ls_map.h"
#include "ls_tui_ui.h"
#include "esp_attr.h"
#include <stdio.h>
#include <string.h>
#include "ls_textpack.h"
static EXT_RAM_BSS_ATTR ls_cells_view view;
static EXT_RAM_BSS_ATTR char query[64],text[256];
static bool opened,jump,typing;
static int focus=-1,slot;
static unsigned visible=1;
static char notice[80];
static unsigned notice_done;
static void (*navigate)(double,double);
static unsigned page,selected,first_row,radius=25;
static tui_rect rows;
static const char *kind(unsigned k) { return k==1?"COUNTRY":k==2?"STATE":"CITY"; }
static void typed(const char *s) { snprintf(query,sizeof(query),"%s",s);ls_cells_query(query);typing=false; }
static void server(const char *s) { ls_cells_server(s); }
void ls_place_search_open(bool go) {
    navigate=NULL;focus=-1;notice[0]=0;opened=true;jump=go;page=selected=0;query[0]=0;typing=false;ls_cells_query("");
}
void ls_place_search_goto(void (*go)(double,double)) { ls_place_search_open(true);navigate=go; }
void ls_place_search_close(void) { opened=false;if(typing)ls_keyboard_close();typing=false; }
void ls_place_search_progress(void) { focus=-1;notice[0]=0;opened=true;jump=false;page=5; }
void ls_place_search_installed(void) { focus=-1;notice[0]=0;opened=true;jump=false;page=2;selected=0; }
const char *ls_place_search_hint(void) {
    return page==0?"TYPE search  UP/DOWN place  ENTER pick  TAB controls":page==2?"UP/DOWN place  TAB controls  F1 update  F2 delete":page==1?"F1 download  LEFT/RIGHT radius  TAB controls":page==4?"F1 confirm delete  ESC cancel":"TAB controls  ESC back";
}
bool ls_place_search_active(void) { return opened; }
static void pick(void) {
    if(!ls_cells_snapshot(&view) || view.busy || strcmp(view.query,query) || selected>=view.count)return;
    if(jump) { if(navigate) navigate(view.hits[selected].lat,view.hits[selected].lon);
        else { ls_map_follow_set(false);ls_map_center(view.hits[selected].lat,view.hits[selected].lon); }
        ls_map_zoom_by((view.hits[selected].kind==3?14:8)-ls_map_zoom());opened=false;return; }
    radius=25;if(ls_cells_pick(selected,radius))page=1;
}
static bool command(unsigned action,uint32_t key) {
    bool ok=ls_cells_action(action,key);notice_done=view.action_done;
    snprintf(notice,sizeof(notice),"%s",ok?"Request accepted; waiting for SD acknowledgement":"Request not accepted; busy or queue full, retry");
    return ok;
}
static void button(unsigned b) {
    focus=-1;notice[0]=0;
    if((page==0 && (b==6 || b==7)) || (page==2 && (b==3 || b==4))) {
        unsigned count=page==0?view.count:view.library_count;
        bool next=page==0?b==7:b==4;
        if(next && first_row+visible<count)selected=first_row+visible;
        if(!next)selected=first_row>visible?first_row-visible:0;
        return;
    }
    if(b==0) { if(page)page=0;else opened=false;return; }
    if(page==0) {
        if(b==1) { typing=true;ls_keyboard_open("SEARCH PLACES",query,63,typed); }
        if(b==2) { page=2;selected=0; }
        if(b==3)ls_keyboard_open("MAP SERVER",view.server,250,server);
        if(b==4)page=3;
        if(b==5)page=5;
    } else if(page==1) {
        if(b==1 && view.ready)command(1,0);
        if(b==2 && view.picked.kind==3 && radius>5) { radius-=5;ls_cells_pick(selected,radius); }
        if(b==3 && view.picked.kind==3 && radius<500) { radius+=5;ls_cells_pick(selected,radius); }
    } else if(page==2 && selected<view.library_count) {
        if(b==1)command(3,view.keys[selected]);
        if(b==2)page=4;
    } else if(page==4 && b==1 && selected<view.library_count) { if(command(2,view.keys[selected]))page=2; }
}
bool ls_place_search_key(ls_tk_t k,char ch) {
    if(!opened)return false;
    if(k==LS_TK_ESC) { button(0);return true; }
    if(k>=LS_TK_F1 && k<=LS_TK_F5) { button(1+k-LS_TK_F1);return true; }
    if(k==LS_TK_TAB) { if(focus<0)ls_btn_navigate(k,&slot,&focus,false);else focus=-1;return true; }
    if(focus>=0) {
        if(ls_btn_navigate(k,&slot,&focus,false))return true;
        if(k==LS_TK_ENTER) { if(ls_btn_enabled(slot,focus))button((unsigned)focus);return true; }
    }
    unsigned count=page==2?view.library_count:view.count;
    if(k==LS_TK_DOWN && selected+1<count)selected++;
    if(k==LS_TK_UP && selected)selected--;
    if(k==LS_TK_ENTER) { if(page==0)pick();else if(page==1)button(1); }
    if(page==1 && (k==LS_TK_LEFT || ch=='<'))button(2);
    if(page==1 && (k==LS_TK_RIGHT || ch=='>'))button(3);
    if(page==0) {
        size_t n=strlen(query);
        if(k==LS_TK_BACKSPACE && n) { query[n-1]=0;ls_cells_query(query);selected=0;focus=-1;notice[0]=0; }
        if(k==LS_TK_CHAR && (unsigned char)ch>=32 && n<63) { query[n]=ch;query[n+1]=0;ls_cells_query(query);selected=0;focus=-1;notice[0]=0; }
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
    if(view.action_done!=notice_done)notice[0]=0;
    if(typing) { const char *s=ls_keyboard_text();if(s && strcmp(s,query))typed(s);typing=ls_keyboard_active(); }
    ls_panel_box(sf,a,jump?"MAP / SEARCH PLACES":page==2?"TILES / INSTALLED":page==3?"TILES / INFO":page==5?"TILES / DOWNLOAD PROGRESS":"TILES / SEARCH PLACES",TUI_CYAN);
    const uint8_t ink=TUI_ATTR(TUI_YELLOW|TUI_BRIGHT,TUI_BLACK);
    ls_btn_t btn[8]={{.label="BACK"},{.label="KEYBOARD",.value="F1"},{.label="INSTALLED",.value="F2"},{.label="SERVER",.value="F3"},{.label="INFO",.value="F4"},{.label="PROGRESS",.value="F5"},{.label="PREV"},{.label="NEXT"}};
    int nb=8;
    if(page==1) { nb=4;btn[1]=(ls_btn_t){.label="DOWNLOAD",.value="F1",.dim=!view.ready};btn[2]=(ls_btn_t){.label="< RADIUS",.dim=view.picked.kind!=3};btn[3]=(ls_btn_t){.label="RADIUS >",.dim=view.picked.kind!=3}; }
    if(page==2) { nb=5;btn[3]=(ls_btn_t){.label="PREV"};btn[4]=(ls_btn_t){.label="NEXT"};btn[1]=(ls_btn_t){.label="UPDATE",.value="F1"};btn[2]=(ls_btn_t){.label="DELETE",.value="F2"}; }
    if(page==3 || page==5)nb=1;
    if(page==4) { nb=2;btn[1]=(ls_btn_t){.label="CONFIRM DELETE",.value="F1"}; }
    int bh=ls_btn_raised_height(a,nb);
    tui_rect body=tui_rect_make(a.x+1,a.y+2,a.w-2,a.h-bh-4);
    unsigned count=page==2?view.library_count:view.count;
    if(selected>=count)selected=count?count-1:0;
    if(page==0 || page==2) {
        int n=(body.h-(page==0?3:2))/3;visible=n>0?(unsigned)n:1;
        first_row=selected/visible*visible;
        int prev=page==0?6:3,next=prev+1;
        btn[prev].dim=first_row==0;btn[next].dim=first_row+visible>=count;
    }
    ls_btn_bar_raised(sf,tui_rect_make(a.x+1,a.y+a.h-bh-1,a.w-2,bh),btn,nb,focus);
    int y=body.y;rows=tui_rect_make(0,0,0,0);
#define LINE(s,at) ls_safe_line(sf,body,y++,(s),(at))
    if(page==0) {
        snprintf(text,sizeof(text),"> %s%s",query,view.busy?"  ...":"");LINE(text,ink);
        LINE(strlen(query)<2?"Type at least 2 letters. F1 opens keyboard.":notice[0]?notice:view.message,LS_ATTR_DIM);y++;
        rows=tui_rect_make(body.x,y,body.w,(int)visible*3);
        for(unsigned i=first_row;i<view.count && i<first_row+visible && y+2<body.y+body.h;i++) {
            snprintf(text,sizeof(text),"%c %s",i==selected?'>':' ',view.hits[i].name);LINE(text,ink);
            snprintf(text,sizeof(text),"  %s / %s",kind(view.hits[i].kind),view.hits[i].parent);LINE(text,LS_ATTR_DIM);y++;
        }
    } else if(page==1) {
        LINE(view.picked.name,ink);
        snprintf(text,sizeof(text),"%s / %s",kind(view.picked.kind),view.picked.parent);LINE(text,LS_ATTR_DIM);
        LS_SNPRINTF(t_ls_place_search_u_cells_u_installed, text,sizeof(text),"%u cells / %u installed / %u unavailable",view.cells,view.installed,view.unavailable);LINE(text,ink);
        LS_SNPRINTF(t_ls_place_search_download_2f_mb_sd, text,sizeof(text),"Download %.2f MB / SD free %.2f GB",view.bytes/1e6,view.free/1e9);LINE(text,ink);
        if(view.picked.kind==3) { LS_SNPRINTF(t_ls_place_search_city_radius_u_km, text,sizeof(text),"City radius: %u km   < > to adjust",radius);LINE(text,LS_ATTR_DIM); }
        LINE(notice[0]?notice:view.message,LS_ATTR_DIM);
        if(view.downloading) { LS_SNPRINTF(t_ls_place_search_2f_2f_mb_continues, text,sizeof(text),"%.2f / %.2f MB - continues after BACK",view.done/1e6,view.total/1e6);LINE(text,ink); }
        for(unsigned i=0;i<view.preview_count && i<4;i++) { unsigned c=view.preview[i];
            snprintf(text,sizeof(text),"8/%u/%u  %s",c/256,c%256,view.present[i]==1?"INSTALLED":view.present[i]==2?"UNAVAILABLE":"DOWNLOAD");LINE(text,LS_ATTR_DIM); }
        ls_tiles_search_globe(sf,tui_rect_make(body.x,y,body.w,body.y+body.h-y),view.total?(float)view.done/view.total:0,view.picked.lat,view.picked.lon);
    } else if(page==5) {
        LINE(view.queue_name[0]?view.queue_name:"MAP DOWNLOADS",ink);
        LINE(notice[0]?notice:view.message,LS_ATTR_DIM);
        LS_SNPRINTF(t_ls_place_search_current_file_2f_2f, text,sizeof(text),"Current file: %.2f / %.2f MB",view.done/1e6,view.total/1e6);LINE(text,ink);
        LINE(view.queued?"Queue saved on SD; safe to leave TILES":"No queued places",LS_ATTR_DIM);
        ls_tiles_search_globe(sf,tui_rect_make(body.x,y,body.w,body.y+body.h-y),view.total?(float)view.done/view.total:0,view.queue_lat,view.queue_lon);
    } else if(page==2) {
        LINE(notice[0]?notice:view.message,LS_ATTR_DIM);y++;rows=tui_rect_make(body.x,y,body.w,(int)visible*3);
        for(unsigned i=first_row;i<view.library_count && i<first_row+visible && y+2<body.y+body.h;i++) {
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
        if(notice[0])LINE(notice,LS_ATTR_DIM);
    }
#undef LINE
}
