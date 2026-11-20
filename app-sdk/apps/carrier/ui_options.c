/* Small STATUS subpanels; keep the main screen's layout and navigation alone. */
#include "internal.h"
#include "art.h"
#include "ui.h"
static ui_rect choices[10],back_key,minus_key,plus_key;
static int code_pos,jump_system;
static bool digits;
void ui_options_reset(void) {code_pos=jump_system=0;digits=false;ui.options=0;}
void ui_options_open(int page) {ui.options=page;code_pos=0;}
static bool active(void) {
    if(game_state()->panel!=PANEL_STATUS) {ui.options=0;code_pos=0;}
    return ui.options!=0;
}
static void word(int x,int y,const char *s) {
    char text[104];int width=ui.view.w-x-1;if(width<1 || y>=ui.view.h-3) return;
    snprintf(text,sizeof text,"%.*s",width,s);ui_put(ui.view.x+x,ui.view.y+y,text,LAP_ATTR(HUD,0));
}
bool ui_options_draw(void) {
    if(!active()) return false;
    lap_fill(ui.view.x,ui.view.y,ui.view.w,ui.view.h,' ',LAP_ATTR(HUD,0));
    memset(choices,0,sizeof choices);minus_key=plus_key=(ui_rect){0};
    word(1,1,ui.options==1?"SOUND SETTINGS":"ADMIN / COMMANDER CHEATS");
    if(ui.options==1) {
        const char *names[]={ui.view.w<40?"UI":"UI SOUNDS",ui.view.w<40?"FX":"EFFECTS","MUSIC","VOLUME"};
        bool enabled[]={ui.ui_sounds,ui.sound,ui.music,ui.volume>0};
        char volume[16];snprintf(volume,sizeof volume,"%d%%",ui.volume*20);
        for(int i=0;i<4;i++) {
            int x=ui.view.x+ui.view.w*i/4;
            choices[i]=(ui_rect){x,ui.view.y+5,ui.view.x+ui.view.w*(i+1)/4-x,4};
            ui_toggle_box(&choices[i],names[i],i==3?volume:enabled[i]?"ON":"OFF",HUD,enabled[i]);
        }
        word(1,11,"Tap a setting; volume cycles 0 to 100%.");
        word(1,13,"Speech uses the separate VOICE toggle.");
    } else {
        const game_player *p=&game_state()->player;
        word(1,3,p->cheats_used?"SAVE MARKED: CHEATS USED":"Actions mark this commander's save.");
        const char *names[]={"+100,000 CREDITS","REFUEL","FULL REPAIR / SHIELDS","RESTOCK MISSILES / CHAFF",
            "MAX CARGO (1000 t)","JUMP TO SYSTEM","GOD MODE","SPAWN FOE (IN FLIGHT)","COMPLETE FIRST ACTIVE MISSION","REVEAL GALAXY / POIS"};
        int columns=(ui.view.h<30 || ui.view.w<45)?2:1,rows=10/columns;
        int h=(ui.view.h-13)/rows;if(h>3) h=3;if(h<1) h=1;
        const char *short_names[]={"+100K CR","REFUEL","REPAIR","RESTOCK","1000t HOLD","JUMP","GOD MODE","SPAWN FOE","FINISH JOB","REVEAL"};
        for(int i=0;i<10;i++) {
            int x=ui.view.x+1+(ui.view.w-2)*(i%columns)/columns;
            int width=(ui.view.w-2)/columns;
            choices[i]=(ui_rect){x,ui.view.y+5+(i/columns)*h,width,h};
            ui_toggle_box(&choices[i],columns==2?short_names[i]:names[i],"",HUD,i==ADMIN_GOD?p->god_mode:i==ADMIN_CARGO?p->cheat_cargo:i==ADMIN_REVEAL?p->galaxy_revealed:false);
        }
        int y=ui.view.y+6+rows*h;char text[64];
        snprintf(text,sizeof text,"%03d %.15s",jump_system,game_state()->systems[jump_system].name);
        minus_key=(ui_rect){ui.view.x+1,y,6,3};plus_key=(ui_rect){ui.view.x+ui.view.w-7,y,6,3};
        ui_toggle_box(&minus_key,"-","",HUD,false);
        ui_toggle_box(&plus_key,"+","",HUD,false);
        word(9,y-ui.view.y+1,text);
        word(1,y-ui.view.y+3,"System 0-255 / - +; G jumps.");
    }
    back_key=(ui_rect){ui.view.x,ui.view.y+ui.view.h-3,ui.view.w,3};
    ui_toggle_box(&back_key,"BACK TO STATUS","",HUD,false);
    return true;
}
static void choose(int i) {
    if(ui.options==1) {
        if(i==0) ui.ui_sounds=!ui.ui_sounds;
        else if(i==1) ui.sound=!ui.sound;
        else if(i==2) ui.music=!ui.music;
        else ui.volume=(ui.volume+1)%6;
        ui_sound_reset();ui_save_settings();ui_sound_feedback(LAP_SOUND_KEY);
    } else ui_sound_feedback(game_admin((game_admin_action)i,jump_system)?LAP_SOUND_ROGER:LAP_SOUND_ERROR);
}
bool ui_options_tap(int col,int row) {
    if(!active()) return false;
    if(ui_in(&back_key,col,row)) ui.options=0;
    else if(ui.options==2 && ui_in(&minus_key,col,row)) {jump_system=(jump_system+GAME_SYSTEMS-1)%GAME_SYSTEMS;digits=false;}
    else if(ui.options==2 && ui_in(&plus_key,col,row)) {jump_system=(jump_system+1)%GAME_SYSTEMS;digits=false;}
    else for(int i=0;i<(ui.options==1?4:10);i++) if(ui_in(&choices[i],col,row)) {choose(i);break;}
    return true;
}
bool ui_options_key(lap_key_t key,char ch) {
    bool open=active();
    if(open) {
        if(key==LAP_KEY_BACK || key==LAP_KEY_BACKSPACE) {ui.options=0;return true;}
        if(ui.options==2 && key==LAP_KEY_CHAR) {
            if(ch>='0' && ch<='9') {
                int next=(digits?jump_system*10:0)+ch-'0';
                jump_system=next<GAME_SYSTEMS?next:ch-'0';digits=true;
            } else if(ch=='+' || ch=='-' || ch=='=') {
                jump_system=(jump_system+(ch=='-'?GAME_SYSTEMS-1:1))%GAME_SYSTEMS;digits=false;
            } else if(ch=='g') choose(ADMIN_JUMP);
        }
        return true;
    }
    /* Begin with '/' so typing the deliberate code cannot operate flight keys. */
    static const char code[]="/admin";
    if(game_state()->panel!=PANEL_STATUS || key!=LAP_KEY_CHAR) {code_pos=0;return false;}
    if(!code_pos && ch!='/') return false;
    if((ch|32)==code[code_pos]) code_pos++;else code_pos=ch=='/'?1:0;
    if(code_pos==(int)sizeof(code)-1) {ui_options_open(2);digits=false;}
    return true;
}
