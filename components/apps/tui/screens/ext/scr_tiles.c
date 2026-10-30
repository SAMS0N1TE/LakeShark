#include "../../ls_cells.h"
/* TILES: a region library and live transfers, with no I/O in draw. */
#include "../../ls_tiles.h"
#include "../../ls_tui_ui.h"
#include "../../ls_options.h"
#include "../../ls_picker.h"
#include "../../ls_glyph.h"
#include "../../ls_text.h"
#include "../../ls_map.h"
#include "../../ls_value.h"
#include "../../ls_action.h"
#include "../../ls_quick.h"
#include "../../ls_i18n.h"
#include "../../ls_globe.h"
#include "../../ls_map_ink.h"
#include "../../ls_braille.h"
#include "../../ls_calls.h"
#include <stdlib.h>
#include "esp_timer.h"
#include "esp_attr.h"
#include <math.h>
#include <ctype.h>
#include <stdio.h>
#include <string.h>
#include "ls_textpack.h"

static EXT_RAM_BSS_ATTR ls_tiles_catalog catalog;
static ls_carto_transfer transfer;
static int selected, first, page, focus=-1, slot, confirm;
static int info_scroll,info_max;
static bool refreshed,opening;
static int64_t seen_transfer;
static char active_file[128];
static char hint[96]="UP/DOWN pick  ENTER manage  M get maps  S search  BACK";
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
    int offset=centre;
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
/* Pages: 0 ON THIS CARD, 1 region info, 2 transfer, 3 confirm, 4 US states,
   5 region files. Region info and its delete go back to where they came from. */
static int info_from=5;
static void back(void) {
    if(page==2 && transfer.state==1) { confirm=1; page=3; }
    else if(page==3) { page=confirm==1?2:info_from; confirm=0; }
    else if(page==1) page=info_from;
    else if(page) page=0;
    else home();
}
static void map_open(void) {
    if(!catalog.count) return;
    if(!opening && ls_tiles_action(2,&catalog.region[selected])) { opening=true;page=0; }
}
static bool map_open_done(bool copied) {
    if(!opening || !copied || catalog.open_state<2)return false;
    opening=false;
    if(catalog.open_state!=2)return false;
    const ls_app_t *app=ls_app_by_id("map");if(app)ls_tui_screen_show(ls_tui_screen_index_of(app->screen));
    return true;
}
static void action(const ls_opt_t *o) {
    if(catalog.busy) return;
    switch(o->arg) {
    case 0: if(catalog.count && ls_tiles_action(0,&catalog.region[selected])) { page=2; refreshed=false; transfer=(ls_carto_transfer){0}; } break;
    case 1: if(ls_tiles_action(1,NULL)) { page=2; refreshed=false; transfer=(ls_carto_transfer){0}; } break;
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
        page=2; refreshed=false; transfer=(ls_carto_transfer){0};
    } break;
    case 8: page=5; break;
    }
}
static const char *available(const ls_opt_t *o) {
    if(o->arg==8) return NULL;
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
    {.label="REGION FILES",.kind=LS_OPT_ACTION,.arg=8,.act=action,.why_not=available,.leaves=true},
    {.label="SELECTED REGION",.kind=LS_OPT_MENU,.sub=&region_ctx},
    {.label="REFRESH CATALOG",.kind=LS_OPT_ACTION,.arg=5,.act=action,.why_not=available,.leaves=true},
    {.label="RECEIVE FROM LAPTOP",.kind=LS_OPT_ACTION,.arg=1,.act=action,.why_not=available,.leaves=true},
    {.label="SEARCH PLACES",.kind=LS_OPT_ACTION,.act=search_action,.leaves=true},
    {.label="DOWNLOAD PROGRESS",.kind=LS_OPT_ACTION,.act=search_action,.arg=1,.leaves=true},
};
static const ls_opt_ctx_t ctx={.name="TILES",.job=-1,.radio=LS_RSEL_NONE,.back_to_screen=true,LS_OPT_ROWS(options)};
static void enter(void) {
    focus=-1; page=0; refreshed=false;opening=false;
    ls_tiles_refresh(true);
    ls_carto_transfer_snapshot(&transfer); seen_transfer=transfer.start_us; if(transfer.state==1) page=2;
#ifdef LS_TILES_HOST
    extern void ls_tiles_host_page(int *); ls_tiles_host_page(&page);
#endif
}
static void line(tui_surface *sf,tui_rect a,int y,const char *s,uint8_t at) { ls_safe_line(sf,a,y,s,at); }

/* ---- US STATES ----------------------------------------------------------
   Whole states of map cells (ls_cells_states_worker.h does the work). The
   page is a tile map of the US, one box per state where the state roughly
   sits, coloured by what is true now: on the card, part on it, downloading,
   free to get, or Plus. The bar under a code is that state's cells on the
   card, so the map fills in as a download lands; nothing moves otherwise. */
typedef struct { char st[3]; unsigned char c, r; } us_tile_t;
static const us_tile_t US_TILES[] = {
    {"AK",0,0},{"ME",10,0},
    {"WI",5,1},{"VT",9,1},{"NH",10,1},
    {"WA",0,2},{"ID",1,2},{"MT",2,2},{"ND",3,2},{"MN",4,2},{"IL",5,2},{"MI",6,2},{"NY",8,2},{"MA",9,2},
    {"OR",0,3},{"NV",1,3},{"WY",2,3},{"SD",3,3},{"IA",4,3},{"IN",5,3},{"OH",6,3},{"PA",7,3},{"NJ",8,3},{"CT",9,3},{"RI",10,3},
    {"CA",0,4},{"UT",1,4},{"CO",2,4},{"NE",3,4},{"MO",4,4},{"KY",5,4},{"WV",6,4},{"VA",7,4},{"MD",8,4},{"DE",9,4},
    {"AZ",1,5},{"NM",2,5},{"KS",3,5},{"AR",4,5},{"TN",5,5},{"NC",6,5},{"SC",7,5},{"DC",8,5},
    {"OK",3,6},{"LA",4,6},{"MS",5,6},{"AL",6,6},{"GA",7,6},
    {"HI",0,7},{"TX",3,7},{"FL",8,7},{"PR",10,7},
};
#define US_N ((int)(sizeof(US_TILES)/sizeof(US_TILES[0])))
/* Every state is free to z12 (MAPS); Plus adds z13+ street detail (DETAIL). */
/* outdated: cells on the card whose bytes are not the server's current revision. */
typedef struct { char name[28]; bool known, detail; unsigned cells, have, partial, outdated; unsigned long long base, extra; } us_state_t;
static EXT_RAM_BSS_ATTR us_state_t us_state[US_N];
static tui_rect us_hit[US_N];
static int us_sel=-1, us_busy=-1;
static char us_status[128];
static float us_progress;
static bool us_plus; static int us_free_z=12;
/* Everything this board is entitled to is on the card. */
static bool us_full(const us_state_t *s) { return s->cells && s->have>=s->cells && (s->detail || !s->extra || !us_plus); }

static int us_index(const char *st) { for(int i=0;i<US_N;i++) if(US_TILES[i].st[0]==st[0] && US_TILES[i].st[1]==st[1]) return i; return -1; }

/* maps.states rows: "ST|name|tier|cells|bytes|installed|partial|base|detail|detail_on|outdated";
   the tier field is legacy and ignored, and old 7-field rows are all base. */
/* A read that fails (the worker publishing at that instant) keeps what the
   last good read said: blanking it drew one empty frame and the map flashed. */
static bool us_listed;
static bool us_load(void) {
    ls_val_t v; bool any=false;
    if(ls_value_read("maps.state",&v,NULL) && v.kind==LS_VAL_TEXT && v.s) snprintf(us_status,sizeof(us_status),"%s",v.s);
    if(ls_value_read("maps.progress",&v,NULL)) us_progress=v.f;
    if(ls_value_read("maps.plus",&v,NULL)) us_plus=v.i;
    if(ls_value_read("maps.free_max_zoom",&v,NULL) && v.kind==LS_VAL_INT && v.i>0 && v.i<16) us_free_z=(int)v.i;
    const char *rows=NULL;
    if(ls_value_read("maps.states",&v,NULL) && v.kind==LS_VAL_TEXT && v.s && v.s[0]) rows=v.s;
#ifdef LS_TILES_HOST
    /* lssim has no map host: a sample list so the page can be looked at. */
    if(!rows) { rows="NH|New Hampshire|free|7|248000000|7|2|79000000|169000000|1\nVT|Vermont|free|7|190000000|3|3|61000000|129000000|0\n"
        "ME|Maine|free|16|300000000|0|9|96000000|204000000|0\nMA|Massachusetts|free|7|260000000|7|3|83000000|177000000|0\n"
        "CO|Colorado|plus|30|900000000|0|10|288000000|612000000|0\nTX|Texas|plus|57|2100000000|0|20|672000000|1428000000|0\n"
        "CA|California|plus|43|1900000000|0|18|608000000|1292000000|0\nNY|New York|plus|21|800000000|0|9|256000000|544000000|0\n";
        us_progress=0.42f; us_busy=1; us_plus=getenv("LSSIM_MAPS_TOKEN")!=NULL;
        snprintf(us_status,sizeof(us_status),"VT: cell 4 of 7, part 2 of 3"); }
#endif
    if(!rows) return us_listed;
    memset(us_state,0,sizeof(us_state));
    for(const char *p=rows;*p;) {
        const char *end=strchr(p,'\n'); if(!end) end=p+strlen(p);
        char row[160]; int len=(int)(end-p)<(int)sizeof(row)-1?(int)(end-p):(int)sizeof(row)-1;
        memcpy(row,p,(size_t)len); row[len]=0;
        char st[3]={0},name[28]={0},tier[8]={0}; unsigned cells=0,have=0,partial=0,on=0,outdated=0; unsigned long long bytes=0,base=0,extra=0;
        const int n=sscanf(row,"%2[A-Z]|%27[^|]|%7[^|]|%u|%llu|%u|%u|%llu|%llu|%u|%u",st,name,tier,&cells,&bytes,&have,&partial,&base,&extra,&on,&outdated);
        if(n==7 || n==10 || n==11) {
            int i=us_index(st);
            if(i>=0) { us_state_t *s=&us_state[i]; snprintf(s->name,sizeof(s->name),"%s",name); s->known=true;
                s->cells=cells; s->have=have; s->partial=partial; any=true;
                s->base=n>=10?base:bytes; s->extra=n>=10?extra:0; s->detail=n>=10 && on; s->outdated=n==11?outdated:0; }
        }
        p=*end?end+1:end;
    }
    us_listed=any;
    return any;
}

