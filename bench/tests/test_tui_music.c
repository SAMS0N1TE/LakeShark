#include "ls_test.h"
#include "ls_tui_screen.h"
#include "ls_tui_ui.h"
#include "ls_music_backend.h"
#include "ls_options.h"
#include "ls_audio_view.h"
#include "ls_spectro.h"
#include "esp_timer.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

extern const ls_tui_screen_t ls_scr_music;
static bool wide;
static int rounded_height;
static int tracks=24, last_play=-1, volume=35, closes;
static ls_music_state_t state;
static unsigned format_epoch;
bool ls_tui_is_wide(void){return wide;}
int ls_tui_corner_pad(int row){return rounded_height && row>=rounded_height-4?5:0;}
bool ls_music_open(void){state=LS_MUSIC_STOPPED;return true;}
void ls_music_close(void){state=LS_MUSIC_STOPPED;closes++;}
bool ls_music_scan(int source){(void)source;state=LS_MUSIC_STOPPED;return true;}
int ls_music_count(void){return tracks;}
const char *ls_music_name(int i){static const char *names[]={"Night Dive.wav","Neon Reef.wav","Shark After Dark.wav","Coral Radio.wav"};return i==5?"REC-007.wav":i>=0&&i<tracks?names[i%4]:"";}
const char *ls_music_error(void){return "";}
bool ls_music_play(int i){if(i<0||i>=tracks)return false;last_play=i;state=LS_MUSIC_PLAYING;return true;}
bool ls_music_seek_ms(uint32_t ms){(void)ms;return true;}
bool ls_music_toggle(void){state=state==LS_MUSIC_PLAYING?LS_MUSIC_PAUSED:LS_MUSIC_PLAYING;return true;}
void ls_music_stop(void){state=LS_MUSIC_STOPPED;}
ls_music_state_t ls_music_state(void){return state;}
int ls_music_volume(void){return volume;}
uint32_t ls_music_position_ms(void){return 73000;}
uint32_t ls_music_duration_ms(void){return 594000;}
static unsigned fixed_peak;
unsigned ls_music_peak(int ch){static unsigned phase;if(fixed_peak)return state==LS_MUSIC_PLAYING?fixed_peak:0;return (phase++*997+ch*401)%32769;}
static bool warm;
bool ls_music_warm(void){return warm;}
void ls_music_set_warm(bool enabled){warm=enabled;}
void ls_music_volume_step(int d){volume+=d;}
const char *ls_music_output(void){return "TEST AUDIO";}
static bool keypad;
static int spectrum_calls;
bool ls_tui_keyboard_mode(void){return keypad;}
int ls_music_spectrum(float lo,float hi,int n,float *db){(void)lo;(void)hi;spectrum_calls++;for(int i=0;i<n;i++)db[i]=-12.0f-3*i;return state==LS_MUSIC_PLAYING?n:0;}
bool ls_music_format(ls_music_format_t *f){f->epoch=format_epoch;f->rate_hz=44100;f->bits=16;f->channels=2;f->mono_out=true;return true;}
float ls_music_volume_db(void){return (volume-100)*0.5f;}
static ls_music_mic_t mic;static int mic_starts;
bool ls_music_mic_start(bool record){mic_starts++;if(state==LS_MUSIC_PLAYING)state=LS_MUSIC_STOPPED;mic=record?LS_MIC_RECORDING:LS_MIC_LIVE;return true;}
void ls_music_mic_stop(void){mic=LS_MIC_OFF;}
ls_music_mic_t ls_music_mic_state(void){return mic;}
const char *ls_music_mic_file(void){return "/sdcard/music/REC-007.wav";}
uint32_t ls_music_mic_ms(void){return 12000;}
bool ls_music_mic_step(void){return true;}
bool ls_music_live(void){return state==LS_MUSIC_PLAYING||mic!=LS_MIC_OFF;}
static int gram_calls,cursor_restarts;static unsigned last_hop;
/* One line in two, a 1 kHz tone at -30 dB over a -110 dB floor. */
bool ls_music_spectrogram_line(ls_music_cursor_t *c,unsigned hop,float *p){gram_calls++;last_hop=hop;if(!c->started){cursor_restarts++;c->started=true;}if(state!=LS_MUSIC_PLAYING||(c->next++ & 1))return false;for(int i=0;i<LS_MUSIC_FFT_SIZE/2;i++)p[i]=i==46?1e-3f:1e-11f;return true;}
/* The panel's image cells: what the waterfall last handed over. */
static tui_rect image_cells;static const uint16_t *image_px;static int image_w,image_h;static uint32_t image_serial;
void ls_tui_geometry(int *cols,int *rows,int *cw,int *ch){if(cols)*cols=0;if(rows)*rows=0;if(cw)*cw=10;if(ch)*ch=17;}
static int image_x0,image_moved;
void ls_tui_image(tui_rect cells,const uint16_t *src,int w,int h,uint32_t serial){image_cells=cells;image_px=src;image_w=w;image_h=h;image_serial=serial;image_x0=0;}
void ls_tui_image_ring(tui_rect cells,const uint16_t *src,int w,int h,int x0,int y0,int dx,int dy,uint32_t serial){(void)y0;(void)dy;ls_tui_image(cells,src,w,h,serial);image_x0=x0;image_moved=dx;}
/* The picture as seen: column x of the ring starts at x0. */
static uint16_t seen(int row,int col){return image_px[(size_t)row*image_w+(col+image_x0)%image_w];}
bool ls_tui_image_now(tui_rect *cells,const uint16_t **src,int *w,int *h,uint32_t *serial){if(!image_px)return false;if(cells)*cells=image_cells;if(src)*src=image_px;if(w)*w=image_w;if(h)*h=image_h;if(serial)*serial=image_serial;return true;}

