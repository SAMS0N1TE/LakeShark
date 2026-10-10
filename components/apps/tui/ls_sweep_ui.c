#include "ls_sweep_ui.h"
#include "ls_sweep_app.h"
#include "ls_rid.h"
#include "ls_options.h"
#include "ls_gps.h"
#include "ls_glyph.h"
#include "ls_theme.h"
#include <stdlib.h>
#include "ls_sweep_scope.h"
#include "esp_attr.h"
#include "esp_timer.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

/* SWEEP screen.

   Palette roles, kept strict so colour always means something:
     chrome    dim cyan: frames, rules, joints (the house frame colour)
     words     bright white for values, dim white for labels and notes
     category  one hue per category, only on its spine, ID letter, label
               and history strip (CAM yellow, BWC magenta, UAS cyan,
               TRK green, ATK red)
     heat      the HUNT gauge alone: cold blue, cyan, yellow, hot red
     alert     red inverse, only for a GPS-backed WITH YOU and a flood
     freshness brightness: bright under 2 s, normal after, faint after 10 s

   Grid: one table geometry (cols_t) is computed per frame from the area
   and every panel, rule and row hangs off its left edge and width, so the
   header box, section rules, columns and HUNT panels share both edges. */

static EXT_RAM_BSS_ATTR ls_sweep_device_t view[LS_SWEEP_CAP];
static EXT_RAM_BSS_ATTR ls_rid_drone_t drones[LS_RID_MAX];
/* Capacity-sized rendering scratch belongs in PSRAM, not the UI task stack. */
static EXT_RAM_BSS_ATTR int row_device[128];

static unsigned count,pick;
static uint32_t selected;
static bool owner_selected;
static ls_sweep_device_t near_target;
static tui_rect list_area;
typedef enum { VIEW_LIST, VIEW_HUNT } sweep_view_t;
static sweep_view_t screen_view; /* Retained across app visits; no frame-time NVS. */
static const char *const view_names[]={"LIST","HUNT"};
static unsigned list_shown;
static bool slow;
static int mode_row=-1; /* HUNT: the averaging line, which toggles FAST/SLOW on a tap */
__attribute__((weak)) bool ls_sweep_find_target(const uint8_t *mac,uint8_t radio,const char *label) {(void)mac;(void)radio;(void)label;return false;}
static const char *const modes[]={"OFF","SOUND","MUTED"};
static const char *const filters[]={"ALL","CAMERA","BODYCAM","DRONE","TRACKER","ATTACK"};
static int get(const ls_opt_t *o) {
    ls_sweep_settings_t s;ls_sweep_settings_get(&s);
    if(o->arg<SW_CATS) return !(s.enabled&(1<<o->arg))?0:s.muted&(1<<o->arg)?2:1;
    return o->arg==5?s.logging:o->arg==6?s.receive_only:s.filter;
}
static void set(const ls_opt_t *o,int v) {
    ls_sweep_settings_t s;ls_sweep_settings_get(&s);
    if(o->arg<SW_CATS) {s.enabled=(s.enabled&~(1<<o->arg))|(v?1<<o->arg:0);s.muted=(s.muted&~(1<<o->arg))|(v==2?1<<o->arg:0);}
    else if(o->arg==5) s.logging=v;else if(o->arg==7) s.filter=v;
    ls_sweep_settings_set(&s);
}
static void reload(const ls_opt_t *o) {(void)o;ls_sweep_reload_request();}
static const char *receive_only(const ls_opt_t *o) {(void)o;return "SWEEP always listens passively; no transmissions";}
static const ls_opt_t cats[]={
    {.label="CAMERA",.kind=LS_OPT_CYCLE,.arg=0,.names=modes,.n=3,.get=get,.set=set},
    {.label="BODYCAM",.kind=LS_OPT_CYCLE,.arg=1,.names=modes,.n=3,.get=get,.set=set},
    {.label="DRONE",.kind=LS_OPT_CYCLE,.arg=2,.names=modes,.n=3,.get=get,.set=set},
    {.label="TRACKER",.kind=LS_OPT_CYCLE,.arg=3,.names=modes,.n=3,.get=get,.set=set},
    {.label="ATTACK (OPT IN)",.kind=LS_OPT_CYCLE,.arg=4,.names=modes,.n=3,.get=get,.set=set}
};
static const ls_opt_ctx_t cats_ctx={.name="CATEGORIES",.job=-1,.radio=LS_RSEL_NONE,LS_OPT_ROWS(cats)};
static const ls_opt_t privacy[]={
    {.label="LOG ALERTS TO SD",.kind=LS_OPT_TOGGLE,.arg=5,.get=get,.set=set},
    {.label="RECEIVE ONLY",.kind=LS_OPT_TOGGLE,.arg=6,.get=get,.set=set,.why_not=receive_only}
};
static const ls_opt_ctx_t privacy_ctx={.name="PRIVACY",.job=-1,.radio=LS_RSEL_NONE,LS_OPT_ROWS(privacy)};
static const ls_opt_t display[]={
    {.label="FILTER",.kind=LS_OPT_CYCLE,.arg=7,.names=filters,.n=6,.get=get,.set=set}
};
static const ls_opt_ctx_t display_ctx={.name="DISPLAY",.job=-1,.radio=LS_RSEL_NONE,LS_OPT_ROWS(display)};
static const ls_opt_t opts[]={
    {.label="CATEGORIES / MUTE",.kind=LS_OPT_MENU,.sub=&cats_ctx},
    {.label="PRIVACY / LOGGING",.kind=LS_OPT_MENU,.sub=&privacy_ctx},
    {.label="DISPLAY / FILTER",.kind=LS_OPT_MENU,.sub=&display_ctx},
    {.label="RELOAD SD RULES",.kind=LS_OPT_ACTION,.act=reload}
};
static const ls_opt_ctx_t ctx={.name="SWEEP",.job=-1,.radio=LS_RSEL_NONE,.back_to_screen=true,LS_OPT_ROWS(opts)};

/* ------------------------------------------------------------ palette -- */

