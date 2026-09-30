#include "ls_test.h"
#include "ls_tui_screen.h"
#include "ls_tui_ui.h"
#include "ls_music_backend.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

extern const ls_tui_screen_t ls_scr_music;
static bool wide;
static int rounded_height;
static int tracks=24, last_play=-1, volume=35, closes;
static ls_music_state_t state;
bool ls_tui_is_wide(void){return wide;}
int ls_tui_corner_pad(int row){return rounded_height && row>=rounded_height-4?5:0;}
bool ls_music_open(void){state=LS_MUSIC_STOPPED;return true;}
void ls_music_close(void){state=LS_MUSIC_STOPPED;closes++;}
bool ls_music_scan(int source){(void)source;state=LS_MUSIC_STOPPED;return true;}
int ls_music_count(void){return tracks;}
const char *ls_music_name(int i){static const char *names[]={"Night Dive.wav","Neon Reef.wav","Shark After Dark.wav","Coral Radio.wav"};return i>=0&&i<tracks?names[i%4]:"";}
const char *ls_music_error(void){return "";}
bool ls_music_play(int i){if(i<0||i>=tracks)return false;last_play=i;state=LS_MUSIC_PLAYING;return true;}
bool ls_music_toggle(void){state=state==LS_MUSIC_PLAYING?LS_MUSIC_PAUSED:LS_MUSIC_PLAYING;return true;}
void ls_music_stop(void){state=LS_MUSIC_STOPPED;}
ls_music_state_t ls_music_state(void){return state;}
int ls_music_volume(void){return volume;}
uint32_t ls_music_position_ms(void){return 73000;}
uint32_t ls_music_duration_ms(void){return 594000;}
unsigned ls_music_peak(int ch){static unsigned phase;return (phase++*997+ch*401)%32769;}
static bool warm;
bool ls_music_warm(void){return warm;}
void ls_music_set_warm(bool enabled){warm=enabled;}
void ls_music_volume_step(int d){volume+=d;}
const char *ls_music_output(void){return "TEST AUDIO";}
static bool keypad;
static int spectrum_calls;
bool ls_tui_keyboard_mode(void){return keypad;}
int ls_music_spectrum(float lo,float hi,int n,float *db){(void)lo;(void)hi;spectrum_calls++;for(int i=0;i<n;i++)db[i]=-12.0f-3*i;return state==LS_MUSIC_PLAYING?n:0;}
bool ls_music_format(ls_music_format_t *f){f->rate_hz=44100;f->bits=16;f->channels=2;f->mono_out=true;return true;}
float ls_music_volume_db(void){return (volume-100)*0.5f;}
static ls_music_mic_t mic;static int mic_starts;
bool ls_music_mic_start(bool record){mic_starts++;if(state==LS_MUSIC_PLAYING)state=LS_MUSIC_STOPPED;mic=record?LS_MIC_RECORDING:LS_MIC_LIVE;return true;}
void ls_music_mic_stop(void){mic=LS_MIC_OFF;}
ls_music_mic_t ls_music_mic_state(void){return mic;}
const char *ls_music_mic_file(void){return "/sdcard/music/REC-007.wav";}
uint32_t ls_music_mic_ms(void){return 12000;}
bool ls_music_mic_step(void){return true;}
bool ls_music_live(void){return state==LS_MUSIC_PLAYING||mic!=LS_MIC_OFF;}
static int gram_calls;
bool ls_music_spectrogram(ls_music_cursor_t *c,int hops,int rows,float *db){(void)hops;gram_calls++;if(state!=LS_MUSIC_PLAYING||(c->next++ & 1))return false;for(int i=0;i<rows;i++)db[i]=i==rows/2?-30.0f:-110.0f;return true;}

