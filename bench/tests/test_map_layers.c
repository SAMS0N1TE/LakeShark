#include "ls_test.h"
#include "../../components/apps/tui/screens/ext/scr_map.c"
#include "../../components/apps/tui/ls_map_ink.c"
#include "../../components/apps/tui/ls_motion.c"

static uint32_t saved_layers;
void settings_set_map_layers(uint32_t bits) { saved_layers = bits; }
void ls_picker_open(const char *title, ls_picker_done_t cb)
{ (void)title; (void)cb; }
bool ls_picker_add(const char *text, const char *detail)
{ (void)text; (void)detail; return true; }

LS_CASE(aprs_and_route_picker_switches_are_independent_and_persist_style)
{
    s_palette = 1;
    s_view = MAP_VIEW_FIELD;
    layers_set(LAYERS_DEFAULT);
    LS_CHECK(layer(L_APRS) && layer(L_ROUTE));
    int aprs = -1, route = -1;
    for (int i = 0; i < N_LAYER_ROWS; i++) {
        if (!strcmp(LAYER_ROWS[i].name, "APRS stations")) aprs = i;
        if (!strcmp(LAYER_ROWS[i].name, "Route")) route = i;
    }
    LS_CHECK(aprs >= 0 && route >= 0);
    layer_pick(aprs);
    LS_CHECK(!layer(L_APRS) && layer(L_ROUTE));
    LS_CHECK(!(saved_layers & L_APRS) && (saved_layers & L_ROUTE));
    layer_pick(route);
    LS_CHECK(!layer(L_APRS) && !layer(L_ROUTE));
    layer_pick(aprs);
    LS_CHECK(layer(L_APRS) && !layer(L_ROUTE));
    LS_CHECK((saved_layers & L_APRS) && !(saved_layers & L_ROUTE));
    LS_CHECK((saved_layers & ~LAYER_BITS) == style_bits());
}

LS_CASE(ais_layer_has_its_own_persisted_switch)
{
    layers_set(LAYERS_DEFAULT);
    LS_CHECK(layer(L_AIS));
    int ais = -1;
    for (int i = 0; i < N_LAYER_ROWS; ++i) if (LAYER_ROWS[i].bit == L_AIS) ais = i;
    LS_CHECK(ais >= 0);
    uint32_t before = saved_layers;
    layer_pick(ais); LS_EQ_UINT(before ^ L_AIS, saved_layers);
    LS_CHECK(layer(L_AIR) && layer(L_APRS) && layer(L_ROUTE) && !layer(L_AIS));
    layer_pick(ais); LS_EQ_UINT(before, saved_layers);
}

LS_CASE(sonde_layer_switch_is_independent)
{
    layers_set(LAYERS_DEFAULT); LS_CHECK(layer(L_SONDE));
    int slot = -1;
    for (int i = 0; i < N_LAYER_ROWS; ++i) if (LAYER_ROWS[i].bit == L_SONDE) slot = i;
    LS_CHECK(slot >= 0); uint32_t before = saved_layers;
    layer_pick(slot); LS_EQ_UINT(before ^ L_SONDE,saved_layers);
    LS_CHECK(layer(L_AIR) && layer(L_APRS) && layer(L_AIS) && layer(L_ROUTE));
    layer_pick(slot); LS_EQ_UINT(before,saved_layers);
}

uint32_t settings_get_map_layers(uint32_t fallback) { (void)fallback;return saved_layers; }
LS_CASE(cartocore_fills_roundtrip_settings_without_changing_layers_or_legacy_fills)
{
    for(int mode=0;mode<MAP_VIEW__COUNT;mode++) {
        s_view=(map_view_t)mode;s_palette=2;
        layers_set(LAYERS_DEFAULT);
        LS_EQ_UINT(LAYERS_DEFAULT,saved_layers&LAYER_BITS);
        s_view=MAP_VIEW_FIELD;s_palette=0;palette_load();
        LS_EQ_INT(mode,s_view);LS_EQ_INT(2,s_palette);
    }
    LS_CHECK(!strcmp(VIEW_NAME[MAP_VIEW_CC_SMOOTH],"CartoCore smooth"));
    LS_CHECK(!strcmp(VIEW_NAME[MAP_VIEW_CC_BRAILLE],"CartoCore braille"));
}
int ls_map_tile_px(void) { return 256; }
static int test_zoom=14;
int ls_map_zoom(void) { return test_zoom; }
static double test_lat=43.4445,test_lon=-71.6473;
void ls_map_get_center(double *lat,double *lon) { *lat=test_lat;*lon=test_lon; }
LS_CASE(cartocore_projection_keeps_overlay_centres_and_inverse_touch_coordinates)
{
    s_view=MAP_VIEW_CC_BRAILLE;
    double x,y,lat,lon;
    LS_CHECK(frame_px(43.4445,-71.6473,104,240,&x,&y));
    LS_NEAR(52,x,1e-8);LS_NEAR(120,y,1e-8);
    frame_ll(30,70,104,240,&lat,&lon);
    LS_CHECK(frame_px(lat,lon,104,240,&x,&y));
    LS_NEAR(30,x,1e-7);LS_NEAR(70,y,1e-7);
    int cx=0,cy=0;
    LS_CHECK(map_cell_of(lat,lon,tui_rect_make(0,0,52,60),104,240,&cx,&cy));
    LS_CHECK(cx==14 || cx==15);
    LS_EQ_INT(17,cy);
}