#define FRESH_US INT64_C(2000000)
#define STALE_US INT64_C(10000000)
#define INK(c)   TUI_ATTR((c),TUI_BLACK)
#define CHROME   INK(TUI_CYAN)
#define VALUE    INK(TUI_WHITE|TUI_BRIGHT)
#define NOTE     LS_ATTR_DIM
#define GHOST    LS_ATTR_FAINT
#define ALERT    TUI_ATTR(TUI_WHITE|TUI_BRIGHT,TUI_RED)
static const uint8_t cat_hue[SW_CATS]={TUI_YELLOW,TUI_MAGENTA,TUI_CYAN,TUI_GREEN,TUI_RED};
static const uint8_t zone_hue[4]={TUI_BLUE,TUI_CYAN,TUI_YELLOW,TUI_RED};

static uint8_t category_ink(unsigned cat,int64_t age) {
    uint8_t c=cat<SW_CATS?cat_hue[cat]:TUI_WHITE;
    return INK(c|(age<FRESH_US?TUI_BRIGHT:0));
}
/* Received strength on the -100..-35 dBm scale every meter here shares. */
static float scale01(float rssi) {float v=(rssi+100.0f)/65.0f;return !isfinite(v)||v<0?0:v>1?1:v;}
static int zone(float rssi) {int z=(int)(scale01(rssi)*4);return z>3?3:z;}
/* Heat ink. Daylight's bright blue and bright cyan are both near-navy on
   white, so its cool end steps down to plain teal to stay distinct. */
static uint8_t zone_ink(int z,bool lit) {
    static const uint8_t day[4]={TUI_BLUE|TUI_BRIGHT,TUI_CYAN,TUI_YELLOW,TUI_RED};
    if(ls_tui_daylight()) return INK(day[z]);
    return INK(zone_hue[z]|(lit?TUI_BRIGHT:0));
}

/* -------------------------------------------------------------- cells -- */

static void put(tui_surface *sf,tui_rect clip,int x,int y,int16_t ch,uint8_t at) {
    clip=tui_rect_intersect(clip,tui_surface_rect(sf));
    if(tui_rect_contains(clip,x,y)) sf->back[y*sf->w+x]=(tui_cell){ch,at};
}
static void str(tui_surface *sf,tui_rect clip,int x,int y,int w,const char *s,uint8_t at) {
    if(w>0) tui_put_str(sf,tui_rect_intersect(clip,tui_rect_make(x,y,w,1)),x,y,s,at);
}
static void centre(tui_surface *sf,tui_rect clip,int x,int w,int y,const char *s,uint8_t at) {
    int n=(int)strlen(s);if(n>w) n=w;
    str(sf,clip,x+(w-n)/2,y,n,s,at);
}
static void right(tui_surface *sf,tui_rect clip,int x,int w,int y,const char *s,uint8_t at) {
    int n=(int)strlen(s);if(n>w) n=w;
    str(sf,clip,x+w-n,y,n,s,at);
}
/* A house panel: dim cyan frame, inverse title chip on the left of the top
   edge, optional quiet text on the right of it. */
static void panel(tui_surface *sf,tui_rect clip,tui_rect r,const char *chip,const char *note,uint8_t note_at) {
    if(r.w<8 || r.h<2) return;
    tui_box(sf,tui_rect_intersect(r,clip),NULL,CHROME);
    if(chip) str(sf,clip,r.x+2,r.y,r.w-4,chip,TUI_ATTR(TUI_BLACK,TUI_CYAN|TUI_BRIGHT));
    if(note && *note) {
        char t[64];snprintf(t,sizeof(t)," %s ",note);
        int n=(int)strlen(t),room=r.w-4-(chip?(int)strlen(chip)+2:0);
        if(n<=room) str(sf,clip,r.x+r.w-2-n,r.y,n,t,note_at);
    }
}
/* A full-width rule with its title centred on it. Empty sections recede. */
static void rule(tui_surface *sf,tui_rect clip,int x,int w,int y,const char *title,unsigned n,bool alarm) {
    for(int i=0;i<w;i++) tui_put_char(sf,clip,x+i,y,'-',CHROME);
    char num[12];snprintf(num,sizeof(num),"%u",n);
    int tl=(int)strlen(title),nl=(int)strlen(num),total=tl+nl+4,xs=x+(w-total)/2;
    str(sf,clip,xs,y,total,"                ",CHROME);
    str(sf,clip,xs+1,y,tl,title,alarm?INK(TUI_RED|TUI_BRIGHT):n?VALUE:NOTE);
    str(sf,clip,xs+tl+3,y,nl,num,NOTE);
}

/* ------------------------------------------------------------- layout -- */

/* Column origins, all absolute. `wide` puts a contact on one line with its
   history beside it; otherwise the history sits on a second line, under
   the meter, so the strip reads as that meter's last minute. */
typedef struct {
    int x,w,id,label,label_w,state,meter,segs,dbm,trend,age,strip,sub,sub_w;
    bool wide;
} cols_t;
static cols_t layout(tui_rect a) {
    cols_t c={0};
    int avail=a.w-2;
    c.segs=12;
    if(avail>=99) {c.wide=true;c.label_w=avail-79>28?28:avail-79;}
    else {
        c.label_w=avail-35;
        if(c.label_w<8) {c.segs=6;c.label_w=avail-29;}
        if(c.label_w>24) c.label_w=24;
        if(c.label_w<4) c.label_w=4;
    }
    int w=35+c.label_w-(12-c.segs)+(c.wide?44:0);
    c.x=a.x+(a.w-w)/2;if(c.x<a.x) c.x=a.x;
    c.w=w;
    /* The spine is a left half-block, so the ID can sit hard against it. */
    c.id=c.x+1;c.label=c.id+5;c.state=c.label+c.label_w+1;
    c.meter=c.state+6;c.dbm=c.meter+c.segs+1;c.trend=c.dbm+4;c.age=c.trend+2;
    if(c.wide) {c.strip=c.age+5;c.sub=c.strip+14;c.sub_w=c.x+c.w-c.sub;}
    else {c.strip=c.meter+c.segs-12;c.sub=c.id;c.sub_w=c.strip-2-c.sub;}
    return c;
}
/* Cut a label at a word boundary when it does not fit its column. */
static void fit(char *out,size_t n,const char *s,int w) {
    snprintf(out,n,"%s",s);
    if(w<=0 || (int)strlen(out)<=w) return;
    out[w]=0;
    char *sp=strrchr(out,' ');
    if(sp && sp-out>=w*3/5) *sp=0;
}

