/* Production drag/wheel handlers, viewport projection and finger channels. */
#include "ls_test.h"
#include "../../components/apps/tui/screens/ext/scr_map.c"

static int cell_w=10,cell_h=17;
void ls_tui_geometry(int *cols,int *rows,int *cw,int *ch)
{
    if(cols)*cols=160;
    if(rows)*rows=60;
    if(cw)*cw=cell_w;
    if(ch)*ch=cell_h;
}
static int glass_x,glass_y;
static bool glass_down;
__attribute__((externally_visible)) bool ls_touch_read(uint16_t *x,uint16_t *y,bool *pressed)
{ *x=glass_x;*y=glass_y;*pressed=glass_down;return true; }
__attribute__((externally_visible)) bool ls_tui_pixel_to_cell(int x,int y,int *col,int *row)
{ if(col)*col=x/cell_w;if(row)*row=y/cell_h;return x>=0 && y>=0; }
__attribute__((externally_visible)) bool ls_tui_pixel_to_grid(int x,int y,int *gx,int *gy)
{ if(gx)*gx=x;if(gy)*gy=y;return true; }

static void finger(bool window,int x,int y,bool down)
{
    if(window) ls_tui_touch_client(x/cell_w,y/cell_h,x,y,down);
    else {
        glass_x=x;glass_y=y;glass_down=down;
        ls_tui_touch_poll(NULL);
    }
}
static void prepare(int view,int width,int height)
{
    ls_tui_touch_client_clear();
    ls_tui_touch_remote(0,0,0,0,false);
    glass_down=false;ls_tui_touch_poll(NULL);
    drag_reset();s_view=(map_view_t)view;
    s_map_cells=(tui_rect){3,5,width,height};
    s_pw=width*SUB_X;s_ph=height*sub_y();
    ls_map_external_view(s_pw,s_ph);
    ls_map_set_tile_px(cartocore_view()?256:256*SUB_X/cell_w);
    ls_map_zoom_by(14-ls_map_zoom());ls_map_center(43.4445,-71.6473);
}
static void check_pointer(double lat,double lon,int x,int y)
{
    double fx,fy;
    LS_CHECK(frame_px(lat,lon,s_pw,s_ph,&fx,&fy));
    LS_NEAR((x/(double)cell_w-s_map_cells.x)*SUB_X,fx,1e-7);
    LS_NEAR((y/(double)cell_h-s_map_cells.y)*sub_y(),fy,1e-7);
}
LS_CASE(every_view_follows_grid_pixels_on_glass_and_in_a_window)
{
    const int geometry[][2]={{10,17},{8,14},{12,24}};
    for(int g=0;g<3;g++) for(int window=0;window<2;window++)
    for(int view=0;view<MAP_VIEW__COUNT;view++) {
        cell_w=geometry[g][0];cell_h=geometry[g][1];
        prepare(view,window?115:52,window?20:56);
        const int x0=43*cell_w+3,y0=15*cell_h+2;
        double lat,lon;
        frame_ll((x0/(double)cell_w-s_map_cells.x)*SUB_X,
                 (y0/(double)cell_h-s_map_cells.y)*sub_y(),s_pw,s_ph,&lat,&lon);
        finger(window,x0,y0,true);drag_pan();LS_CHECK(!s_dragging);
        int x=x0-20*cell_w,y=y0-6*cell_h;
        finger(window,x,y,true);drag_pan();LS_CHECK(s_dragging);
        check_pointer(lat,lon,x,y);
        /* Sub-cell steps and a stationary redraw must not lose or add motion. */
        for(int i=0;i<25;i++) {
            x++;y+=2;finger(window,x,y,true);drag_pan();check_pointer(lat,lon,x,y);
            drag_pan();check_pointer(lat,lon,x,y);
        }
        finger(window,x,y,false);drag_pan();LS_CHECK(!s_dragging);
    }
}
LS_CASE(cartocore_pan_uses_its_own_scale_even_before_the_next_draw)
{
    cell_w=10;cell_h=17;
    for(int view=MAP_VIEW_CC_SMOOTH;view<MAP_VIEW__COUNT;view++) {
        prepare(view,115,20);
        double lat,lon;frame_ll(40,30,s_pw,s_ph,&lat,&lon);
        ls_map_set_tile_px(76); /* Previous libcarto preview / view. */
        pan(7.25,-3.75);
        double x,y;LS_CHECK(frame_px(lat,lon,s_pw,s_ph,&x,&y));
        LS_NEAR(32.75,x,1e-7);LS_NEAR(33.75,y,1e-7);
    }
}
LS_CASE(wheel_direction_and_pointer_anchor_work_for_every_view)
{
    cell_w=10;cell_h=17;
    LS_CHECK(ls_scr_map.wheel);
    for(int view=0;view<MAP_VIEW__COUNT;view++) {
        prepare(view,115,20);
        int c=29,r=12;double lat,lon;
        frame_ll((c-s_map_cells.x+0.5)*SUB_X,(r-s_map_cells.y+0.5)*sub_y(),s_pw,s_ph,&lat,&lon);
        ls_tui_pointer_set(c,r);
        LS_CHECK(wheel_zoom(LS_TK_WHEEL_UP));LS_EQ_INT(15,ls_map_zoom());
        /* Check the exact cell centre for odd cell heights as well. */
        double x,y;frame_px(lat,lon,s_pw,s_ph,&x,&y);
        LS_NEAR((c-s_map_cells.x+0.5)*SUB_X,x,1e-7);
        LS_NEAR((r-s_map_cells.y+0.5)*sub_y(),y,1e-7);
        LS_CHECK(wheel_zoom(LS_TK_WHEEL_DOWN));LS_EQ_INT(14,ls_map_zoom());
        double clat,clon;ls_map_get_center(&clat,&clon);
        LS_NEAR(43.4445,clat,1e-9);LS_NEAR(-71.6473,clon,1e-9);
        ls_tui_pointer_set(-1,-1);
        LS_CHECK(wheel_zoom(LS_TK_WHEEL_UP));
        double a,b;ls_map_get_center(&a,&b);LS_NEAR(clat,a,1e-12);LS_NEAR(clon,b,1e-12);
        LS_CHECK(!wheel_zoom(LS_TK_UP));LS_CHECK(!wheel_zoom(LS_TK_CHAR));
        ls_tui_pointer_set(c,r);ls_map_zoom_by(100);
        ls_map_get_center(&a,&b);LS_CHECK(wheel_zoom(LS_TK_WHEEL_UP));
        ls_map_get_center(&clat,&clon);LS_NEAR(a,clat,1e-12);LS_NEAR(b,clon,1e-12);
    }
    ls_tui_pointer_set(-1,-1);
}

LS_CASE(release_applies_the_last_pixels_once_even_between_frames)
{
    cell_w=10;cell_h=17;
    for(int window=0;window<2;window++) for(int view=0;view<MAP_VIEW__COUNT;view++)
    for(int drawn=0;drawn<2;drawn++) {
        prepare(view,115,20);
        int x0=433,y0=257;double lat,lon;
        frame_ll((x0/10.0-s_map_cells.x)*SUB_X,(y0/17.0-s_map_cells.y)*sub_y(),s_pw,s_ph,&lat,&lon);
        finger(window,x0,y0,true);
        if(drawn) {
            finger(window,x0-40,y0-34,true);drag_pan();
            check_pointer(lat,lon,x0-40,y0-34);
        }
        /* Glass reports its last pressed sample on release, as the driver does. */
        finger(window,x0-203,y0-105,true);
        finger(window,x0-203,y0-105,false);drag_pan();
        check_pointer(lat,lon,x0-203,y0-105);
        drag_pan();check_pointer(lat,lon,x0-203,y0-105);
    }
}
