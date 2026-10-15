#include "ls_catch_ui.h"
#include "ls_options.h"
#include "ls_picker.h"
#include "ls_numpad.h"
#include "ls_braille.h"
#include "ls_radio_select.h"
#include "apps/rec/rec_catch.h"
#include "esp_attr.h"
#include "esp_timer.h"
#include <math.h>
#include <stdio.h>
#include <string.h>
static uint32_t center=433920000u;
static int preset=1, selected;
static const char *const BANDS[]={"315","433.92","868.3","915","CUSTOM"};
static const uint32_t CENTERS[]={315000000u,433920000u,868300000u,915000000u};
static EXT_RAM_BSS_ATTR catch_result_t result;
static EXT_RAM_BSS_ATTR catch_status_t status;
static tui_rect buttons, list_area, slice_area;
static int first_visible, visible;
static char notice[80];
static int band_get(const ls_opt_t *o) {(void)o;return preset;}
static void band_set(const ls_opt_t *o,int v) {(void)o;preset=v;if(v<4)center=CENTERS[v];}
static double center_get(const ls_opt_t *o) {(void)o;return center/1e6;}
static void center_set(const ls_opt_t *o,double v) {(void)o;center=(uint32_t)(v*1e6+0.5);preset=4;}
static const char *idle_only(const ls_opt_t *o)
{ (void)o;catch_status_t *s=&status;rec_catch_status(s);return s->running||s->stopping?"Stop CATCH first":NULL; }

/* ---- watch: ALL, or only the chosen channel windows ---- */
static const char *const WATCH_NAMES[]={"ALL","CHOSEN"};
/* The slice in view: the running one, else the one START would open. The
   last drawn status is enough; options never poll the worker. */
static uint32_t slice_hz(void) {return status.running?status.center:center;}
static void watch_put(catch_watch_t *w) {catch_watch_set(w);}
static int watch_get(const ls_opt_t *o) {(void)o;catch_watch_t w;catch_watch_get(&w);return w.mode;}
static void watch_set(const ls_opt_t *o,int v)
{ (void)o;catch_watch_t w;catch_watch_get(&w);w.mode=v?CATCH_WATCH_CHOSEN:CATCH_WATCH_ALL;watch_put(&w); }
/* A first channel turns CHOSEN on: a list nobody applies helps nobody. */
static const char *chan_add(uint32_t hz,uint32_t width)
{
    catch_watch_t w;catch_watch_get(&w);bool first=!catch_watch_count(&w);
    int r=catch_watch_add(&w,hz,width);
    if(r==-2)return "All 8 channels used";
    if(r<0)return "Frequency out of range";
    if(first)w.mode=CATCH_WATCH_CHOSEN;
    watch_put(&w);return NULL;
}
static const char *full_why(const ls_opt_t *o)
{ (void)o;catch_watch_t w;catch_watch_get(&w);return catch_watch_count(&w)>=CATCH_CHAN_MAX?"All 8 used":NULL; }
static uint32_t preset_hz[CATCH_PRESETS];
static unsigned preset_n;
static const char *preset_why(const ls_opt_t *o)
{
    const char *full=full_why(o);if(full)return full;
    return catch_watch_presets(slice_hz(),preset_hz,CATCH_PRESETS)?NULL:"None in this slice";
}
static void preset_done(int i)
{
    if(i<0 || i>=(int)preset_n)return;
    const char *e=chan_add(preset_hz[i],CATCH_CHAN_WIDTH);
    snprintf(notice,sizeof(notice),"%s",e?e:"Channel added");
}
/* Only the presets inside the slice in view are offered. */
static void preset_open(const ls_opt_t *o)
{
    (void)o;preset_n=catch_watch_presets(slice_hz(),preset_hz,CATCH_PRESETS);
    ls_picker_open("ADD PRESET",preset_done);
    for(unsigned i=0;i<preset_n;i++) {
        char label[24];snprintf(label,sizeof(label),"%.2f MHz",preset_hz[i]/1e6);
        ls_picker_add(label,preset_hz[i]==433920000u?"EU ISM":
            preset_hz[i]==315000000u?"US remotes":
            preset_hz[i]==868350000u?"EU SRD":"US/AU ISM");
    }
}
static double add_num(const ls_opt_t *o) {(void)o;return slice_hz()/1e6;}
static void add_set(const ls_opt_t *o,double v)
{
    (void)o;const char *e=chan_add((uint32_t)(v*1e6+0.5),CATCH_CHAN_WIDTH);
    snprintf(notice,sizeof(notice),"%s",e?e:"Channel added");
}
static void add_show(const ls_opt_t *o,char *out,size_t n) {(void)o;snprintf(out,n,"MHz");}
/* One tap for the common case: watch 433.92 MHz and nothing else. */
static const char *only_why(const ls_opt_t *o)
{ (void)o;return status.running && !catch_chan_in_slice(slice_hz(),433920000u)?"Stop CATCH to change band":NULL; }
static void only_433(const ls_opt_t *o)
{
    (void)o;
    if(!catch_chan_in_slice(slice_hz(),433920000u)){center=433920000u;preset=1;}
    catch_watch_t w={.mode=CATCH_WATCH_CHOSEN};
    catch_watch_add(&w,433920000u,CATCH_CHAN_WIDTH);watch_put(&w);
}
static void only_show(const ls_opt_t *o,char *out,size_t n) {(void)o;snprintf(out,n,"433.92 MHz");}
static void chan_summary(const ls_opt_t *o,char *out,size_t n)
{ (void)o;catch_watch_t w;catch_watch_get(&w);snprintf(out,n,"%u of %d",catch_watch_count(&w),CATCH_CHAN_MAX); }