static bool locate(tui_cell *cells,int w,int h,const char*text,int*x,int*y) {
    int n=(int)strlen(text);
    for(int yy=0;yy<h;yy++)for(int xx=0;xx+n<=w;xx++) {
        int j=0;while(j<n && cells[yy*w+xx+j].ch==text[j])j++;
        if(j==n && (xx+n==w || cells[yy*w+xx+n].ch==' ' || cells[yy*w+xx+n].ch=='|')){*x=xx+n/2;*y=yy;return true;}
    }
    return false;
}
static void paint(tui_surface*sf,int w,int h){tui_frame_begin(sf);ls_scr_music.draw(sf,tui_rect_make(0,0,w,h));}
static void tap_label(tui_surface*sf,tui_cell*c,int w,int h,const char*text){
    int x=-1,y=-1;paint(sf,w,h);LS_CHECK_MSG(locate(c,w,h,text,&x,&y),"missing button %s at %dx%d",text,w,h);
    if(x>=0)LS_CHECK(ls_scr_music.touch(x,y));
}
static void exercise(int w,int h) {
    wide=w>h;volume=35;warm=false;last_play=-1;
    size_t n=(size_t)w*h;tui_cell *back=calloc(n+2,sizeof(*back)),*front=calloc(n+2,sizeof(*front));LS_CHECK(back&&front);
    back[0].ch='A';back[n+1].ch='Z';tui_surface sf;tui_surface_setup(&sf,back+1,front+1,w,h);
    ls_scr_music.enter();paint(&sf,w,h);
    tap_label(&sf,back+1,w,h,"PLAY");LS_EQ_INT(last_play,0);LS_EQ_INT(state,LS_MUSIC_PLAYING);
    tap_label(&sf,back+1,w,h,"PAUSE");LS_EQ_INT(state,LS_MUSIC_PAUSED);
    tap_label(&sf,back+1,w,h,"PLAY");LS_EQ_INT(state,LS_MUSIC_PLAYING);
    tap_label(&sf,back+1,w,h,"VOL +");LS_EQ_INT(volume,40);
    tap_label(&sf,back+1,w,h,"VOL -");LS_EQ_INT(volume,35);
    tap_label(&sf,back+1,w,h,"MUTE");LS_EQ_INT(volume,0);
    tap_label(&sf,back+1,w,h,"UNMUTE");LS_EQ_INT(volume,35);
    tap_label(&sf,back+1,w,h,"TRACKS");
    paint(&sf,w,h);ls_scr_music.key(LS_TK_DOWN,0);ls_scr_music.key(LS_TK_ENTER,0);
    LS_EQ_INT(last_play,1);LS_EQ_INT(state,LS_MUSIC_PLAYING);
    tap_label(&sf,back+1,w,h,"TRACKS");tap_label(&sf,back+1,w,h,"TONE FLAT");LS_CHECK(warm);
    tap_label(&sf,back+1,w,h,"TONE WARM");LS_CHECK(!warm);
    tap_label(&sf,back+1,w,h,"NEXT PAGE");
    paint(&sf,w,h);ls_scr_music.key(LS_TK_ENTER,0);LS_CHECK(last_play>1);
    ls_scr_music.key(LS_TK_CHAR,'s');LS_EQ_INT(state,LS_MUSIC_STOPPED);
    for(int i=0;i<48;i++)paint(&sf,w,h);
    if(n==2600){int rw=w==40?100:40,rh=h==65?26:65;wide=rw>rh;tui_surface_setup(&sf,back+1,front+1,rw,rh);paint(&sf,rw,rh);tap_label(&sf,back+1,rw,rh,"PLAY");LS_EQ_INT(state,LS_MUSIC_PLAYING);}
    LS_EQ_INT(back[0].ch,'A');LS_EQ_INT(back[n+1].ch,'Z');ls_scr_music.leave();free(back);free(front);
}
LS_CASE(portrait_touch_and_keys){exercise(40,65);}
LS_CASE(p4_portrait){exercise(52,70);}
LS_CASE(p4_controls_clear_rounded_corners){rounded_height=70;exercise(52,70);rounded_height=0;}
LS_CASE(landscape_keyboard_and_touch){exercise(100,26);}
LS_CASE(compact_landscape){exercise(65,20);}
LS_CASE(empty_playlist_never_starts_invalid_track){tracks=0;last_play=-1;ls_scr_music.enter();ls_scr_music.key(LS_TK_ENTER,0);ls_scr_music.key(LS_TK_RIGHT,0);LS_EQ_INT(last_play,-1);ls_scr_music.leave();tracks=24;}