static bool locate(tui_cell *cells,int w,int h,const char*text,int*x,int*y) {
    int n=(int)strlen(text);
    for(int yy=0;yy<h;yy++)for(int xx=0;xx+n<=w;xx++) {
        int j=0;while(j<n && cells[yy*w+xx+j].ch==text[j])j++;
        if(j==n && (xx+n==w || cells[yy*w+xx+n].ch==' ' || cells[yy*w+xx+n].ch=='|')){*x=xx+n/2;*y=yy;return true;}
    }
    return false;
}
/* OPTIONS is the shared list's to draw; here it is enough that the player asks for its own. */
static const ls_opt_ctx_t *opened;
void ls_opt_open(const ls_opt_ctx_t *ctx){opened=ctx;}
static void paint(tui_surface*sf,int w,int h){tui_frame_begin(sf);ls_scr_music.draw(sf,tui_rect_make(0,0,w,h));}
static void tap_label(tui_surface*sf,tui_cell*c,int w,int h,const char*text){
    int x=-1,y=-1;paint(sf,w,h);if(!strcmp(text,"TRACKS") && !locate(c,w,h,text,&x,&y))text="LIST";LS_CHECK_MSG(locate(c,w,h,text,&x,&y),"missing button %s at %dx%d",text,w,h);
    if(x>=0)LS_CHECK(ls_scr_music.touch(x,y));
}
static void exercise(int w,int h) {
    wide=w>h;volume=35;warm=false;last_play=-1;
    size_t n=(size_t)w*h;tui_cell *back=calloc(n+2,sizeof(*back)),*front=calloc(n+2,sizeof(*front));LS_CHECK(back&&front);
    back[0].ch='A';back[n+1].ch='Z';tui_surface sf;tui_surface_setup(&sf,back+1,front+1,w,h);
    ls_scr_music.enter();paint(&sf,w,h);
    /* A button where the row can spell it, and the key everywhere. */
    if(!wide){opened=NULL;tap_label(&sf,back+1,w,h,"OPTIONS");
        LS_CHECK(opened && !strcmp(opened->name,"MUSIC") && opened->n==8);}
    opened=NULL;LS_CHECK(ls_scr_music.key(LS_TK_CHAR,'o'));LS_CHECK(opened!=NULL);
    tap_label(&sf,back+1,w,h,"PLAY");LS_EQ_INT(last_play,0);LS_EQ_INT(state,LS_MUSIC_PLAYING);
    tap_label(&sf,back+1,w,h,"PAUSE");LS_EQ_INT(state,LS_MUSIC_PAUSED);
    tap_label(&sf,back+1,w,h,"PLAY");LS_EQ_INT(state,LS_MUSIC_PLAYING);
    tap_label(&sf,back+1,w,h,"VOL +");LS_EQ_INT(volume,40);
    tap_label(&sf,back+1,w,h,"VOL -");LS_EQ_INT(volume,35);
    tap_label(&sf,back+1,w,h,"MUTE");LS_EQ_INT(volume,35);LS_CHECK(ls_music_muted());
    tap_label(&sf,back+1,w,h,"UNMUTE");LS_EQ_INT(volume,35);LS_CHECK(!ls_music_muted());
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
    /* SPAN is the audio in one pixel column: 512, 1024, 2048 samples. */
    LS_CHECK(contains(back,w,h,"11.6 ms"));LS_EQ_INT(last_hop,512);
    LS_CHECK(ls_scr_music.key(LS_TK_CHAR,'z'));paint(&sf,w,h);LS_CHECK(contains(back,w,h,"23.2 ms"));
    LS_EQ_INT(last_hop,1024);
    LS_CHECK(contains(back,w,h,"NOW"));LS_CHECK(contains(back,w,h,"10k"));
    LS_CHECK(contains(back,w,h,"Hz/BIN"));LS_CHECK(contains(back,w,h,"HOLD"));
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

static bool music_muted;
bool ls_music_muted(void){return music_muted;}
void ls_music_mute_toggle(void){music_muted=!music_muted;}
bool ls_music_mic_repair(void){return true;}

LS_CASE(stop_options_and_key_stop_recording_and_playback){
    ls_scr_music.enter();ls_scr_music.key(LS_TK_CHAR,'d');ls_scr_music.key(LS_TK_CHAR,'w');LS_EQ_INT(mic,LS_MIC_RECORDING);
    ls_scr_music.key(LS_TK_CHAR,'s');LS_EQ_INT(mic,LS_MIC_OFF);LS_EQ_INT(state,LS_MUSIC_STOPPED);
    ls_scr_music.key(LS_TK_CHAR,'i');ls_scr_music.key(LS_TK_CHAR,'o');
    const ls_opt_t *stop=NULL;
    for(int i=0;i<opened->n;i++)if(!strcmp(opened->opt[i].label,"STOP"))stop=&opened->opt[i];
    LS_CHECK(stop && stop->act);stop->act(stop);LS_EQ_INT(mic,LS_MIC_OFF);
    ls_scr_music.key(LS_TK_CHAR,'p');LS_EQ_INT(state,LS_MUSIC_PLAYING);LS_EQ_INT(last_play,5);
    stop->act(stop);LS_EQ_INT(state,LS_MUSIC_STOPPED);ls_scr_music.leave();
}
LS_CASE(options_choices_are_directional_and_span_bounded){
    ls_scr_music.enter();ls_scr_music.key(LS_TK_CHAR,'o');
    for(int i=0;i<2;i++){
        const ls_opt_t *o=&opened->opt[i];LS_EQ_INT(o->kind,LS_OPT_CYCLE);LS_CHECK(o->step>0);
        o->set(o,0);o->set(o,o->n-1);LS_EQ_INT(o->get(o),o->n-1);
        o->set(o,1);LS_EQ_INT(o->get(o),1);o->set(o,0);
    }
    const ls_opt_t *span=&opened->opt[2];LS_EQ_INT(span->kind,LS_OPT_LEVEL);
    LS_EQ_INT(span->lo,0);LS_EQ_INT(span->hi,2);LS_EQ_INT(span->step,1);
    span->set_num(span,2);LS_EQ_INT(span->num(span),2);span->set_num(span,0);LS_EQ_INT(span->num(span),0);
    ls_scr_music.leave();
}

LS_CASE(waterfall_history_resets_on_source_epoch){
    tui_cell *back=calloc(52*70,sizeof(*back)),*front=calloc(52*70,sizeof(*front));
    tui_surface sf;tui_surface_setup(&sf,back,front,52,70);wide=false;
    ls_scr_music.enter();ls_scr_music.key(LS_TK_CHAR,'4');ls_scr_music.key(LS_TK_CHAR,'p');
    paint(&sf,52,70);int before=cursor_restarts;
    paint(&sf,52,70);LS_EQ_INT(cursor_restarts,before);
    format_epoch++;paint(&sf,52,70);LS_EQ_INT(cursor_restarts,before+1);
    ls_scr_music.leave();free(back);free(front);
}

/* LEVEL is what is heard: the same source peak reads lower as the volume
   comes down, nothing while muted, and the source mark stays where it was. */
LS_CASE(output_db_follows_volume_and_mute){
    LS_CHECK(fabsf(ls_audio_view_output_db(16384,100,0.0f,false)-(-6.02f))<0.01f);
    LS_CHECK(fabsf(ls_audio_view_output_db(16384,50,-25.0f,false)-(-31.02f))<0.01f);
    LS_CHECK(fabsf(ls_audio_view_output_db(32768,20,-40.0f,false)-(-40.0f))<0.01f);
    LS_CHECK(isinf(ls_audio_view_output_db(16384,50,-25.0f,true)));
    LS_CHECK(isinf(ls_audio_view_output_db(16384,0,-50.0f,false)));
    LS_CHECK(isinf(ls_audio_view_output_db(0,100,0.0f,false)));
}
static bool over_lit(tui_cell *cells,int w,int h){
    for(int y=0;y<h;y++)for(int x=0;x+4<=w;x++)
        if(cells[y*w+x].ch=='O'&&cells[y*w+x+1].ch=='V'&&cells[y*w+x+2].ch=='E'&&cells[y*w+x+3].ch=='R'&&
           TUI_ATTR_BG(cells[y*w+x].attr)==(TUI_RED|TUI_BRIGHT))return true;
    return false;
}
static void level_follows_volume(int w,int h){
    tui_cell *back=calloc((size_t)w*h,sizeof(*back)),*front=calloc((size_t)w*h,sizeof(*front));
    LS_CHECK(back&&front);if(!back||!front){free(back);free(front);return;}
    tui_surface sf;tui_surface_setup(&sf,back,front,w,h);wide=w>h;rounded_height=0;keypad=false;music_muted=false;
    ls_scr_music.enter();ls_scr_music.key(LS_TK_CHAR,'1');ls_scr_music.key(LS_TK_CHAR,'p');
    fixed_peak=16384;volume=50;paint(&sf,w,h);
    LS_CHECK_MSG(contains(back,w,h,"-31.0"),"output at volume 50 at %dx%d",w,h);
    LS_CHECK(contains(back,w,h,"SOURCE  L  -6.0  R  -6.0"));
    LS_CHECK(contains(back,w,h,"OUTPUT dBFS"));
    /* Turned up, the bars follow at once. */
    volume=100;paint(&sf,w,h);
    LS_CHECK(contains(back,w,h,"-6.0"));LS_CHECK(!contains(back,w,h,"-31.0"));
    /* Muted, they fall to nothing; the source mark does not. */
    music_muted=true;
    for(int i=0;i<16;i++){ls_shim_time_advance(250000);paint(&sf,w,h);}
    LS_CHECK(contains(back,w,h,"L   -- "));
    LS_CHECK(contains(back,w,h,"SOURCE  L  -6.0"));
    /* A clipped file is OVER only when it is heard at full scale. */
    music_muted=false;fixed_peak=32768;volume=50;paint(&sf,w,h);
    for(int i=0;i<12;i++){ls_shim_time_advance(250000);paint(&sf,w,h);}
    LS_CHECK(!over_lit(back,w,h));
    volume=100;paint(&sf,w,h);LS_CHECK(over_lit(back,w,h));
    fixed_peak=0;volume=35;ls_scr_music.key(LS_TK_CHAR,'s');ls_scr_music.leave();free(back);free(front);
}
LS_CASE(level_follows_volume_portrait){level_follows_volume(52,70);}
LS_CASE(level_follows_volume_landscape){level_follows_volume(118,28);}

/* WATERFALL is drawn in panel pixels behind image cells, one image pixel to
   one panel pixel, and HOLD freezes it while the audio plays on. */
static void waterfall_pixels(int w,int h){
    tui_cell *back=calloc((size_t)w*h,sizeof(*back)),*front=calloc((size_t)w*h,sizeof(*front));
    LS_CHECK(back&&front);if(!back||!front){free(back);free(front);return;}
    tui_surface sf;tui_surface_setup(&sf,back,front,w,h);wide=w>h;rounded_height=0;keypad=false;
    ls_scr_music.enter();ls_scr_music.key(LS_TK_CHAR,'4');ls_scr_music.key(LS_TK_CHAR,'p');
    /* A frame every 40 ms, the board's idle cadence. */
    for(int i=0;i<6;i++){ls_shim_time_advance(40000);paint(&sf,w,h);}
    LS_CHECK(image_px!=NULL);if(!image_px){ls_scr_music.leave();free(back);free(front);return;}
    LS_EQ_INT(image_w,image_cells.w*10);LS_EQ_INT(image_h,image_cells.h*17);
    LS_CHECK(image_cells.w>=30 && image_cells.h>=10);
    int cells=0;
    for(int y=image_cells.y;y<image_cells.y+image_cells.h;y++)for(int x=image_cells.x;x<image_cells.x+image_cells.w;x++)
        cells+=(uint8_t)back[y*w+x].ch==(uint8_t)LS_TUI_IMAGE_CELL;
    LS_EQ_INT(cells,image_cells.w*image_cells.h);
    /* The 990 Hz line shows at the right edge, at its row on a linear axis;
       far from it, and to the left where nothing has arrived yet, is black. */
    int tone=ls_spectro_hz_row(46*44100.0f/LS_MUSIC_FFT_SIZE,image_h,false,22050.0f);
    int quiet=ls_spectro_hz_row(15000.0f,image_h,false,22050.0f);
    LS_CHECK(tone>0 && quiet>=0);
    LS_CHECK(seen(tone,image_w-1)!=0);
    LS_EQ_INT(seen(quiet,image_w-1),0);
    LS_EQ_INT(seen(tone,image_w/2),0);
    /* HOLD: no more lines read, the picture unchanged, and it says so. */
    LS_CHECK(ls_scr_music.key(LS_TK_CHAR,'h'));paint(&sf,w,h);
    uint32_t serial=image_serial;int calls=gram_calls;
    for(int i=0;i<4;i++)paint(&sf,w,h);
    LS_EQ_INT(gram_calls,calls);LS_EQ_INT(image_serial,serial);
    LS_CHECK(contains(back,w,h," HOLD "));LS_CHECK(contains(back,w,h,"HELD"));
    /* Held, the reading can still change: a log axis redraws the history. */
    LS_CHECK(ls_scr_music.key(LS_TK_CHAR,'a'));paint(&sf,w,h);
    LS_CHECK(image_serial!=serial);LS_EQ_INT(gram_calls,calls);
    int tone_log=ls_spectro_hz_row(46*44100.0f/LS_MUSIC_FFT_SIZE,image_h,true,22050.0f);
    LS_CHECK(seen(tone_log,image_w-1)!=0);
    ls_scr_music.key(LS_TK_CHAR,'a');
    LS_CHECK(ls_scr_music.key(LS_TK_CHAR,'h'));for(int i=0;i<3;i++)paint(&sf,w,h);LS_CHECK(gram_calls>calls);
    /* Another view, or leaving, hands the cells back. */
    ls_scr_music.key(LS_TK_CHAR,'1');paint(&sf,w,h);LS_CHECK(image_px==NULL);
    ls_scr_music.key(LS_TK_CHAR,'4');paint(&sf,w,h);LS_CHECK(image_px!=NULL);
    ls_scr_music.key(LS_TK_CHAR,'s');ls_scr_music.leave();LS_CHECK(image_px==NULL);
    ls_scr_music.key(LS_TK_CHAR,'1');free(back);free(front);
}
LS_CASE(waterfall_pixels_portrait){waterfall_pixels(52,70);}
LS_CASE(waterfall_pixels_landscape){waterfall_pixels(118,28);}

LS_CASE(waterfall_options_are_one_level_down){
    ls_scr_music.enter();opened=NULL;ls_scr_music.key(LS_TK_CHAR,'o');LS_CHECK(opened!=NULL);
    const ls_opt_t *menu=NULL;
    for(int i=0;i<opened->n;i++)if(opened->opt[i].kind==LS_OPT_MENU)menu=&opened->opt[i];
    LS_CHECK(menu && !strcmp(menu->label,"WATERFALL") && menu->sub);
    if(menu && menu->sub){
        const ls_opt_ctx_t *wf=menu->sub;
        for(int i=0;i<wf->n;i++){
            const ls_opt_t *o=&wf->opt[i];
            if(o->kind==LS_OPT_LEVEL){
                /* Floor and range step by 5 dB and hold to their range. */
                double was=o->num(o);
                LS_CHECK(o->step==5);o->set_num(o,o->hi+50);LS_CHECK(o->num(o)==o->hi);
                o->set_num(o,o->lo-50);LS_CHECK(o->num(o)==o->lo);
                o->set_num(o,was);LS_CHECK(o->num(o)==was);
            }
        }
    }
    ls_scr_music.leave();
}