/* ------------------------------------------------------------- meters -- */

/* Twelve thin bars rising like a signal wedge: lit length is strength,
   ink brightness is freshness, unlit bars leave a faint baseline. */
static void meter(tui_surface *sf,tui_rect clip,int x,int y,int segs,int rssi,int64_t age) {
    int lit=(int)ceilf(scale01((float)rssi)*segs);
    uint8_t at=age<FRESH_US?VALUE:age<STALE_US?INK(TUI_WHITE):GHOST;
    for(int k=0;k<segs;k++) {
        int e=segs>1?2+k*6/(segs-1):8;
        tui_put_char(sf,clip,x+k,y,k<lit?LS_TUI_TRACE(e):LS_TUI_TRACE(1),k<lit?at:GHOST);
    }
}
static const uint8_t dot_l[4]={0x40,0x04,0x02,0x01},dot_r[4]={0x80,0x20,0x10,0x08};
static int bucket_level(int r,int levels) {
    int v=1+(r+100)*(levels-1)/65;return v<1?1:v>levels?levels:v;
}
/* 24 x 2.5 s buckets in twelve braille cells, two buckets to a cell, each a
   column of up to four dots. A bucket with no advert stays blank. */
static void strip(tui_surface *sf,tui_rect clip,int x,int y,const ls_sweep_device_t *d,int64_t now,uint8_t at) {
    for(int cell=0;cell<12;cell++) {
        unsigned bits=0;
        for(int half=0;half<2;half++) {
            int r=ls_sweep_bucket(d,now,23-(cell*2+half));
            if(r==-128) continue;
            for(int k=0,lv=bucket_level(r,4);k<lv;k++) bits|=half?dot_r[k]:dot_l[k];
        }
        put(sf,clip,x+cell,y,bits?(int16_t)(0x2800|bits):' ',at);
    }
}
/* HUNT: one bucket per `pitch` cells, two rows tall, eight dot levels. */
static void strip_tall(tui_surface *sf,tui_rect clip,int x,int y,int pitch,const ls_sweep_device_t *d,int64_t now,uint8_t at) {
    for(int b=0;b<24;b++) {
        int r=ls_sweep_bucket(d,now,23-b);
        unsigned lo=0,hi=0;
        if(r!=-128) for(int k=0,lv=bucket_level(r,8);k<lv;k++) {if(k<4) lo|=dot_l[k]|dot_r[k];else hi|=dot_l[k-4]|dot_r[k-4];}
        for(int p=0;p<pitch;p++) {
            put(sf,clip,x+b*pitch+p,y,hi?(int16_t)(0x2800|hi):' ',at);
            put(sf,clip,x+b*pitch+p,y+1,lo?(int16_t)(0x2800|lo):' ',at);
        }
    }
}
static char trend(const ls_sweep_device_t *d,int64_t now) {
    float slope=ls_sweep_trend(d,now);return !isfinite(slope)?' ':slope>3?'^':slope<-3?'v':'=';
}
static void bearing(tui_surface *sf,tui_rect clip,int x,int w,int y,const char *name,double lat,double lon,const ls_gps_state_t *gps) {
    const double rad=.0174532925199433;
    double l1=gps->lat_deg*rad,l2=lat*rad,dl=(lon-gps->lon_deg)*rad;
    double h=pow(sin((l2-l1)/2),2)+cos(l1)*cos(l2)*pow(sin(dl/2),2);
    double distance=6371000*2*atan2(sqrt(h),sqrt(fmax(0,1-h)));
    double angle=fmod(atan2(sin(dl)*cos(l2),cos(l1)*sin(l2)-sin(l1)*cos(l2)*cos(dl))/rad+360,360);
    /* Same two columns as the TARGET facts it is listed with. */
    char t[64];snprintf(t,sizeof(t),"%03.0f deg  %.0f m  GPS",angle,distance);
    str(sf,clip,x,y,9,name,NOTE);str(sf,clip,x+10,y,w-10,t,VALUE);
}
/* Stroke figures from the house glyph set, plus a minus bar. */
static int big_width(const char *s,int sx) {int n=(int)strlen(s);return n?n*3*sx+(n-1)*sx:0;}
static void big(tui_surface *sf,tui_rect clip,int x,int y,const char *s,int sx,int sy,uint8_t at) {
    for(int i=0;s[i];i++,x+=4*sx) {
        if(s[i]=='-') {for(int r=0;r<sy;r++) for(int k=0;k<3*sx;k++) tui_put_char(sf,clip,x+k,y+2*sy+r,LS_TUI_SHADE_FULL,at);}
        else if(ls_glyph_has(s[i])) ls_glyph_draw(sf,tui_rect_make(x-sx,y-(sy>1),5*sx,6*sy),s[i],at);
    }
}

/* ------------------------------------------------------------- header -- */

