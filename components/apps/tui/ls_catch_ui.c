#include "ls_catch_ui.h"
#include "ls_options.h"
#include "ls_radio_select.h"
#include "apps/rec/rec_catch.h"
#include "esp_attr.h"
#include <math.h>
#include <stdio.h>
#include <string.h>
static uint32_t center=433920000u;
static int preset=1, selected;
static const char *const BANDS[]={"315","433.92","868.3","915","CUSTOM"};
static const uint32_t CENTERS[]={315000000u,433920000u,868300000u,915000000u};
static EXT_RAM_BSS_ATTR catch_result_t result;
static EXT_RAM_BSS_ATTR catch_status_t status;
static tui_rect buttons, list_area;
static int first_visible, visible;
static char notice[80];
static int band_get(const ls_opt_t *o) {(void)o;return preset;}
static void band_set(const ls_opt_t *o,int v) {(void)o;preset=v;if(v<4)center=CENTERS[v];}
static double center_get(const ls_opt_t *o) {(void)o;return center/1e6;}
static void center_set(const ls_opt_t *o,double v) {(void)o;center=(uint32_t)(v*1e6+0.5);preset=4;}
static const char *idle_only(const ls_opt_t *o)
{ (void)o;catch_status_t *s=&status;rec_catch_status(s);return s->running||s->stopping?"Stop CATCH first":NULL; }
static const ls_opt_t BAND_OPTIONS[]={
    {.label="Preset",.kind=LS_OPT_CYCLE,.names=BANDS,.n=5,.get=band_get,.set=band_set,.why_not=idle_only},
    {.label="Custom centre",.kind=LS_OPT_NUMBER,.num=center_get,.set_num=center_set,.lo=25.2,.hi=1764.8,.unit="MHz",.why_not=idle_only},
};
static const ls_opt_ctx_t BAND_MENU={.name="BAND",.job=-1,.radio=LS_RSEL_SDR_RTL,LS_OPT_ROWS(BAND_OPTIONS),.menu_arrows=true};
static const ls_opt_t OPTIONS[]={ {.label="Band",.kind=LS_OPT_MENU,.sub=&BAND_MENU} };
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
/* The body under REC's shared bar: CATCH START/STOP and RAW SAVE live on
   that bar, the band under OPTIONS. */
void ls_catch_body_draw(tui_surface *sf,tui_rect a)
{
    buttons=tui_rect_make(0,0,0,0);
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
    list_area=tui_rect_make(a.x,a.y+5,a.w,a.h-10);visible=list_area.h/3;
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
    if(!s->count)tui_put_str(sf,list_area,a.x,list_area.y,"Waiting for real bursts",LS_ATTR_DIM);
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
    tui_put_str(sf,a,a.x,a.y+a.h-1,"UP/DOWN select a burst",LS_ATTR_DIM);
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
    if(tui_rect_contains(list_area,x,y)){selected=first_visible+(y-list_area.y)/3;return true;}
    return false;
}