/* Editing a chosen channel: pick it from a list, then type its width on the
   keypad; 0 removes it. (To move one, remove it and add the new centre.) */
static int edit_slot;
static int slot_map[CATCH_CHAN_MAX];
static unsigned slot_n;
static const char *slot_why(const ls_opt_t *o)
{ (void)o;catch_watch_t w;catch_watch_get(&w);return catch_watch_count(&w)?NULL:"No channels yet"; }
static void slot_width_done(double v)
{
    catch_watch_t w;catch_watch_get(&w);
    if(v==0)catch_watch_remove(&w,(unsigned)edit_slot);
    else w.chan[edit_slot].width=(uint32_t)(v*1e3+0.5);
    if(catch_watch_valid(&w))watch_put(&w);
    else snprintf(notice,sizeof(notice),"Width: 5 to 2400 kHz, or 0 to remove");
}
static void slot_pick_done(int i)
{
    if(i<0 || i>=(int)slot_n)return;
    edit_slot=slot_map[i];
    catch_watch_t w;catch_watch_get(&w);
    ls_numpad_open("Width, 0 removes","kHz total",w.chan[edit_slot].width/1e3,slot_width_done);
}
static void slot_pick_open(const ls_opt_t *o)
{
    (void)o;catch_watch_t w;catch_watch_get(&w);slot_n=0;
    ls_picker_open("EDIT CHANNEL",slot_pick_done);
    for(int i=0;i<CATCH_CHAN_MAX;i++) {
        const catch_chan_t *c=&w.chan[i];
        if(!c->centre)continue;
        char label[24],d[LS_PICKER_DETAIL];
        snprintf(label,sizeof(label),"%.3f MHz",c->centre/1e6);
        snprintf(d,sizeof(d),"+/-%lu kHz%s",(unsigned long)(c->width/2000),
            catch_chan_in_slice(slice_hz(),c->centre)?"":", not in slice");
        slot_map[slot_n++]=i;ls_picker_add(label,d);
    }
}
static const ls_opt_t CHAN_OPTIONS[]={
    {.label="Add preset",.kind=LS_OPT_ACTION,.act=preset_open,.why_not=preset_why},
    {.label="Add frequency",.kind=LS_OPT_NUMBER,.num=add_num,.set_num=add_set,.lo=25.2,.hi=1764.8,.unit="MHz",.show=add_show,.why_not=full_why},
    {.label="Edit channel",.kind=LS_OPT_ACTION,.act=slot_pick_open,.why_not=slot_why},
};
static const ls_opt_ctx_t CHAN_MENU={.name="CHANNELS",.job=-1,.radio=LS_RSEL_SDR_RTL,LS_OPT_ROWS(CHAN_OPTIONS),.back_to_screen=true,.menu_arrows=true};