/* Instrument strip: five category counters, a joint, adverts/s and GPS. */
static int header(tui_surface *sf,tui_rect a,const cols_t *c,const ls_sweep_settings_t *s,const ls_sweep_status_t *st,bool fix,int64_t now) {
    tui_rect box=tui_rect_make(c->x,a.y,c->w,4);
    panel(sf,a,box," SWEEP ",NULL,0);
    unsigned n[SW_CATS]={0};for(unsigned i=0;i<count;i++) if(view[i].match.category<SW_CATS) n[view[i].match.category]++;
    static const unsigned order[]={SW_TRACKER,SW_CAMERA,SW_BODYCAM,SW_DRONE,SW_ATTACK};
    static const char *const code[]={"TRK","CAM","BWC","UAS","ATK"};
    /* Five equal category cells, a joint, then two receiver cells sharing
       what is left, so the divider never touches a label. */
    int iw=box.w-2,cw=(iw-1)/7,x0=box.x+1,rw=iw-1-5*cw,r1=rw/2,r2=rw-r1;
    char t[40];
    for(int j=0;j<5;j++) {
        unsigned cat=order[j];bool off=cat==SW_ATTACK && !(s->enabled&(1<<SW_ATTACK));
        centre(sf,a,x0+j*cw,cw,box.y+1,code[j],INK(cat_hue[cat]));
        snprintf(t,sizeof(t),"%u",n[cat]);
        centre(sf,a,x0+j*cw,cw,box.y+2,off?"off":t,off||!n[cat]?NOTE:INK(cat_hue[cat]|TUI_BRIGHT));
    }
    int jx=x0+5*cw;
    tui_put_char(sf,a,jx,box.y,'+',CHROME);tui_put_char(sf,a,jx,box.y+3,'+',CHROME);
    tui_put_char(sf,a,jx,box.y+1,'|',CHROME);tui_put_char(sf,a,jx,box.y+2,'|',CHROME);
    centre(sf,a,jx+1,r1,box.y+1,"ADV/S",NOTE);
    snprintf(t,sizeof(t),st->adverts_s>=999.5f?"%.0f":"%.1f",st->adverts_s);centre(sf,a,jx+1,r1,box.y+2,t,VALUE);
    centre(sf,a,jx+1+r1,r2,box.y+1,"GPS",NOTE);
    centre(sf,a,jx+1+r1,r2,box.y+2,fix?"FIX":"--",fix?VALUE:NOTE);
    /* View and sound on the bottom edge, left; receiver chips on the right. */
    snprintf(t,sizeof(t)," %s / %s ",view_names[screen_view],s->alerts_muted?"MUTED":"SOUND");
    if(box.x+2+(int)strlen(t)<jx) str(sf,a,box.x+2,box.y+3,(int)strlen(t),t,NOTE);
    int xr=box.x+box.w-2;
    if(st->flood) {xr-=7;str(sf,a,xr,box.y+3,7," FLOOD ",ALERT);xr-=1;}
    if(now-st->last_advert_us>=FRESH_US && (count || st->owner_nearby)) {
        snprintf(t,sizeof(t)," QUIET %lds ",(long)((now-st->last_advert_us)/1000000));
        int l=(int)strlen(t);str(sf,a,xr-l,box.y+3,l,t,NOTE);
    }
    return box.y+box.h;
}

/* --------------------------------------------------------------- LIST -- */

static void sub_text(char *t,size_t n,const ls_sweep_device_t *d,unsigned section,int64_t now,bool fix,bool *badge) {
    int64_t age=now-d->seen_us;*badge=false;
    if(section==SW_WITH_YOU) {
        if(age>=60000000) {snprintf(t,n,"LOST  unheard %ldm",(long)(age/60000000));return;}
        long m=(long)((now-d->separated_us)/60000000);
        char moved[24];
        if(d->places) snprintf(moved,sizeof(moved),"%u places",d->places);
        else if(d->moved_m>=1000) snprintf(moved,sizeof(moved),"moved %.1f km",d->moved_m/1000);
        else snprintf(moved,sizeof(moved),"moved %.0f m",d->moved_m);
        if(ls_sweep_alarm(d,fix)) {*badge=true;snprintf(t,n,"%ldm  %s",m,moved);return;}
        if(fix) snprintf(t,n,"with you %ldm  %s",m,moved);
        else snprintf(t,n,"with you %ldm (time only)",m);
        return;
    }
    if(d->places) snprintf(t,n,"%s  %u places",d->match.vendor,d->places);
    else snprintf(t,n,"%s",d->match.vendor);
}
static int draw_row(tui_surface *sf,tui_rect a,const cols_t *c,const ls_sweep_device_t *d,int y,int64_t now,bool fix,unsigned section,bool sel) {
    int64_t age=now-d->seen_us;uint8_t hue=category_ink(d->match.category,age);
    bool two=!c->wide && section!=SW_PASSING;
    char t[64],id[16];ls_sweep_track_id(d,id,sizeof(id));
    /* Spine: a half-block tab in the category hue down the entry, its last
       line cut short so neighbours of one category never run together. */
    if(two) tui_put_char(sf,a,c->x,y,LS_TUI_BLOCK_LEFT,hue);
    tui_put_char(sf,a,c->x,y+(two?1:0),LS_TUI_SEXT(0x05),hue);
    /* Selection is the house inverse, on the ID alone. */
    if(sel) str(sf,a,c->id,y,4,id,TUI_ATTR(TUI_BLACK,TUI_ATTR_FG(hue)|TUI_BRIGHT));
    else {str(sf,a,c->id,y,4,id,NOTE);put(sf,a,c->id,y,id[0],hue);}
    fit(t,sizeof(t),d->match.label,c->label_w);
    str(sf,a,c->label,y,c->label_w,t,hue);
    str(sf,a,c->state,y,4,d->match.state==SW_STATE_SEPARATED?"sep":d->match.state==SW_STATE_NEAR?"near":" -",NOTE);
    meter(sf,a,c->meter,y,c->segs,d->rssi,age);
    snprintf(t,sizeof(t),"%d",d->rssi<-99?-99:d->rssi);
    right(sf,a,c->dbm,3,y,t,age<STALE_US?VALUE:NOTE);
    tui_put_char(sf,a,c->trend,y,trend(d,now),VALUE);
    long seconds=(long)((now-d->first_us)/1000000);
    if(seconds<60) snprintf(t,sizeof(t),"%lds",seconds);
    else if(seconds<3600) snprintf(t,sizeof(t),"%ldm",seconds/60);
    else snprintf(t,sizeof(t),"%ldh",seconds/3600>99?99:seconds/3600);
    right(sf,a,c->age,3,y,t,NOTE);
    if(section==SW_PASSING && !c->wide) return 1;
    int sy=c->wide?y:y+1;
    if(section!=SW_PASSING) strip(sf,a,c->strip,sy,d,now,hue);
    bool badge;sub_text(t,sizeof(t),d,section,now,fix,&badge);
    int x=c->sub,w=c->sub_w;
    /* The one alarm on this screen: GPS-backed WITH YOU, red inverse. It
       sits under the numbers in portrait, ahead of the detail in landscape. */
    if(badge) {
        if(c->wide) {str(sf,a,x,sy,10," WITH YOU ",ALERT);x+=11;w-=11;}
        else str(sf,a,c->dbm-1,sy,10," WITH YOU ",ALERT);
    }
    str(sf,a,x,sy,w,t,NOTE);
    return two?2:1;
}
/* Concentric listening rings in braille, sized in pixels so they stay round
   on any font. Static: nothing here moves unless the receiver changes. */
