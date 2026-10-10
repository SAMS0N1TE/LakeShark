#ifndef LS_SWEEP_SCOPE_H
#define LS_SWEEP_SCOPE_H
#include "ls_sweep_app.h"
#include <stdio.h>
/* Scope fade only; listed contacts use the independent five-minute cache. */
#ifndef LS_SWEEP_SCOPE_US
#define LS_SWEEP_SCOPE_US INT64_C(45000000)
#endif
static inline int ls_sweep_scope_level(int64_t age) {
    if(age<0) age=0;
    return age>=LS_SWEEP_SCOPE_US?4:(int)(age*4/LS_SWEEP_SCOPE_US);
}
static inline double ls_sweep_scope_radius(int rssi) {
    double r=(-rssi-30)/60.0;
    return r<.10?.10:r>.96?.96:r;
}
static inline double ls_sweep_scope_degrees(const ls_sweep_device_t *d) {
    /* Clockwise: TRACKER, CAMERA, BODYCAM, DRONE, ATTACK, OTHER.
     * OTHER is reserved: unmatched broadcasts are not fabricated contacts. */
    static const unsigned sector[]={1,2,3,0,4,5};
    uint32_t hash=2166136261u;
    for(unsigned i=0;i<6;i++) hash=(hash^d->mac[i])*16777619u;
    hash=(hash^d->radio)*16777619u;
    hash=(hash^d->address_type)*16777619u;
    return sector[d->match.category<SW_CATS?d->match.category:SW_CATS]*60+8+(hash%44001)/1000.0;
}
static inline void ls_sweep_track_id(const ls_sweep_device_t *d,char *out,size_t n) {
    /* Serial is assigned once per retained contact, never its snapshot rank. */
    snprintf(out,n,"%c%02lu",d->match.category<SW_CATS?"CBDTA"[d->match.category]:'O',(unsigned long)d->serial);
}
#endif