tui_rect tui_rect_make(int x,int y,int w,int h) { return (tui_rect){x,y,w,h}; }
void ls_tui_geometry(int *cols,int *rows,int *cw,int *ch) {
    if(cols) *cols=52;
    if(rows) *rows=70;
    if(cw) *cw=10;
    if(ch) *ch=17;
}

static const ls_tui_theme_t *test_theme=&ls_theme_terminal_bay;
const ls_tui_theme_t *ls_tui_active_theme(void) { return test_theme; }
bool ls_tui_daylight(void) { return false; }
static unsigned colour_distance(uint16_t a,uint16_t b) {
    carto_rgb x=rgb565(a),y=rgb565(b);
    return abs(x.r-y.r)+abs(x.g-y.g)+abs(x.b-y.b);
}
LS_CASE(all_map_themes_switch_live_and_reserve_the_data_accent)
{
    for(int i=0;i<=ls_tui_theme_count();i++) {
        test_theme=i==ls_tui_theme_count()?&ls_theme_daylight:ls_tui_theme_at(i);
        s_palette=0;s_basemap_busy=false;
        const map_palette_t *p=palette();
        LS_CHECK(!strcmp(p->name,test_theme->name));
        unsigned normal=colour_distance(carto_rgb565(p->ground),carto_rgb565(p->water));
        int generation=s_palette_key;
        LS_EQ_INT(generation,(palette(),s_palette_key));
        s_basemap_busy=true;p=palette();
        LS_CHECK(s_palette_key>generation);
        LS_CHECK(colour_distance(carto_rgb565(p->ground),carto_rgb565(p->water))<normal);
        uint16_t radar=test_theme->palette[TUI_ATTR_FG(radar_attr())];
        LS_CHECK(colour_distance(radar,carto_rgb565(p->water))>70);
        LS_CHECK(colour_distance(radar,carto_rgb565(p->road_hi))>70);
        LS_CHECK(carto_rgb565(p->ground)!=carto_rgb565(p->water));
        LS_CHECK(carto_rgb565(p->ground)!=carto_rgb565(p->park));
        LS_CHECK(carto_rgb565(p->water)!=carto_rgb565(p->road_hi));
        if(test_theme==&ls_theme_daylight) {
            LS_CHECK(p->ground.r>230 && p->ground.g>230 && p->ground.b>230);
            LS_CHECK(p->road_hi.r<p->ground.r);
        }
    }
}
LS_CASE(style_offers_every_palette_and_saved_choices_keep_their_meaning)
{
    test_theme=&ls_theme_terminal_bay;s_basemap_busy=false;
    /* 0 and 6 follow the theme, as they have since 2.10. */
    s_palette=STYLE_THEME;
    LS_EQ_INT(-1,style_palette(s_palette));
    LS_CHECK(!strcmp(palette()->name,test_theme->name));
    s_palette=STYLE_THEME_OUTLINES;
    LS_EQ_INT(-1,style_palette(s_palette));
    LS_CHECK(!strcmp(palette()->name,test_theme->name));
    LS_EQ_INT(PALETTES[PAL_VECTOR].fill,palette()->fill);
    /* Each palette saves a code of its own that names it again, and gives
       its own colours and tint. */
    for(int i=0;i<N_PALETTES;i++) {
        s_palette=palette_style(i);
        LS_CHECK(s_palette<STYLE__COUNT && s_palette!=STYLE_THEME && s_palette!=STYLE_THEME_OUTLINES);
        LS_EQ_INT(i,style_palette(s_palette));
        const map_palette_t *p=palette();
        LS_CHECK(!strcmp(p->name,PALETTES[i].name));
        LS_EQ_INT(PALETTES[i].tint,p->tint);
        LS_EQ_INT(carto_rgb565(PALETTES[i].ground),carto_rgb565(p->ground));
    }
    /* Saves from before 2.10 of 1-5 and 7 still name their palette. */
    for(int code=1;code<=7;code++) if(code!=STYLE_THEME_OUTLINES) LS_EQ_INT(code,style_palette(code));
    /* The CartoCore ramp follows a palette too: RED is nothing but red. */
    s_palette=palette_style(2);
    LS_CHECK(!strcmp(palette()->name,"RED"));
    for(int i=0;i<16;i++) {
        carto_rgb c=rgb565(s_cc_colours[i]);
        LS_CHECK(c.g<=c.r/2+8 && c.b<=c.r/2+8);
    }
    s_palette=STYLE_THEME;
}
LS_CASE(map_label_caps_scale_with_zoom_and_pane_area)
{
    s_basemap_busy=true;tui_rect a={0,0,52,60};
    test_zoom=10;LS_EQ_INT(6,map_label_cap(a));
    test_zoom=13;LS_EQ_INT(10,map_label_cap(a));
    test_zoom=15;LS_EQ_INT(16,map_label_cap(a));
    a.w=26;a.h=30;LS_EQ_INT(4,map_label_cap(a));test_zoom=14;
}
static bool test_blocked;
static bool test_free(void *ctx,int x,int y,int w,int rows) {
    (void)ctx;(void)w;(void)rows;return !(test_blocked && x==4 && y==5);
}
LS_CASE(labels_keep_their_slot_and_wait_after_an_overlay_collision)
{
    map_label_history h={0};int x=0,y=0;
    int spots[2][2]={{4,5},{8,5}}, reversed[2][2]={{8,5},{4,5}};
    test_blocked=false;
    LS_CHECK(map_label_choose(&h,1,3,3,3,1,spots,2,2,0,test_free,NULL,&x,&y));
    LS_EQ_INT(4,x);
    LS_CHECK(map_label_choose(&h,1,3,3,3,1,reversed,2,2,10000,test_free,NULL,&x,&y));
    LS_EQ_INT(4,x);
    test_blocked=true;
    LS_CHECK(!map_label_choose(&h,1,3,3,3,1,spots,2,2,20000,test_free,NULL,&x,&y));
    test_blocked=false;
    LS_CHECK(!map_label_choose(&h,1,3,3,3,1,spots,2,2,100000,test_free,NULL,&x,&y));
    test_blocked=true;
    LS_CHECK(map_label_choose(&h,1,3,3,3,1,spots,2,2,420000,test_free,NULL,&x,&y));
    LS_EQ_INT(8,x);
    test_blocked=false;
    LS_CHECK(map_label_choose(&h,1,4,3,3,1,spots,2,2,500000,test_free,NULL,&x,&y));
    LS_EQ_INT(9,x); /* Anchor-relative placement follows a one-cell pan. */
}

