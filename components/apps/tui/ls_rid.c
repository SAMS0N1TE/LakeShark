/* Independently written from public ASTM F3411 byte-layout tables.
 * Sources and receiver limitations: bench/REMOTE_ID.md. */
#include "ls_rid.h"
#include "ls_ble_ad.h"
#include "esp_attr.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "esp_timer.h"
static StaticSemaphore_t lock_memory;
static SemaphoreHandle_t mutex;
static unsigned once;
static void lock(void) {
    unsigned expected=0;
    if (__atomic_compare_exchange_n(&once,&expected,1,false,__ATOMIC_ACQUIRE,__ATOMIC_RELAXED)) {
        mutex=xSemaphoreCreateMutexStatic(&lock_memory);
        __atomic_store_n(&once,2,__ATOMIC_RELEASE);
    } else while (__atomic_load_n(&once,__ATOMIC_ACQUIRE)!=2) vTaskDelay(1);
    xSemaphoreTake(mutex,portMAX_DELAY);
}
static void unlock(void) { xSemaphoreGive(mutex); }

static EXT_RAM_BSS_ATTR ls_rid_drone_t drones[LS_RID_MAX];
static uint32_t generation;
static uint16_t u16(const uint8_t *p) { return (uint16_t)p[0]|((uint16_t)p[1]<<8); }
static int32_t i32(const uint8_t *p) {
    uint32_t u=(uint32_t)p[0]|((uint32_t)p[1]<<8)|((uint32_t)p[2]<<16)|((uint32_t)p[3]<<24);
    return u<=INT32_MAX ? (int32_t)u : (int32_t)((int64_t)u-4294967296LL);
}
static float altitude(const uint8_t *p) { unsigned u=u16(p); return u ? u*.5f-1000 : NAN; }
static void text(char *out,const uint8_t *p,size_t n) {
    size_t i; for(i=0;i<n && p[i];i++) out[i]=p[i]>=32 && p[i]<=126 ? (char)p[i] : '?';
    out[i]=0;
}
static bool position(double lat,double lon) {
    return lat>=-90 && lat<=90 && lon>=-180 && lon<=180 && (lat!=0 || lon!=0);
}
static void expire(int64_t now) {
    for(unsigned i=0;i<LS_RID_MAX;i++) if(drones[i].serial && now-drones[i].last_us>=60000000LL)
        memset(&drones[i],0,sizeof(drones[i]));
}
static bool valid(const uint8_t *p,size_t n) {
    if(n<1 || (p[0]&15)>2) return false;
    unsigned type=p[0]>>4;
    if(type==15) {
        if(n<3 || p[1]!=25 || !p[2] || p[2]>9 || n!=3+(size_t)p[2]*25) return false;
        for(unsigned i=0;i<p[2];i++) if((p[3+i*25]>>4)>5 || !valid(p+3+i*25,25)) return false;
        return true;
    }
    if(n!=25 || type>5) return false;
    if(type==0 && ((p[1]>>4)==0 || (p[1]>>4)>4)) return false;
    return true;
}
static ls_rid_drone_t *entry(const uint8_t *mac,uint8_t at,const uint8_t *basic,int64_t now) {
    ls_rid_drone_t *empty=NULL,*old=&drones[0],*active=NULL,*pending=NULL;
    for(unsigned i=0;i<LS_RID_MAX;i++) {
        ls_rid_drone_t *d=&drones[i];
        if(!d->serial) { if(!empty) empty=d; continue; }
        if(d->last_us<old->last_us) old=d;
        if(d->address_type!=at || memcmp(d->mac,mac,6)) continue;
        if(basic && d->id_type==(basic[1]>>4) && !memcmp(d->uas_raw,basic+2,20)) return d;
        if(!d->id_type) pending=d;
        if(!active || d->basic_us>active->basic_us) active=d;
    }
    if(!basic && active) return active;
    ls_rid_drone_t *d=basic && pending ? pending : empty ? empty : old;
    if(!(basic && pending)) {
        memset(d,0,sizeof(*d)); memcpy(d->mac,mac,6); d->address_type=at;
        d->serial=++generation; if(!d->serial) d->serial=++generation;
        d->last_us=now;
        d->geo_m=d->baro_m=d->height_m=d->speed_mps=d->vertical_mps=d->track_deg=NAN;
        d->area_ceiling_m=d->area_floor_m=d->operator_geo_m=NAN;
    }
    return d;
}
static void decode(ls_rid_drone_t *d,const uint8_t *p,int64_t now) {
    unsigned t=p[0]>>4; d->messages|=(uint16_t)(1u<<t); d->version=p[0]&15;
    switch(t) {
    case 0:
        d->id_type=p[1]>>4; d->ua_type=p[1]&15; memcpy(d->uas_raw,p+2,20); d->basic_us=now;
        if(d->id_type>=3) { for(unsigned i=0;i<20;i++) snprintf(d->uas_id+i*2,3,"%02X",p[i+2]); }
        else text(d->uas_id,p+2,20);
        break;
    case 1:
        d->status=p[1]>>4; d->height_agl=(p[1]>>2)&1;
        d->track_deg=p[2]<180 ? p[2]+((p[1]&2)?180:0) : NAN;
        d->speed_mps=p[3]==255 && (p[1]&1) ? NAN : (p[1]&1) ? 63.75f+p[3]*.75f : p[3]*.25f;
        /* 63 m/s is the standard's unavailable value (encoded 126).
           -64 is outside the signed field's specified physical range. */
        d->vertical_mps=p[4]==126 || p[4]==128 ? NAN : (p[4]<128 ? p[4] : (int)p[4]-256)*.5f;
        d->lat=i32(p+5)*1e-7; d->lon=i32(p+9)*1e-7; d->position_valid=position(d->lat,d->lon);
        d->baro_m=altitude(p+13); d->geo_m=altitude(p+15); d->height_m=altitude(p+17); d->location_us=now;
        break;
    case 2: d->auth_type=p[1]>>4; d->auth_page=p[1]&15; break;
    case 3: d->description_type=p[1]; text(d->self_id,p+2,23); break;
    case 4:
        d->operator_location_type=p[1]&3; d->classification_type=(p[1]>>2)&7;
        d->operator_lat=i32(p+2)*1e-7; d->operator_lon=i32(p+6)*1e-7;
        d->operator_valid=position(d->operator_lat,d->operator_lon);
        d->area_count=u16(p+10); d->area_radius_m=p[12]*10;
        d->area_ceiling_m=altitude(p+13); d->area_floor_m=altitude(p+15);
        if(d->version) { d->category=p[17]>>4; d->class_id=p[17]&15; d->operator_geo_m=altitude(p+18); }
        break;
    case 5: d->operator_id_type=p[1]; text(d->operator_id,p+2,20); break;
    }
}
static void receive(const uint8_t *mac,uint8_t at,int rssi,uint8_t counter,const uint8_t *p,size_t n,int64_t now) {
    (void)n;
    const uint8_t *basic=NULL;
    unsigned count=1; const uint8_t *first=p;
    if((p[0]>>4)==15) { count=p[2]; first=p+3; }
    for(unsigned i=0;i<count;i++) if((first[i*25]>>4)==0) { basic=first+i*25; break; }
    ls_rid_drone_t *d=entry(mac,at,basic,now);
    /* Packs describe one aircraft; select its first Basic ID as identity.
       Other Basic IDs are alternate identifiers, not extra aircraft. */
    for(unsigned i=0;i<count;i++) {
        const uint8_t *m=first+i*25;
        if((m[0]>>4)!=0 || m==basic) decode(d,m,now);
    }
    d->rssi=(int8_t)rssi; d->last_us=now; d->counter=counter;
}
bool ls_rid_is_advert(const uint8_t *data,size_t len) {
    if(!data) return false;
    for(size_t off=0;off<len && data[off];) {
        size_t n=data[off];
        if(len-off>=5 && n>=4 && data[off+1]==0x16 && data[off+2]==0xfa &&
           data[off+3]==0xff && data[off+4]==0x0d) return true;
        if(n>len-off-1) return false;
        off+=n+1;
    }
    return false;
}
bool ls_rid_advert(const uint8_t mac[6],uint8_t at,int rssi,const uint8_t *data,size_t len,int64_t now) {
    if(!mac || !data || now<0) return false;
    bool heard=false;
    /* Validate AD framing before changing state. */
    if(!ls_ble_ad_valid(data,len)) return false;
    size_t off=0; ls_ble_ad_field_t field;
    lock(); expire(now);
    while(ls_ble_ad_next(data,len,&off,&field)>0) {
        size_t n=field.len; const uint8_t *p=field.data;
        if(n>=4 && field.type==0x16 && p[0]==0xfa && p[1]==0xff && p[2]==0x0d && valid(p+4,n-4)) {
            receive(mac,at,rssi,p[3],p+4,n-4,now); heard=true;
        }
    }
    unlock(); return heard;
}
size_t ls_rid_snapshot(ls_rid_drone_t *out,size_t cap,int64_t now) {
    size_t n=0; lock(); expire(now);
    if(out) for(unsigned i=0;i<LS_RID_MAX && n<cap;i++) if(drones[i].serial) out[n++]=drones[i];
    unlock(); return n;
}
void ls_rid_summary(ls_rid_summary_t *out,bool gps,double lat,double lon,int64_t now) {
    memset(out,0,sizeof(*out));out->nearest_m=-1;
    double nearest=INFINITY;
    gps=gps && isfinite(lat) && isfinite(lon) && lat>=-90 && lat<=90 && lon>=-180 && lon<=180;
    lock();expire(now);
    for(unsigned i=0;i<LS_RID_MAX;i++) {
        const ls_rid_drone_t *d=&drones[i];if(!d->serial) continue;
        out->count++;
        if(!gps || !d->position_valid || now<d->location_us || now-d->location_us>=60000000LL) continue;
        const double rad=0.017453292519943295;
        double a=sin((d->lat-lat)*rad/2),b=sin((d->lon-lon)*rad/2);
        double h=a*a+cos(lat*rad)*cos(d->lat*rad)*b*b;
        double range=12742000.0*asin(sqrt(fmin(1.0,fmax(0.0,h))));
        if(isfinite(range) && range<nearest) {
            nearest=range;snprintf(out->id,sizeof(out->id),"%.20s",d->uas_id);
        }
    }
    if(isfinite(nearest) && nearest<2147483646.0) out->nearest_m=(int)lround(nearest);
    unlock();
}
void ls_rid_clear(void) { lock(); memset(drones,0,sizeof(drones)); unlock(); }
uint32_t ls_rid_generation(void) { lock(); uint32_t g=generation; unlock(); return g; }
int ls_rid_command(int argc,char **argv) {
    static EXT_RAM_BSS_ATTR ls_rid_drone_t view[LS_RID_MAX];
    if(argc==2 && !strcmp(argv[1],"clear")) { ls_rid_clear(); puts("Remote ID table cleared"); return 0; }
    size_t n=ls_rid_snapshot(view,LS_RID_MAX,esp_timer_get_time());
    bool detail=argc==3 && !strcmp(argv[1],"detail");
    char *end=NULL; long pick=detail ? strtol(argv[2],&end,10) : 0;
    if((argc!=1 && !detail) || (detail && (!argv[2][0] || *end || pick<1 || pick>(long)n))) {
        puts("rid | rid detail N (1-based) | rid clear"); return 1;
    }
    for(size_t i=0;i<n;i++) {
        if(detail && (long)i+1!=pick) continue;
        ls_rid_drone_t *d=&view[i];
        printf("%u %s %02X:%02X:%02X:%02X:%02X:%02X RSSI %d age %.1fs status %u\n",(unsigned)i+1,
            d->uas_id[0]?d->uas_id:"pending ID",d->mac[5],d->mac[4],d->mac[3],d->mac[2],d->mac[1],d->mac[0],
            d->rssi,(esp_timer_get_time()-d->last_us)/1e6,d->status);
        if(detail) {
            printf("position %s %.7f %.7f; geo %.1f baro %.1f height %.1f m (%s)\n",d->position_valid?"valid":"unknown",d->lat,d->lon,d->geo_m,d->baro_m,d->height_m,d->height_agl?"AGL":"takeoff");
            printf("speed %.2f vertical %.2f m/s track %.1f; location age %.1fs\n",d->speed_mps,d->vertical_mps,d->track_deg,d->location_us?(esp_timer_get_time()-d->location_us)/1e6:NAN);
            printf("operator %s %.7f %.7f type %u altitude %.1f; ID %s\n",d->operator_valid?"valid":"unknown",d->operator_lat,d->operator_lon,d->operator_location_type,d->operator_geo_m,d->operator_id);
            printf("area %u radius %u m floor %.1f ceiling %.1f; class type %u category %u class %u\n",d->area_count,d->area_radius_m,d->area_floor_m,d->area_ceiling_m,d->classification_type,d->category,d->class_id);
            printf("self %s; authentication %s type %u page %u (unverified); version %u counter %u messages %04x\n",d->self_id,(d->messages&4)?"seen":"absent",d->auth_type,d->auth_page,d->version,d->counter,d->messages);
        }
    }
    if(!n) puts("No Remote ID broadcasts in the last 60 seconds");
    return 0;
}