static void rings(tui_surface *sf,tui_rect clip,int cx,int y,int cols,int rows,bool live) {
    int cw=11,ch=18;ls_tui_geometry(NULL,NULL,&cw,&ch);
    if(cw<4 || ch<4) {cw=11;ch=18;}
    float half_w=cols*cw/2.0f,half_h=rows*ch/2.0f,R[3]={half_h*.24f,half_h*.58f,half_h*.92f};
    static const uint8_t bit[4][2]={{0x01,0x08},{0x02,0x10},{0x04,0x20},{0x40,0x80}};
    const uint8_t hue[4]={live?INK(TUI_WHITE|TUI_BRIGHT):NOTE,live?INK(TUI_CYAN|TUI_BRIGHT):GHOST,live?CHROME:GHOST,GHOST};
    int x0=cx-cols/2;
    for(int j=0;j<rows;j++) for(int i=0;i<cols;i++) {
        unsigned bits=0;int best=3;
        for(int dy=0;dy<4;dy++) for(int dx=0;dx<2;dx++) {
            float px=(i*2+dx+.5f)*cw/2-half_w,py=(j*4+dy+.5f)*ch/4-half_h,r=sqrtf(px*px+py*py);
            int ring=r<cw*.45f?0:-1;
            for(int k=0;k<3 && ring<0;k++) if(fabsf(r-R[k])<cw*.30f) ring=k+1;
            if(ring<0) continue;
            bits|=bit[dy][dx];if(ring<best) best=ring;
        }
        if(bits) put(sf,clip,x0+i,y+j,(int16_t)(0x2800|bits),hue[best]);
    }
}
static void quiet(tui_surface *sf,tui_rect a,const cols_t *c,int y,const ls_sweep_status_t *st,int64_t now) {
    bool live=st->requested;
    const char *word=live?"QUIET":"OFF";
    char t[64];
    int room=a.y+a.h-y,ring_rows=room>=34?9:room>=24?7:5,body=ring_rows+1+5+2+1+1,box_h=body+4;
    int box_w=c->w<38?c->w:38;
    if(a.y+a.h-y<box_h) {box_h=a.y+a.h-y;ring_rows=0;}
    tui_rect box=tui_rect_make(c->x+(c->w-box_w)/2,y+(a.y+a.h-y-box_h)/2,box_w,box_h);
    panel(sf,a,box,live?" RECEIVER ":" STOPPED ",NULL,0);
    int yy=box.y+2,cx=box.x+box.w/2;
    if(ring_rows) {rings(sf,a,cx,yy,ring_rows*2+3,ring_rows,live);yy+=ring_rows+1;}
    big(sf,a,cx-big_width(word,1)/2,yy,word,1,1,live?VALUE:NOTE);yy+=6;
    if(live) snprintf(t,sizeof(t),"no adverts for %lds",(long)((now-st->last_advert_us)/1000000));
    else snprintf(t,sizeof(t),"receiver stopped");
    centre(sf,a,box.x+1,box.w-2,yy,t,VALUE);
    centre(sf,a,box.x+1,box.w-2,yy+2,live?"BLE + Wi-Fi  passive receive":"START to listen",NOTE);
}
static void draw_list(tui_surface *sf,tui_rect a,const cols_t *c,int y,const ls_sweep_status_t *st,bool fix,int64_t now) {
    if(!count && !st->owner_nearby) {quiet(sf,a,c,y,st,now);return;}
    /* Column heads, quiet, over the columns they name. */
    str(sf,a,c->id,y,4,"ID",NOTE);str(sf,a,c->label,y,c->label_w,"CONTACT",NOTE);
    str(sf,a,c->state,y,5,"STATE",NOTE);str(sf,a,c->meter,y,c->segs,c->segs>=12?"SIGNAL":"SIG",NOTE);
    right(sf,a,c->dbm,3,y,"dBm",NOTE);right(sf,a,c->age,3,y,"AGE",NOTE);
    if(c->wide) {str(sf,a,c->strip,y,12,"LAST 60 s",NOTE);str(sf,a,c->sub,y,c->sub_w,"DETAIL",NOTE);}
    y+=2;
    static const char *const sections[]={"WITH YOU","NEW","PASSING"};
    unsigned in[3]={0};bool alarm=false;
    for(unsigned i=0;i<count;i++) {unsigned s=ls_sweep_section(&view[i],now,fix);in[s]++;if(s==SW_WITH_YOU && ls_sweep_alarm(&view[i],fix)) alarm=true;}
    /* A page starts at the selected rank when the list cannot fit. */
    unsigned start=0;if(selected && !owner_selected && pick>3) start=pick-3;
    int bottom=a.y+a.h;
    /* Breathing room between sections only when everything fits; a page
       that cannot show it all keeps its last line for the count. */
    int need=3+(st->owner_nearby?1+(in[SW_PASSING]?1:0):0);
    for(unsigned i=start;i<count;i++) need+=c->wide || ls_sweep_section(&view[i],now,fix)==SW_PASSING?1:2;
    int gap=need+2<=bottom-y?1:0;
    bool paged=start>0 || need>bottom-y;
    if(paged) bottom--;
    for(unsigned section=0;section<3 && y<bottom;section++) {
        rule(sf,a,c->x,c->w,y++,sections[section],in[section],section==SW_WITH_YOU && alarm);
        for(unsigned i=start;i<count && y<bottom;i++) {
            ls_sweep_device_t *d=&view[i];if(ls_sweep_section(d,now,fix)!=section) continue;
            int need=c->wide || section==SW_PASSING?1:2;
            if(y+need>bottom) {y=bottom;break;}
            int used=draw_row(sf,a,c,d,y,now,fix,section,d->serial==selected && !owner_selected);
            for(int r=0;r<used;r++) if(y+r-a.y>=0 && y+r-a.y<128) row_device[y+r-a.y]=(int)i;
            list_shown++;y+=used;
        }
        if(section==SW_PASSING && st->owner_nearby && y+1<bottom) {
            /* A footnote: one quiet line set apart from the rows above it. */
            char t[64];snprintf(t,sizeof(t),"owner-nearby tags: %u (rotating, not tracked)",st->owner_nearby);
            if(in[SW_PASSING] && gap) y++;
            if(owner_selected) tui_put_char(sf,a,c->x,y,'>',VALUE);
            str(sf,a,c->id,y,c->x+c->w-c->id,t,NOTE);
            if(y-a.y>=0 && y-a.y<128) row_device[y-a.y]=-2;
            y++;
        }
        if(section<2) y+=gap;
    }
    if(paged && bottom>=a.y) {
        /* The page footer is a rule like the section heads, naming what is
           off the page above and below. */
        unsigned below=count-start-list_shown;char t[48];
        if(start && below) snprintf(t,sizeof(t)," %u above  %u below ",start,below);
        else if(start) snprintf(t,sizeof(t)," %u above ",start);
        else snprintf(t,sizeof(t)," %u below ",below);
        for(int i=0;i<c->w;i++) tui_put_char(sf,a,c->x+i,bottom,'-',CHROME);
        centre(sf,a,c->x,c->w,bottom,t,NOTE);
    }
}