void ls_picker_set_detail(int i,const char *text) { (void)i;(void)text; }
static int64_t test_now=1000000000;
int64_t esp_timer_get_time(void) { return test_now; }
uint32_t ls_mesh_now(void) { return (uint32_t)(test_now/1000000); }
bool settings_get_home(float *lat,float *lon) { (void)lat;(void)lon;return false; }
static adsb_aircraft_t test_ac;
const adsb_aircraft_t *adsb_state_get(int i) { return i==0 ? &test_ac : NULL; }
bool ls_map_render_busy(void) { return false; }
void ls_map_center(double lat,double lon) { test_lat=lat;test_lon=lon; }
static bool test_follow;
void ls_map_follow_set(bool enabled) { test_follow=enabled; }
LS_CASE(saved_mesh_contacts_are_never_fresh) {
    ls_mesh_peer_t peer={0};
    LS_EQ_UINT(UINT32_MAX,node_age(&peer));
    LS_EQ_INT(LS_DIM_FG,node_hue(node_age(&peer),false));
    peer.last_heard=ls_mesh_now()-2;LS_EQ_UINT(2,node_age(&peer));
    LS_EQ_INT(TUI_MAGENTA|TUI_BRIGHT,node_hue(node_age(&peer),false));
}
LS_CASE(coverage_rejects_stale_relayed_and_pre_move_positions) {
    s_receiver_fresh=true;s_receiver.lat_deg=43;s_receiver.lon_deg=-71;
    s_cover_lat=999;memset(&test_ac,0,sizeof(test_ac));cover_update();
    test_ac.active=test_ac.pos_valid=true;test_ac.lat=43.1;test_ac.lon=-71;
    test_ac.pos_ts_us=test_now-1;cover_update();LS_NEAR(0,s_cover_max,0.001);
    test_now+=1000000;test_ac.pos_ts_us=test_now;test_ac.rebroadcast=true;
    cover_update();LS_NEAR(0,s_cover_max,0.001);
    test_ac.rebroadcast=false;test_ac.pos_ts_us=test_now-AIRCRAFT_SHOW_US-1;
    cover_update();LS_NEAR(0,s_cover_max,0.001);
    test_ac.pos_ts_us=test_now;cover_update();LS_CHECK(s_cover_max>0);
    s_receiver.lat_deg=43.05;cover_update();LS_NEAR(0,s_cover_max,0.001);
    test_now+=1000000;test_ac.pos_ts_us=test_now;cover_update();LS_CHECK(s_cover_max>0);
}
LS_CASE(target_follow_recentres_before_layer_projection) {
    s_view=MAP_VIEW_FIELD;test_zoom=14;test_lat=43.4445;test_lon=-71.6473;
    memset(&test_ac,0,sizeof(test_ac));test_ac.active=test_ac.pos_valid=true;
    test_ac.icao=1;test_ac.lat=44;test_ac.lon=-72;test_ac.pos_ts_us=test_now;
    s_sel=SEL_AIR;s_sel_icao=1;s_track=true;
    track_selection(200,200);
    double x,y;LS_CHECK(frame_px(44,-72,200,200,&x,&y));
    LS_NEAR(100,x,0.001);LS_NEAR(100,y,0.001);
    s_track=true;test_follow=true;jump_to(42,-70);
    LS_CHECK(!s_track && !test_follow);LS_NEAR(42,test_lat,0.001);
    test_lat=43.4445;test_lon=-71.6473;s_sel=SEL_NONE;
}