/* MAP's outline view, where the card has no tiles, asks which regions are
   on it: green whole, yellow part. Read at most every two seconds; a change
   tells the outlines to redraw. */
uint8_t ls_maps_region_fill(const char *code,void *ctx) {
    (void)ctx; static int64_t at; static uint32_t sum;
    const int64_t now=esp_timer_get_time();
    if(!at || now-at>2000000) {
        at=now; (void)us_load();
        uint32_t s=0; for(int i=0;i<US_N;i++) s=s*31u+(us_state[i].have+us_state[i].outdated)*7u+us_state[i].cells;
        if(s!=sum) { sum=s; ls_globe_refill(); }
    }
    if(strncmp(code,"US-",3)) return 0;
    const int i=us_index(code+3);
    /* Cells older than the server are still on the card and still draw:
       counted with the current ones, a state waiting on UPDATE is not a gap. */
    const unsigned on=i<0?0:us_state[i].have+us_state[i].outdated;
    if(i<0 || !us_state[i].known || !on) return 0;
    return on>=us_state[i].cells?TUI_GREEN:TUI_YELLOW;
}
static uint8_t us_hue(int i,int *dither) {
    const us_state_t *s=&us_state[i]; int d; uint8_t hue;
    if(!s->known) { hue=0; d=-1; }
    else if(i==us_busy) { hue=TUI_WHITE; d=LS_DITHER_MEDIUM; }
    else if(s->cells && s->have>=s->cells && s->detail) { hue=TUI_MAGENTA; d=LS_DITHER_HEAVY; }  /* maps + Plus detail */
    else if(s->cells && s->have>=s->cells) { hue=TUI_GREEN; d=LS_DITHER_HEAVY; }               /* maps to z12 */
    else if(s->have) { hue=TUI_YELLOW; d=LS_DITHER_MEDIUM; }
    else { hue=TUI_CYAN; d=LS_DITHER_LIGHT; }                                                   /* ready to get */
    if(dither) *dither=d;
    return hue;
}
/* The globe asks per region code ("US-NH"); states known to the list get
   their install colour, everything else is outline only. */
static uint8_t us_globe_fill(const char *code,void *ctx) {
    (void)ctx;
    if(strncmp(code,"US-",3)) return 0;
    const int i=us_index(code+3); return i<0?0:us_hue(i,NULL);
}
/* Which picture the states page draws. Asked as "is the globe READY", a
   reload of the outline file (a new one comes with each state list) put the
   tile grid up for the length of the load and the globe back after it, so
   the page swapped between two layouts. The globe now stays while a newer
   file loads, and the tile grid is only for a card that has no outline
   file at all. */
enum { US_VIEW_GRID, US_VIEW_GLOBE, US_VIEW_WAIT };
static int us_view(void) {
#ifdef LS_TILES_HOST
    const int st=ls_globe_load(getenv("LSSIM_OUTLINES"));
#else
    /* Cheap every frame: a stat, and the loader task once the file lands. */
    const int st=ls_globe_load(NULL);
#endif
    if(ls_globe_has_shapes()) return US_VIEW_GLOBE;
    return st==LS_GLOBE_LOADING?US_VIEW_WAIT:US_VIEW_GRID;
}
#define US_VIEW_LAT 38.5f
#define US_VIEW_LON (-96.0f)
#define US_VIEW_ZOOM 3.1f
static bool us_globe_world;

static void us_draw(tui_surface *sf,tui_rect body) {
    const bool any=us_load();
    const bool working=us_progress>0.0f && us_progress<1.0f;
    if(!working) us_busy=-1;
    const bool wide=body.w>90;
    tui_rect grid=body, info=body;
    if(wide) { grid.w=body.w*3/5; info.x=grid.x+grid.w+1; info.w=body.w-grid.w-1; }
    int tw=grid.w/11; if(tw>7) tw=7;
    /* Taller tiles where portrait has the rows; info needs about twelve. */
    int th=wide?grid.h/8:(body.h-12)/8; if(th>5) th=5; if(th<1) th=1;
    const int view=us_view();
    /* The globe and the wait share the globe's layout, so the page does not
       jump when the outlines arrive. */
    const bool globe=view!=US_VIEW_GRID;
    if(!wide) { grid.h=globe?body.h-13:th*8; info.y=grid.y+grid.h+1; info.h=body.y+body.h-info.y; }
    const int gx=grid.x+(grid.w-tw*11)/2;
    if(view==US_VIEW_WAIT) {
        memset(us_hit,0,sizeof(us_hit));
        line(sf,grid,grid.y+grid.h/2,"      Loading state outlines...",LS_ATTR_DIM);
    } else if(globe) {
        /* THE GLOBE: real borders, each state filled with what is on the
           card. Tiles stay for a card without the outline file. */
        memset(us_hit,0,sizeof(us_hit));
        char sel[8]; snprintf(sel,sizeof(sel),"US-%s",us_sel>=0?US_TILES[us_sel].st:"");
        ls_globe_draw(sf,grid,us_globe_fill,NULL,us_sel>=0?sel:NULL);
    }
    for(int i=0;i<US_N && !globe;i++) {
        const us_state_t *s=&us_state[i];
        tui_rect t=tui_rect_make(gx+US_TILES[i].c*tw,grid.y+US_TILES[i].r*th,tw-1,th);
        us_hit[i]=t;
        int dither; uint8_t hue=us_hue(i,&dither); if(!hue) hue=LS_FAINT_FG;
        if(dither>=0) ls_fill_dither(sf,t,dither,hue);
        const bool sel=i==us_sel;
        const uint8_t face=sel?TUI_ATTR(TUI_BLACK,TUI_WHITE|TUI_BRIGHT):s->known?TUI_ATTR(TUI_WHITE|TUI_BRIGHT,dither>=0?hue:TUI_BLACK):TUI_ATTR(LS_DIM_FG,TUI_BLACK);
        tui_put_str(sf,t,t.x+(t.w-2)/2,t.y,US_TILES[i].st,face);
        if(th>=2 && s->known && t.w>=3) {
            /* cells on the card, as a filament per quarter of the state */
            const int w=t.w-1>4?4:t.w-1; const int lit=s->cells?(int)((s->have*w+s->cells-1)/s->cells):0;
            for(int k=0;k<w;k++) tui_put_char(sf,t,t.x+(t.w-w)/2+k,t.y+1,k<lit?'|':'.',
                TUI_ATTR(k<lit?(hue|TUI_BRIGHT):LS_FAINT_FG,TUI_BLACK));
        }
        /* '+': street detail is on the card too */
        if(th>=3 && s->detail) tui_put_char(sf,t,t.x+t.w-1,t.y+th-1,'+',TUI_ATTR(TUI_MAGENTA|TUI_BRIGHT,TUI_BLACK));
    }
    /* the key, so the colours are read and not guessed */
    int ky=grid.y+grid.h; if(!wide && ky<info.y) {
        int x=grid.x; static const struct { const char *t; uint8_t h; } KEY[]={{"MAPS",TUI_GREEN},{"+DETAIL",TUI_MAGENTA},{"PART",TUI_YELLOW},{"GET",TUI_CYAN}};
        for(unsigned k=0;k<4;k++) { tui_put_char(sf,body,x,ky,'#',TUI_ATTR(KEY[k].h|TUI_BRIGHT,TUI_BLACK)); tui_put_str(sf,body,x+2,ky,ls_tr(KEY[k].t),LS_ATTR_DIM); x+=(int)strlen(ls_tr(KEY[k].t))+4; }
        info.y++; info.h--;
    }
    char text[160]; int y=info.y;
    if(!any) {
        line(sf,info,y++,catalog.sd?"No state list yet. REFRESH fetches it.":"No SD card. Insert a card to install maps.",LS_ATTR_DIM);
    } else if(us_sel<0) {
        unsigned have=0,total=0; for(int i=0;i<US_N;i++) { have+=us_state[i].have; total+=us_state[i].cells; }
        LS_SNPRINTF(t_scr_tiles_u_of_u_cells, text,sizeof(text),"%u of %u cells on the card",have,total); line(sf,info,y++,text,INK);
        LS_SNPRINTF(t_scr_tiles_every_state_free_to, text,sizeof(text),"Every state free to z%d. %s",us_free_z,us_plus?"Plus: street detail on":"Plus adds street detail");
        line(sf,info,y++,text,us_plus?TUI_ATTR(TUI_MAGENTA|TUI_BRIGHT,TUI_BLACK):LS_ATTR_DIM);
        line(sf,info,y++,LS_TEXT(t_scr_tiles_tap_a_state_to, "Tap a state to get or remove its maps"),LS_ATTR_DIM);
    } else {
        const us_state_t *s=&us_state[us_sel];
        snprintf(text,sizeof(text),"%s  %s",US_TILES[us_sel].st,s->known?s->name:"no maps yet");
        line(sf,info,y++,text,INK);
        if(s->known) {
            /* Two layers, each with what it costs and whether it is here. */
            const bool base_on=s->cells && s->have>=s->cells;
            LS_SNPRINTF(t_scr_tiles_maps_z0_2d_4, text,sizeof(text),"MAPS    z0-%-2d  %4.0f MB  %s",us_free_z,s->base/1e6,
                     base_on?"on card":s->have?"part":"free");
            line(sf,info,y++,text,base_on?GOOD:INK);
            if(s->extra) {
                LS_SNPRINTF(t_scr_tiles_detail_z_d_16, text,sizeof(text),"DETAIL  z%d-16  %4.0f MB  %s",us_free_z+1,s->extra/1e6,
                         s->detail?"on card":us_plus?"with Plus":"needs Plus");
                line(sf,info,y++,text,s->detail||us_plus?TUI_ATTR(TUI_MAGENTA|TUI_BRIGHT,TUI_BLACK):LS_ATTR_DIM);
            }
            LS_SNPRINTF(t_scr_tiles_on_the_card_u, text,sizeof(text),"On the card: %u of %u cells",s->have,s->cells);
            line(sf,info,y++,text,base_on?GOOD:LS_ATTR_DIM);
            if(s->outdated) { snprintf(text,sizeof(text),"%u cells older than server: UPDATE",s->outdated);
                line(sf,info,y++,text,TUI_ATTR(TUI_YELLOW|TUI_BRIGHT,TUI_BLACK)); }
            if(s->partial) { LS_SNPRINTF(t_scr_tiles_u_border_cells_hold, text,sizeof(text),"%u border cells hold US data only",s->partial); line(sf,info,y++,text,LS_ATTR_DIM); }
        }
    }
    if(working || us_status[0]) {
        y++;
        line(sf,info,y++,us_status[0]?us_status:"Working",working?INK:LS_ATTR_DIM);
        if(working && y<info.y+info.h) {
            const int w=info.w-8, lit=(int)(us_progress*w);
            for(int i=0;i<w;i++) tui_put_char(sf,info,info.x+1+i,y,i<lit?'=':'-',i<lit?GOOD:LS_ATTR_FAINT);
            snprintf(text,sizeof(text),"%3d%%",(int)(us_progress*100.0f+0.5f));
            tui_put_str(sf,info,info.x+w+2,y,text,INK);
        }
    }
}

