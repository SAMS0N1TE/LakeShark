/* Simulator-only design fixtures; no receiver or settings actions. */
#include "ls_tui_ui.h"
#include <stdio.h>
#include <string.h>

static bool fm, scanning, holding, empty, bank_b;
static int channel = 2;
static const uint8_t cyan = TUI_ATTR(TUI_CYAN | TUI_BRIGHT, TUI_BLACK);
static const uint8_t amber = TUI_ATTR(TUI_YELLOW | TUI_BRIGHT, TUI_BLACK);
static const uint8_t white = TUI_ATTR(TUI_WHITE | TUI_BRIGHT, TUI_BLACK);
static const uint8_t green = TUI_ATTR(TUI_GREEN | TUI_BRIGHT, TUI_BLACK);

static void line(tui_surface *sf, tui_rect a, int y, const char *s, uint8_t color)
{
    tui_put_str(sf, a, a.x + 2, a.y + y, s, color);
}

static void frequency(tui_surface *sf, tui_rect a, int y, const char *s, uint8_t color)
{
    static const uint8_t digits[10][5] = {
        {7,5,5,5,7},{2,6,2,2,7},{7,1,7,4,7},{7,1,7,1,7},{5,5,7,1,1},
        {7,4,7,1,7},{7,4,7,5,7},{7,1,1,1,1},{7,5,7,5,7},{7,5,7,1,7}
    };
    int width = 0;
    for (const char *p = s; *p; ++p) width += *p == '.' ? 2 : 4;
    int x = a.x + (a.w - width + 1) / 2;
    for (const char *p = s; *p; ++p) {
        if (*p == '.') {
            tui_put_char(sf, a, x, a.y+y+4, LS_TUI_BLOCK_FULL, color);
            x += 2;
            continue;
        }
        if (*p >= '0' && *p <= '9')
            for (int r=0;r<5;r++) for (int c=0;c<3;c++)
                if (digits[*p-'0'][r] & (4>>c))
                    tui_put_char(sf,a,x+c,a.y+y+r,LS_TUI_BLOCK_FULL,color);
        x += 4;
    }
}

static const char *current_frequency(void)
{
    static const char *fm_values[] = {"146.5200", "152.6000", "162.5500", "446.0000"};
    if (fm) return scanning ? fm_values[channel%4] : bank_b ? "152.6000" : "162.5500";
    static const char *values[] = {"154.7700", "154.8000", "154.7850", "155.0100"};
    return values[channel % 4];
}

static void list(tui_surface *sf, tui_rect a)
{
    tui_box(sf,a,"FIELD LIST / 4 CHANNELS",cyan);
    const char *names[] = {"VHF PRESET A", "VHF PRESET B", fm?"WEATHER PRESET":"VHF PRESET C", "PRESET D"};
    for (int i=0;i<4;i++) {
        char text[56];
        snprintf(text,sizeof(text),"%c %02d  %-16s %s",i==channel?'>':' ',i+1,names[i],i==channel?"CHECK":"[x]");
        line(sf,a,2+i*2,text,i==channel?amber:white);
    }
}

static void signal(tui_surface *sf, tui_rect a)
{
    tui_box(sf,a,"SIGNAL / SAMPLE TRACE",cyan);
    int baseline = a.y+a.h-3;
    for (int c=2;c<a.w-2;c++) {
        int dist = c-a.w/2; if(dist<0)dist=-dist;
        int h = dist<3 ? a.h-5-dist : 1+(c%3==0);
        for(int r=0;r<h;r++)
            tui_put_char(sf,a,a.x+c,baseline-r,'|',dist<3?amber:cyan);
    }
    line(sf,a,a.h-2,"-120k          CENTER          +120k",LS_ATTR_DIM);
}

static void controls(tui_surface *sf,tui_rect a)
{
    const bool manual = fm && !scanning && !holding;
    ls_btn_t buttons[] = {
        {empty?"SCAN":manual?"A / B":holding?"RESUME":"HOLD",NULL,manual?'A':'H',holding,empty},
        {manual?"TUNE":"NEXT",NULL,manual?'T':'N',false,empty},
        {manual?"SCAN":"SKIP",NULL,manual?'S':'K',false,empty},
        {"LISTS",NULL,'L',false,false},
        {fm?"SQUELCH":"TUNE",NULL,fm?'Q':'T',false,false},
        {"MORE",NULL,'M',false,false},
    };
    ls_btn_bar_raised(sf,a,buttons,6,-1);
}

