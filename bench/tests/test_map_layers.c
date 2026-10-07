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