static const ls_opt_t BAND_OPTIONS[]={
    {.label="Preset",.kind=LS_OPT_CYCLE,.names=BANDS,.n=5,.get=band_get,.set=band_set,.why_not=idle_only},
    {.label="Custom centre",.kind=LS_OPT_NUMBER,.num=center_get,.set_num=center_set,.lo=25.2,.hi=1764.8,.unit="MHz",.why_not=idle_only},
};
static const ls_opt_ctx_t BAND_MENU={.name="BAND",.job=-1,.radio=LS_RSEL_SDR_RTL,LS_OPT_ROWS(BAND_OPTIONS),.menu_arrows=true};
static const ls_opt_t OPTIONS[]={
    {.label="Band",.kind=LS_OPT_MENU,.sub=&BAND_MENU},
    {.label="Watch",.kind=LS_OPT_CYCLE,.names=WATCH_NAMES,.n=2,.get=watch_get,.set=watch_set,.step=1},
    {.label="Channels",.kind=LS_OPT_MENU,.sub=&CHAN_MENU,.show=chan_summary},
    {.label="Only 433.92",.kind=LS_OPT_ACTION,.act=only_433,.show=only_show,.why_not=only_why},
};
static const ls_opt_ctx_t OPTION_MENU={.name="CATCH",.job=-1,.radio=LS_RSEL_SDR_RTL,LS_OPT_ROWS(OPTIONS),.back_to_screen=true,.menu_arrows=true};
static void toggle(void)
{
    catch_status_t *s=&status;rec_catch_status(s);
    if(s->running) rec_catch_stop();
    else {
        ls_radio_err_t e=rec_catch_start(center);
        snprintf(notice,sizeof(notice),"%s",e==LS_RADIO_OK?"Start requested":
            e==LS_RADIO_ERR_BUSY?"CATCH still stopping or already running":
            e==LS_RADIO_ERR_NO_MEMORY?"CATCH worker allocation failed":"RTL-SDR unavailable");
    }
}
static void save(void)
{ snprintf(notice,sizeof(notice),"%s",rec_catch_save(selected)?"Save queued":"No raw capture to save, or save already queued"); }
void ls_catch_toggle(void){toggle();}
void ls_catch_save_selected(void){save();}
bool ls_catch_running(void){catch_status_t *s=&status;rec_catch_status(s);return s->running;}
void ls_catch_stop(void){if(ls_catch_running())rec_catch_stop();}
const ls_opt_ctx_t *ls_catch_options(void){return &OPTION_MENU;}
void ls_catch_enter(void){catch_watch_load();}
void ls_catch_watch_cycle(void)
{
    catch_watch_t w;catch_watch_get(&w);
    w.mode=w.mode==CATCH_WATCH_ALL?CATCH_WATCH_CHOSEN:CATCH_WATCH_ALL;
    catch_watch_set(&w);
    snprintf(notice,sizeof(notice),"%s",catch_watch_note(&w)?:"");
}
/* The bar key's second line, and whether the filter is really applying. */
const char *ls_catch_watch_label(bool *on)
{
    static char label[12];
    catch_watch_t w;catch_watch_get(&w);
    if(on)*on=catch_watch_filtering(&w);
    if(w.mode==CATCH_WATCH_ALL)return "ALL";
    snprintf(label,sizeof(label),"CHOSEN %u",catch_watch_count(&w));
    return label;
}
/* The 2.4 MHz slice as three rows: the last ten seconds of real peaks drawn
   as braille columns (green inside a chosen window, red outside it, cyan when
   nothing is chosen), the chosen windows as magenta brackets under them, and
   the scale. Nothing moves unless the RTL saw it. */
