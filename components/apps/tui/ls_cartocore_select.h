/* Shared SD selection and geographic fit; no flash/NVS or renderer buffers. */
#ifndef LS_CARTOCORE_SELECT_H
#define LS_CARTOCORE_SELECT_H
#include "cartocore/ctile.h"
#include <stdbool.h>
#include <dirent.h>
#include <sys/stat.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <math.h>
static double carto_bbox_degrees(const uint8_t *p) {
    uint32_t v=(uint32_t)p[0]|(uint32_t)p[1]<<8|(uint32_t)p[2]<<16|(uint32_t)p[3]<<24;
    int64_t n=v; if(v&UINT32_C(0x80000000)) n-=INT64_C(0x100000000);
    return n/1e7;
}
static bool carto_bounds(const uint8_t *h,double *w,double *s,double *e,double *n) {
    *w=carto_bbox_degrees(h+12);*s=carto_bbox_degrees(h+16);*e=carto_bbox_degrees(h+20);*n=carto_bbox_degrees(h+24);
    return *w<=*e && *s<=*n && *w>=-180 && *e<=180 && *s>=-85.05112878 && *n<=85.05112878;
}
static bool carto_choose_file(const char *dir,double lat,double lon,char *out,size_t cap) {
    DIR *d=opendir(dir);if(!d) return false;
    bool found=false,best_covers=false;uint64_t best_size=0;struct dirent *ent;
    while((ent=readdir(d))) {
        size_t len=strlen(ent->d_name); if(len<7 || strcasecmp(ent->d_name+len-6,".ctile")) continue;
        char path[256];int count=snprintf(path,sizeof(path),"%s/%s",dir,ent->d_name);
        if(count<0 || (size_t)count>=sizeof(path) || (size_t)count>=cap) continue;
        struct stat st;if(stat(path,&st) || !S_ISREG(st.st_mode) || st.st_size<64) continue;
        uint8_t h[64];FILE *f=fopen(path,"rb");size_t got=f?fread(h,1,sizeof(h),f):0;if(f) fclose(f);
        if(!cc_ctile_header_check(h,got,st.st_size)) continue;
        double w,s,e,n;if(!carto_bounds(h,&w,&s,&e,&n)) continue;
        bool covers=lon>=w && lon<=e && lat>=s && lat<=n;
        uint64_t bytes=st.st_size;
        if(!found || covers>best_covers || (covers==best_covers &&
            (bytes>best_size || (bytes==best_size && strcmp(path,out)<0)))) {
            snprintf(out,cap,"%s",path);best_size=bytes;best_covers=covers;found=true;
        }
    }
    closedir(d);return found;
}
static bool carto_fit_map(const cc_ctile *map,double *lat,double *lon,unsigned *z,bool recenter) {
    const uint8_t *h=map->source?map->source->header:map->bytes.data;
    double w,s,e,n;if(!h || !carto_bounds(h,&w,&s,&e,&n)) return false;
    if(recenter && (!isfinite(*lat) || !isfinite(*lon) || *lon<w || *lon>e || *lat<s || *lat>n)) {
        *lat=(s+n)/2;*lon=(w+e)/2;
        printf("carto MAP: recentred %.7f,%.7f bbox=%.7f,%.7f..%.7f,%.7f\n",*lat,*lon,w,s,e,n);
    }
    if(*z<map->zmin) *z=map->zmin;
    unsigned hi=(map->source?map->source->header[8]:map->bytes.data[8])==7?16:map->zmax;
    if(*z>hi) *z=hi;
    return true;
}
#endif