/* --------------------------------------------------------------- HUNT -- */

/* Cold-to-hot VFD gauge: 16 segments in four heat zones, the 15 s peak held
   in white, a zone-coloured scale under it. Returns rows used. */
static int gauge(tui_surface *sf,tui_rect a,int x,int w,int y,float value,float peak,bool peak_live) {
    const int segs=16,pitch=w>=46?2:1,span=segs*pitch;
    int gx=x+(w-span)/2;
    str(sf,a,gx-6,y,4,"COLD",zone_ink(0,true));
    str(sf,a,gx+span+2,y,3,"HOT",zone_ink(3,true));
    int lit=(int)ceilf(scale01(value)*segs),held=peak_live?(int)ceilf(scale01(peak)*segs)-1:-1;
    if(!isfinite(value)) lit=0;
    for(int k=0;k<segs;k++) {
        uint8_t at=k<lit?zone_ink(k*4/segs,true):k==held?VALUE:GHOST;
        for(int r=0;r<2;r++) for(int p=0;p<pitch;p++)
            tui_put_char(sf,a,gx+k*pitch+p,y+r,p<pitch-1||pitch==1?LS_TUI_BLOCK_FULL:LS_TUI_BLOCK_LEFT,at);
    }
    /* Zone boundaries: -100 -84 -67 -51 -35, each in the hue it opens. */
    for(int z=0;z<=4;z++) {
        char t[8];snprintf(t,sizeof(t),"%d",-100+z*65/4);
        int n=(int)strlen(t),bx=gx+z*span/4-(z==4?(pitch==2?1:0):0),tx=bx-n/2;
        str(sf,a,tx,y+2,n,t,zone_ink(z>3?3:z,false));
    }
    return 3;
}
/* Two aligned columns of facts about the target: quiet label, bright value. */
static void fact(tui_surface *sf,tui_rect clip,int x,int w,int y,const char *label,const char *value) {
    str(sf,clip,x,y,9,label,NOTE);str(sf,clip,x+10,y,w-10,value,VALUE);
}
static void draw_hunt(tui_surface *sf,tui_rect a,const cols_t *c,int y,const ls_sweep_device_t *d,bool fix,int64_t now) {
    char id[16],t[96];
    if(owner_selected) snprintf(id,sizeof(id),"near-owner");else ls_sweep_track_id(d,id,sizeof(id));
    int64_t age=now-d->seen_us;
    float value=slow?d->slow:d->fast,delta=ls_sweep_hunt_trend(d,now,slow);
    float pk=slow?d->slow_peak:d->peak;bool pk_live=now-(slow?d->slow_peak_us:d->peak_us)<15000000;
    bool near=owner_selected || d->match.state==SW_STATE_NEAR;
    size_t nd=0;const ls_rid_drone_t *drone=NULL;
    if(d->match.category==SW_DRONE && fix) {
        nd=ls_rid_snapshot(drones,LS_RID_MAX,now);
        for(size_t j=0;j<nd;j++) if(!memcmp(drones[j].mac,d->mac,6) && drones[j].address_type==d->address_type) {drone=&drones[j];break;}
    }
    int extra=(near?1:0)+(drone?2:0);
    int bottom=a.y+a.h,avail=bottom-y;
    /* Portrait stacks hero, history and facts; landscape puts the hero on
       the left and the other two in a column on the right. Either way the
       group sits a little above the optical centre of the free space. */
    int sx=c->w>=46?2:1,sy=1;
    tui_rect hero,hist,facts;
    int fh=10+extra;
    if(c->wide) {
        int lw=c->w/2;
        int hh=5*sy+12;if(hh>avail) hh=avail;
        int rh=6+1+fh,gh=hh>rh?hh:rh;if(gh>avail) gh=avail;
        int y0=y+(avail-gh)/3;
        hero=tui_rect_make(c->x,y0,lw,hh);
        hist=tui_rect_make(c->x+lw+1,y0,c->w-lw-1,6);
        facts=tui_rect_make(hist.x,y0+7,hist.w,hh-7>fh?hh-7:fh);
        if(facts.y+facts.h>bottom) facts.h=bottom-facts.y;
    } else {
        int hh=5*sy+12,gh=hh+1+6+1+fh;
        int y0=y+(avail>gh?(avail-gh)/3:0);
        hero=tui_rect_make(c->x,y0,c->w,hh);
        hist=tui_rect_make(c->x,y0+hh+1,c->w,6);
        facts=tui_rect_make(c->x,hist.y+7,c->w,fh);
        if(facts.y+facts.h>bottom) facts.h=bottom-facts.y;
    }
    panel(sf,a,hero," HUNT ",NULL,0);
    {   /* Identity on the top edge, right side, in the category hue. */
        snprintf(t,sizeof(t),"%s  %.20s",id,d->match.label);
        int n=(int)strlen(t);
        if(n+12<hero.w) {
            str(sf,a,hero.x+hero.w-3-n,hero.y,1," ",CHROME);
            str(sf,a,hero.x+hero.w-2-n,hero.y,n,t,category_ink(d->match.category,age));
            str(sf,a,hero.x+hero.w-2,hero.y,1," ",CHROME);
        }
    }
    int ix=hero.x+1,iw=hero.w-2,yy=hero.y+2;
    /* Hero readout in the colour of its heat zone. */
    char num[8];bool have=isfinite(value) && d->adverts;
    if(have) snprintf(num,sizeof(num),"%d",(int)lroundf(value<-99?-99:value>0?0:value));
    else snprintf(num,sizeof(num),"-");
    int bw=big_width(num,sx),bx=ix+(iw-(bw+4))/2;
    big(sf,a,bx,yy,num,sx,sy,have?zone_ink(zone(value),true):NOTE);
    str(sf,a,bx+bw+1,yy+5*sy-1,3,"dBm",NOTE);
    yy+=5*sy+1;
    if(isfinite(delta)) {
        const char *word=delta>3?"^ CLOSER":delta<-3?"v FARTHER":"= STEADY";
        char tail[32];snprintf(tail,sizeof(tail),"%+.0f dB in 10 s",delta);
        int wl=(int)strlen(word),tl=(int)strlen(tail),tx=ix+(iw-(wl+3+tl))/2;
        str(sf,a,tx,yy,wl,word,VALUE);str(sf,a,tx+wl+3,yy,tl,tail,NOTE);
    } else centre(sf,a,ix,iw,yy,"waiting for adverts",NOTE);
    yy+=2;
    yy+=gauge(sf,a,ix,iw,yy,have?value:NAN,pk,pk_live)+1;
    if(pk_live) {
        char p[16];snprintf(p,sizeof(p),"%.0f dBm",pk);
        int pl=(int)strlen(p),tx=ix+(iw-(6+pl+12))/2;
        str(sf,a,tx,yy,4,"PEAK",NOTE);str(sf,a,tx+6,yy,pl,p,VALUE);str(sf,a,tx+6+pl+2,yy,10,"held 15 s",NOTE);
    } else centre(sf,a,ix,iw,yy,"PEAK  --",NOTE);
    /* History: the last minute in the target's own hue, oldest left. */
    if(hist.y+hist.h<=bottom) {
        /* Strip on the left, its summary on the right, as one centred unit. */
        int pitch=hist.w-2>=48+4+14?2:1,sw=24*pitch,unit=sw+4+14,sx0=hist.x+(hist.w-unit)/2;
        panel(sf,a,hist," LAST 60 s ",NULL,0);
        strip_tall(sf,a,sx0,hist.y+2,pitch,d,now,category_ink(d->match.category,age));
        str(sf,a,sx0,hist.y+4,4,"-60s",NOTE);centre(sf,a,sx0,sw,hist.y+4,"-30s",NOTE);right(sf,a,sx0,sw,hist.y+4,"now",NOTE);
        int hi=-128,lo=127,gaps=0;
        for(unsigned b=0;b<24;b++) {int r=ls_sweep_bucket(d,now,b);if(r==-128) {gaps++;continue;}if(r>hi) hi=r;if(r<lo) lo=r;}
        int kx=sx0+sw+4;char v[16];
        str(sf,a,kx,hist.y+2,4,"HIGH",NOTE);snprintf(v,sizeof(v),hi>-128?"%d dBm":"--",hi);right(sf,a,kx+5,9,hist.y+2,v,VALUE);
        str(sf,a,kx,hist.y+3,4,"LOW",NOTE);snprintf(v,sizeof(v),hi>-128?"%d dBm":"--",lo);right(sf,a,kx+5,9,hist.y+3,v,VALUE);
        str(sf,a,kx,hist.y+4,4,"GAPS",NOTE);snprintf(v,sizeof(v),"%d of 24",gaps);right(sf,a,kx+5,9,hist.y+4,v,gaps?VALUE:NOTE);
    }
    if(facts.h<4) return;
    panel(sf,a,facts," TARGET ",NULL,0);
    int fx=facts.x+3,fw=facts.w-5,fy=facts.y+2,fb=facts.y+facts.h-1;
    const char *v=*d->match.vendor?d->match.vendor:ls_sweep_category(d->match.category);
    if(fy<fb) fact(sf,a,fx,fw,fy++,"VENDOR",v);
    snprintf(t,sizeof(t),"%02X:%02X:%02X:%02X:%02X:%02X  %s",d->mac[0],d->mac[1],d->mac[2],d->mac[3],d->mac[4],d->mac[5],
             d->radio?"Wi-Fi":d->address_type&1?"BLE random":"BLE public");
    if(fy<fb) fact(sf,a,fx,fw,fy++,d->radio?"BSSID":"ADDRESS",t);
    if(fy<fb) fact(sf,a,fx,fw,fy++,"STATE",d->match.state==SW_STATE_SEPARATED?"separated from owner":d->match.state==SW_STATE_NEAR?"near its owner":"not reported");
    snprintf(t,sizeof(t),"%lu  (%.1f/s)",(unsigned long)d->adverts,ls_sweep_rate(d,now));
    if(fy<fb) fact(sf,a,fx,fw,fy++,"ADVERTS",t);
    snprintf(t,sizeof(t),"%lds ago",(long)(age/1000000));
    if(fy<fb) fact(sf,a,fx,fw,fy++,"HEARD",t);
    /* The averaging row is the FAST/SLOW control; a tap toggles it. */
    if(fy<fb) {fact(sf,a,fx,fw,fy,"AVERAGE",slow?"SLOW 8 s":"FAST 2 s");right(sf,a,fx+20,fw-20,fy,slow?"tap for FAST":"tap for SLOW",NOTE);mode_row=fy++;}
    if(near && fy<fb) {
        const char *w="! owner-nearby: address may change";
        str(sf,a,fx,fy,fw,w,VALUE);tui_put_char(sf,a,fx,fy,'!',INK(TUI_RED|TUI_BRIGHT));fy++;
    }
    if(drone) {
        ls_gps_state_t gps;ls_gps_get(&gps);
        if(drone->position_valid && now-drone->location_us<60000000 && fy<fb) bearing(sf,a,fx,fw,fy++,"AIRCRAFT",drone->lat,drone->lon,&gps);
        if(drone->operator_valid && now-drone->last_us<60000000 && fy<fb) bearing(sf,a,fx,fw,fy++,"OPERATOR",drone->operator_lat,drone->operator_lon,&gps);
    }
}

