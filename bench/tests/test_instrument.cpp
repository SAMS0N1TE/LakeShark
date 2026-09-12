#include "home/AppHome.hpp"
#include "shell/ls_shell.hpp"
#include "ui/ls_instrument.h"
#include "ui/ls_ui.h"
#include "sdr_ui/sdr_ui.h"
#include "shell/ls_input.h"
#include <vector>
#include <cstdio>
#include <cstring>
extern "C" {
#include "ls_test.h"
#include "settings.h"
#include "ls_time.h"
static ls_hub_state_t hub;
int ls_hub_subscribe(ls_hub_fn,void *){return 0;}
void ls_hub_unsubscribe(int){}
const ls_hub_state_t *ls_hub_state(){return &hub;}
int settings_get_home_widget(){return 0;}
bool settings_set_home_widget(int){return true;}
}
const lv_img_dsc_t *ls_icon_for(const char *,int){return nullptr;}
LsShell &LsShell::instance(){static LsShell shell;return shell;}
static char launched[32];
bool LsShell::launchByName(const char *name){snprintf(launched,sizeof(launched),"%s",name);return true;}
void LsShell::cycleApp(int){}
bool LsApp::back(){return true;}
bool LsApp::exitToLauncher(){return true;}
static lv_point_t pointer_point;
static bool pointer_down;
static void pointer_read(lv_indev_drv_t *,lv_indev_data_t *data){data->point=pointer_point;data->state=pointer_down?LV_INDEV_STATE_PR:LV_INDEV_STATE_REL;}
static void pointer_tick(){lv_tick_inc(40);lv_timer_handler();}
static std::vector<lv_color_t> frame;
static int frame_width;
static unsigned flushes;
static void flush(lv_disp_drv_t *d,const lv_area_t *a,lv_color_t *pixels)
{
    int w=a->x2-a->x1+1;
    for(int y=a->y1;y<=a->y2;y++)memcpy(frame.data()+y*frame_width+a->x1,pixels+(y-a->y1)*w,w*sizeof(lv_color_t));
    flushes++;lv_disp_flush_ready(d);
}
static void check_buttons(lv_obj_t *o,const lv_area_t &bounds,int &count)
{
    if(lv_obj_has_flag(o,LV_OBJ_FLAG_HIDDEN))return;
    if(lv_obj_check_type(o,&lv_btn_class)){
        lv_area_t a;lv_obj_get_coords(o,&a);
        LS_CHECK(a.x1>=bounds.x1 && a.y1>=bounds.y1 && a.x2<=bounds.x2 && a.y2<=bounds.y2);
        LS_CHECK(lv_obj_get_height(o)>=36);count++;
    }
    for(unsigned i=0;i<lv_obj_get_child_cnt(o);i++)check_buttons(lv_obj_get_child(o,i),bounds,count);
}
static void run_home(int width,int height)
{
    static bool initialized;if(!initialized){lv_init();initialized=true;}
    static lv_color_t pixels[1232*16];
    lv_disp_draw_buf_t buffer;lv_disp_draw_buf_init(&buffer,pixels,nullptr,1232*16);
    lv_disp_drv_t drv;lv_disp_drv_init(&drv);drv.hor_res=width;drv.ver_res=height;drv.draw_buf=&buffer;drv.flush_cb=flush;
    frame_width=width;frame.assign(width*height,lv_color_black());
    auto *display=lv_disp_drv_register(&drv);lv_disp_set_default(display);
    auto *parent=lv_obj_create(lv_scr_act());ls_instrument_flat(parent);
    lv_obj_set_pos(parent,8,64);lv_obj_set_size(parent,width-16,height-192);
    hub.freq_hz=154785000;snprintf(hub.target_app,sizeof(hub.target_app),"P25");
    AppHome home;LS_CHECK(home.run(parent));lv_obj_update_layout(parent);lv_refr_now(display);
    lv_area_t bounds;lv_obj_get_coords(parent,&bounds);int count=0;check_buttons(parent,bounds,count);
    LS_EQ_INT(count,13);
    unsigned before=flushes;home.resume();lv_refr_now(display);LS_EQ_INT(flushes,before);
    const char *dir=getenv("LS_INSTRUMENT_RENDER_DIR");
    if(dir){char path[512];snprintf(path,sizeof(path),"%s/home-%dx%d.ppm",dir,width,height);FILE *f=fopen(path,"wb");LS_CHECK(f!=nullptr);
        if(f){fprintf(f,"P6\n%d %d\n255\n",width,height);for(auto pixel:frame){auto c=lv_color_to32(pixel);unsigned char rgb[]={(unsigned char)(c>>16),(unsigned char)(c>>8),(unsigned char)c};fwrite(rgb,1,3,f);}fclose(f);}}
    auto *target=sdr_btn(parent,"TEST",[](lv_event_t *e){++*static_cast<int *>(lv_event_get_user_data(e));},&count,nullptr);
    lv_obj_set_pos(target,20,20);lv_obj_set_size(target,160,60);lv_obj_update_layout(parent);
    lv_indev_drv_t pointer;lv_indev_drv_init(&pointer);pointer.type=LV_INDEV_TYPE_POINTER;pointer.read_cb=pointer_read;pointer.disp=display;
    auto *input=lv_indev_drv_register(&pointer);
    lv_area_t target_bounds;lv_obj_get_coords(target,&target_bounds);
    int clicks=count;
    pointer_point={static_cast<lv_coord_t>(target_bounds.x1+20),static_cast<lv_coord_t>(target_bounds.y1+40)};
    pointer_down=true;pointer_tick();pointer_point.y-=25;pointer_tick();pointer_down=false;pointer_tick();
    LS_EQ_INT(count,clicks);
    pointer_point.y+=25;pointer_down=true;pointer_tick();pointer_down=false;pointer_tick();LS_EQ_INT(count,clicks+1);
    lv_indev_delete(input);lv_obj_del(target);
    int changes[2]={};
    auto *meter=sdr_seg_slider(parent,ls_instrument_accent(),100,50,
        [](void *p,int value){static_cast<int *>(p)[0]=value;},changes,nullptr);
    sdr_seg_on_release(meter,[](void *p,int value){static_cast<int *>(p)[1]=value;});
    auto *steps=lv_obj_get_child(lv_obj_get_child(parent,-1),1);
    lv_event_send(lv_obj_get_child(steps,0),LV_EVENT_CLICKED,nullptr);
    LS_EQ_INT(sdr_seg_value(meter),45);LS_EQ_INT(changes[0],45);LS_EQ_INT(changes[1],45);
    for(int i=0;i<30;i++)lv_event_send(lv_obj_get_child(steps,1),LV_EVENT_CLICKED,nullptr);
    LS_EQ_INT(sdr_seg_value(meter),100);
    for(int i=0;i<30;i++)lv_event_send(lv_obj_get_child(steps,0),LV_EVENT_CLICKED,nullptr);
    LS_EQ_INT(sdr_seg_value(meter),0);
    sdr_seg_set(meter,60);LS_EQ_INT(sdr_seg_value(meter),60);
    lv_event_send(lv_obj_get_child(steps,0),LV_EVENT_LONG_PRESSED_REPEAT,nullptr);
    LS_EQ_INT(sdr_seg_value(meter),55);
    auto *tabs=ls_ui_tab_strip(parent,LV_DIR_TOP,0);
    LS_CHECK(!lv_obj_has_flag(lv_tabview_get_content(tabs),LV_OBJ_FLAG_SCROLLABLE));
    ls_ui_screen_t pages={};
    ls_ui_screen_create(parent,nullptr,true,LS_UI_COLOR_ACCENT,&pages);
    ls_ui_screen_add_tab(&pages,"LIVE");
    ls_ui_screen_add_tab(&pages,"SIGNAL");
    int page_events=0;
    lv_obj_add_event_cb(pages.tabs,[](lv_event_t *e){++*static_cast<int *>(lv_event_get_user_data(e));},LV_EVENT_VALUE_CHANGED,&page_events);
    lv_obj_update_layout(parent);
    LS_CHECK(pages.pager!=nullptr);
    LS_CHECK(lv_obj_get_height(lv_obj_get_child(pages.pager,0))>=48);
    lv_event_send(lv_obj_get_child(pages.pager,2),LV_EVENT_SHORT_CLICKED,nullptr);
    LS_EQ_INT(lv_tabview_get_tab_act(pages.tabs),1);
    LS_EQ_INT(page_events,1);
    lv_event_send(lv_obj_get_child(pages.pager,0),LV_EVENT_SHORT_CLICKED,nullptr);
    LS_EQ_INT(lv_tabview_get_tab_act(pages.tabs),0);
    LS_EQ_INT(page_events,2);
    LS_CHECK(home.close());lv_disp_remove(display);
}
LS_CASE(real_home_directory_fits_portrait){run_home(568,1232);}
LS_CASE(real_home_directory_fits_landscape){run_home(1232,568);}