static void focus_navigation(int w,int h,int corner_height){
    size_t n=(size_t)w*h;
    tui_cell *back=calloc(n,sizeof(*back)),*front=calloc(n,sizeof(*front));
    LS_CHECK(back&&front);
    if(!back||!front){free(back);free(front);return;}
    tui_surface sf;
    wide=w>h;rounded_height=corner_height;last_play=-1;volume=35;tracks=24;
    tui_surface_setup(&sf,back,front,w,h);
    ls_scr_music.enter();
    paint(&sf,w,h);
    LS_CHECK(ls_scr_music.key(LS_TK_TAB,0)); /* Player PREV has focus. */
    LS_CHECK(ls_scr_music.key(LS_TK_DOWN,0));
    LS_CHECK(ls_scr_music.key(LS_TK_ENTER,0)); /* No redraw between keys. */
    LS_EQ_INT(last_play,1);
    paint(&sf,w,h);
    LS_CHECK(ls_scr_music.key(LS_TK_TAB,0));
    LS_CHECK(ls_scr_music.key(LS_TK_CHAR,'l'));
    LS_CHECK(ls_scr_music.key(LS_TK_ENTER,0)); /* View switch clears player focus. */
    LS_EQ_INT(last_play,1);
    paint(&sf,w,h);
    LS_CHECK(ls_scr_music.key(LS_TK_TAB,0)); /* Player PREV has focus again after starting a track. */
    LS_CHECK(ls_scr_music.key(LS_TK_DOWN,0));
    LS_CHECK(ls_scr_music.key(LS_TK_ENTER,0)); /* Selection clears library focus. */
    LS_EQ_INT(last_play,2);
    paint(&sf,w,h);
    LS_CHECK(ls_scr_music.key(LS_TK_TAB,0));
    LS_CHECK(ls_scr_music.key(LS_TK_UP,0));
    LS_CHECK(ls_scr_music.key(LS_TK_ENTER,0));
    LS_EQ_INT(last_play,1);
    paint(&sf,w,h);
    LS_CHECK(ls_scr_music.key(LS_TK_CHAR,'l'));
    paint(&sf,w,h);
    LS_CHECK(ls_scr_music.key(LS_TK_TAB,0));
    LS_CHECK(ls_scr_music.key(LS_TK_UP,0));
    LS_CHECK(ls_scr_music.key(LS_TK_ENTER,0));
    LS_EQ_INT(last_play,0);
    ls_scr_music.leave();
    rounded_height=0;free(back);free(front);
}
LS_CASE(portrait_selection_clears_stale_focus){focus_navigation(40,65,0);}
LS_CASE(landscape_selection_clears_stale_focus){focus_navigation(100,26,0);}
LS_CASE(rounded_landscape_selection_clears_stale_focus){focus_navigation(100,26,26);}

LS_CASE(rounded_landscape_touch_controls){rounded_height=26;exercise(100,26);rounded_height=0;}