static void us_call(const char *act,const char *st) {
    ls_args_t a={.n=st?1:0}; if(st) { a.v[0].kind=LS_VAL_TEXT; a.v[0].s=st; }
    ls_val_t out; (void)ls_action_call(act,&a,&out,ls_quick_grant_builtin());
}
static void us_get_or_remove(void) {
    if(us_sel<0 || !us_state[us_sel].known) return;
    const us_state_t *s=&us_state[us_sel];
    /* Complete for this board: remove. Maps on but Plus detail missing: get adds it. */
    if(us_full(s) && !s->outdated) { us_call("maps.state.remove",US_TILES[us_sel].st); return; }
    us_busy=us_sel; us_call("maps.state.get",US_TILES[us_sel].st);
}
/* ---- WHAT IS DOWNLOADING ------------------------------------------------
   Four ways put maps on the card: a whole state, the cells around a searched
   place, a region file, a push from a laptop. Each showed its progress on
   its own page, if at all, so nothing said what was downloading. This asks
   all of them; TILES and MAP draw the answer. */
static EXT_RAM_BSS_ATTR ls_cells_view activity_cells;
int ls_maps_activity(char lines[][64],float *fraction,int max) {
    static int64_t at; static int n, empty_reads; static char l[LS_MAPS_ACTIVITY_MAX][64]; static float f[LS_MAPS_ACTIVITY_MAX];
    const int64_t now=esp_timer_get_time();
    if(!at || now-at>500000) {
        at=now;
        /* Built aside and kept only when every source answered: a read that
           lost the worker's lock is not "nothing downloading". */
        int m=0; char ml[LS_MAPS_ACTIVITY_MAX][64]; float mf[LS_MAPS_ACTIVITY_MAX];
        float p=0;
        const bool got_p=ls_cells_maps_progress(&p), got_c=ls_cells_snapshot(&activity_cells);
        const bool state=got_p && p>0.0f && p<1.0f;
        if(state) {
            /* The queue's name is the state's code while a state downloads. */
            const char *code=got_c && activity_cells.queue_name[0]?activity_cells.queue_name:"";
            const int i=code[0]?us_index(code):-1;
            snprintf(ml[m],64,"STATE %.56s",i>=0 && us_state[i].name[0]?us_state[i].name:code[0]?code:"maps"); mf[m++]=p;
        }
        /* The worker does one thing at a time: a state download is not also
           shown as a place. */
        if(!state && got_c && activity_cells.queued && activity_cells.queue_name[0]) {
            /* a state's queue resumed after a restart carries its code as the name */
            const char *q=activity_cells.queue_name; const int si=strlen(q)==2?us_index(q):-1;
            if(si>=0) snprintf(ml[m],64,"STATE %.56s",us_state[si].name[0]?us_state[si].name:q);
            else snprintf(ml[m],64,"PLACE %.56s",q);
            mf[m++]=activity_cells.downloading && activity_cells.total?(float)activity_cells.done/activity_cells.total:-1.0f;
        }
        ls_carto_transfer t; ls_carto_transfer_snapshot(&t);
        if(m<LS_MAPS_ACTIVITY_MAX && t.state==1) {
            snprintf(ml[m],64,"%s %.55s",t.receiving?"LAPTOP":"FILE",t.file[0]?t.file:"starting");
            mf[m++]=t.total?(float)t.bytes/t.total:-1.0f;
        }
        /* An empty answer counts after two in a row (a second): the step
           between one cell and the next reads as idle for an instant. */
        const bool answered=got_p && got_c;
        if(m) { empty_reads=0; n=m; memcpy(l,ml,sizeof(ml[0])*(size_t)m); memcpy(f,mf,sizeof(mf[0])*(size_t)m); }
        else if(answered && ++empty_reads>=2) n=0;
    }
    const int k=n<max?n:max;
    for(int i=0;i<k;i++) { snprintf(lines[i],64,"%s",l[i]); if(fraction) fraction[i]=f[i]; }
    return k;
}
/* ---- ON THIS CARD -------------------------------------------------------
   The front page: what is downloading, then every map on the card in one
   list (states, places, region files), each to open or manage. */
typedef struct { char kind; int index; } card_row_t;   /* 's' state, 'p' place, 'r' region file */
static EXT_RAM_BSS_ATTR card_row_t card[US_N+LS_CELLS_PLACES+LS_TILES_MAX];
static EXT_RAM_BSS_ATTR ls_cells_view card_places;
static int card_n, card_sel, card_first, card_visible, card_states, card_place_n, card_files;
/* The state list has been read since boot. Before that "0 states" read as "your maps are gone". */
static bool card_listed;
/* UPDATE ALL: outdated states, fetched one after another. */
static char card_update[US_N][3]; static int card_update_n, card_update_row=-1; static int64_t card_update_at;
static tui_rect card_area;
static void card_build(void) {
    static int64_t places_at;
    const int64_t now=esp_timer_get_time();
    /* A read that lost the worker's lock keeps the last list, which is still
       true; emptying it made places vanish for a second. */
    if(!places_at || now-places_at>1000000) { places_at=now; (void)ls_cells_snapshot(&card_places); }
    card_listed=us_load();
    card_n=card_states=card_place_n=card_files=0;
    for(int i=0;i<US_N;i++) if(us_state[i].known && (us_state[i].have || us_state[i].outdated)) { card[card_n++]=(card_row_t){'s',i}; card_states++; }
    for(unsigned i=0;i<card_places.library_count && i<LS_CELLS_PLACES;i++) {
        if((card_places.keys[i]>>16)==0x5354u) continue;   /* a state download, not a place */
        card[card_n++]=(card_row_t){'p',(int)i}; card_place_n++;
    }
    for(int i=0;i<catalog.count;i++) if(catalog.region[i].local_bytes) { card[card_n++]=(card_row_t){'r',i}; card_files++; }
    if(card_sel>=card_n) card_sel=card_n?card_n-1:0;
    /* Next state of UPDATE ALL once nothing is downloading; a few seconds
       between asks so the worker has started the last one. */
    const bool busy=(us_progress>0.0f && us_progress<1.0f) || card_places.queued;
    if(card_update_n && !busy && now-card_update_at>6000000) {
        us_call("maps.state.get",card_update[0]); card_update_at=now;
        memmove(card_update,card_update+1,sizeof(card_update[0])*(size_t)(--card_update_n));
    }
}
static void card_update_all(void) {
    card_update_n=0;
    for(int i=0;i<US_N;i++) if(us_state[i].known && us_state[i].outdated) memcpy(card_update[card_update_n++],US_TILES[i].st,3);
    card_update_at=0;
}
/* Turn the globe to a state. The outlines may not be loaded yet (the states
   page loads them); then it turns once they are. */