/* --------------------------------------------------------------- draw -- */

static void draw(tui_surface *sf,tui_rect a) {
    int64_t now=esp_timer_get_time();ls_sweep_settings_t s;ls_sweep_status_t st;
    ls_sweep_settings_get(&s);ls_sweep_status(&st,now);
    count=ls_sweep_snapshot(view,LS_SWEEP_CAP,now);
    /* Stable IDs survive rank changes; a target that expires returns to LIST. */
    bool found=owner_selected && st.requested && st.owner_nearby && now-near_target.seen_us<60000000;
    if(owner_selected) {
        ls_sweep_device_t latest;
        if(ls_sweep_nearby_target(&latest,now) && !memcmp(latest.mac,near_target.mac,6) && latest.address_type==near_target.address_type) near_target=latest;
    }
    for(unsigned i=0;i<count;i++) if(view[i].serial==selected) {pick=i;found=true;break;}
    if(!found) {selected=0;pick=0;owner_selected=false;screen_view=VIEW_LIST;if(s.hunt) ls_sweep_hunt(NULL,0,0);}
    if(a.w<26 || a.h<18) {ls_panel_notice(sf,a,"SWEEP","More screen space needed","Use a smaller font");return;}
    int bh=ls_tui_is_wide()?3:5;
    ls_btn_t b[]={ {st.requested?"STOP":"START","RX",'s',false,false,false},
        {screen_view==VIEW_HUNT?"FIND":"SELECT",screen_view==VIEW_HUNT?"TURN FOR BEARING":"DEVICE",screen_view==VIEW_HUNT?'f':'h',false,!count && !st.owner_nearby,false},
        {s.alerts_muted?"MUTED":"MUTE","SOUND",'m',s.alerts_muted,false,false},
        {"VIEW",view_names[screen_view],'v',false,false,false},ls_opt_button(&ctx)};
    ls_btn_bar_raised_row(sf,tui_rect_make(a.x,a.y+a.h-bh,a.w,bh),b,5,-1);a.h-=bh+1;
    ls_gps_state_t gps;ls_gps_get(&gps);bool fix=gps.fix && now-gps.last_fix_us<5000000;
    cols_t c=layout(a);
    list_area=a;list_shown=0;mode_row=-1;for(unsigned i=0;i<128;i++) row_device[i]=-1;
    int y=header(sf,a,&c,&s,&st,fix,now)+1;
    if(screen_view==VIEW_HUNT && selected && (count || owner_selected)) {
        draw_hunt(sf,a,&c,y,owner_selected?&near_target:&view[pick],fix,now);
        return;
    }
    draw_list(sf,a,&c,y,&st,fix,now);
}
static void choose(void) {
    if(count) {owner_selected=false;selected=view[pick].serial;if(screen_view==VIEW_HUNT) ls_sweep_hunt(view[pick].mac,view[pick].radio,view[pick].address_type);}
    else if(ls_sweep_nearby_target(&near_target,esp_timer_get_time())) {owner_selected=true;selected=UINT32_MAX;}
}
static bool key(ls_tk_t k,char c) {
    if(ls_opt_key(&ctx,c)) return true;
    if(c=='m' || c=='M') {ls_sweep_settings_t s;ls_sweep_settings_get(&s);s.alerts_muted=!s.alerts_muted;ls_sweep_settings_set(&s);return true;}
    if(c=='v' || c=='V') {screen_view=selected && (count || owner_selected)?(screen_view==VIEW_LIST?VIEW_HUNT:VIEW_LIST):VIEW_LIST;ls_sweep_device_t *d=owner_selected?&near_target:count?&view[pick]:NULL;ls_sweep_hunt(screen_view==VIEW_HUNT && d?d->mac:NULL,d?d->radio:0,d?d->address_type:0);return true;}
    if(c=='b' || c=='B') {slow=!slow;return true;}
    if(c=='f' || c=='F') {if(selected && (count || owner_selected)) {ls_sweep_device_t *d=owner_selected?&near_target:&view[pick];ls_sweep_find_target(d->mac,d->radio,d->match.label);}return true;}
    if(c=='s' || c=='S') {ls_sweep_status_t s;ls_sweep_status(&s,esp_timer_get_time());ls_sweep_start(!s.requested);return true;}
    if(c=='h' || c=='H' || k==LS_TK_ENTER) {choose();return true;}
    if(count && (k==LS_TK_UP || k==LS_TK_DOWN || c=='[' || c==']')) {owner_selected=false;pick=(pick+count+(k==LS_TK_UP || c=='['?-1:1))%count;selected=view[pick].serial;if(screen_view==VIEW_HUNT) choose();return true;}
    return false;
}
static bool touch(int x,int y) {
    int b=ls_btn_hit(x,y);if(b==0) return key(LS_TK_CHAR,'s');if(b==1) return key(LS_TK_CHAR,screen_view==VIEW_HUNT?'f':'h');if(b==2) return key(LS_TK_CHAR,'m');if(b==3) return key(LS_TK_CHAR,'v');if(b==4) {ls_opt_open(&ctx);return true;}
    if(screen_view==VIEW_HUNT && y==mode_row) return key(LS_TK_CHAR,'b');
    int row=y-list_area.y;
    if(screen_view==VIEW_LIST && x>=list_area.x && x<list_area.x+list_area.w && row>=0 && row<128 && row_device[row]!=-1) {
        if(row_device[row]==-2) {if(ls_sweep_nearby_target(&near_target,esp_timer_get_time())) {owner_selected=true;selected=UINT32_MAX;}}
        else {pick=row_device[row];choose();}return true;
    }return false;
}
const ls_tui_screen_t ls_scr_sweep={.name="SWEEP",.hint="M Mute  V List/Hunt  B Fast/Slow  F Find  Up/Down select",.draw=draw,.key=key,.touch=touch};