#define STRIP_MAX 128
static void slice_draw(tui_surface *sf,tui_rect r,const catch_watch_t *w,const catch_status_t *s,uint32_t sc)
{
    const int cols=r.w<STRIP_MAX?r.w:STRIP_MAX;
    uint8_t high[STRIP_MAX]={0},hue[STRIP_MAX]={0};
    const bool filtering=catch_watch_filtering(w);
    const uint64_t now=(uint64_t)esp_timer_get_time();
    const uint32_t lo=sc-CATCH_SLICE_HZ/2;
    for(unsigned i=0;i<CATCH_RECENT_SECONDS;i++) {
        const catch_peak_t *p=&s->recent[i];
        if(!s->running || !p->frequency || now<p->peak_us || now-p->peak_us>CATCH_PEAK_WINDOW_US ||
           !catch_chan_in_slice(sc,p->frequency))continue;
        const int x=catch_slice_col(sc,p->frequency,cols);
        uint8_t h=(uint8_t)(1+(p->snr_db>=12)+(p->snr_db>=20)+(p->snr_db>=30));
        const uint8_t c=!filtering?1:catch_watch_allows(w,p->frequency)?2:3;
        if(h>high[x])high[x]=h;
        if(c>hue[x])hue[x]=c;
    }
    const uint8_t ink[]={0,TUI_ATTR(TUI_CYAN|TUI_BRIGHT,TUI_BLACK),TUI_ATTR(TUI_GREEN|TUI_BRIGHT,TUI_BLACK),TUI_ATTR(TUI_RED,TUI_BLACK)};
    for(int x=0;x<cols;x++) {
        if(high[x])
            ls_braille_put(sf,r,r.x+x,r.y,(int16_t)(0x2800|ls_braille_column(high[x],0)|ls_braille_column(high[x],1)),ink[hue[x]]);
        else tui_put_char(sf,r,r.x+x,r.y,'.',LS_ATTR_DIM);
    }
    const uint8_t bracket=TUI_ATTR(TUI_MAGENTA|(filtering?TUI_BRIGHT:0),TUI_BLACK);
    for(int i=0;i<CATCH_CHAN_MAX;i++) {
        int l,rt;
        if(!catch_chan_cols(sc,&w->chan[i],cols,&l,&rt))continue;
        if(rt<=l){if(l+1<cols)rt=l+1;else l=rt-1;}
        for(int x=l+1;x<rt;x++)tui_put_char(sf,r,r.x+x,r.y+1,'-',bracket);
        tui_put_char(sf,r,r.x+l,r.y+1,'[',bracket);
        tui_put_char(sf,r,r.x+rt,r.y+1,']',bracket);
    }
    for(int i=0;i<3;i++) {
        char t[12];int n=snprintf(t,sizeof(t),"%.2f",(lo+(uint32_t)i*(CATCH_SLICE_HZ/2))/1e6);
        tui_put_str(sf,r,r.x+(cols-n)*i/2,r.y+2,t,i==1?TUI_ATTR(TUI_CYAN,TUI_BLACK):LS_ATTR_DIM);
    }
}
/* The body under REC's shared bar: CATCH START/STOP, WATCH and RAW SAVE live
   on that bar, the band and channels under OPTIONS. */