static int card_turn_to=-1;
static void card_state_view(int i) {
    float la,lo; char code[8]; snprintf(code,sizeof(code),"US-%s",US_TILES[i].st);
    if(ls_globe_region(code,NULL,0,&la,&lo)) { ls_globe_target(la,lo,US_VIEW_ZOOM*1.6f); card_turn_to=-1; }
    else { ls_globe_target(US_VIEW_LAT,US_VIEW_LON,US_VIEW_ZOOM); card_turn_to=i; }
}
/* ENTER on a row: manage it where it is managed. */
static void card_manage(void) {
    if(card_sel>=card_n) return;
    const card_row_t r=card[card_sel];
    if(r.kind=='s') { page=4; us_sel=r.index; focus=-1; us_globe_world=false; card_state_view(r.index); }
    else if(r.kind=='p') ls_place_search_installed();
    else { selected=r.index; info_from=0; page=1; info_scroll=0; }
}
/* OPEN: show it in MAP. A place has no stored centre; its list opens instead. */
static void card_open(void) {
    if(card_sel>=card_n) return;
    const card_row_t r=card[card_sel];
    if(r.kind=='r') { selected=r.index; map_open(); return; }
    if(r.kind=='p') { ls_place_search_installed(); return; }
    float la,lo; char code[8]; snprintf(code,sizeof(code),"US-%s",US_TILES[r.index].st);
    if(!ls_globe_region(code,NULL,0,&la,&lo)) return;
    ls_map_center(la,lo);
    const ls_app_t *app=ls_app_by_id("map"); if(app) ls_tui_screen_show(ls_tui_screen_index_of(app->screen));
}
/* Where the maps come from. A card set to another server (a test copy was
   left on one for weeks) got old maps with nothing on screen saying why; it
   is shown here, and a tap goes back to the live one. */
#define LIVE_MAP_SERVER "https://cartocore-maps.pages.dev/"
static int card_server_row=-1;
static void card_use_live_server(void) { if(ls_cells_server(LIVE_MAP_SERVER)) card_server_row=-1; }
static void card_server_draw(tui_surface *sf,tui_rect body,int *y) {
    const char *url=card_places.server; const char *host=strstr(url,"://"); host=host?host+3:url;
    char text[160]; char shown[64]; snprintf(shown,sizeof(shown),"%.*s",(int)strcspn(host,"/"),host);
    card_server_row=-1;
    if(!url[0]) return;
    if(!strcmp(url,LIVE_MAP_SERVER)) { snprintf(text,sizeof(text),"Maps from %s",shown); line(sf,body,(*y)++,text,LS_ATTR_DIM); return; }
    snprintf(text,sizeof(text),"NOT LIVE: maps from %s",shown);
    line(sf,body,(*y)++,text,TUI_ATTR(TUI_YELLOW|TUI_BRIGHT,TUI_BLACK));
    card_server_row=*y;
    line(sf,body,(*y)++,"> TAP HERE or L: use the live map server",TUI_ATTR(TUI_BLACK,TUI_YELLOW|TUI_BRIGHT));
}
/* ---- THE HERO PANEL -----------------------------------------------------
   The top of ON THIS CARD, readable from arm's length: what is happening to
   the maps, in large type. Downloading: the state or place by name, a big
   percent, a smooth bar, cells, speed and time left. Idle: updates waiting
   (one key updates them all), everything ready, or nothing yet. Everything
   in it follows real numbers; the RX light shows only while bytes arrive. */
/* net: bytes taken off the network so far (one cell's count while a cell
   downloads), net_ok while there is such a count; checking cells already on
   the card moves frac but not net, so it never reads as download speed. */
typedef struct { bool on, waiting, net_ok; char what[24], name[32], big[8]; float frac; unsigned have, cells; double bytes_total; uint64_t net; } hero_job_t;

/* Stopped, not just between two cells: the worker drops "downloading" for a
   tick after each install, which is no reason to turn the panel yellow. */
static bool hero_stopped(bool downloading) {
    static int64_t since;
    const int64_t now = esp_timer_get_time();
    if (downloading) { since = 0; return false; }
    if (!since) since = now;
    return now - since > 1500000;
}

static void hero_job(hero_job_t *j) {
    memset(j, 0, sizeof(*j));
    ls_carto_transfer t; ls_carto_transfer_snapshot(&t);
    const bool state = us_progress > 0.0f && us_progress < 1.0f;
    if (state) {
        const char *code = card_places.queue_name; int i = code[0] ? us_index(code) : -1;
        j->on = true; snprintf(j->what, sizeof(j->what), "US STATE");
        if (i >= 0) {
            snprintf(j->name, sizeof(j->name), "%s", us_state[i].name[0] ? us_state[i].name : US_TILES[i].st);
            snprintf(j->big, sizeof(j->big), "%s", US_TILES[i].st);
            j->have = us_state[i].have; j->cells = us_state[i].cells;
            j->bytes_total = (double)us_state[i].base + (us_state[i].detail ? (double)us_state[i].extra : 0);
        } else snprintf(j->name, sizeof(j->name), "%s", code[0] ? code : "STATE MAPS");
        j->frac = us_progress;
        /* a state stopped on a failed cell said DOWNLOADING with "no data" for hours (2026-10-30) */
        j->waiting = card_places.queued && hero_stopped(card_places.downloading);
        j->net_ok = card_places.downloading; j->net = card_places.done;
    } else if (card_places.queued && card_places.queue_name[0]) {
        /* a state's queue resumed after a restart is named by its code */
        const char *q = card_places.queue_name; const int si = strlen(q) == 2 ? us_index(q) : -1;
        j->on = true; snprintf(j->what, sizeof(j->what), "%s", si >= 0 ? "US STATE" : "PLACE");
        if (si >= 0) {
            snprintf(j->name, sizeof(j->name), "%s", us_state[si].name[0] ? us_state[si].name : q);
            snprintf(j->big, sizeof(j->big), "%s", US_TILES[si].st);
            j->have = us_state[si].have; j->cells = us_state[si].cells;
        } else snprintf(j->name, sizeof(j->name), "%s", q);
        j->frac = card_places.downloading && card_places.total ? (float)card_places.done / card_places.total : -1.0f;
        j->bytes_total = card_places.total;
        /* queued but not fetching: between tries, or blocked; the worker says why */
        j->waiting = hero_stopped(card_places.downloading);
        j->net_ok = card_places.downloading; j->net = card_places.done;
    } else if (t.state == 1) {
        j->on = true; snprintf(j->what, sizeof(j->what), "%s", t.receiving ? "FROM LAPTOP" : "REGION FILE");
        snprintf(j->name, sizeof(j->name), "%.31s", t.file[0] ? t.file : "starting");
        j->frac = t.total ? (float)t.bytes / t.total : -1.0f; j->bytes_total = (double)t.total;
        j->net_ok = true; j->net = t.bytes;
    }
}

/* Speed, measured once a second from the network bytes that arrived: the
   trace, the figures and the RX lamp show nothing else. `steps` counts each
   move of the job (a cell checked or fetched); the meter's pattern and the
   cell spinner advance on it and on nothing else. */
#define HERO_HIST 48
static struct {
    float frac; int64_t at, moved_at, rx_at, sample_at; char name[32];
    uint64_t last_net; bool have_net; double got;
    float hist[HERO_HIST]; int hist_n; uint32_t steps;
} hero_rate;
static void hero_track(const hero_job_t *j) {
    const int64_t now = esp_timer_get_time();
    if (!j->on || strcmp(j->name, hero_rate.name)) {
        memset(&hero_rate, 0, sizeof(hero_rate)); snprintf(hero_rate.name, sizeof(hero_rate.name), "%s", j->name);
        hero_rate.frac = j->frac; hero_rate.at = hero_rate.sample_at = now;
        hero_rate.last_net = j->net; hero_rate.have_net = j->net_ok; return;
    }
    if (j->frac >= 0 && j->frac > hero_rate.frac) { hero_rate.frac = j->frac; hero_rate.at = hero_rate.moved_at = now; hero_rate.steps++; }
    if (j->net_ok) {
        /* a smaller count is the next cell starting from nothing */
        const uint64_t add = !hero_rate.have_net ? 0 : j->net >= hero_rate.last_net ? j->net - hero_rate.last_net : j->net;
        if (add) { hero_rate.got += (double)add; hero_rate.rx_at = now; }
        hero_rate.last_net = j->net; hero_rate.have_net = true;
    } else hero_rate.have_net = false;
    if (now - hero_rate.sample_at >= 1000000) {
        const double dt = (now - hero_rate.sample_at) / 1e6, got = hero_rate.got;
        hero_rate.got = 0;
        if (hero_rate.hist_n == HERO_HIST) memmove(hero_rate.hist, hero_rate.hist + 1, sizeof(hero_rate.hist[0]) * (HERO_HIST - 1));
        else hero_rate.hist_n++;
        hero_rate.hist[hero_rate.hist_n - 1] = (float)(got / dt);
        hero_rate.sample_at = now;
    }
}
/* Bytes a second over the last eight seconds (cells arrive in bursts);
   0 when nothing came. */
