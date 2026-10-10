#include "ls_test.h"
#include "../../components/apps/tui/screens/ext/scr_map.c"

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
void ls_map_get_center(double *lat,double *lon) { *lat=43.4445;*lon=-71.6473; }
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
