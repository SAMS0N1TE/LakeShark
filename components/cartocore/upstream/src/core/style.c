#include "cartocore/core.h"
typedef struct { const char *layer, *kind; uint8_t fill, line, priority; } rule;
/* Ordered specific-to-general. Empty kind is the layer fallback. */
static const rule rules[] = {
    {"water","",CC_WATER,0,3}, {"waterway","",0,CC_WATER,4},
    {"earth","",CC_LAND,0,1},
    {"land","park",CC_PARK,0,2}, {"land","farmland",CC_PARK,0,2},
    {"land","playground",CC_PARK,0,2},
    {"land","forest",CC_WOOD,0,2}, {"land","wood",CC_WOOD,0,2},
    {"land","grass",CC_PARK,0,2}, {"land","meadow",CC_PARK,0,2},
    {"land","recreation_ground",CC_PARK,0,2}, {"land","cemetery",CC_PARK,0,2},
    {"land","nature_reserve",CC_PARK,0,2}, {"land","garden",CC_PARK,0,2}, {"land","",CC_LAND,0,1},
    {"landcover","",CC_PARK,0,2}, {"landuse","",CC_PARK,0,2},
    {"park","",CC_PARK,0,2}, {"building","",CC_BUILDING,0,5},
    {"buildings","",CC_BUILDING,0,5},
    {"roads","rail",0,CC_RAIL,8}, {"roads","railway",0,CC_RAIL,8},
    {"roads","highway",0,CC_MAJOR,10}, {"roads","major_road",0,CC_MAJOR,10},
    {"roads","motorway",0,CC_MOTORWAY,10}, {"roads","trunk",0,CC_MOTORWAY,10},
    {"roads","primary",0,CC_MAJOR,10}, {"roads","secondary",0,CC_SECONDARY,10},
    {"roads","",0,CC_MINOR,7},
    {"transportation","rail",0,CC_RAIL,8},
    {"transportation","motorway",0,CC_MOTORWAY,10},
    {"transportation","trunk",0,CC_MOTORWAY,10},
    {"transportation","primary",0,CC_MAJOR,10},
    {"transportation","secondary",0,CC_SECONDARY,10},
    {"transportation","",0,CC_MINOR,7},
    {"streets","rail",0,CC_RAIL,8},
    {"streets","motorway",0,CC_MOTORWAY,10}, {"streets","trunk",0,CC_MOTORWAY,10},
    {"streets","primary",0,CC_MAJOR,10}, {"streets","secondary",0,CC_SECONDARY,10},
    {"streets","",0,CC_MINOR,7},
    {"street_polygons","",CC_MINOR,0,7}, {"bridges","",CC_MINOR,CC_MINOR,9},
    {"water_lines","",0,CC_WATER,4}, {"water_polygons","",CC_WATER,0,3},
    {"boundaries","",0,CC_BOUNDARY,6}, {"boundary","",0,CC_BOUNDARY,6},
    {"rail","",0,CC_RAIL,8}, {"railway","",0,CC_RAIL,8}
};
cc_style cc_style_lookup(cc_str layer, cc_str kind) {
    cc_style s = {0,0,0};
    for (size_t i = 0; i < sizeof(rules)/sizeof(rules[0]); ++i)
        if (cc_str_eq(layer,rules[i].layer) &&
            (!rules[i].kind[0] || cc_str_eq(kind,rules[i].kind))) {
            s.fill = rules[i].fill; s.line = rules[i].line; s.priority = rules[i].priority; break;
        }
    return s;
}
uint32_t cc_palette(uint8_t ink) {
    static const uint32_t palette[CC_INK_COUNT] = {
        0x000000,0x151c22,0x203b2c,0x183f62,0x414750,
        0x89969f,0xefc477,0x936c9f,0xba8880,0x244a32,0x303a25,0x252b30,0x343039,0xffdf96,0xcbb98c,0x626d73,0x3b4b40,0x526653
    };
    return ink < CC_INK_COUNT ? palette[ink] : 0;
}

uint8_t cc_palette16(uint8_t ink) {
    static const uint8_t indices[CC_INK_COUNT]={0,0,2,12,8,15,3,6,5,2,2,0,8,11,3,7,8,2};
    return ink<CC_INK_COUNT?indices[ink]:0;
}
uint32_t cc_style_colour(uint8_t ink,int colors16) {
    return cc_palette(ink)|(colors16?((uint32_t)(0x80|cc_palette16(ink))<<24):0);
}