static double hero_speed(void) {
    double s = 0; int n = 0;
    for (int i = hero_rate.hist_n - 1; i >= 0 && n < 8; i--, n++) s += hero_rate.hist[i];
    return n ? s / n : 0;
}

/* The meter: '=' up to the fill behind a '>' head, '.' after it, quarter
   ticks and figures under it. A '-' every sixth character of the run steps
   along with each piece that arrives. */
static void hero_meter(tui_surface *sf, tui_rect clip, int x, int y, int w, float frac, uint8_t hue) {
    const float f = frac < 0 ? 0 : frac > 1 ? 1 : frac;
    const int inner = w - 2, lit = (int)(f * inner + 0.5f), phase = (int)(hero_rate.steps % 6u);
    tui_put_char(sf, clip, x, y, '[', LS_ATTR_DIM); tui_put_char(sf, clip, x + w - 1, y, ']', LS_ATTR_DIM);
    for (int i = 0; i < inner; i++) {
        char c = '.'; uint8_t at = LS_ATTR_FAINT;
        if (i < lit && (i < lit - 1 || lit == inner)) {
            const bool gap = ((i - phase) % 6 + 6) % 6 == 0;
            c = gap ? '-' : '='; at = TUI_ATTR(hue | (gap ? 0 : TUI_BRIGHT), TUI_BLACK);
        } else if (i == lit - 1) { c = '>'; at = TUI_ATTR(TUI_WHITE | TUI_BRIGHT, TUI_BLACK); }
        tui_put_char(sf, clip, x + 1 + i, y, c, at);
    }
    for (int e = 0; e <= 8; e++) {
        const int tx = x + 1 + (inner - 1) * e / 8;
        tui_put_char(sf, clip, tx, y + 1, e % 2 ? '.' : '\'', e % 2 ? LS_ATTR_FAINT : LS_ATTR_DIM);
        if (e % 2) continue;
        char n[4]; snprintf(n, sizeof(n), "%d", e * 25 / 2);
        const int len = (int)strlen(n), nx = e == 0 ? tx : e == 8 ? tx - len + 1 : tx - len / 2;
        tui_put_str(sf, clip, nx, y + 2, n, LS_ATTR_FAINT);
    }
}

/* The job's cells, one character each (several to a character on a big
   region): 'o' on the card, the spinner on the one arriving, '.' to come. */
static void hero_cells(tui_surface *sf, tui_rect clip, int x, int y, int w, int rows, unsigned have, unsigned cells, bool live, uint8_t hue) {
    if (!cells || w <= 0 || rows <= 0) return;
    const unsigned cap = (unsigned)(w * rows), per = (cells + cap - 1) / cap;
    const unsigned slots = (cells + per - 1) / per, done = (have > cells ? cells : have) / per;
    static const char SPIN[4] = { '|', '/', '-', '\\' };
    for (unsigned s = 0; s < slots; s++) {
        const int cx = x + (int)(s % (unsigned)w), cy = y + (int)(s / (unsigned)w);
        if (s < done) tui_put_char(sf, clip, cx, cy, 'o', TUI_ATTR(hue | TUI_BRIGHT, TUI_BLACK));
        else if (s == done && have < cells) tui_put_char(sf, clip, cx, cy, live ? SPIN[hero_rate.steps % 4u] : ':', live ? TUI_ATTR(TUI_WHITE | TUI_BRIGHT, TUI_BLACK) : TUI_ATTR(TUI_YELLOW | TUI_BRIGHT, TUI_BLACK));
        else tui_put_char(sf, clip, cx, cy, '.', LS_ATTR_FAINT);
    }
}

/* Speed over the last stretch as a braille trace two rows (eight dots)
   tall, two seconds to a character, newest on the right, scaled to the
   fastest second shown; a faint baseline where nothing came. */
static void hero_trace(tui_surface *sf, tui_rect clip, int x, int y, int w, uint8_t hue) {
    float top = 1;
    for (int i = 0; i < hero_rate.hist_n; i++) if (hero_rate.hist[i] > top) top = hero_rate.hist[i];
    for (int c = 0; c < w; c++) {
        unsigned hi = 0, lo = 0;
        for (int half = 0; half < 2; half++) {
            const int k = hero_rate.hist_n - (w - c) * 2 + half;
            unsigned h = k >= 0 && k < hero_rate.hist_n && hero_rate.hist[k] > 0 ? (unsigned)(hero_rate.hist[k] / top * 8.0f + 0.5f) : 0;
            if (k >= 0 && k < hero_rate.hist_n && hero_rate.hist[k] > 0 && !h) h = 1;
            lo |= ls_braille_column(h > 4 ? 4 : h, half);
            hi |= ls_braille_column(h > 4 ? h - 4 : 0, half);
        }
        const uint8_t at = TUI_ATTR(hue | TUI_BRIGHT, TUI_BLACK);
        if (hi) ls_braille_put(sf, clip, x + c, y, (int16_t)(0x2800 | hi), at);
        if (lo) ls_braille_put(sf, clip, x + c, y + 1, (int16_t)(0x2800 | lo), at);
        else tui_put_char(sf, clip, x + c, y + 1, '_', LS_ATTR_FAINT);
    }
}

/* Character art for the quiet panels, five rows each. */
static const char *const HERO_GLOBE[5] = { "  .-'''-.  ", " / |   | \\ ", "|--+---+--|", " \\ |   | / ", "  '-...-'  " };
static const char *const HERO_MAP[5]   = { " ___.___.___ ", "|  /|\\  |  /|", "| / | \\ | / |", "|/__|__\\|/__|", "             " };
static const char *const HERO_FETCH[5] = { "    _|_    ", "    \\ /    ", "  ___V___  ", " |_______| ", "           " };
static void hero_art(tui_surface *sf, tui_rect clip, int x, int y, const char *const *art, uint8_t hue) {
    for (int r = 0; r < 5; r++)
        for (int c = 0; art[r][c]; c++) {
            const char ch = art[r][c]; if (ch == ' ') continue;
            const bool joint = ch == '+' || ch == 'V' || ch == '.';
            tui_put_char(sf, clip, x + c, y + r, ch, TUI_ATTR(hue | (joint ? TUI_BRIGHT : 0), TUI_BLACK));
        }
}

