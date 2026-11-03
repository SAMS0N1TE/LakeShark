/* Shared-library tests with deterministic SD/mixer backpressure, no GUI. */
#include "lap_game.h"
#include "lap_music.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../templates/arcade2d/assets.inc"
#include "../templates/flight3d/assets.inc"
static uint8_t arena_memory[65536];
static uint16_t inks[16]={0,0xa800,0x0540,0xad40,0x0015,0xa815,0x0555,0xad55,0x52aa,0xfaaa,0x57ea,0xffea,0x52bf,0xfabf,0x57ff,0xffff};
static int job_result,job_pending,read_count,mix_limit,mix_calls;
static unsigned read_offset,read_len; static void *read_buffer;
int lap_sd_size(const char *path) { assert(strstr(path,"/sdcard/apps_data/test/")); job_pending=1; return 10; }
int lap_sd_read(const char *path,uint32_t offset,void *p,uint32_t n) {
    assert(strstr(path,"/sdcard/apps_data/test/")); assert(n<=65536);
    read_count++; read_offset=offset; read_len=n; read_buffer=p; job_pending=1; return 11;
}
int lap_sd_poll(int job) {
    if(job_pending) { job_pending=0; return LAP_SD_PENDING; }
    if(job==10) return job_result;
    memset(read_buffer,7,read_len); return (int)read_len;
}
int lap_mixer_write(int channel,const int16_t *pcm,int n) { assert(channel==LAP_MIX_MUSIC); assert(pcm); mix_calls++; return n<mix_limit?n:mix_limit; }
void lap_mixer_clear(int channel) { assert(channel==LAP_MIX_MUSIC); }
static void assets(void) {
    lap_asset_arena_t a; lap_image_t image={0}; l3_mesh mesh={0};
    lap_asset_arena_init(&a,arena_memory,sizeof(arena_memory));
    assert(!lap_image_png(&image,SHEET_PNG,sizeof(SHEET_PNG),&a,inks));
    assert(image.w==24&&image.h==8&&image.pixels[0]==255&&image.pixels[3]==14);
    size_t mark=a.used; const uint8_t *pixels=image.pixels;
    assert(!lap_mesh_load(&mesh,SHIP_L3M,sizeof(SHIP_L3M),&a)); assert(mesh.nv==5&&mesh.ntri==6);
    assert(a.used>mark&&pixels[3]==14);
    size_t keep=a.used; uint8_t broken[sizeof(SHIP_L3M)]; memcpy(broken,SHIP_L3M,sizeof(broken));
    broken[16+5*12]=255; broken[17+5*12]=255;
    assert(lap_mesh_load(&mesh,broken,sizeof(broken),&a)==LAP_ASSET_EFORMAT); assert(a.used==keep&&mesh.nv==5);
    memcpy(broken,SHIP_L3M,sizeof(broken)); broken[18]=128; broken[19]=127; /* +inf */
    assert(lap_mesh_load(&mesh,broken,sizeof(broken),&a)==LAP_ASSET_EFORMAT&&a.used==keep);
    lap_asset_arena_init(&a,arena_memory,32);
    assert(lap_image_png(&image,SHEET_PNG,sizeof(SHEET_PNG),&a,inks)==LAP_ASSET_EBUDGET&&a.used==0&&image.pixels==pixels);
    assert(lap_mesh_load(&mesh,SHIP_L3M,sizeof(SHIP_L3M),&a)==LAP_ASSET_EBUDGET&&a.used==0);
    lap_asset_file_t f={0}; static uint8_t source[70000]; job_result=70000;
    assert(lap_asset_begin(&f,"test","../bad",source,sizeof(source))==LAP_SD_EINVAL);
    assert(!lap_asset_begin(&f,"test","large.png",source,sizeof(source)));
    assert(lap_asset_begin(&f,"test","large.png",source,sizeof(source))==LAP_ASSET_ESTATE);
    assert(lap_asset_poll(&f)==0); assert(lap_asset_poll(&f)==0); assert(read_len==65536&&read_offset==0);
    assert(lap_asset_poll(&f)==0); assert(lap_asset_poll(&f)==0); assert(read_len==4464&&read_offset==65536);
    assert(lap_asset_poll(&f)==0); assert(lap_asset_poll(&f)==1); assert(read_count==2&&f.size==70000&&source[69999]==7);
    job_result=70001; assert(!lap_asset_begin(&f,"test","huge.png",source,sizeof(source)));
    assert(!lap_asset_poll(&f)); assert(lap_asset_poll(&f)==LAP_ASSET_EBUDGET);
    job_result=-5; assert(!lap_asset_begin(&f,"test","missing.png",source,sizeof(source)));
    assert(!lap_asset_poll(&f)); assert(lap_asset_poll(&f)==-5);
}
static void games(void) {
    uint8_t pixels[8]={1,2,255,3,4,5,6,7},dest[24],pal[16];
    lap_sprite_t sheet={{pixels,4,2},2,2}; lap_canvas_t c={dest,6,4,3,0};
    for(int i=0;i<16;i++) pal[i]=(uint8_t)(15-i);
    memset(dest,0,sizeof(dest)); lap_sprite_draw(&c,&sheet,0,1,0,2,LAP_SPRITE_FLIP_X|LAP_SPRITE_FLIP_Y,pal);
    static const uint8_t golden[24]={0,10,10,11,11,0,0,10,10,11,11,0,0,13,13,14,14,0,0,13,13,14,14,0};
    assert(!memcmp(dest,golden,sizeof(dest)));
    lap_sprite_draw(&c,&sheet,1,-1,0,1,0,NULL); assert(dest[0]==3&&dest[6]==7);
    lap_sprite_draw(&c,&sheet,200,0,0,1,0,NULL); assert(dest[0]==3);
    lap_animation_t anim={2,3,0,100,0,true,false}; assert(lap_animation_step(&anim,750)==3&&anim.elapsed==50);
    anim.loop=false; assert(lap_animation_step(&anim,900)==4&&anim.done);
    uint16_t tiles[4]={0,1,65535,0}; uint8_t flags[4]={1,0,0,2}; lap_tilemap_t map={&sheet,tiles,flags,2,2};
    memset(dest,0,sizeof(dest)); lap_tilemap_draw(&c,&map,1,0); assert(dest[0]==2&&dest[1]==0&&dest[2]==3);
    assert(lap_collision_tiles(&map,(lap_aabb_t){-1,-1,2,2},1));
    assert(!lap_collision_tiles(&map,(lap_aabb_t){2,0,2,2},1));
    assert(!lap_collision_tiles(&map,(lap_aabb_t){-100,-100,1,1},1));
    assert(lap_collision_aabb((lap_aabb_t){0,0,2,2},(lap_aabb_t){1,1,2,2}));
    assert(!lap_collision_aabb((lap_aabb_t){0,0,2,2},(lap_aabb_t){2,0,2,2}));
    assert(lap_collision_circle(0,0,2,3,0,2)); assert(!lap_collision_circle(0,0,2,4,0,2));
    lap_particle_t items[1]={0}; lap_particles_t p={items,1};
    assert(lap_particle_emit(&p,(lap_particle_t){1,1,4,0,0.2f,7})); assert(!lap_particle_emit(&p,items[0]));
    lap_particles_step(&p,0.1f,10); assert(items[0].x>1&&items[0].vy==1); lap_particles_draw(&c,&p,0,0);
    lap_particles_step(&p,1,10); assert(items[0].life<=0);
    lap_shake_t shake={100,100,1,5,0,0}; lap_shake_step(&shake,50); assert(shake.x>=-2&&shake.x<=2);
    lap_shake_step(&shake,50); assert(!shake.left_ms&&!shake.x&&!shake.y);
    lap_fade(&c,8,1); for(unsigned i=0;i<sizeof(dest);i++) assert(dest[i]==8);
}
static void music(void) {
    static lap_music_song_t song; static lap_music_player_t p,q; static int16_t pcm[16000],other[16000]; int line=0;
    assert(!lap_music_parse(&song,TUNE,sizeof(TUNE)-1,&line)&&line==0&&song.bpm==132);
    lap_music_start(&p,&song); lap_music_render(&p,pcm,16000);
    lap_music_start(&q,&song); lap_music_render(&q,other,3000); lap_music_render(&q,other+3000,13000);
    assert(!memcmp(pcm,other,sizeof(pcm))); int energy=0; for(unsigned i=0;i<16000;i++) energy|=pcm[i]; assert(energy);
    lap_music_start(&p,&song); mix_limit=0; unsigned calls=(unsigned)mix_calls;
    assert(lap_music_pump(&p)==0&&p.count==1024&&p.offset==0&&(unsigned)mix_calls==calls+1);
    unsigned row=p.row,left=p.row_left; assert(lap_music_pump(&p)==0&&p.row==row&&p.row_left==left);
    mix_limit=17; assert(lap_music_pump(&p)==17&&p.offset==17&&p.row_left==left);
    mix_limit=4096; assert(lap_music_pump(&p)==1007&&p.offset==p.count);
    lap_music_stop(&p); assert(!p.playing&&!p.song);
    assert(!lap_music_parse(&song,SECOND_TUNE,sizeof(SECOND_TUNE)-1,&line)&&song.order_count==2);
    const char *sharp="LTM1\ntempo 120 4\ninst 0 0 25 0 10\npattern 0 1\nrow 0 C#4/0 ... ... ... # comment\norder 0\nloop 255\n";
    assert(!lap_music_parse(&song,sharp,strlen(sharp),&line)&&song.patterns[0][0][0].note==61);
    lap_music_start(&p,&song); lap_music_render(&p,pcm,16000); assert(!p.playing&&p.order==1);
    const char *bad="LTM1\ntempo 120 4\npattern 0 1\nrow 1 C4/0 ... ... ...\norder 0\nloop 0\n";
    assert(lap_music_parse(&song,bad,strlen(bad),&line)==-200&&line==4&&!song.bpm);
    char too_many[192]="LTM1\ntempo 120 4\npattern 0 1\norder";
    for(int i=0;i<33;i++) strcat(too_many," 0");
    strcat(too_many,"\nloop 0\n");
    assert(lap_music_parse(&song,too_many,strlen(too_many),&line)==-200&&line==4&&!song.bpm);
    assert(lap_music_parse(&song,NULL,1,&line)==-200&&!song.bpm&&line==0);
}
static uint8_t *file(const char *name,size_t *n) {
    FILE *f=fopen(name,"rb"); assert(f); fseek(f,0,SEEK_END); long len=ftell(f); assert(len>=0); rewind(f);
    uint8_t *p=malloc((size_t)len+1); assert(p); *n=(size_t)len; assert(fread(p,1,*n,f)==*n); fclose(f); return p;
}
int main(int argc,char **argv) {
    if(argc>=4) {
        size_t n=0; uint8_t *data=file(argv[2],&n); int expected=atoi(argv[3]);
        lap_asset_arena_t a; lap_asset_arena_init(&a,arena_memory,sizeof(arena_memory));
        if(!strcmp(argv[1],"png")) {
            lap_image_t image={0}; int r=lap_image_png(&image,data,n,&a,inks); assert(r==expected);
            if(!r) { assert(argc==7); size_t raw_n; uint8_t *raw=file(argv[4],&raw_n); assert(image.w==atoi(argv[5])&&image.h==atoi(argv[6])); assert(raw_n==(size_t)image.w*image.h&&!memcmp(raw,image.pixels,raw_n)); free(raw); }
        } else { l3_mesh mesh={0}; assert(lap_mesh_load(&mesh,data,n,&a)==expected); }
        free(data); return 0;
    }
    assets(); games(); music(); puts("PASS games/assets: decode, budgets, SD chunks, golden blit, collision, music/backpressure"); return 0;
}
