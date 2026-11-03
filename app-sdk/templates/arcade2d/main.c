/* @NAME@: @SUB@. SDK 2.5 arcade starter. Arrows/WASD or held touch move;
 * collect the yellow stars. P saves, N selects the other score slot.
 * assets.inc embeds a PNG fallback; copy assets/sheet.png to this app's
 * apps_data/<id>/sheet.png to override it without rebuilding firmware. */
#include "lakeshark_app.h"
#include "lap_game.h"
#include "lap_loop.h"
#include "lap_music.h"
#include <stdio.h>
#include <string.h>
#include "assets.inc"

static struct {
    lap_canvas_t cv;
    lap_caps_t caps;
    lap_loop_t loop;
    lap_sprite_t sheet;
    lap_tilemap_t map;
    lap_animation_t anim;
    lap_particle_t sparks[32];
    lap_particles_t particles;
    lap_shake_t shake;
    lap_asset_arena_t arena;
    lap_asset_file_t file;
    uint8_t memory[16384],source[4096];
    uint16_t inks[16],tiles[64*16];
    uint8_t flags[64*16],remap[16];
    lap_music_song_t song;
    lap_music_player_t music;
    lap_input_state_t input;
    lap_input_event_t event;
    int score,slot,save_job,save_error,asset_error;
    int16_t effect[256];
    float x,y,intro;
    bool configured,audio,focused;
} s;
static const char *slot_name(void) { return s.slot?"score1":"score0"; }
static void load_score(void) {
    int score=0; s.score=0;
    if((s.caps.hardware&LAP_HW_SD)&&lap_load(slot_name(),&score,sizeof(score))==(int)sizeof(score)&&score>=0&&score<=999999) s.score=score;
}
static void save_score(void) {
    if((s.caps.hardware&LAP_HW_SD)&&s.save_job<0) { s.save_job=lap_save(slot_name(),&s.score,sizeof(s.score)); if(s.save_job<0) s.save_error=s.save_job; }
}
static void music_resume(void) {
    if(s.audio&&s.configured&&s.focused) {
        const char *text=s.slot?SECOND_TUNE:TUNE;
        if(!lap_music_parse(&s.song,text,strlen(text),NULL)) lap_music_start(&s.music,&s.song);
    }
}
static void enter(void) {
    memset(&s,0,sizeof(s)); s.save_job=-1; s.x=16; s.y=32; s.intro=1; s.focused=true;
    lap_caps(&s.caps,sizeof(s.caps)); s.audio=(s.caps.hardware&LAP_HW_SPEAKER)!=0;
    load_score();
    lap_loop_init(&s.loop,60,6,lap_millis());
    s.particles=(lap_particles_t){s.sparks,32}; s.anim=(lap_animation_t){0,2,0,160,0,true,false};
    for(int i=0;i<16;i++) { s.inks[i]=lap_palette(i); s.remap[i]=(uint8_t)i; }
    s.remap[LC_BR_CYAN]=LC_BR_GREEN;
    lap_asset_arena_init(&s.arena,s.memory,sizeof(s.memory));
    s.asset_error=lap_image_png(&s.sheet.image,SHEET_PNG,sizeof(SHEET_PNG),&s.arena,s.inks);
    s.sheet.frame_w=s.sheet.frame_h=8;
    s.map=(lap_tilemap_t){&s.sheet,s.tiles,s.flags,64,16};
    for(int y=0;y<16;y++) for(int x=0;x<64;x++) {
        int i=y*64+x; bool wall=y==0||y==15||x==0||x==63||(x%13==0&&y>5&&y<12);
        s.tiles[i]=wall?2:65535; s.flags[i]=(uint8_t)wall;
    }
    if(s.caps.hardware&LAP_HW_SD) lap_asset_begin(&s.file,"@ID@","sheet.png",s.source,sizeof(s.source));
    for(int i=0;i<256;i++) s.effect[i]=(int16_t)((i%18<9?1:-1)*(256-i)*30);
}
static void update(void) {
    float dx=(lap_key_down(LAP_KEY_RIGHT)||lap_key_down(LAP_KEY_CHAR('d')))-(lap_key_down(LAP_KEY_LEFT)||lap_key_down(LAP_KEY_CHAR('a')));
    float dy=(lap_key_down(LAP_KEY_DOWN)||lap_key_down(LAP_KEY_CHAR('s')))-(lap_key_down(LAP_KEY_UP)||lap_key_down(LAP_KEY_CHAR('w')));
    if(s.input.buttons&LAP_BUTTON_LEFT) {
        int cw=1,ch=1; lap_cell_px(&cw,&ch);
        float target_x=s.input.x*2.0f/cw+s.x-s.cv.w/2,target_y=s.input.y*3.0f/ch;
        dx=lc_clampf((target_x-s.x)/8,-1,1); dy=lc_clampf((target_y-s.y)/8,-1,1);
    }
    lap_aabb_t box={s.x+dx, s.y, 6,6};
    if(!lap_collision_tiles(&s.map,box,1)) s.x=lc_clampf(box.x,8,498);
    box=(lap_aabb_t){s.x,s.y+dy,6,6};
    if(!lap_collision_tiles(&s.map,box,1)) s.y=lc_clampf(box.y,8,112);
    float coin_x=40+(s.score%15)*24,coin_y=24+(s.score%4)*16;
    if(lap_collision_aabb((lap_aabb_t){s.x,s.y,8,8},(lap_aabb_t){coin_x-3,coin_y-3,6,6})&&
       lap_collision_circle(s.x+4,s.y+4,5,coin_x,coin_y,4)) {
        if(s.score<999999) s.score++;
        save_score(); s.shake=(lap_shake_t){160,160,1,3,0,0};
        for(int i=0;i<12;i++) lap_particle_emit(&s.particles,(lap_particle_t){coin_x,coin_y,(i-6)*8.0f,-20.0f-(i%3)*8,0.6f,LC_BR_YELLOW});
        if(s.audio) lap_mixer_write(LAP_MIX_EFFECTS,s.effect,256);
    }
    lap_particles_step(&s.particles,1.0f/60,60); lap_shake_step(&s.shake,17);
    lap_animation_step(&s.anim,17); s.intro=lc_clampf(s.intro-0.025f,0,1);
}
static void draw(void) {
    if(!lap_canvas_init(&s.cv,lap_cols(),lap_rows())) return;
    /* The audio worker may still be resetting its app generation in enter(). */
    if(s.audio&&!s.configured) {
        int music=lap_mixer_config(LAP_MIX_MUSIC,55,35);
        int effects=lap_mixer_config(LAP_MIX_EFFECTS,60,100);
        if(!music&&!effects) { s.configured=true; music_resume(); }
    }
    lap_clear(LAP_ATTR(LAP_WHITE,LAP_BLACK));
    if(s.file.state==1||s.file.state==2) {
        int r=lap_asset_poll(&s.file);
        if(r==1) {
            /* Decode transactionally into spare arena; fallback remains live on failure. */
            lap_image_t image;
            int e=lap_image_png(&image,s.source,s.file.size,&s.arena,s.inks);
            if(!e&&image.w>=24&&image.h>=8) s.sheet.image=image; else s.asset_error=e?e:LAP_ASSET_EFORMAT;
        } else if(r<0&&r!=LAP_SD_ENOENT) s.asset_error=r;
    }
    if(s.save_job>=0) { int r=lap_sd_poll(s.save_job); if(r!=LAP_SD_PENDING) { s.save_job=-1; s.save_error=r<0?r:0; } }
    while(lap_input_next(&s.event)>0) {
        lap_loop_event(&s.loop,&s.event,lap_millis());
        if(s.event.kind==LAP_INPUT_FOCUS_LOST||s.event.kind==LAP_INPUT_PAUSE) { s.focused=false; if(s.audio) lap_music_stop(&s.music); }
        if(s.event.kind==LAP_INPUT_FOCUS_GAINED||s.event.kind==LAP_INPUT_RESUME) { s.focused=true; music_resume(); }
    }
    lap_input_state(&s.input);
    unsigned steps=lap_loop_advance(&s.loop,lap_millis(),NULL); while(steps--) update();
    if(s.audio&&s.configured&&s.focused) lap_music_pump(&s.music);
    int cx=(int)s.x-s.cv.w/2+s.shake.x,cy=s.shake.y;
    lc_clear(&s.cv,LC_BLACK); lap_tilemap_draw(&s.cv,&s.map,cx,cy);
    lc_fill_circle(&s.cv,40+(s.score%15)*24-cx,24+(s.score%4)*16-cy,3,LC_BR_YELLOW);
    lap_sprite_draw(&s.cv,&s.sheet,s.anim.first+s.anim.frame,(int)s.x-cx,(int)s.y-cy,1,
                    lap_key_down(LAP_KEY_LEFT)?LAP_SPRITE_FLIP_X:0,s.slot?s.remap:NULL);
    lap_particles_draw(&s.cv,&s.particles,cx,cy); lap_fade(&s.cv,LC_BLACK,s.intro);
    lap_canvas_show(&s.cv,0,0);
    char label[96]; snprintf(label,sizeof(label),"SCORE %d SLOT %d %s",s.score,s.slot+1,s.save_job>=0?"saving":s.save_error?"save error":"P save N slot");
    lap_put_glass(0,0,label,LAP_ATTR(LAP_WHITE|LAP_BRIGHT,LAP_BLACK));
    if(!s.audio) lap_put_glass(0,1,"no speaker: silent play",LAP_ATTR(LAP_WHITE,LAP_BLACK));
    if(!(s.caps.hardware&LAP_HW_SD)) lap_put_glass(0,2,"no SD: embedded art, session score",LAP_ATTR(LAP_WHITE,LAP_BLACK));
    if(s.asset_error) lap_put_glass(0,3,"asset rejected: embedded art",LAP_ATTR(LAP_YELLOW,LAP_BLACK));
}
static bool key(lap_key_t key,char ch) {
    if(key!=LAP_KEY_CHAR) return false;
    if(ch=='p') { save_score(); return true; }
    if(ch=='n'&&s.save_job<0) {
        s.slot^=1; s.score=0;
        load_score();
        if(s.audio) { lap_music_stop(&s.music); music_resume(); }
        return true;
    }
    return false;
}
static void leave(void) {
    if(s.audio) { lap_music_stop(&s.music); lap_mixer_clear(LAP_MIX_EFFECTS); }
    lap_canvas_free(&s.cv);
}
LAP_APP(.id="@ID@",.name="@NAME@",.sub="@SUB@",.version="0.1",.author="@AUTHOR@",
        .color=LAP_CYAN,.enter=enter,.draw=draw,.key=key,.leave=leave);