static int hero_draw(tui_surface *sf, tui_rect body, int y) {
    hero_job_t j; hero_job(&j); hero_track(&j);
    char text[96];
    const int64_t now = esp_timer_get_time();
    if (j.on) {
        const uint8_t hue = j.waiting ? TUI_YELLOW : TUI_GREEN;
        const uint8_t bright = TUI_ATTR(hue | TUI_BRIGHT, TUI_BLACK), white = TUI_ATTR(TUI_WHITE | TUI_BRIGHT, TUI_BLACK);
        const bool live = hero_rate.moved_at && now - hero_rate.moved_at < 1500000;
        tui_rect box = tui_rect_make(body.x, y, body.w, j.cells ? 17 : 13);
        ls_panel_box(sf, box, j.waiting ? "WAITING" : "DOWNLOADING", hue);
        /* the RX lamp on the frame: lit while bytes arrive */
        const bool rx = hero_rate.rx_at && now - hero_rate.rx_at < 1500000;
        tui_put_str(sf, box, box.x + box.w - 7, box.y, " RX ", rx ? TUI_ATTR(TUI_BLACK, TUI_GREEN | TUI_BRIGHT) : LS_ATTR_FAINT);
        const int x = box.x + 2, inner = box.w - 4;
        /* headline: the percent on the right as a dot-matrix sign, the region's
           name on the left, or its code when the name will not fit */
        int pw = 0;
        if (j.frac >= 0) {
            snprintf(text, sizeof(text), "%d", (int)(j.frac * 100.0f));
            pw = ls_calls_big_width(text) + 2;
            const int px = x + inner - pw;
            ls_calls_big_text(sf, box, px, box.y + 1, text, bright);
            tui_put_char(sf, box, px + pw - 1, box.y + 3, '%', bright);
        }
        char up[32]; int n = 0;
        for (const char *p = j.name; *p && n < 31; p++) up[n++] = (char)toupper((unsigned char)*p);
        up[n] = 0;
        const bool sign_name = ls_calls_big_width(up) <= inner - pw - 2;
        if (sign_name || j.big[0]) ls_calls_big_text(sf, box, x, box.y + 1, sign_name ? up : j.big, white);
        else {   /* no code and too long for the sign: one bright line, clear of the percent */
            snprintf(text, sizeof(text), "%.*s", inner - pw - 2 > 0 ? inner - pw - 2 : 0, j.name);
            tui_put_str(sf, box, x, box.y + 2, text, white);
        }
        /* the kind, the code and (if the sign shows the code) the name */
        const bool named = sign_name || !j.big[0];   /* the name is already up there */
        snprintf(text, sizeof(text), "%s%s%s%s%s", j.what, j.big[0] ? "  " : "", j.big, named ? "" : "  ", named ? "" : j.name);
        snprintf(up, sizeof(up), "%.*s", inner > 0 && inner < 31 ? inner : 31, text);
        tui_put_str(sf, box, x, box.y + 4, up, LS_ATTR_DIM);
        hero_meter(sf, box, x, box.y + 6, inner, j.frac, hue);
        int ry = box.y + 10;
        if (j.cells) {
            tui_put_str(sf, box, x, ry, "CELLS", LS_ATTR_DIM);
            snprintf(text, sizeof(text), "%u/%u", j.have, j.cells);
            if ((int)strlen(text) <= 7) tui_put_str(sf, box, x, ry + 1, text, LS_ATTR_DIM);
            hero_cells(sf, box, x + 7, ry, inner - 7, 3, j.have, j.cells, live, hue);
            ry += 4;
        }
        /* speed: the trace, then the figures beside it */
        tui_put_str(sf, box, x, ry, "SPEED", LS_ATTR_DIM);
        const int tw = inner - 7 - 14 > 24 ? 24 : inner - 7 - 14;
        hero_trace(sf, box, x + 7, ry, tw, hue);
        const int fx = x + 7 + tw + 2; const double sp = hero_speed();
        if (j.waiting && card_places.message[0]) {
            snprintf(text, sizeof(text), "%.*s", inner > 0 ? inner : 0, card_places.message);
            tui_put_str(sf, box, x, ry + 2, text, bright);
        } else if (sp > 1000) {
            snprintf(text, sizeof(text), "%.1f MB/s", sp / 1e6); tui_put_str(sf, box, fx, ry, text, white);
            if (j.frac > 0 && j.bytes_total > 0) {
                const double left = (1.0 - j.frac) * j.bytes_total / sp;
                if (left < 3600) snprintf(text, sizeof(text), "%dm%02ds left", (int)left / 60, (int)left % 60);
                else snprintf(text, sizeof(text), "%.1fh left", left / 3600);
                tui_put_str(sf, box, fx, ry + 1, text, LS_ATTR_DIM);
            }
        } else tui_put_str(sf, box, fx, ry, live ? "starting" : "no data", LS_ATTR_FAINT);
        return box.y + box.h + 1;
    }
    /* idle: updates waiting, all ready, or nothing yet; each with its picture */
    int stale = 0; for (int i = 0; i < US_N; i++) if (us_state[i].known && us_state[i].outdated) stale++;
    tui_rect box = tui_rect_make(body.x, y, body.w, 7);
    const int ax = box.x + 2, tx = box.x + 16;
    const tui_rect tr = tui_rect_make(tx, box.y + 1, box.w - (tx - box.x) - 2, 5);
    const uint8_t yel = TUI_ATTR(TUI_YELLOW | TUI_BRIGHT, TUI_BLACK);
    if (card_update_n || stale) {
        const int count = card_update_n ? card_update_n : stale;
        ls_panel_box(sf, box, card_update_n ? "UPDATING" : "UPDATES READY", TUI_YELLOW);
        hero_art(sf, box, ax, box.y + 1, HERO_FETCH, TUI_YELLOW);
        snprintf(text, sizeof(text), "%d", count);
        const int end = ls_calls_big_text(sf, tr, tx, box.y + 1, text, yel);
        tui_put_str(sf, tr, end + 2, box.y + 2, card_update_n ? "waiting" : "newer", INK);
        tui_put_str(sf, tr, end + 2, box.y + 3, card_update_n ? "to update" : (count == 1 ? "region" : "regions"), LS_ATTR_DIM);
        if (card_update_n) tui_put_str(sf, tr, tx, box.y + 5, "one after another", LS_ATTR_DIM);
        else { tui_put_str(sf, tr, tx, box.y + 5, " U  UPDATE ALL ", TUI_ATTR(TUI_BLACK, TUI_YELLOW | TUI_BRIGHT)); card_update_row = box.y + 5; }
    } else if (card_n) {
        ls_panel_box(sf, box, "MAPS READY", TUI_GREEN);
        hero_art(sf, box, ax, box.y + 1, HERO_MAP, TUI_GREEN);
        ls_calls_big_text(sf, tr, tx, box.y + 1, "READY", TUI_ATTR(TUI_GREEN | TUI_BRIGHT, TUI_BLACK));
        snprintf(text, sizeof(text), "%d region%s %d place%s %d file%s", card_states, card_states == 1 ? "" : "s",
                 card_place_n, card_place_n == 1 ? "" : "s", card_files, card_files == 1 ? "" : "s");
        tui_put_str(sf, tr, tx, box.y + 4, text, INK);
        tui_put_str(sf, tr, tx, box.y + 5, "all match the server", LS_ATTR_DIM);
    } else {
        ls_panel_box(sf, box, "NO MAPS YET", TUI_CYAN);
        hero_art(sf, box, ax, box.y + 1, HERO_GLOBE, TUI_CYAN);
        tui_put_str(sf, tr, tx, box.y + 1, "GET MAPS", TUI_ATTR(TUI_CYAN | TUI_BRIGHT, TUI_BLACK));
        tui_put_str(sf, tr, tx, box.y + 2, "whole regions, free to z12", INK);
        tui_put_str(sf, tr, tx, box.y + 4, "SEARCH", TUI_ATTR(TUI_CYAN | TUI_BRIGHT, TUI_BLACK));
        tui_put_str(sf, tr, tx, box.y + 5, "the area around one place", INK);
    }
    return box.y + box.h + 1;
}

static void card_draw(tui_surface *sf,tui_rect body) {
    card_build();
    int y=body.y;
    card_update_row=-1;
    card_server_draw(sf,body,&y);
    y=hero_draw(sf,body,y);
    char text[160];
    if(catalog.sd && !card_listed) snprintf(text,sizeof(text),"ON THIS CARD   checking the region maps...");
    else snprintf(text,sizeof(text),"ON THIS CARD   %d region%s  %d place%s  %d file%s",card_states,card_states==1?"":"s",
             card_place_n,card_place_n==1?"":"s",card_files,card_files==1?"":"s");
    line(sf,body,y++,text,TUI_ATTR(TUI_WHITE|TUI_BRIGHT,TUI_BLACK));
    tui_rect storage=tui_rect_make(body.x,body.y+body.h-3,body.w,3);
    card_area=tui_rect_make(body.x,y,body.w,storage.y-y-1);
    card_visible=card_area.h/2; if(card_visible<1) card_visible=1;
    if(card_sel<card_first) card_first=card_sel;
    if(card_sel>=card_first+card_visible) card_first=card_sel-card_visible+1;
    if(!card_n) {
        line(sf,card_area,card_area.y+1,catalog.sd?"No maps on the card yet.":"No SD card. Insert a card to install maps.",LS_ATTR_DIM);
        if(catalog.sd) {
            line(sf,card_area,card_area.y+3,"GET MAPS: whole states, free to z12.",LS_ATTR_DIM);
            line(sf,card_area,card_area.y+4,"SEARCH: the area around one place.",LS_ATTR_DIM);
        }
    }
    for(int i=card_first;i<card_n && i<card_first+card_visible;i++) {
        const card_row_t r=card[i]; const int ry=card_area.y+(i-card_first)*2;
        tui_rect row=tui_rect_make(card_area.x,ry,card_area.w,2);
        if(i==card_sel) ls_fill_dither(sf,row,LS_DITHER_LIGHT,TUI_CYAN);
        const char *mark=i==card_sel?"> ":"  ";
        if(r.kind=='s') {
            const us_state_t *s=&us_state[r.index]; const bool all=s->cells && s->have>=s->cells;
            if(s->outdated) {
                snprintf(text,sizeof(text),"%s%s  %s",mark,US_TILES[r.index].st,s->name); line(sf,row,ry,text,INK);
                snprintf(text,sizeof(text),"    UPDATE  %u/%u cells older than server",s->outdated,s->cells);
                line(sf,row,ry+1,text,TUI_ATTR(TUI_YELLOW|TUI_BRIGHT,TUI_BLACK));
                continue;
            }
            snprintf(text,sizeof(text),"%s%s  %s",mark,US_TILES[r.index].st,s->name); line(sf,row,ry,text,INK);
            snprintf(text,sizeof(text),"    MAPS z0-%d%s%s  %.0f MB  %u/%u cells",us_free_z,all?"":" PART",s->detail?" + DETAIL":"",
                     (s->base+(s->detail?s->extra:0))/1e6,s->have,s->cells);
            line(sf,row,ry+1,text,all?(s->detail?TUI_ATTR(TUI_MAGENTA|TUI_BRIGHT,TUI_BLACK):GOOD):TUI_ATTR(TUI_YELLOW|TUI_BRIGHT,TUI_BLACK));
        } else if(r.kind=='p') {
            const char *name=card_places.library[r.index]; const bool queued=!strncmp(name,"[queued] ",9);
            snprintf(text,sizeof(text),"%sPLACE  %s",mark,queued?name+9:name); line(sf,row,ry,text,INK);
            line(sf,row,ry+1,queued?"    waiting in the download queue":"    the area around a searched place",queued?TUI_ATTR(TUI_YELLOW|TUI_BRIGHT,TUI_BLACK):GOOD);
        } else {
            const ls_tile_region *g=&catalog.region[r.index];
            snprintf(text,sizeof(text),"%sFILE  %s%s",mark,g->name,!strcmp(g->file,active_file)?"  [IN MAP]":""); line(sf,row,ry,text,INK);
            static const char *const SHORT[]={"NOT ON CARD","ON CARD","PARTIAL","UPDATE"};
            snprintf(text,sizeof(text),"    region file  %.1f MB  z%d-%d  %s",g->local_bytes/1e6,g->zmin,g->zmax,SHORT[g->status&3]);
            line(sf,row,ry+1,text,g->status==1?GOOD:LS_ATTR_DIM);
        }
    }
    if(card_n>card_visible) {
        snprintf(text,sizeof(text),"%d-%d of %d",card_first+1,card_first+card_visible<card_n?card_first+card_visible:card_n,card_n);
        tui_put_str(sf,body,body.x+body.w-(int)strlen(text)-1,card_area.y+card_area.h,text,LS_ATTR_DIM);
    }
    snprintf(text,sizeof(text),"SD %.2f GB free / %.2f GB used",catalog.free/1e9,(catalog.total-catalog.free)/1e9);
    line(sf,storage,storage.y,catalog.sd?text:"SD unavailable",LS_ATTR_DIM);
    ls_bar(sf,storage,1,storage.x+1,storage.w-2,catalog.total?(float)(catalog.total-catalog.free)/catalog.total:0);
}