static void draw(tui_surface *sf,tui_rect a)
{
    const bool wide = a.w>72;
    a.x++; a.w-=2;
    line(sf,a,0,"DESIGN PREVIEW / SAMPLE DATA",LS_ATTR_DIM);
    tui_rect status=tui_rect_make(a.x,a.y+2,a.w,3);
    tui_box(sf,status,NULL,cyan);
    line(sf,status,1,empty?"NO RECEIVER / CONNECT USB SDR":holding?"HOLD / RECEIVING":scanning?">> SCANNING SAVED CHANNELS":fm?"FM / MANUAL / NFM":"P25 / RECEIVING",empty?LS_ATTR_DIM:holding?green:amber);
    const int bh=wide?5:10;
    tui_rect buttons=tui_rect_make(a.x,a.y+a.h-bh-3,a.w,bh);
    controls(sf,buttons);
    tui_rect content=tui_rect_make(a.x,a.y+6,a.w,buttons.y-(a.y+6)-1);
    if(empty) {
        ls_panel_notice(sf,content,"READY WHEN YOU ARE","No receiver connected","Saved lists remain available");
        return;
    }
    tui_rect left=content, right=content;
    if(wide) {
        left.w=(content.w-2)/2;
        right.x=left.x+left.w+2; right.w=content.w-left.w-2;
    }
    tui_rect primary=left; primary.h=wide?left.h:12;
    tui_box(sf,primary,fm?(bank_b?"> B / ACTIVE":"> A / ACTIVE"):"FIELD LIST / VHF DIGITAL",fm?amber:cyan);
    frequency(sf,primary,2,current_frequency(),fm?amber:white);
    line(sf,primary,8,fm?"MHz   NFM   STEP 12.5k":"MHz   P25 PHASE 1",cyan);
    line(sf,primary,10,fm?"ONE RECEIVER / SELECTED BANK ONLY":"CONVENTIONAL CHANNEL",LS_ATTR_DIM);
    if(wide) {
        if(scanning) list(sf,right);
        else {
            tui_box(sf,right,fm?"RECEIVER":"CHANNEL / DECODE",cyan);
            line(sf,right,2,fm?"B  152.6000 MHz / STANDBY":"NAC 293    TG 1201    UNIT 4827",white);
            line(sf,right,4,fm?"SQL 03   GAIN 22 dB   VOL 50":"SYNC LOCKED     VOICE CLEAR",green);
            line(sf,right,6,"AUDIO  [||||||||.....]",amber);
            line(sf,right,8,"LAST HEARD: VHF PRESET A  00:42",white);
        }
        return;
    }
    tui_rect middle=tui_rect_make(content.x,primary.y+13,content.w,12);
    if(scanning) list(sf,middle);
    else if(fm) {
        tui_box(sf,middle,bank_b?"A / STANDBY":"B / STANDBY",cyan);
        frequency(sf,middle,2,bank_b?"162.5500":"152.6000",cyan);
        line(sf,middle,8,"SAVED PRESET / TAP A-B TO SELECT",LS_ATTR_DIM);
        line(sf,middle,10,"MODE  [NFM]   WFM   AM   DATA >",white);
    } else {
        tui_box(sf,middle,"CHANNEL / DECODE",cyan);
        line(sf,middle,2,"NAC 293    TG 1201    UNIT 4827",white);
        line(sf,middle,4,"SYNC LOCKED          VOICE CLEAR",green);
        line(sf,middle,6,"FRAME QUALITY 98%    CALL 00:12",white);
        line(sf,middle,8,"AUDIO  [||||||||||||........]",amber);
        line(sf,middle,10,"GAIN 22 dB              VOL 50",LS_ATTR_DIM);
    }
    tui_rect lower=tui_rect_make(content.x,middle.y+13,content.w,content.y+content.h-middle.y-13);
    if(lower.h<4)return;
    if(fm || scanning) {
        tui_rect details=lower;
        if(lower.h>15) details.h=8;
        tui_box(sf,details,scanning?"SCAN STATUS":"RECEIVER / LAST HEARD",cyan);
        char progress[48];
        snprintf(progress,sizeof(progress),"CHECKING CHANNEL %02d / 04",channel+1);
        line(sf,details,2,scanning?progress:"CARRIER OPEN    SQL 03    VOL 50",scanning?amber:green);
        line(sf,details,4,scanning?"RESUME AFTER 2s QUIET":"GAIN 22 dB    STEP 12.5k",white);
        if(details.h>7) line(sf,details,6,"LAST: VHF PRESET A      00:42 AGO",LS_ATTR_DIM);
        if(details.h!=lower.h)
            signal(sf,tui_rect_make(lower.x,lower.y+9,lower.w,lower.h-9));
    } else signal(sf,lower);
}

static bool key(ls_tk_t k,char c)
{
    if(c>='a'&&c<='z') c-=32;
    if(k==LS_TK_UP || k==LS_TK_DOWN || c=='N' || c=='K') {
        channel=(channel+(k==LS_TK_UP?3:1))%4;return true;
    }
    if(c=='S'||c=='L'){scanning=true;holding=false;return true;}
    if(c=='H'){holding=!holding;scanning=!holding;return true;}
    if(c=='A'){bank_b=!bank_b;return true;}
    if(c=='D'){empty=!empty;return true;}
    return false;
}
static bool touch(int x,int y)
{
    int hit=ls_btn_hit(x,y);
    const bool manual=fm&&!scanning&&!holding;
    const char *keys=manual?"ATSLQM":"HNKLTM";
    return hit>=0&&hit<6?key(LS_TK_CHAR,keys[hit]):false;
}
static void p25_enter(void){fm=false;scanning=true;holding=empty=bank_b=false;channel=2;}
static void fm_enter(void){fm=true;scanning=holding=empty=bank_b=false;channel=2;}
const ls_tui_screen_t lssim_p25_preview={.name="P25",.enter=p25_enter,.draw=draw,.key=key,.touch=touch};
const ls_tui_screen_t lssim_fm_preview={.name="FM",.enter=fm_enter,.draw=draw,.key=key,.touch=touch};