static bool contains(tui_cell *cells,int w,int h,const char *text){
    int n=(int)strlen(text);
    for(int yy=0;yy<h;yy++)for(int xx=0;xx+n<=w;xx++){
        int j=0;while(j<n && cells[yy*w+xx+j].ch==text[j])j++;
        if(j==n)return true;
    }
    return false;
}
/* Lit gauge segments on the row under the volume readout. */
static int gauge_lit(tui_cell *cells,int w,int h,int value){
    char text[32];int x,y;snprintf(text,sizeof(text),"VOLUME %d%%",value);
    if(!locate(cells,w,h,text,&x,&y)||y+1>=h)return -1;
    int lit=0,segments=0;
    for(int xx=0;xx<w;xx++){
        tui_cell c=cells[(y+1)*w+xx];
        if(c.ch!=LS_TUI_BLOCK_LEFT)continue;
        segments++;
        if(TUI_ATTR_FG(c.attr)!=LS_FAINT_FG)lit++;
    }
    return segments==20?lit:-2;
}
static void volume_display(int w,int h){
    size_t n=(size_t)w*h;
    tui_cell *back=calloc(n,sizeof(*back)),*front=calloc(n,sizeof(*front));
    LS_CHECK(back&&front);
    if(!back||!front){free(back);free(front);return;}
    tui_surface sf;int x,y;
    wide=w>h;rounded_height=0;volume=0;
    tui_surface_setup(&sf,back,front,w,h);ls_scr_music.enter();
    paint(&sf,w,h);
    LS_CHECK(locate(back,w,h,"MUTED",&x,&y));
    LS_EQ_INT(gauge_lit(back,w,h,0),0);
    volume=50;paint(&sf,w,h);
    LS_CHECK(contains(back,w,h,"-25.0 dB"));
    LS_EQ_INT(gauge_lit(back,w,h,50),10);
    volume=100;paint(&sf,w,h);
    LS_EQ_INT(gauge_lit(back,w,h,100),20);
    ls_scr_music.leave();free(back);free(front);
}
LS_CASE(volume_gauge_portrait){volume_display(52,70);}
LS_CASE(volume_gauge_landscape){volume_display(118,28);}
LS_CASE(volume_gauge_compact){volume_display(65,20);}

LS_CASE(volume_moves_in_five_point_steps_and_clamps){
    ls_scr_music.enter();
    volume=43;ls_scr_music.key(LS_TK_CHAR,'+');LS_EQ_INT(volume,45);
    ls_scr_music.key(LS_TK_CHAR,'=');LS_EQ_INT(volume,50);
    ls_scr_music.key(LS_TK_CHAR,'-');LS_EQ_INT(volume,45);
    volume=43;ls_scr_music.key(LS_TK_CHAR,'-');LS_EQ_INT(volume,40);
    volume=98;ls_scr_music.key(LS_TK_CHAR,'+');LS_EQ_INT(volume,100);
    ls_scr_music.key(LS_TK_CHAR,'+');LS_EQ_INT(volume,100);
    volume=3;ls_scr_music.key(LS_TK_CHAR,'-');LS_EQ_INT(volume,0);
    ls_scr_music.key(LS_TK_CHAR,'-');LS_EQ_INT(volume,0);
    ls_scr_music.leave();volume=35;
}

