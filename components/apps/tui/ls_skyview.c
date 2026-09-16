#include "ls_skyview.h"
#include "ls_skyplot.h"
#include "ls_tui_ui.h"
#include "esp_timer.h"
#include <stdio.h>

void ls_skyview_draw(tui_surface *sf, tui_rect a, const ls_gps_state_t *g, int selected)
{
    if(!g || a.w<12 || a.h<5) return;
    char text[100];
    const uint8_t cyan=TUI_ATTR(TUI_CYAN|TUI_BRIGHT,TUI_BLACK);
    const uint8_t green=TUI_ATTR(TUI_GREEN|TUI_BRIGHT,TUI_BLACK);
    const uint8_t amber=TUI_ATTR(TUI_YELLOW|TUI_BRIGHT,TUI_BLACK);
    int64_t now=esp_timer_get_time();
    bool fresh=g->last_sentence_us>0 && now>=g->last_sentence_us && now-g->last_sentence_us<3000000;
    snprintf(text,sizeof(text),"SKY / GSV  RX age %s",fresh?"<3s":"STALE");
    tui_put_str(sf,a,a.x,a.y,text,fresh?cyan:amber);
    tui_put_str(sf,a,a.x,a.y+1,"N=true north; rim=0 mid=45 +=90 deg",LS_ATTR_DIM);
    int count=g->sat_count<LS_GPS_MAX_SATS?g->sat_count:LS_GPS_MAX_SATS;
    if(selected<0 || selected>=count) selected=0;
    int located=0;
    for(int i=0;i<count;i++) if(g->sats[i].prn && g->sats[i].azimuth<360 && g->sats[i].elevation<=90 && ls_sky_located(g->sats[i].azimuth,g->sats[i].elevation)) located++;
    const bool framed=a.w>=85 && a.h>=18;
    tui_rect plot_frame=tui_rect_make(a.x,a.y+2,a.w/2-1,a.h-2);
    tui_rect table_frame=tui_rect_make(a.x+a.w/2,a.y+2,a.w-a.w/2,a.h-9);
    tui_rect detail_frame=tui_rect_make(table_frame.x,table_frame.y+table_frame.h,table_frame.w,7);
    if(framed) {
        ls_panel_box(sf,plot_frame,"SKY / NORTH UP",TUI_CYAN);
        ls_panel_box(sf,table_frame,"SATELLITES",TUI_CYAN);
        ls_panel_box(sf,detail_frame,"SELECTED",TUI_CYAN);
    }
    if(!fresh) {
        tui_put_str(sf,a,a.x+2,a.y+4,"No fresh receiver data",LS_ATTR_DIM);
        if(framed) {
            tui_put_str(sf,table_frame,table_frame.x+2,table_frame.y+2,"Waiting for current GSV reports",LS_ATTR_DIM);
            tui_put_str(sf,detail_frame,detail_frame.x+2,detail_frame.y+1,"No current selection",LS_ATTR_DIM);
        }
        return;
    }
    int plot_h=framed?plot_frame.h-2:a.h-8;
    if(plot_h>21) plot_h=21;
    if(plot_h>=5) {
        tui_rect p=tui_rect_make(a.x+1,a.y+2,a.w>=85?a.w/2-2:a.w-2,plot_h);
        if(framed) p=tui_rect_make(plot_frame.x+1,plot_frame.y+1,plot_frame.w-2,plot_h);
        int cw=10,ch=17;ls_tui_geometry(NULL,NULL,&cw,&ch);
        for(int elev=0;elev<=45;elev+=45) for(int az=0;az<360;az+=6) {
            int r,c;
            if(ls_sky_project(az,elev,p.h,p.w,cw,ch,&r,&c))
                tui_put_char(sf,p,p.x+c,p.y+r,'.',LS_ATTR_DIM);
        }
        tui_put_char(sf,p,p.x+p.w/2,p.y+p.h/2,'+',LS_ATTR_DIM);
        static const char cardinal[]="NESW";
        for(int k=0;k<4;k++) {
            int r,c;ls_sky_project(k*90,0,p.h,p.w,cw,ch,&r,&c);
            tui_put_char(sf,p,p.x+c,p.y+r,cardinal[k],k?cyan:amber);
        }
        int rows[LS_GPS_MAX_SATS],cols[LS_GPS_MAX_SATS];
        for(int i=0;i<count;i++) {
            rows[i]=cols[i]=-1;
            const ls_gps_sat_t *s=&g->sats[i];
            if(!s->prn || s->azimuth>=360 || s->elevation>90 || !ls_sky_located(s->azimuth,s->elevation)) continue;
            if(!ls_sky_project(s->azimuth,s->elevation,p.h,p.w,cw,ch,&rows[i],&cols[i])) continue;
            char mark=i<26?'A'+i:'a'+i-26;
            for(int j=0;j<i;j++) if(rows[j]==rows[i] && cols[j]==cols[i]) mark='*';
            tui_put_char(sf,p,p.x+cols[i],p.y+rows[i],mark,s->used?green:amber);
        }
        if(count && rows[selected]>=0)
            tui_put_char(sf,p,p.x+cols[selected],p.y+rows[selected],'@',TUI_ATTR(TUI_CYAN|TUI_BRIGHT,TUI_BLACK));
    } else plot_h=0;
    if(a.w>=85 && plot_h>=5) {
        tui_rect table=tui_rect_make(a.x+a.w/2,a.y+2,a.w-a.w/2,plot_h);
        if(framed) table=tui_rect_make(table_frame.x+2,table_frame.y+1,table_frame.w-4,table_frame.h-2);
        tui_put_str(sf,table,table.x,table.y,"NORTH-UP / local sky, not orbit",cyan);
        tui_put_str(sf,table,table.x,table.y+1,"MARK ID   AZdeg ELdeg CN0dBHz FIX",cyan);
        int capacity=table.h-4;
        int first=capacity>0?(selected/capacity)*capacity:0;
        for(int i=first;i<count && i<first+capacity;i++) {
            const ls_gps_sat_t *sat=&g->sats[i];
            bool located=sat->prn && sat->azimuth<360 && sat->elevation<=90 && ls_sky_located(sat->azimuth,sat->elevation);
            char az[6],el[4],cn[4];
            if(located) {snprintf(az,sizeof(az),"%03u",sat->azimuth);snprintf(el,sizeof(el),"%02u",sat->elevation);}
            else {snprintf(az,sizeof(az),"---");snprintf(el,sizeof(el),"--");}
            if(sat->snr)snprintf(cn,sizeof(cn),"%u",sat->snr);else snprintf(cn,sizeof(cn),"--");
            snprintf(text,sizeof(text),"%c %c %3u    %s    %s     %2s   %s",i==selected?'>':' ',i<26?'A'+i:'a'+i-26,sat->prn,az,el,cn,sat->used?"USED":"seen");
            tui_put_str(sf,table,table.x,table.y+2+i-first,text,i==selected?cyan:sat->used?green:amber);
        }
        tui_put_str(sf,table,table.x,table.y+table.h-2,"Green USED / amber seen / cyan selected",LS_ATTR_DIM);
        tui_put_str(sf,table,table.x,table.y+table.h-1,"-- unreported; J/K pages through all",LS_ATTR_DIM);
    }
    int y=plot_h?plot_h+2:3;
    if(framed) {
        a=tui_rect_make(detail_frame.x+2,detail_frame.y+1,detail_frame.w-4,detail_frame.h-2);
        y=0;
    }
    tui_put_str(sf,a,a.x,a.y+y,"@ selected  * overlap  J/K select",LS_ATTR_DIM);
    if(!count) { tui_put_str(sf,a,a.x,a.y+y+1,"Waiting for satellite reports",LS_ATTR_DIM);return; }
    const ls_gps_sat_t *s=&g->sats[selected];
    snprintf(text,sizeof(text),"%c ID %u  %d/%d  %s",selected<26?'A'+selected:'a'+selected-26,s->prn,selected+1,count,s->used?"USED in fix":"NOT used in fix");
    tui_put_str(sf,a,a.x,a.y+y+1,text,s->used?green:amber);
    if(s->azimuth<360 && s->elevation<=90 && ls_sky_located(s->azimuth,s->elevation))
        snprintf(text,sizeof(text),"AZ %03u deg true  EL %02u deg",s->azimuth,s->elevation);
    else snprintf(text,sizeof(text),"AZ/EL unreported; not plotted");
    tui_put_str(sf,a,a.x,a.y+y+2,text,cyan);
    if(s->snr) snprintf(text,sizeof(text),"C/N0 %u dB-Hz (GSV)",s->snr);
    else snprintf(text,sizeof(text),"C/N0 unreported / not tracked");
    tui_put_str(sf,a,a.x,a.y+y+3,text,s->snr?green:amber);
    snprintf(text,sizeof(text),"PLOT %d/%d located; compare sky coverage",located,count);
    tui_put_str(sf,a,a.x,a.y+y+4,text,LS_ATTR_DIM);
    /* A portrait plot is width-limited. Use its remaining height for the
       same measurements available beside the landscape plot. */
    const int table_y = y + 6;
    const int capacity = a.h - table_y - 2;
    if (a.w < 85 && a.w >= 38 && capacity >= 3) {
        tui_put_str(sf,a,a.x,a.y+table_y,"MARK ID  AZdeg ELdeg CN0dBHz FIX",cyan);
        const int first = selected / capacity * capacity;
        for (int i=first; i<count && i<first+capacity; i++) {
            const ls_gps_sat_t *sat=&g->sats[i];
            bool valid=sat->prn && sat->azimuth<360 && sat->elevation<=90 &&
                       ls_sky_located(sat->azimuth,sat->elevation);
            char az[6],el[4],cn[4];
            if(valid) {snprintf(az,sizeof(az),"%03u",sat->azimuth);snprintf(el,sizeof(el),"%02u",sat->elevation);}
            else {snprintf(az,sizeof(az),"---");snprintf(el,sizeof(el),"--");}
            if(sat->snr) snprintf(cn,sizeof(cn),"%u",sat->snr);
            else snprintf(cn,sizeof(cn),"--");
            snprintf(text,sizeof(text),"%c%c %3u   %s    %s     %2s   %s",
                     i==selected?'>':' ',i<26?'A'+i:'a'+i-26,sat->prn,az,el,cn,sat->used?"USED":"seen");
            tui_put_str(sf,a,a.x,a.y+table_y+1+i-first,text,
                        i==selected?cyan:sat->used?green:amber);
        }
        tui_put_str(sf,a,a.x,a.y+a.h-1,"-- unreported; J/K selects / pages",LS_ATTR_DIM);
    }
}