static void draw(tui_surface *sf,tui_rect a) {
    if(ls_place_search_active()) { snprintf(hint,sizeof(hint),"%s",ls_place_search_hint());ls_place_search_draw(sf,a);return; }
    snprintf(hint,sizeof(hint),"%s",page==4?"TAP a state  ARROWS move  ENTER/G get or remove  R refresh  BACK":
             page==0?"UP/DOWN pick  ENTER manage  A open in MAP  M get maps  S search":"UP/DOWN region  ENTER info  M states  O options  BACK");
    bool copied=ls_tiles_snapshot(&catalog); ls_carto_transfer_snapshot(&transfer);
    if(map_open_done(copied))return;
    ls_carto_active_file(active_file,sizeof(active_file));
    if(transfer.state==1 && transfer.start_us!=seen_transfer) {
        seen_transfer=transfer.start_us; page=2; focus=-1; refreshed=false;
    }
    if(selected>=catalog.count) selected=catalog.count?catalog.count-1:0;
    if(page==2 && transfer.state>1 && !catalog.busy && !refreshed) { refreshed=ls_tiles_refresh(false); }
    if(page==2 && transfer.state==2 && refreshed && !catalog.busy) {
        for(int i=0;i<catalog.count;i++) if(!strcmp(catalog.region[i].file,transfer.file)) selected=i;
    }
    if(page==2 && transfer.state==0 && !catalog.busy) page=0;
    ls_panel_box(sf,a,page==1?"TILES / REGION INFO":page==2?transfer.verifying?"TILES / VERIFY":transfer.receiving?"TILES / RECEIVE":"TILES / TRANSFER":page==3?"TILES / CONFIRM":page==4?"TILES / GET MAPS":page==5?"TILES / REGION FILES":"TILES / ON THIS CARD",TUI_CYAN);
    ls_btn_t btn[5]={{.label="BACK",.key='b'},{.label="OPTIONS",.value=page==5?"REGIONS":"TILES",.key='o'},
        {.label="OPEN",.value=page==0?"IN MAP":"SELECTED MAP",.key='a',.dim=page==0?card_sel>=card_n:catalog.busy || !catalog.count || !catalog.region[selected].local_bytes},{.label="SEARCH",.value="PLACES",.key='s'}};
    btn[4]=(ls_btn_t){.label="GET MAPS",.value="US STATES",.key='m'};
    int nb=5;
    if(page==4) {
        const bool full=us_sel>=0 && us_full(&us_state[us_sel]);
        const bool more=us_sel>=0 && !full && us_state[us_sel].cells && us_state[us_sel].have>=us_state[us_sel].cells;
        const bool stale=us_sel>=0 && us_state[us_sel].outdated;
        btn[1]=(ls_btn_t){.label=stale?"UPDATE":full?"REMOVE":more?"DETAIL":"GET",.value=us_sel>=0?US_TILES[us_sel].st:"PICK A STATE",.key='g',
                          .dim=us_sel<0 || !us_state[us_sel].known};
        btn[2]=(ls_btn_t){.label="VIEW",.value=us_globe_world?"WORLD":"US",.key='w',.dim=!ls_globe_has_shapes()};
        btn[3]=(ls_btn_t){.label="REFRESH",.value="STATE LIST",.key='r'}; nb=4;
    }
    if(page==1) { btn[1]=(ls_btn_t){.label="OPEN",.value="SELECTED MAP",.key='a',.dim=!catalog.count || !catalog.region[selected].local_bytes}; btn[2]=(ls_btn_t){.label="OPTIONS",.value="REGION",.key='o'}; nb=3; }
    if(page==2) { nb=1; btn[0].label="BACK";
        if(transfer.state==2) { btn[1]=(ls_btn_t){.label="OPEN",.value="SELECTED MAP",.key='a',.dim=catalog.busy}; nb=2; } }
    if(page==3) { btn[1]=(ls_btn_t){.label="CONFIRM",.value=confirm==1?"CANCEL":"DELETE",.key='y'}; nb=2; }
    int bh=ls_btn_raised_height(a,nb); tui_rect bar=tui_rect_make(a.x+1,a.y+a.h-bh-1,a.w-2,bh);
    tui_rect body=tui_rect_make(a.x+1,a.y+2,a.w-2,a.h-bh-4);
    content_area=body;
    if(page==0) {
        card_draw(sf,body);
    } else if(page==5) {
        line(sf,body,body.y,opening?"Opening selected map...":catalog.busy?"Loading catalog / checking SD...":catalog.message,LS_ATTR_DIM);
        tui_rect storage=tui_rect_make(body.x,body.y+body.h-5,body.w,5);
        list_area=tui_rect_make(body.x,body.y+2,body.w,body.h-8); visible=list_area.h/4;
        if(body.w>90) list_area.w=body.w/2;
        if(body.w<=90 && catalog.count*4+10<list_area.h) list_area.h=catalog.count*4;
        visible=list_area.h/4;
        if(visible<1) visible=1;
        if(selected<first) first=selected;
        if(selected>=first+visible) first=selected-visible+1;
        char paging[96]; LS_SNPRINTF(t_scr_tiles_prev_d_d_regions, paging,sizeof(paging),"< PREV   %d / %d REGIONS   NEXT >",catalog.count?selected+1:0,catalog.count);
        line(sf,body,body.y+1,paging,INK);
        for(int i=first;i<catalog.count && i<first+visible;i++) {
            ls_tile_region *r=&catalog.region[i]; int y=list_area.y+(i-first)*4;
            tui_rect row=tui_rect_make(list_area.x,y,list_area.w,4);
            if(i==selected) ls_fill_dither(sf,row,LS_DITHER_LIGHT,TUI_CYAN);
            char text[200]; snprintf(text,sizeof(text),"%s%s%s",i==selected?"> ":"  ",r->name,!strcmp(r->file,active_file)?" [ACTIVE]":""); line(sf,row,y,text,INK);
            LS_SNPRINTF(t_scr_tiles_s_s_2f_mb, text,sizeof(text),"  %s%s  |  %.2f MB  z%d-%d",STATES[r->status],r->verified?" verified":"",r->bytes/1e6,r->zmin,r->zmax); line(sf,row,y+1,text,r->status==1?GOOD:LS_ATTR_DIM);
            LS_SNPRINTF(t_scr_tiles_2f_2f_to_2f, text,sizeof(text),"  %.2f,%.2f to %.2f,%.2f",r->bbox[0],r->bbox[1],r->bbox[2],r->bbox[3]); line(sf,row,y+2,text,LS_ATTR_DIM);
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
        char text[160]; LS_SNPRINTF(t_scr_tiles_sd_2f_gb_free, text,sizeof(text),"SD %.2f GB free / %.2f GB used",catalog.free/1e9,(catalog.total-catalog.free)/1e9);
        line(sf,storage,storage.y,catalog.sd?text:"SD unavailable",LS_ATTR_DIM);
        ls_bar(sf,storage,1,storage.x+1,storage.w-2,catalog.total?(float)(catalog.total-catalog.free)/catalog.total:0);
        LS_SNPRINTF(t_scr_tiles_maps_2f_mb_d, text,sizeof(text),"MAPS %.2f MB  |  %d regions",catalog.maps/1e6,catalog.count); line(sf,storage,storage.y+3,text,INK);
    } else if(page==1 && catalog.count) {
        ls_tile_region *r=&catalog.region[selected]; int y=body.y-info_scroll; char text[200];
        line(sf,body,y++,r->name,INK); line(sf,body,y++,STATES[r->status],r->status==1?GOOD:LS_ATTR_DIM); y++;
        line(sf,body,y++,r->verified?"SHA256 verified":"SHA256 unverified",r->verified?GOOD:LS_ATTR_DIM);
        line(sf,body,y++,r->file,LS_ATTR_DIM);
        LS_SNPRINTF(t_scr_tiles_size_3f_mb_on, text,sizeof(text),"Size %.3f MB  |  on SD %.3f MB",r->bytes/1e6,r->local_bytes/1e6); line(sf,body,y++,text,LS_ATTR_DIM);
        if(r->partial_bytes) { LS_SNPRINTF(t_scr_tiles_partial_download_3f_mb, text,sizeof(text),"Partial download %.3f MB",r->partial_bytes/1e6); line(sf,body,y++,text,LS_ATTR_DIM); }
        snprintf(text,sizeof(text),"Zoom %d to %d",r->zmin,r->zmax); line(sf,body,y++,text,INK); y++;
        LS_SNPRINTF(t_scr_tiles_west_east_5f_5f, text,sizeof(text),"West / East  %.5f / %.5f",r->bbox[0],r->bbox[2]); line(sf,body,y++,text,LS_ATTR_DIM);
        LS_SNPRINTF(t_scr_tiles_south_north_5f_5f, text,sizeof(text),"South / North  %.5f / %.5f",r->bbox[1],r->bbox[3]); line(sf,body,y++,text,LS_ATTR_DIM); y++;
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
        if(transfer.total && rate>0) LS_SNPRINTF(t_scr_tiles_2f_mb_s_eta, text,sizeof(text),"%.2f MB/s  |  ETA %.0f s",rate/1e6,(transfer.total-transfer.bytes)/rate);
        else LS_SNPUTS(t_scr_tiles_waiting_for_bytes, text, sizeof(text), "Waiting for bytes...");
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
            LS_SNPRINTF(t_scr_tiles_http_s_d_maps, text,sizeof(text),"http://%s:%d/maps/NAME.ctile",ip,transfer.port); line(sf,details,y++,text,INK);
            snprintf(text,sizeof(text),"Code: %s",transfer.code); line(sf,details,y++,text,INK);
            line(sf,details,y++,LS_TEXT(t_scr_tiles_put_one_ctile_file, "PUT one .ctile file; expires after 5 min"),LS_ATTR_DIM);
        }
    } else if(page==4) {
        if(card_turn_to>=0 && ls_globe_has_shapes()) card_state_view(card_turn_to);
        us_draw(sf,body);
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
        if((ch=='l' || ch=='L') && page==0 && card_server_row>=0) { card_use_live_server(); return true; }
        if((ch=='u' || ch=='U') && page==0) { card_update_all(); return true; }
        if(ch=='r' || ch=='R') { if(page==4) us_call("maps.refresh",NULL); else ls_tiles_refresh(true); return true; }
        if(ch=='m' || ch=='M') { page=4; us_sel=-1; focus=-1; card_turn_to=-1; us_globe_world=false; ls_globe_target(US_VIEW_LAT,US_VIEW_LON,US_VIEW_ZOOM); return true; }
        if((ch=='w' || ch=='W') && page==4) {
            us_globe_world=!us_globe_world;
            if(us_globe_world) ls_globe_target(US_VIEW_LAT,US_VIEW_LON,1.0f); else ls_globe_target(US_VIEW_LAT,US_VIEW_LON,US_VIEW_ZOOM);
            return true;
        }
        if((ch=='+' || ch=='=' || ch=='-') && page==4) { float la,lo,z; ls_globe_view(&la,&lo,&z); ls_globe_target(la,lo,ch=='-'?z/1.5f:z*1.5f); return true; }
        if((ch=='g' || ch=='G') && page==4) { us_get_or_remove(); return true; }
        if(ch=='v' || ch=='V') { action(&(ls_opt_t){.arg=1}); return true; }
        if(ch=='a' || ch=='A') {
            if(page==0) card_open();
            else if(!catalog.busy && catalog.count && catalog.region[selected].local_bytes) map_open();
            return true;
        }
        if((ch=='y' || ch=='Y') && page==3) {
            if(confirm==1) { ls_carto_transfer_cancel(); page=2; }
            else { ls_tiles_action(3,&catalog.region[selected]); page=0; }
            confirm=0; return true;
        }
    }
    if(page==0 && (k==LS_TK_UP || k==LS_TK_DOWN) && focus<0) {
        card_sel+=k==LS_TK_UP?-1:1; if(card_sel<0) card_sel=0; if(card_sel>=card_n) card_sel=card_n?card_n-1:0; return true;
    }
    if(page==5 && (k==LS_TK_UP || k==LS_TK_DOWN) && focus<0) {
        selected+=k==LS_TK_UP?-1:1; if(selected<0) selected=0; if(selected>=catalog.count) selected=catalog.count?catalog.count-1:0; return true;
    }
    if(page==4 && focus<0 && ls_globe_has_shapes() && (k==LS_TK_LEFT || k==LS_TK_RIGHT || k==LS_TK_UP || k==LS_TK_DOWN)) {
        float la,lo,z; ls_globe_view(&la,&lo,&z); const float step=20.0f/z;
        ls_globe_target(la+(k==LS_TK_UP?step:k==LS_TK_DOWN?-step:0),lo+(k==LS_TK_LEFT?-step:k==LS_TK_RIGHT?step:0),z);
        return true;
    }
    if(page==4 && focus<0 && (k==LS_TK_LEFT || k==LS_TK_RIGHT || k==LS_TK_UP || k==LS_TK_DOWN)) {
        if(us_sel<0) { us_sel=us_index("NH"); return true; }
        const int dc=k==LS_TK_LEFT?-1:k==LS_TK_RIGHT?1:0, dr=k==LS_TK_UP?-1:k==LS_TK_DOWN?1:0;
        int best=-1, bd=99;
        for(int i=0;i<US_N;i++) {
            const int c=US_TILES[i].c-US_TILES[us_sel].c, r=US_TILES[i].r-US_TILES[us_sel].r;
            if((dc && (c*dc<=0)) || (dr && (r*dr<=0))) continue;
            const int d=(dc?c*dc:0)+(dr?r*dr:0)+2*(dc?(r<0?-r:r):(c<0?-c:c));
            if(d<bd) { bd=d; best=i; }
        }
        if(best>=0) us_sel=best;
        return true;
    }
    if(page==4 && k==LS_TK_ENTER && focus<0) { us_get_or_remove(); return true; }
    if(page==1 && (k==LS_TK_UP || k==LS_TK_DOWN) && focus<0) {
        info_scroll+=k==LS_TK_UP?-1:1;
        if(info_scroll<0) info_scroll=0;
        if(info_scroll>info_max) info_scroll=info_max;
        return true;
    }
    if(k==LS_TK_TAB) { focus=focus<0?0:-1; return true; }
    if(focus>=0 && ls_btn_navigate(k,&slot,&focus,false)) return true;
    if(k==LS_TK_ENTER) {
        if(focus>=0) { const char *keys=page==0||page==5?"boasm":page==1?"bao":page==3?"by":page==4?"bgwr":"ba"; if(focus<(int)strlen(keys)) return key(LS_TK_CHAR,keys[focus]); }
        else if(page==0) card_manage();
        else if(page==5 && catalog.count) { page=1; info_from=5; info_scroll=0; }
        return true;
    }
    return false;
}
static bool touch(int col,int row) {
    if(ls_place_search_active())return ls_place_search_touch(col,row);
    int b=ls_btn_hit(col,row);
    if(b>=0) { const char *keys=page==0||page==5?"boasm":page==1?"bao":page==3?"by":page==4?"bgwr":"ba"; if(b<(int)strlen(keys)) key(LS_TK_CHAR,keys[b]); return true; }
    if(page==0 && card_server_row>=0 && row==card_server_row) { card_use_live_server(); return true; }
    if(page==0 && card_update_row>=0 && row==card_update_row) { card_update_all(); return true; }
    if(page==0 && tui_rect_contains(card_area,col,row)) {
        const int i=card_first+(row-card_area.y)/2; if(i<card_n) { card_sel=i; card_manage(); } return true;
    }
    if(page==5 && row==list_area.y-1) {
        selected+=col<list_area.x+list_area.w/2?-1:1;
        if(selected<0) selected=0;
        if(selected>=catalog.count) selected=catalog.count?catalog.count-1:0;
        return true;
    }
    if(page==5 && tui_rect_contains(list_area,col,row)) {
        int i=first+(row-list_area.y)/4; if(i<catalog.count) { selected=i; page=1; info_from=5; info_scroll=0; } return true;
    }
    if(page==4) {
        for(int i=0;i<US_N;i++) if(us_hit[i].w>0 && tui_rect_contains(us_hit[i],col,row)) { us_sel=i; return true; }
        if(ls_globe_has_shapes()) {
            char code[8]; float lat,lon,la,lo,z;
            const int level=ls_globe_hit(col,row,code,&lat,&lon);
            ls_globe_view(&la,&lo,&z);
            if(level==1) {           /* a state: pick it and bring it to the middle */
                const int i=us_index(code+3); if(i>=0) us_sel=i;
                float cl,cn; if(ls_globe_region(code,NULL,0,&cl,&cn)) ls_globe_target(cl,cn,z<US_VIEW_ZOOM?US_VIEW_ZOOM:z);
            } else if(level==0) {    /* a country: turn to it and come closer */
                ls_globe_target(lat,lon,z*1.6f); us_globe_world=false;
            }
        }
        return true;
    }
    if(page==1 && info_max) return key(row<content_area.y+content_area.h/2?LS_TK_UP:LS_TK_DOWN,0);
    return true;
}
const ls_tui_screen_t ls_scr_tiles={.name="TILES",.hint=hint,.enter=enter,.leave=ls_place_search_close,.draw=draw,.key=key,.touch=touch};
