#include "cell_core.h"
#include <stddef.h>
#include <math.h>
const cell_band_t cell_bands[] = {
    {"850 DL / B5",869000000,894000000},
    {"900 DL / B8",925000000,960000000},
    {"700 DL / B12",729000000,746000000},
    {"700 DL / B13",746000000,756000000},
    {"800 DL / B20",791000000,821000000},
};
const unsigned cell_band_count = sizeof(cell_bands)/sizeof(cell_bands[0]);
uint32_t cell_checksum(const cell_baseline_t *b)
{
    const uint8_t *p=(const uint8_t *)b;
    uint32_t h=2166136261u;
    for(unsigned i=0;i<offsetof(cell_baseline_t,checksum);i++) h=(h^p[i])*16777619u;
    return h;
}
bool cell_baseline_valid(const cell_baseline_t *b,unsigned band,int lat,int lon,bool located,bool antenna)
{
    if(band>=cell_band_count || b->magic!=0x43454c4c || b->version!=1 ||
       b->band!=band || b->rate!=CELL_RATE || b->gain!=CELL_GAIN || b->passes!=3 ||
       b->count!=(cell_bands[band].high-cell_bands[band].low)/CELL_BIN ||
       b->count>CELL_MAX_BINS || b->located!=located || b->antenna!=antenna ||
       b->lat_tile!=lat || b->lon_tile!=lon || b->checksum!=cell_checksum(b)) return false;
    for(unsigned i=0;i<b->count;i++) if(b->power[i]==CELL_MISSING) return false;
    return true;
}
unsigned cell_compare(const int8_t *base,const int8_t *now,uint8_t *streak,unsigned n,bool comparable)
{
    unsigned changed=0;
    for(unsigned i=0;i<n;i++) {
        bool rise=comparable && base[i]!=CELL_MISSING && now[i]!=CELL_MISSING &&
                  now[i]>=-65 && (int)now[i]-(int)base[i]>=12;
        if(!rise) streak[i]=0;
        else if(streak[i]<3) streak[i]++;
        if(streak[i]>=3) changed++;
    }
    return changed;
}
bool cell_motion_quiet(float ax,float ay,float az,float gx,float gy,float gz)
{
    float a=ax*ax+ay*ay+az*az, g=gx*gx+gy*gy+gz*gz;
    return isfinite(a) && isfinite(g) && a>.85f && a<1.15f && g<25.f;
}