void ls_catch_body_draw(tui_surface *sf,tui_rect a)
{
    buttons=tui_rect_make(0,0,0,0);slice_area=buttons;
    if(a.w<32 || a.h<14) {
        list_area=tui_rect_make(0,0,0,0);
        ls_panel_notice(sf,a,"CATCH","Enlarge pane","");return;
    }
    catch_status_t *s=&status;rec_catch_status(s);
    char line[160];
    ls_panel_box(sf,a,"CATCH",s->running?TUI_GREEN:TUI_CYAN);
    a=tui_rect_make(a.x+2,a.y+1,a.w-4,a.h-2);
    snprintf(line,sizeof(line),"%s %.6f MHz / 2.4 MHz",s->running?"LISTEN":s->stopping?"STOPPING":"STOPPED",(s->running?s->center:center)/1e6);
    tui_put_str(sf,a,a.x,a.y,line,TUI_ATTR(TUI_CYAN|TUI_BRIGHT,TUI_BLACK));
    tui_put_str(sf,a,a.x,a.y+1,s->rtl_status[0]?s->rtl_status:"RTL-SDR spots; LR2021 decodes repeats",LS_ATTR_DIM);
    tui_put_str(sf,a,a.x,a.y+2,s->lr_status,LS_ATTR_DIM);
    snprintf(line,sizeof(line),"%lu bursts  %lu captures  max %lu us",(unsigned long)s->detections,(unsigned long)s->captures,(unsigned long)s->max_latency_us);
    tui_put_str(sf,a,a.x,a.y+3,line,LS_ATTR_DIM);
    catch_watch_t w;catch_watch_get(&w);
    const char *note=catch_watch_note(&w);
    if(note)snprintf(line,sizeof(line),"WATCH %s",note);
    else if(catch_watch_filtering(&w))snprintf(line,sizeof(line),"WATCH CHOSEN %u  ignored %lu",catch_watch_count(&w),(unsigned long)s->ignored);
    else snprintf(line,sizeof(line),"WATCH ALL  every burst in the slice");
    tui_put_str(sf,a,a.x,a.y+4,line,note?TUI_ATTR(TUI_YELLOW|TUI_BRIGHT,TUI_BLACK):
        catch_watch_filtering(&w)?TUI_ATTR(TUI_GREEN|TUI_BRIGHT,TUI_BLACK):LS_ATTR_DIM);
    int top=a.y+5;
    if(a.h>=20) {
        slice_area=tui_rect_make(a.x,a.y+4,a.w,4);
        slice_draw(sf,tui_rect_make(a.x,a.y+5,a.w,3),&w,s,s->running?s->center:center);
        top=a.y+9;
    }
    list_area=tui_rect_make(a.x,top,a.w,a.y+a.h-5-top);visible=list_area.h/3;
    if(visible<0)visible=0;
    if(selected>=s->count)selected=s->count?s->count-1:0;
    first_visible=selected>=visible && visible?selected-visible+1:0;
    for(int row=0;row<visible;row++) {
        int index=first_visible+row;if(!rec_catch_result(index,&result))break;
        int y=list_area.y+row*3;
        snprintf(line,sizeof(line),"%c %llu ms %.6f MHz %s",index==selected?'>':' ',
            (unsigned long long)(result.burst.start_us/1000),result.burst.frequency/1e6,result.burst.modulation==CATCH_FSK?"FSK?":"OOK?");
        tui_put_str(sf,list_area,a.x,y,line,TUI_ATTR(TUI_CYAN|TUI_BRIGHT,TUI_BLACK));
        snprintf(line,sizeof(line),"RTL %.1f dBFS LR %s",(double)result.burst.rtl_dbfs,isfinite(result.lr_rssi)?"":"unavailable");
        if(isfinite(result.lr_rssi))snprintf(line,sizeof(line),"RTL %.1f dBFS LR %.1f dBm",(double)result.burst.rtl_dbfs,(double)result.lr_rssi);
        tui_put_str(sf,list_area,a.x,y+1,line,LS_ATTR_DIM);
        snprintf(line,sizeof(line),"%s %s",result.decoded?result.message.model:"raw",result.decoded?result.message.id:result.decode_status);
        tui_put_str(sf,list_area,a.x,y+2,line,TUI_ATTR(TUI_CYAN|TUI_BRIGHT,TUI_BLACK));
    }
    if(!s->count)tui_put_str(sf,list_area,a.x,list_area.y,catch_watch_filtering(&w)?"Waiting for bursts in chosen channels":"Waiting for real bursts",LS_ATTR_DIM);
    if(rec_catch_result(selected,&result) && result.decoded) {
        snprintf(line,sizeof(line),"ID %s CH %d BAT %d",result.message.id,result.message.channel,result.message.battery_ok);
        tui_put_str(sf,a,a.x,a.y+a.h-5,line,LS_ATTR_DIM);
        snprintf(line,sizeof(line),"%.1f C %.1f %% %.1f kPa meter %lld",(double)result.message.temp_c,
            (double)result.message.humidity,(double)result.message.kpa,(long long)result.message.consumption);
        tui_put_str(sf,a,a.x,a.y+a.h-4,line,TUI_ATTR(TUI_CYAN|TUI_BRIGHT,TUI_BLACK));
        snprintf(line,sizeof(line),"Wind %.1f m/s rain %.1f mm",(double)result.message.wind_ms,(double)result.message.rain_mm);
        tui_put_str(sf,a,a.x,a.y+a.h-3,line,LS_ATTR_DIM);
    }
    tui_put_str(sf,a,a.x,a.y+a.h-2,s->save_status[0]?s->save_status:notice,LS_ATTR_DIM);
    tui_put_str(sf,a,a.x,a.y+a.h-1,"UP/DOWN burst  H watch  tap slice: channels",LS_ATTR_DIM);
}
bool ls_catch_key(ls_tk_t key,char c)
{
    if(key==LS_TK_UP){if(selected>0)selected--;return true;}
    if(key==LS_TK_DOWN){catch_status_t *s=&status;rec_catch_status(s);if(selected+1<s->count)selected++;return true;}
    if(key==LS_TK_ENTER || c=='c'||c=='C'){toggle();return true;}
    if(c=='e'||c=='E'){save();return true;}
    if(key==LS_TK_CHAR && ls_opt_key(&OPTION_MENU,c))return true;
    return false;
}
bool ls_catch_body_touch(int x,int y)
{
    if(tui_rect_contains(slice_area,x,y)){ls_opt_open(&CHAN_MENU);return true;}
    if(tui_rect_contains(list_area,x,y)){selected=first_visible+(y-list_area.y)/3;return true;}
    return false;
}
