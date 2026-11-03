/* @NAME@: @SUB@. Arrows/WASD or held left mouse steer. R centres.
 * Binary mesh generated with obj2l3.py --binary. Embedded fallback lets the
 * same .lap run without SD; apps_data/<id>/ship.l3m overrides it at runtime. */
#include "lakeshark_app.h"
#include "lap_assets.h"
#include "lap_loop.h"
#include "lap_canvas.h"
#include <string.h>
#include "assets.inc"
static struct {
    l3_scene *scene;
    l3_mesh mesh;
    lap_asset_arena_t arena;
    lap_asset_file_t file;
    lap_caps_t caps;
    lap_loop_t loop;
    lap_input_state_t input;
    lap_input_event_t event;
    uint8_t memory[16384],source[8192];
    l3_xform pose;
    int material,ground,error;
    float yaw,pitch,distance;
} s;
static void enter(void) {
    memset(&s,0,sizeof(s)); lap_caps(&s.caps,sizeof(s.caps));
    lap_asset_arena_init(&s.arena,s.memory,sizeof(s.memory));
    s.error=lap_mesh_load(&s.mesh,SHIP_L3M,sizeof(SHIP_L3M),&s.arena);
    if(s.caps.hardware&LAP_HW_SD) lap_asset_begin(&s.file,"@ID@","ship.l3m",s.source,sizeof(s.source));
    lap_loop_init(&s.loop,60,6,lap_millis());
}
static void draw(void) {
    int w=lap_cols(),h=lap_rows();
    lap_clear(LAP_ATTR(LAP_WHITE,LAP_BLACK));
    if(!s.scene) {
        s.scene=l3_create(w,h);
        if(!s.scene) { lap_put_str(0,0,"3D needs more memory",LAP_ATTR(LAP_RED,LAP_BLACK)); return; }
        s.material=l3_material(s.scene,L3_RAMP_METAL,4,0.6f,0);
        s.ground=l3_material(s.scene,L3_RAMP_GREEN,4,0,L3_CHECKER);
    }
    if(!l3_resize(s.scene,w,h)) return;
    if(s.file.state==1||s.file.state==2) {
        int r=lap_asset_poll(&s.file);
        if(r==1) { l3_mesh mesh; int e=lap_mesh_load(&mesh,s.source,s.file.size,&s.arena); if(!e) s.mesh=mesh; else s.error=e; }
        else if(r<0&&r!=LAP_SD_ENOENT) s.error=r;
    }
    while(lap_input_next(&s.event)>0) lap_loop_event(&s.loop,&s.event,lap_millis());
    lap_input_state(&s.input);
    unsigned steps=lap_loop_advance(&s.loop,lap_millis(),NULL);
    while(steps--) {
        float dx=(lap_key_down(LAP_KEY_RIGHT)||lap_key_down(LAP_KEY_CHAR('d')))-(lap_key_down(LAP_KEY_LEFT)||lap_key_down(LAP_KEY_CHAR('a')));
        float dy=(lap_key_down(LAP_KEY_UP)||lap_key_down(LAP_KEY_CHAR('w')))-(lap_key_down(LAP_KEY_DOWN)||lap_key_down(LAP_KEY_CHAR('s')));
        if(s.input.buttons&LAP_BUTTON_LEFT) {
            int cw=1,ch=1; lap_cell_px(&cw,&ch);
            dx=lc_clampf((s.input.x/(float)cw/w-0.5f)*2,-1,1);
            dy=lc_clampf((0.5f-s.input.y/(float)ch/h)*2,-1,1);
        }
        s.yaw+=dx*0.8f; s.pitch=lc_clampf(s.pitch+dy*0.5f,-45,45); s.distance+=0.06f;
        if(s.distance>1000) s.distance=0;
    }
    l3_begin(s.scene); l3_sky(s.scene,LC_BLUE,LC_CYAN);
    l3_camera(s.scene,l3v(0,2,7),l3v(0,1,0),50);
    l3_floor(s.scene,-1,s.ground);
    for(int i=0;i<8;i++) {
        float z=-i*8+fmodf(s.distance,8);
        l3_line(s.scene,l3v(-8,-0.95f,z),l3v(8,-0.95f,z),LC_BR_GREEN);
    }
    s.pose=l3_pose(l3v(0,0,0),l3_quat_euler(s.yaw,s.pitch,-s.yaw*0.3f),1);
    /* This starter uses one material; games with multiple materials remap
     * converter ordinals to their own scene IDs instead. */
    s.mesh.mats=NULL;
    l3_flags(s.scene,L3_TWO_SIDED); if(s.mesh.nv) l3_draw_mesh(s.scene,&s.mesh,&s.pose,s.material);
    l3_end(s.scene); l3_show(s.scene,0,0);
    lap_put_glass(0,0,"ARROWS/WASD or mouse: steer  R: centre",LAP_ATTR(LAP_WHITE|LAP_BRIGHT,LAP_BLACK));
    if(!(s.caps.hardware&LAP_HW_SD)) lap_put_glass(0,1,"no SD: embedded ship",LAP_ATTR(LAP_WHITE,LAP_BLACK));
    if(s.error) lap_put_glass(0,2,"mesh rejected: embedded ship",LAP_ATTR(LAP_YELLOW,LAP_BLACK));
}
static bool key(lap_key_t k,char ch) { if(k==LAP_KEY_CHAR&&ch=='r') { s.yaw=s.pitch=0; return true; } return false; }
static void leave(void) { l3_destroy(s.scene); s.scene=NULL; }
LAP_APP(.id="@ID@",.name="@NAME@",.sub="@SUB@",.version="0.1",.author="@AUTHOR@",
        .color=LAP_BLUE,.enter=enter,.draw=draw,.key=key,.leave=leave);
