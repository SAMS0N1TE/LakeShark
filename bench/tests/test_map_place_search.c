#include "ls_test.h"
#include "../../components/apps/tui/ls_place_search.c"
static ls_cells_view fixture;
static bool following,called;
static double latitude,longitude;
static int zoom=10;
int ls_tui_corner_pad(int row) { (void)row;return 0; }
bool ls_tui_keyboard_mode(void) { return false; }
static unsigned picked_index;
static bool accepted=true;
bool ls_cells_query(const char *q) { snprintf(fixture.query,sizeof(fixture.query),"%s",q);return true; }
bool ls_cells_snapshot(ls_cells_view *out) { *out=fixture;return true; }
bool ls_cells_pick(unsigned i,unsigned r) { (void)r;picked_index=i;return true; }
bool ls_cells_action(unsigned a,uint32_t k) { (void)a;(void)k;return accepted; }
bool ls_cells_server(const char *s) { (void)s;return true; }
void ls_keyboard_open(const char *t,const char *s,int n,ls_keyboard_done_t cb) { (void)t;(void)s;(void)n;(void)cb; }
void ls_keyboard_close(void) {}
const char *ls_keyboard_text(void) { return ""; }
bool ls_keyboard_active(void) { return false; }
void ls_tiles_search_globe(tui_surface *sf,tui_rect a,float p,double lat,double lon) { (void)sf;(void)a;(void)p;(void)lat;(void)lon; }
void ls_map_follow_set(bool b) { following=b; }
void ls_map_center(double lat,double lon) { latitude=lat;longitude=lon; }
int ls_map_zoom(void) { return zoom; }
void ls_map_zoom_by(int delta) { zoom+=delta; }
static void go(double lat,double lon) { called=true;following=false;ls_map_center(lat,lon); }
LS_CASE(place_selection_uses_maps_navigation_callback) {
    memset(&fixture,0,sizeof(fixture));fixture.count=1;
    fixture.hits[0].lat=42;fixture.hits[0].lon=-70;fixture.hits[0].kind=3;
    called=false;following=true;ls_place_search_goto(go);pick();
    LS_CHECK(called && !following && !opened);
    LS_NEAR(42,latitude,0.001);LS_NEAR(-70,longitude,0.001);LS_EQ_INT(14,zoom);
    called=false;following=true;ls_place_search_open(true);pick();
    LS_CHECK(!called && !following && !opened);
}
static tui_cell back_cells[120*70],front_cells[120*70];
static tui_surface surface;
static int width,height;
static void render(void) {
    tui_surface_setup(&surface,back_cells,front_cells,width,height);
    tui_frame_begin(&surface);
    ls_place_search_draw(&surface,tui_rect_make(0,0,width,height));
}
static bool tap_button(int b) {
    for(int y=0;y<height;y++)for(int x=0;x<width;x++)if(ls_btn_hit(x,y)==b) {
        ls_place_search_touch(x,y);return true;
    }
    return false;
}
LS_CASE(place_lists_page_by_touch_and_reveal_keyboard_selection) {
    for(int orientation=0;orientation<2;orientation++) {
        width=orientation?115:48;height=orientation?24:60;
        memset(&fixture,0,sizeof(fixture));fixture.count=8;fixture.library_count=32;
        for(unsigned i=0;i<32;i++)snprintf(fixture.library[i],64,"Place %02u",i);
        ls_place_search_open(false);page=2;render();
        while(first_row+visible<fixture.library_count) {
            unsigned old=first_row;LS_CHECK(tap_button(4));render();LS_CHECK(first_row>old);
        }
        ls_place_search_touch(rows.x,rows.y+(31-first_row)*3);LS_EQ_UINT(31,selected);
        LS_CHECK(tap_button(3));render();LS_CHECK(selected<31);
        page=0;selected=0;render();
        if(visible<8) { LS_CHECK(tap_button(7));render();LS_CHECK(first_row>0); }
        selected=7;render();LS_CHECK(selected>=first_row && selected<first_row+visible);
        ls_place_search_touch(rows.x,rows.y+(7-first_row)*3);LS_EQ_UINT(7,picked_index);LS_EQ_UINT(1,page);
    }
}
LS_CASE(place_controls_focus_shortcuts_and_rejection_feedback) {
    width=115;height=24;ls_place_search_open(false);render();
    ls_place_search_key(LS_TK_CHAR,'2');LS_CHECK(!strcmp(query,"2") && page==0);
    ls_place_search_key(LS_TK_TAB,0);LS_EQ_INT(0,focus);
    ls_place_search_key(LS_TK_RIGHT,0);LS_EQ_INT(1,focus);
    ls_place_search_key(LS_TK_RIGHT,0);LS_EQ_INT(2,focus);
    ls_place_search_key(LS_TK_ENTER,0);LS_EQ_UINT(2,page);
    ls_place_search_key(LS_TK_ESC,0);ls_place_search_key(LS_TK_F2,0);LS_EQ_UINT(2,page);
    fixture.library_count=1;render();accepted=false;button(1);
    LS_CHECK(strstr(notice,"not accepted"));
    page=4;button(1);LS_EQ_UINT(4,page);LS_CHECK(strstr(notice,"not accepted"));
    accepted=true;button(1);LS_EQ_UINT(2,page);render();LS_CHECK(strstr(notice,"waiting"));
    fixture.action_done++;strcpy(fixture.message,"Queued; safe to leave TILES");render();LS_CHECK(!notice[0]);
}
