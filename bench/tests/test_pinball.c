/* Production game, deterministic clock/input/card, real lap3d renderer. */
#include <assert.h>
#include "../../app-sdk/apps/pinball/pinball.c"

static uint32_t clock_now=1000;
static int cols=52,rows=70;
static lap_touch_t finger;
static char card[128],legacy[64];
static bool writing,fail_write;
static bool immersive,exit_requested;
int lap_cols(void) { return cols; }
int lap_rows(void) { return rows; }
uint32_t lap_millis(void) { return clock_now; }
uint32_t lap_random(void) { static uint32_t r=42; r=r*1664525u+1013904223u; return r; }
void lap_clear(uint8_t a) { (void)a; }
void lap_put_str(int x,int y,const char *s,uint8_t a) { (void)x;(void)y;(void)s;(void)a; }
void lap_fullscreen(bool b) { (void)b; }
void lap_immersive(bool b) { immersive=b; }
void lap_exit(void) { exit_requested=true; }
void lap_image(int x,int y,int w,int h,const uint16_t *p,int pw,int ph,uint32_t serial) {
    (void)x;(void)y;(void)serial;assert(w==cols && h==rows && p && pw==cols*2 && ph==rows*3);
}
void lap_hold_rotation(bool b) { (void)b; }
void lap_keep_awake(bool b) { (void)b; }
bool lap_touch(lap_touch_t *t) { *t=finger; return finger.down; }
bool lap_motion(lap_motion_t *m) { (void)m;return false; }
bool lap_tone(int h,int ms,int v,int w) { (void)h;(void)ms;(void)v;(void)w;return true; }
void lap_tone_stop(void) {}
bool lap_haptic(int n) { (void)n;return true; }
bool lap_vibrate(int m,int s) { (void)m;(void)s;return true; }
int lap_file_open(const char *n,const char *mode) {
    writing=mode[0]=='w';
    if(!strcmp(n,"machines.txt")) return writing || card[0] ? 1:-1;
    return legacy[0]?2:-1;
}
int lap_file_read(int fd,void *b,size_t n) {
    const char *s=fd==1?card:legacy;size_t k=strlen(s);if(k>n)k=n;memcpy(b,s,k);return (int)k;
}
int lap_file_write(int fd,const void *b,size_t n) {
    assert(fd==1 && writing && n<sizeof(card));
    if(fail_write)return -1;
    memcpy(card,b,n);card[n]=0;return (int)n;
}
void lap_file_close(int fd) { (void)fd; }