LS_CASE(coordinate_navigation_is_temporary_and_validated) {
    test_lat=43.4445;test_lon=-71.6473;s_track=true;test_follow=true;
    coordinates_done("91, 0");LS_NEAR(43.4445,test_lat,0.0001);LS_CHECK(s_track);
    coordinates_done("42, -70 trailing");LS_CHECK(s_track);
    coordinates_done("42, -70");LS_NEAR(42,test_lat,0.001);LS_NEAR(-70,test_lon,0.001);
    LS_CHECK(!s_track && !test_follow);test_lat=43.4445;test_lon=-71.6473;
}

/* 2026-10-21, T-Display-P4 CartoCore picture: a town name flashed in a still
   view. A ping ring blocked a name each step, and while that name waited
   out its hold the next one in rank took its cap share and went again. */
LS_CASE(ping_rings_never_move_a_place_name)
{
    tui_rect a={0,0,52,40};const int64_t was=test_now;
    for(int k=0;k<5;k++) {
        test_now=1000000000+k*400000;
        ls_ink_begin(a);s_ntaken=0;
        ping_ring(40,60,TUI_ATTR(TUI_CYAN,TUI_BLACK),2000);
        int inked=0;
        for(int y=0;y<a.h;y++) for(int x=0;x<a.w;x++) if(ls_ink_cell_prio(x,y)) {
            inked++;LS_CHECK(basemap_label_slot_free(&a,x,y,1,1));
        }
        LS_CHECK(inked>0);
    }
    ls_ink_dot(40,60,TUI_ATTR(TUI_CYAN,TUI_BLACK),8);
    LS_CHECK(!basemap_label_slot_free(&a,20,20,1,1));
    test_now=was;
}
static bool test_first_blocked;
static bool test_free_first(void *ctx,int x,int y,int w,int rows) {
    (void)ctx;(void)x;(void)w;(void)rows;return !(test_first_blocked && y<10);
}
/* draw_carto_labels' cap: a placed or held name counts against it. */
static unsigned cap_frame(map_label_history *h,int64_t now) {
    const int first[2][2]={{4,5},{4,6}},second[1][2]={{4,30}};
    unsigned shown=0;int drawn=0,x,y;
    if(map_label_choose(h,1,4,5,3,1,first,2,2,now,test_free_first,NULL,&x,&y)) { drawn++;shown|=1; }
    else if(map_label_held(h,1)) drawn++;
    if(drawn<1 && map_label_choose(h,2,4,30,3,1,second,2,1,now,test_free_first,NULL,&x,&y)) shown|=2;
    return shown;
}
LS_CASE(a_held_name_keeps_its_cap_share_so_the_next_does_not_flash)
{
    map_label_history h={0};
    test_first_blocked=false;
    LS_EQ_UINT(1,cap_frame(&h,0));
    test_first_blocked=true;
    for(int64_t t=100000;t<MAP_LABEL_HOLD_US;t+=100000) LS_EQ_UINT(0,cap_frame(&h,t));
    test_first_blocked=false;
    LS_EQ_UINT(1,cap_frame(&h,MAP_LABEL_HOLD_US+100000));
    /* Blocked past its retry: it is no longer held, so the cap goes on. */
    test_first_blocked=true;
    LS_EQ_UINT(0,cap_frame(&h,MAP_LABEL_HOLD_US+200000));
    LS_EQ_UINT(2,cap_frame(&h,2*MAP_LABEL_HOLD_US+200000));
    LS_EQ_UINT(2,cap_frame(&h,2*MAP_LABEL_HOLD_US+300000));
    /* A name that never had a slot is not held. */
    map_label_history fresh={0};
    LS_EQ_UINT(2,cap_frame(&fresh,0));
    LS_CHECK(!map_label_held(&fresh,1));
}