static void view_switch(int w,int h,int w2,int h2){
    size_t n=(size_t)(w>w2?w:w2)*(h>h2?h:h2);
    tui_cell *back=calloc(n,sizeof(*back)),*front=calloc(n,sizeof(*front));
    LS_CHECK(back&&front);
    if(!back||!front){free(back);free(front);return;}
    tui_surface sf;wide=w>h;rounded_height=0;keypad=false;tracks=24;volume=35;
    tui_surface_setup(&sf,back,front,w,h);
    ls_scr_music.key(LS_TK_CHAR,'1');
    ls_scr_music.enter();paint(&sf,w,h);
    LS_CHECK(contains(back,w,h,"PEAK LEVEL"));
    LS_CHECK(contains(back,w,h,"PEAK HOLD"));
    tap_label(&sf,back,w,h,"PLAY");
    /* Only the view on screen does any analysis. */
    spectrum_calls=gram_calls=0;paint(&sf,w,h);paint(&sf,w,h);
    LS_EQ_INT(spectrum_calls,0);LS_EQ_INT(gram_calls,0);
    tap_label(&sf,back,w,h,"BANDS");paint(&sf,w,h);
    LS_CHECK(spectrum_calls>0);LS_EQ_INT(gram_calls,0);
    LS_CHECK(contains(back,w,h," SPECTRUM 2/3 OCTAVE "));
    LS_CHECK(contains(back,w,h,"16k"));LS_CHECK(contains(back,w,h,"1.6k"));
    LS_CHECK(!contains(back,w,h,"PEAK HOLD"));
    tap_label(&sf,back,w,h,"SCAN");paint(&sf,w,h);
    LS_CHECK(contains(back,w,h," SCAN "));LS_CHECK(contains(back,w,h,"RAINBOW"));
    LS_CHECK(contains(back,w,h,"1k"));
    /* Thin bars in more than one colour. */
    int traces=0;uint8_t first=0;bool colours=false;
    for(size_t i=0;i<(size_t)w*h;i++)if((uint8_t)back[i].ch>=0xA0&&(uint8_t)back[i].ch<=0xA7){
        if(!traces++)first=TUI_ATTR_FG(back[i].attr);else if(TUI_ATTR_FG(back[i].attr)!=first)colours=true;}
    LS_CHECK(traces>20);LS_CHECK(colours);
    tap_label(&sf,back,w,h,"RAINBOW");paint(&sf,w,h);LS_CHECK(contains(back,w,h,"FIRE"));
    LS_CHECK(ls_scr_music.key(LS_TK_CHAR,'c'));paint(&sf,w,h);LS_CHECK(contains(back,w,h,"NEON"));
    spectrum_calls=gram_calls=0;
    LS_CHECK(ls_scr_music.key(LS_TK_CHAR,'v'));paint(&sf,w,h);paint(&sf,w,h);
    LS_CHECK(contains(back,w,h," WATERFALL "));LS_CHECK(gram_calls>0);LS_EQ_INT(spectrum_calls,0);
    LS_CHECK(contains(back,w,h,"46 ms/COL"));
    LS_CHECK(ls_scr_music.key(LS_TK_CHAR,'z'));paint(&sf,w,h);LS_CHECK(contains(back,w,h,"92 ms/COL"));
    LS_CHECK(contains(back,w,h,"NOW"));LS_CHECK(contains(back,w,h,"22k"));
    wide=w2>h2;tui_surface_setup(&sf,back,front,w2,h2);paint(&sf,w2,h2);
    LS_CHECK(contains(back,w2,h2," WATERFALL "));
    ls_scr_music.leave();ls_scr_music.enter();paint(&sf,w2,h2);
    LS_CHECK(contains(back,w2,h2," WATERFALL "));
    LS_CHECK(ls_scr_music.key(LS_TK_CHAR,'v'));paint(&sf,w2,h2);
    LS_CHECK(contains(back,w2,h2,"PEAK LEVEL"));
    LS_CHECK(ls_scr_music.key(LS_TK_CHAR,'3'));paint(&sf,w2,h2);
    LS_CHECK(contains(back,w2,h2," SCAN "));
    ls_scr_music.key(LS_TK_CHAR,'1');ls_scr_music.key(LS_TK_CHAR,'z');ls_scr_music.key(LS_TK_CHAR,'z');
    while(!contains(back,w2,h2,"RAINBOW")){ls_scr_music.key(LS_TK_CHAR,'3');ls_scr_music.key(LS_TK_CHAR,'c');paint(&sf,w2,h2);}
    ls_scr_music.key(LS_TK_CHAR,'1');
    ls_scr_music.leave();free(back);free(front);
}
LS_CASE(view_switch_landscape_then_portrait){view_switch(118,28,52,70);}
LS_CASE(view_switch_portrait_then_compact){view_switch(52,70,65,20);}

/* Every control carries its key while a keyboard is attached, and none do
   without one. */