static void tick(void) { clock_now+=40;draw(); }
static void expect_frame(void) {
    tick();
    if(camera_mode==2) {
        assert(bg_2d && !bg_2d_dirty);
        assert(X(LANE_U)>0 && X(LANE_U)<cv.w && Y(LANE_V)>hud_h && Y(LANE_V)<cv.h);
        return;
    }
    l3_stats_t stats;l3_stats(scene,&stats);
    assert(stats.dropped==0 && stats.shapes>50);
    float x,y;
    assert(l3_project(scene,table_at(LANE_U,LANE_V,3),&x,&y));
    assert(x>0 && x<cv.w && y>hud_h && y<cv.h);
    if(cols==52 && rows==70 && camera_mode==0) {
        float top,bottom;
        assert(l3_project(scene,table_at(50,-3.5f,94),NULL,&top));
        assert(l3_project(scene,table_at(50,186,0),NULL,&bottom));
        assert(bottom-top>cv.h*.80f);
    }
}
static void records_and_migration(void) {
    strcpy(legacy,"100 200 300 1\n"); card[0]=0;
    machine=REEF;level=NORMAL;load_best();
    assert(records[REEF][NORMAL]==200 && records[ION][NORMAL]==0);
    records[ION][ARCADE]=400;machine=ION;level=ARCADE;camera_mode=2;save_best();
    memset(records,0,sizeof(records));machine=REEF;load_best();
    assert(machine==ION && level==ARCADE && camera_mode==2 && records[ION][ARCADE]==400 && records[REEF][NORMAL]==200);
    fail_write=true;save_best();assert(dirty_best);fail_write=false;save_best();assert(!dirty_best);
    card[0]=0;strcpy(legacy,"9876");load_best();assert(records[REEF][ARCADE]==9876);
    strcpy(card,"broken");load_best();assert(records[REEF][ARCADE]==9876);
    puts("PASS records: both machines, migration, write retry, malformed save");
}
static void controls_and_scoring(void) {
    card[0]=legacy[0]=0;machine=REEF;level=NORMAL;camera_mode=0;enter();expect_frame();assert(immersive);
    key(LAP_KEY_CHAR,'2');assert(machine==ION && bumper_count==4 && state==ST_TITLE);
    expect_frame();
    key(LAP_KEY_RIGHT,0);assert(level==ARCADE);
    key(LAP_KEY_ENTER,0);assert(state==ST_PLAY && in_lane);
    finger=(lap_touch_t){.down=true,.x=cols*10-20,.y=400,.held_ms=900};
    tick();assert(charging && power==1);
    finger.down=false;tick();assert(!in_lane && launched && vel.y<0);
    int before=score;
    const seg_t spinner={39,105,53,105,K_SPINNER,0};
    for(int i=0;i<5;i++) {
        clock_now+=250;ball=(v2){46,104};vel=(v2){0,100};hit_seg(&spinner,clock_now);
    }
    assert(score-before==5*150+2500 && spinner_hits==5 && spinner_speed>0);
    tilted=true;clock_now+=250;hit_seg(&spinner,clock_now);assert(spinner_hits==5);
    start();assert(!tilted && spinner_hits==0 && !msg_until && !key_l && !key_r);
    clock_now+=500;
    finger=(lap_touch_t){.down=true,.x=10,.y=20};tick();
    finger.down=false;tick();
    assert(camera_mode==1 && in_lane && !charging);
    key(LAP_KEY_CHAR,'v');assert(camera_mode==2);expect_frame();
    int same_score=score,same_ball=ball_no;
    key(LAP_KEY_CHAR,'v');assert(camera_mode==0);expect_frame();
    assert(score==same_score && ball_no==same_ball && in_lane);
    state=ST_TITLE;expect_frame();clock_now+=500;
    menu_tap(machine_btn[REEF].x+2,machine_btn[REEF].y+2);
    assert(machine==REEF && bumper_count==3 && state==ST_TITLE);
    menu_tap(cv.w-5,4);assert(exit_requested);
    leave();
    puts("PASS inputs: touch plunger, key/touch selection, spinner awards, reset, camera");
}
static void simulation_and_views(void) {
    const int dimensions[][2]={{52,70},{80,42},{40,64}};
    for(int d=0;d<3;d++) for(int m=0;m<MACHINES;m++) {
        cols=dimensions[d][0];rows=dimensions[d][1];machine=m;build_table();state=ST_TITLE;
        for(camera_mode=0;camera_mode<3;camera_mode++) { view_dirty=true;expect_frame(); }
        camera_mode=0;view_dirty=true;level=ARCADE;start();launch(.9f);
        for(int f=0;f<450;f++) {
            clock_now+=40;
            flip[0].held=f%19<8;flip[1].held=f%23<10;
            if(state==ST_PLAY) physics(.04f,clock_now);
            if(in_lane) launch(.85f);
            if(state==ST_DRAIN) {reset_ball();state=ST_PLAY;}
            assert(isfinite(ball.x) && isfinite(ball.y) && isfinite(vel.x) && isfinite(vel.y));
            assert(ball.x>=0 && ball.x<=TW && ball.y>=0 && ball.y<=TH+5);
            if(f%150==0) expect_frame();
        }
    }
    leave();puts("PASS physics/render: 2700 frames, two machines, three sizes, all 3 views, no dropped geometry");
}
/* The engine redraws only what changed (the ball, a flipper, a lamp that
   blinked) and keeps the rest of last frame. Every frame of real play must
   still be exactly the picture a fresh scene draws from scratch. */
static void partial_frames_match_fresh(void) {
    cols=52;rows=70;
    for(int m=0;m<MACHINES;m++) for(int cam=0;cam<2;cam++) {
        machine=m;camera_mode=cam;enter();build_table();view_dirty=true;level=ARCADE;
        tick();start();launch(.9f);
        unsigned partial=0;
        for(int f=0;f<240;f++) {
            flip[0].held=f%19<8;flip[1].held=f%23<10;
            if(in_lane && f%7==0) launch(.85f);
            if(state==ST_DRAIN||state==ST_OVER) {reset_ball();state=ST_PLAY;}
            tick();
            l3_stats_t st;l3_stats(scene,&st);
            partial+=st.dots_shaded<(uint32_t)(cols*2*rows*3)/2;
            if(f%3) continue;
            size_t n=(size_t)cols*2*rows*3;
            uint8_t *kept=malloc(n);assert(kept);memcpy(kept,l3_dots(scene),n);
            l3_scene *live=scene;
            scene=l3_create(cols,rows);assert(scene);setup_3d();view_dirty=true;
            draw_table(clock_now);
            size_t diff=0;for(size_t i=0;i<n;i++) diff+=kept[i]!=l3_dots(scene)[i];
            if(diff) printf("machine %d view %d frame %d: %zu dots differ\n",m,cam,f,diff);
            assert(diff==0);
            l3_destroy(scene);scene=live;free(kept);
        }
        printf("  machine %d view %d: %u of 240 frames drew under half the dots\n",m,cam,partial);
        assert(partial>200);
        leave();
    }
    puts("PASS partial redraw: real play frames match a fresh full render dot for dot");
}
int main(void) {
    records_and_migration();controls_and_scoring();simulation_and_views();partial_frames_match_fresh();return 0;
}
