#include "../../ls_tui_screen.h"
#include "../../ls_tui_ui.h"
#include "../../ls_keyboard.h"
#include "../../ls_field.h"
#include "../../ls_motion.h"
#include "ls_nfc_suite.h"
#include "nfc_classic.h"
#include "esp_attr.h"
#include "esp_timer.h"
#include <stdio.h>
#include <string.h>
static ls_nfc_suite_status_t card;
static unsigned selected_block;
static bool detail,key_b;
static int focus=-1,slot;
static char feedback[96];
static tui_rect cells;
static unsigned columns,page_first;
static unsigned cell_width=5,cell_height=1,page_size=256;
static bool portrait;
EXT_RAM_BSS_ATTR static uint8_t previous_state[256];
EXT_RAM_BSS_ATTR static int64_t block_changed[256];
static void page_move(int direction){int next=(int)selected_block+direction*(int)page_size;if(next<0)next=0;if(next>=card.blocks)next=card.blocks?card.blocks-1:0;selected_block=next;focus=-1;}
static void key_done(const char *value){snprintf(feedback,sizeof(feedback),"%s",(card.identified?ls_nfc_suite_sector_key(ls_classic_sector(selected_block),key_b,value):ls_nfc_suite_key(key_b,value))?"Key set in RAM; press READ to use it":"Key requires exactly 12 hex digits; stop scan before editing");}
static void action(int i){
 bool ok=true;feedback[0]=0;
 if(i==0)ok=ls_nfc_suite_start(false);
 if(i==1)ok=ls_nfc_suite_start(true);
 if(i==2)ls_nfc_suite_stop();
 if(i==3)detail=!detail;
 if(i==4||i==5){key_b=i==5;char title[40];if(card.identified)snprintf(title,sizeof(title),"SECTOR %u KEY %c / 12 HEX",ls_classic_sector(selected_block),key_b?'B':'A');else snprintf(title,sizeof(title),"DEFAULT KEY %c / 12 HEX",key_b?'B':'A');ls_keyboard_open(title,"",12,key_done);}
 if(i==6)ok=ls_nfc_suite_save();
 if(i==7){char note[240],uid[32]={0};for(unsigned n=0;n<card.uid_len;n++)snprintf(uid+n*2,sizeof(uid)-n*2,"%02X",card.uid[n]);snprintf(note,sizeof(note),"NFC UID %s ATQA %04X SAK %02X. Verified blocks %u/%u; authentication attempts %lu, successful %lu. Unread bytes remain unknown.",uid,card.atqa,card.sak,card.read_blocks,card.blocks,(unsigned long)card.attempts,(unsigned long)card.auth_ok);ok=ls_field_mark_radio("NFC card survey",note,LS_FIELD_NFC,13560000,card.read_blocks);}
 if(!ok)snprintf(feedback,sizeof(feedback),"Not accepted: finish/stop current operation or identify a card first");
}
static void enter(void){focus=-1;slot=0;selected_block=0;detail=false;feedback[0]=0;}
static void draw(tui_surface *sf,tui_rect a){
 ls_nfc_suite_snapshot(&card);if(card.blocks && selected_block>=card.blocks)selected_block=0;
 int64_t now=esp_timer_get_time();
 for(unsigned b=0;b<card.blocks;b++)if(previous_state[b]!=card.block_state[b]){previous_state[b]=card.block_state[b];block_changed[b]=now;}
 portrait=a.w<90;
 if(portrait && a.h>35){
  ls_btn_t nav[]={{"PREV","PAGE",',',false,!card.blocks||selected_block<page_size},{"NEXT","PAGE",'.',false,!card.blocks||page_first+page_size>=card.blocks},{"BACK","RADIOS",0,false,false}};
  int nav_h=5;
  ls_btn_bar_raised_slot(sf,tui_rect_make(a.x,a.y+a.h-nav_h,a.w,nav_h),nav,3,-1,LS_BTN_SLOT_WATERFALL);
  a.h-=nav_h;
 }
 ls_btn_t buttons[]={{"SCAN","ID",'i',false,card.busy},{"READ","A + B",'r',card.reading,card.busy},{"STOP","KEEP",'x',false,!card.busy},{"VIEW",detail?"HEX":"MAP",'v',detail,false},{"KEY A","SET",'a',false,card.busy},{"KEY B","SET",'b',false,card.busy},{"SAVE","SD",'s',false,card.busy||!card.identified},{"MARK","JOURNAL",'j',false,!card.identified}};
 int h=ls_btn_raised_height(a,8);ls_btn_bar_raised(sf,tui_rect_make(a.x,a.y,a.w,h),buttons,8,focus);
 tui_rect box=tui_rect_make(a.x,a.y+h,a.w,a.h-h-2);ls_panel_box(sf,box,"NFC / CARD WORKBENCH",TUI_CYAN);
 ls_motion_busy(sf,box,card.busy);
 char line[112],uid[32]={0};for(unsigned n=0;n<card.uid_len;n++)snprintf(uid+n*2,sizeof(uid)-n*2,"%02X",card.uid[n]);
 snprintf(line,sizeof(line),"UID %s  ATQA %04X  SAK %02X",card.identified?uid:"--",card.atqa,card.sak);tui_put_str(sf,box,box.x+2,box.y+1,card.identified?line:"UID --  ATQA --  SAK --",LS_ATTR_DIM);
 snprintf(line,sizeof(line),portrait?"%u/%u read | auth %lu/%lu | E%lu N%lu":"%u/%u blocks read | %lu auth OK / %lu attempts | %lu errors / %lu NAKs",card.read_blocks,card.blocks,(unsigned long)card.auth_ok,(unsigned long)card.attempts,(unsigned long)card.errors,(unsigned long)card.naks);tui_put_str(sf,box,box.x+2,box.y+2,line,LS_ATTR_DIM);
 if(!card.identified){tui_put_str(sf,box,box.x+2,box.y+4,"SCAN verifies the UID and memory layout.",LS_ATTR_DIM);tui_put_str(sf,box,box.x+2,box.y+6,"READ uses Key A/B. Default: FFFFFFFFFFFF.",LS_ATTR_DIM);tui_put_str(sf,box,box.x+2,box.y+8,"Set your keys with KEY A/B. Colours are live.",LS_ATTR_DIM);}
 else if(!card.blocks)tui_put_str(sf,box,box.x+2,box.y+4,"Identity verified. Memory protocol unsupported; no blocks invented.",LS_ATTR_DIM);
 else {
  unsigned sector=ls_classic_sector(selected_block),first=ls_classic_first(sector),count=ls_classic_count(sector);
  snprintf(line,sizeof(line),"Block %03u / sector %02u / %s | Key A %s  B %s",selected_block,sector,selected_block==first+count-1?"TRAILER":selected_block==0?"MANUFACTURER":"DATA",card.sector_keys[sector]&1?"verified":"unverified",card.sector_keys[sector]&2?"verified":"unverified");if(portrait)snprintf(line,sizeof(line),"B%03u S%02u %s | A:%s B:%s",selected_block,sector,selected_block==first+count-1?"TRAILER":selected_block==0?"MFR":"DATA",card.sector_keys[sector]&1?"OK":"?",card.sector_keys[sector]&2?"OK":"?");tui_put_str(sf,box,box.x+2,box.y+3,line,LS_ATTR_DIM);
  if(detail){
   uint8_t data[16]={0};uint16_t mask=0;ls_nfc_suite_block(selected_block,data,&mask);
   for(unsigned row=0;row<2;row++){
    char hex[64],ascii[9];int pos=snprintf(hex,sizeof(hex),"%02X: ",row*8);
    for(unsigned col=0;col<8;col++){unsigned b=row*8+col;bool valid=mask&(1u<<b);pos+=snprintf(hex+pos,sizeof(hex)-pos,valid?"%02X ":"?? ",data[b]);ascii[col]=valid?(data[b]>=32&&data[b]<127?data[b]:'.'):'?';}ascii[8]=0;
    snprintf(line,sizeof(line),"%s  |%s|",hex,ascii);tui_put_str(sf,box,box.x+2,box.y+6+row*2,line,TUI_ATTR(TUI_CYAN|TUI_BRIGHT,TUI_BLACK));
   }
   tui_put_str(sf,box,box.x+2,box.y+11,"?? = unread or non-readable bytes",LS_ATTR_DIM);
   if(selected_block==first+count-1 && (mask&0x03c0)==0x03c0){
    unsigned c1=data[7]>>4,c2=data[8]&15,c3=data[8]>>4;
    bool valid=((data[6]&15)^c1)==15 && ((data[6]>>4)^c2)==15 && ((data[7]&15)^c3)==15;
    snprintf(line,sizeof(line),portrait?"Access %s | C1/2/3 %X/%X/%X | GPB %02X":"Access bits: %s | groups C1/C2/C3 %X/%X/%X | GPB %02X",valid?(portrait?"OK":"redundancy OK"):"INVALID",c1,c2,c3,data[9]);tui_put_str(sf,box,box.x+2,box.y+13,line,LS_ATTR_DIM);
   }
  }else{
   cells=tui_rect_make(box.x+2,box.y+5,box.w-4,box.h-7);cell_width=portrait?6:5;cell_height=portrait?3:1;columns=cells.w/cell_width;if(columns>(portrait?8u:16u))columns=portrait?8:16;if(!columns)columns=1;
   unsigned rows=cells.h>(int)cell_height?cells.h/cell_height:1,capacity=columns*rows;page_size=capacity;page_first=selected_block/capacity*capacity;
   for(unsigned n=0;n<capacity && page_first+n<card.blocks;n++){
    unsigned b=page_first+n,v=card.block_state[b];char text[6];snprintf(text,sizeof(text),b==selected_block?">%02X<":"[%02X]",b);
    uint8_t color=v==LS_CARD_READ?TUI_GREEN:v==LS_CARD_ACTIVE?TUI_CYAN:v==LS_CARD_AUTH_FAILED?TUI_YELLOW:v==LS_CARD_ERROR?TUI_RED:v==LS_CARD_NAK?TUI_MAGENTA:TUI_WHITE;
    if(v==LS_CARD_READ && block_changed[b] && now-block_changed[b]<350000)color=TUI_WHITE|TUI_BRIGHT;
    int x=cells.x+(n%columns)*cell_width,y=cells.y+(n/columns)*cell_height;
    if(portrait)ls_fill_dither(sf,tui_rect_make(x,y,cell_width-1,cell_height-1),LS_DITHER_LIGHT,color);
    if(v==LS_CARD_ACTIVE)text[0]=ls_motion_pip(card.busy);
    tui_put_str(sf,cells,x,y+(portrait?1:0),text,TUI_ATTR(color|(b==selected_block?TUI_BRIGHT:0),TUI_BLACK));
   }
   if(box.w>=110){
    tui_rect preview=tui_rect_make(box.x+84,box.y+5,box.w-86,box.h-8);
    ls_panel_box(sf,preview,"SELECTED BLOCK",TUI_CYAN);
    uint8_t data[16]={0};uint16_t mask=0;ls_nfc_suite_block(selected_block,data,&mask);
    snprintf(line,sizeof(line),"%03u / sector %02u",selected_block,sector);
    tui_put_str(sf,preview,preview.x+2,preview.y+1,line,LS_ATTR_DIM);
    for(unsigned row=0;row<4;row++){
     char text[40],ascii[5];int pos=snprintf(text,sizeof(text),"%02X ",row*4);
     for(unsigned col=0;col<4;col++){unsigned b=row*4+col;bool valid=mask&(1u<<b);pos+=snprintf(text+pos,sizeof(text)-pos,valid?"%02X ":"?? ",data[b]);ascii[col]=valid?(data[b]>=32&&data[b]<127?data[b]:'.'):'?';}ascii[4]=0;
     snprintf(line,sizeof(line),"%s %s",text,ascii);
     tui_put_str(sf,preview,preview.x+2,preview.y+3+row,line,LS_ATTR_DIM);
    }
    tui_put_str(sf,preview,preview.x+2,preview.y+8,mask?"Parity + CRC verified":"No verified bytes",LS_ATTR_DIM);
    tui_put_str(sf,preview,preview.x+2,preview.y+10,"?? = unknown bytes",LS_ATTR_DIM);
   }
  }
 }
 tui_put_str(sf,box,box.x+2,box.y+box.h-2,portrait?"Grey ? / Green read / Yellow auth / Red error":"Grey unread / cyan active / green read / yellow auth failed / red error / purple NAK",LS_ATTR_DIM);
 if(card.blocks && !detail){
  int width=box.w-4;if(width>40)width=40;
  for(int x=0;x<width;x++)tui_put_char(sf,box,box.x+2+x,box.y+4,x*card.blocks<card.read_blocks*width?'=':'.',TUI_ATTR(TUI_GREEN,TUI_BLACK));
 }
 snprintf(line,sizeof(line),"%c %s",ls_motion_pip(card.busy),feedback[0]?feedback:card.status);
 ls_safe_line(sf,a,a.y+a.h-2,line,LS_ATTR_DIM);
 ls_safe_line(sf,a,a.y+a.h-1,portrait?"Purple NAK | tap block / VIEW for bytes":"Arrows: blocks | Enter: hex | Tab: controls | Esc: receive history",LS_ATTR_DIM);
}
static bool key(ls_tk_t k,char c){
    if (c >= 'A' && c <= 'Z') c += 'a' - 'A';
 if(k==LS_TK_TAB){ls_btn_navigate(k,&slot,&focus,false);return true;}
 if(k==LS_TK_ENTER){if(focus>=0){if(ls_btn_enabled(0,focus))action(focus);}else detail=!detail;return true;}
 if(k==LS_TK_UP||k==LS_TK_DOWN||k==LS_TK_LEFT||k==LS_TK_RIGHT){focus=-1;int delta=k==LS_TK_LEFT?-1:k==LS_TK_RIGHT?1:k==LS_TK_UP?-(int)(columns?columns:16):(int)(columns?columns:16);int n=(int)selected_block+delta;if(n>=0&&n<card.blocks)selected_block=n;return true;}
 if(k!=LS_TK_CHAR)return false;
 if(c==','){page_move(-1);return true;}if(c=='.'){page_move(1);return true;}
 const char *p=c?strchr("irxvabsj",c):NULL;if(!p)return false;action(p-"irxvabsj");return true;
}
static bool touch(int x,int y){int i=ls_btn_hit(x,y);if(i>=0){action(i);return true;}if(portrait){i=ls_btn_hit_slot(x,y,LS_BTN_SLOT_WATERFALL);if(i==2)return false;if(i>=0){page_move(i?1:-1);return true;}}if(!detail && x>=cells.x && y>=cells.y && x<cells.x+(int)(columns*cell_width) && y<cells.y+(int)(page_size/columns*cell_height)){unsigned b=page_first+(y-cells.y)/cell_height*columns+(x-cells.x)/cell_width;if(b<card.blocks){selected_block=b;focus=-1;}}return true;}
const ls_tui_screen_t ls_scr_nfc={.name="NFC",.enter=enter,.draw=draw,.key=key,.touch=touch};