static void badges(int w,int h){
    size_t n=(size_t)w*h;
    tui_cell *back=calloc(n,sizeof(*back)),*front=calloc(n,sizeof(*front));
    LS_CHECK(back&&front);
    if(!back||!front){free(back);free(front);return;}
    tui_surface sf;wide=w>h;rounded_height=0;tracks=24;volume=35;
    tui_surface_setup(&sf,back,front,w,h);ls_scr_music.enter();
    keypad=true;paint(&sf,w,h);
    static const char *const player[]={"[B]","[P]","[N]","[-]","[M]","[+]","[L]","[1]","[2]","[3]","[4]"};
    for(unsigned i=0;i<sizeof(player)/sizeof(*player);i++)
        LS_CHECK_MSG(contains(back,w,h,player[i]),"missing %s at %dx%d",player[i],w,h);
    ls_scr_music.key(LS_TK_CHAR,'l');paint(&sf,w,h);
    static const char *const lib[]={"[ESC]","[U]","[J]","[E]","[D]","[R]"};
    for(unsigned i=0;i<sizeof(lib)/sizeof(*lib);i++)
        LS_CHECK_MSG(contains(back,w,h,lib[i]),"missing %s at %dx%d",lib[i],w,h);
    /* The legends are the bindings. */
    keypad=false;ls_scr_music.key(LS_TK_ESC,0);paint(&sf,w,h);
    LS_CHECK(!contains(back,w,h,"[P]"));LS_CHECK(!contains(back,w,h,"[1]"));
    ls_scr_music.leave();free(back);free(front);
}
LS_CASE(key_badges_landscape){badges(118,28);}
LS_CASE(key_badges_portrait){badges(52,70);}
LS_CASE(key_badges_compact){badges(65,20);}

LS_CASE(page_keys_only_act_in_library){
    ls_scr_music.enter();tracks=24;
    LS_CHECK(!ls_scr_music.key(LS_TK_CHAR,'j'));
    LS_CHECK(ls_scr_music.key(LS_TK_CHAR,'l'));
    LS_CHECK(ls_scr_music.key(LS_TK_CHAR,'j'));
    LS_CHECK(ls_scr_music.key(LS_TK_CHAR,'u'));
    ls_scr_music.leave();
}

/* MIC listens, REC records; either feeds the plots with no track playing,
   and both are reachable by touch, by key and by the keyboard's record key. */
static void microphone(int w,int h){
    size_t n=(size_t)w*h;
    tui_cell *back=calloc(n,sizeof(*back)),*front=calloc(n,sizeof(*front));
    LS_CHECK(back&&front);
    if(!back||!front){free(back);free(front);return;}
    tui_surface sf;wide=w>h;rounded_height=0;tracks=24;keypad=false;mic=LS_MIC_OFF;
    tui_surface_setup(&sf,back,front,w,h);ls_scr_music.enter();
    ls_scr_music.key(LS_TK_CHAR,'2');paint(&sf,w,h);
    tap_label(&sf,back,w,h,"MIC");LS_EQ_INT(mic,LS_MIC_LIVE);
    spectrum_calls=0;paint(&sf,w,h);LS_CHECK(spectrum_calls>0);
    LS_CHECK(contains(back,w,h,"MICROPHONE"));LS_CHECK(contains(back,w,h,"Live input"));
    tap_label(&sf,back,w,h,"MIC");LS_EQ_INT(mic,LS_MIC_OFF);
    tap_label(&sf,back,w,h,"REC");LS_EQ_INT(mic,LS_MIC_RECORDING);paint(&sf,w,h);
    LS_CHECK(contains(back,w,h,"REC-007.wav"));LS_CHECK(contains(back,w,h,"00:12"));
    LS_CHECK(ls_scr_music.key(LS_TK_MIC,0));LS_EQ_INT(mic,LS_MIC_OFF);
    LS_CHECK(ls_scr_music.key(LS_TK_CHAR,'i'));LS_EQ_INT(mic,LS_MIC_LIVE);
    LS_CHECK(ls_scr_music.key(LS_TK_CHAR,'w'));LS_EQ_INT(mic,LS_MIC_RECORDING);
    keypad=true;paint(&sf,w,h);
    LS_CHECK(contains(back,w,h,"[I]"));LS_CHECK(contains(back,w,h,"[W]"));
    keypad=false;ls_scr_music.key(LS_TK_CHAR,'1');
    ls_scr_music.leave();mic=LS_MIC_OFF;free(back);free(front);
}
LS_CASE(microphone_landscape){microphone(118,28);}
LS_CASE(microphone_portrait){microphone(52,70);}
LS_CASE(microphone_compact){microphone(65,20);}
