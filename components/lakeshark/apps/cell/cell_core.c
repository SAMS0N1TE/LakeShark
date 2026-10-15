#include "cell_core.h"
#include <stddef.h>
#include <string.h>
#include <math.h>
/* Positions 0-4 are the first five presets and must not move: a saved
 * baseline names its band by index. */
const cell_band_t cell_bands[] = {
    {"850 DL / B5",869000000,894000000,5,2400},
    {"900 DL / B8",925000000,960000000,8,3450},
    {"700 DL / B12",729000000,746000000,12,5010},
    {"700 DL / B13",746000000,756000000,13,5180},
    {"800 DL / B20",791000000,821000000,20,6150},
    {"700 DL / B14",758000000,768000000,14,5280},
    {"700 DL / B17",734000000,746000000,17,5730},
    {"850 DL / B26",859000000,894000000,26,8690},
    {"600 DL / B71",617000000,652000000,71,68586},
    /* Above the RTL-SDR tuner: HackRF only. */
    {"1900 DL / B2",1930000000,1990000000,2,600},
    {"AWS DL / B4",2110000000,2155000000,4,1950},
    {"1900 DL / B25",1930000000,1995000000,25,8040},
    {"AWS DL / B66",2110000000,2200000000,66,66436},
};
const unsigned cell_band_count = 9;
const unsigned cell_band_total = sizeof(cell_bands)/sizeof(cell_bands[0]);
bool cell_earfcn_to_hz(unsigned band,unsigned earfcn,uint32_t *hz)
{
    if(band>=cell_band_total || earfcn<cell_bands[band].offs) return false;
    uint32_t f=cell_bands[band].low+(uint32_t)(earfcn-cell_bands[band].offs)*CELL_RASTER_HZ;
    if(f>=cell_bands[band].high) return false;
    if(hz)*hz=f;
    return true;
}
bool cell_hz_to_earfcn(unsigned band,uint32_t hz,unsigned *earfcn)
{
    if(band>=cell_band_total || hz<cell_bands[band].low || hz>=cell_bands[band].high ||
       (hz-cell_bands[band].low)%CELL_RASTER_HZ) return false;
    if(earfcn)*earfcn=cell_bands[band].offs+(hz-cell_bands[band].low)/CELL_RASTER_HZ;
    return true;
}
void cell_band_earfcns(unsigned band,unsigned *first,unsigned *last)
{
    if(band>=cell_band_total){if(first)*first=0;if(last)*last=0;return;}
    if(first)*first=cell_bands[band].offs;
    if(last)*last=cell_bands[band].offs+(cell_bands[band].high-cell_bands[band].low)/CELL_RASTER_HZ-1;
}
int cell_band_for_hz(uint32_t hz)
{
    for(unsigned i=0;i<cell_band_total;i++) if(hz>=cell_bands[i].low && hz<cell_bands[i].high) return (int)i;
    return -1;
}
bool cell_gps_valid(bool fix,int64_t timestamp,int64_t now,double lat,double lon,float hdop)
{
    return fix && timestamp>0 && now>=timestamp && now-timestamp<5000000 &&
        isfinite(lat) && isfinite(lon) && fabs(lat)<=90 && fabs(lon)<=180 &&
        isfinite(hdop) && hdop>0 && hdop<=5;
}
bool cell_coverage_complete(unsigned planned,unsigned attempted,unsigned usable,unsigned discarded)
{
    return planned && attempted==planned && usable==planned && !discarded;
}
uint32_t cell_checksum(const cell_baseline_t *b)
{
    const uint8_t *p=(const uint8_t *)b;
    uint32_t h=2166136261u;
    for(unsigned i=0;i<offsetof(cell_baseline_t,checksum);i++) h=(h^p[i])*16777619u;
    return h;
}
bool cell_baseline_valid(const cell_baseline_t *b,unsigned band,int lat,int lon,bool located,bool antenna)
{
    if(band>=cell_band_count || b->magic!=0x43454c4c || b->version!=CELL_BASELINE_VERSION ||
       b->receiver!=CELL_RECEIVER_RTL || b->calibration!=CELL_CALIBRATION ||
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
void cell_motion_reset(cell_motion_t *m)
{
    m->seeded=false;
}
bool cell_motion_step(cell_motion_t *m,float ax,float ay,float az,float gx,float gy,float gz)
{
    const float g[3]={gx,gy,gz};
    float a=ax*ax+ay*ay+az*az, raw=gx*gx+gy*gy+gz*gz;
    if(!isfinite(a) || !isfinite(raw)) return false;
    if(!m->seeded) {for(int i=0;i<3;i++) m->rest[i]=g[i]; m->seeded=true;}
    float d=0;
    for(int i=0;i<3;i++) {float e=g[i]-m->rest[i]; d+=e*e; m->rest[i]+=e*.05f;}
    /* 5 deg/s off the resting reading, and never more than 60 deg/s in all. */
    return a>.85f && a<1.15f && d<25.f && raw<3600.f;
}

void cell_ppm_reset(cell_ppm_t *p){memset(p,0,sizeof(*p));}
bool cell_ppm_add(cell_ppm_t *p,uint32_t hz,int pci,int cfo,float applied)
{
    if(!p || !hz) return false;
    float v=applied-(float)cfo*1e6f/(float)hz;
    if(!isfinite(v) || fabsf(v)>150.f) return false;
    unsigned i=0;
    for(;i<p->n;i++) if(p->hz[i]==hz && p->pci[i]==pci) break;
    if(i==p->n) {
        if(p->n<CELL_PPM_MAX) p->n++;
        else {i=p->next;p->next=(p->next+1)%CELL_PPM_MAX;}
    }
    p->hz[i]=hz;p->pci[i]=(int16_t)pci;p->ppm[i]=v;
    return true;
}
bool cell_ppm_estimate(const cell_ppm_t *p,cell_ppm_est_t *out)
{
    if(!p || !out || !p->n) return false;
    float v[CELL_PPM_MAX];
    for(unsigned i=0;i<p->n;i++){float x=p->ppm[i];unsigned j=i;while(j && v[j-1]>x){v[j]=v[j-1];j--;}v[j]=x;}
    float med=p->n&1?v[p->n/2]:(v[p->n/2-1]+v[p->n/2])*.5f;
    float lo=1e9f,hi=-1e9f;unsigned agree=0;
    for(unsigned i=0;i<p->n;i++) if(fabsf(v[i]-med)<=CELL_PPM_AGREE) {
        agree++;if(v[i]<lo)lo=v[i];if(v[i]>hi)hi=v[i];
    }
    *out=(cell_ppm_est_t){med,agree?hi-lo:0,p->n,agree};
    return true;
}
void cell_cfo_window(bool known,float est,float applied,uint32_t hz,int *centre,int *span)
{
    int c=0,s=40000;
    if(known && hz) {
        float expect=-(est-applied)*(float)hz/1e6f;
        c=(int)lroundf(expect/CELL_CFO_STEP)*CELL_CFO_STEP;
        if(c>35000)c=35000;
        if(c<-35000)c=-35000;
        s=CELL_CFO_STEP;
    }
    if(centre)*centre=c;
    if(span)*span=s;
}

/* The 25 kHz bin at i, smoothed over five bins (125 kHz) so one noisy bin is
 * not a carrier edge. Missing bins do not count. */
static int smooth(const int8_t *db,unsigned n,unsigned i)
{
    int sum=0,count=0;
    for(int d=-2;d<=2;d++) {
        int k=(int)i+d;
        if(k<0 || k>=(int)n || db[k]==CELL_MISSING) continue;
        sum+=db[k];count++;
    }
    return count?(sum-(sum<0?count/2:-count/2))/count:CELL_MISSING;
}
unsigned cell_plateaus(const int8_t *db,unsigned n,uint32_t start,uint32_t bin,
                       cell_plateau_t *out,unsigned max,int *floor_db)
{
    if(floor_db)*floor_db=CELL_MISSING;
    if(!db || !out || !max || !bin || n<8) return 0;
    /* The floor is the fifth percentile of the pass, not its minimum or its
     * median: a band that is one 10 MHz carrier is still 10% guard band. */
    uint16_t hist[129]={0};unsigned valid=0;
    for(unsigned i=0;i<n;i++) {
        int v=smooth(db,n,i);if(v==CELL_MISSING)continue;
        if(v>0)v=0;
        if(v<-128)v=-128;
        hist[-v]++;valid++;
    }
    if(valid<8) return 0;
    unsigned target=valid/20,seen=0;int fl=0;
    for(int k=128;k>=0;k--) {seen+=hist[k];if(seen>target){fl=-k;break;}}
    if(floor_db)*floor_db=fl;
    const int thr=fl+CELL_GATE_DB,min_level=fl+CELL_GATE_DB+3;
    const uint32_t min_width=800000;
    unsigned count=0;
    for(unsigned i=0;i<n;) {
        if(smooth(db,n,i)<thr){i++;continue;}
        unsigned first=i,last=i,gap=0;
        for(i++;i<n;i++) {
            if(smooth(db,n,i)>=thr){last=i;gap=0;}
            else if(++gap>3)break;
        }
        unsigned w=last-first+1;
        if((uint32_t)w*bin<min_width) continue;
        long sum=0;unsigned c=0;
        for(unsigned k=first+w/4;k<=last-w/4;k++){sum+=smooth(db,n,k);c++;}
        int level=c?(int)(sum/(long)c):thr;
        if(level<min_level) continue;
        unsigned lo=first,hi=last;
        for(unsigned k=first;k<=last;k++) if(smooth(db,n,k)>=level-4){lo=k;break;}
        for(unsigned k=last+1;k-->first;) if(smooth(db,n,k)>=level-4){hi=k;break;}
        cell_plateau_t pl={start+(uint32_t)lo*bin,start+(uint32_t)(hi+1)*bin,(int8_t)level};
        if(pl.hi-pl.lo<min_width) continue;
        /* Smoothing and the -4 dB edge sit about 40 kHz inside a carrier's own
         * edge on each side; put them back. */
        pl.lo=pl.lo>start+40000?pl.lo-40000:start;
        pl.hi+=40000;
        unsigned slot=count;
        if(count<max) count++;
        else {
            slot=0;
            for(unsigned k=1;k<max;k++) if(out[k].level<out[slot].level) slot=k;
            if(out[slot].level>=pl.level) continue;
        }
        out[slot]=pl;
    }
    for(unsigned a=1;a<count;a++) {cell_plateau_t t=out[a];unsigned b=a;while(b && out[b-1].lo>t.lo){out[b]=out[b-1];b--;}out[b]=t;}
    return count;
}
static uint32_t raster_snap(uint32_t x,uint32_t first,uint32_t last)
{
    if(x<=first) return first;
    if(x>=last) return last;
    return first+((x-first+CELL_RASTER_HZ/2)/CELL_RASTER_HZ)*CELL_RASTER_HZ;
}
static unsigned add_point(uint32_t *out,unsigned n,unsigned max,uint32_t hz)
{
    for(unsigned i=0;i<n;i++) if(out[i]==hz) return n;
    if(n<max) out[n++]=hz;
    return n;
}
/* The raster point nearest an estimated centre. The true centre is on the
 * raster, so an estimate more than 30 kHz from the nearest point may belong
 * to its neighbour: take that one too. */
static unsigned add_near(uint32_t *out,unsigned n,unsigned max,uint32_t x,uint32_t first,uint32_t last)
{
    uint32_t centre=raster_snap(x,first,last);
    n=add_point(out,n,max,centre);
    int32_t e=(int32_t)x-(int32_t)centre;
    if(e>30000 && centre+CELL_RASTER_HZ<=last) n=add_point(out,n,max,centre+CELL_RASTER_HZ);
    if(e<-30000 && centre>=first+CELL_RASTER_HZ) n=add_point(out,n,max,centre-CELL_RASTER_HZ);
    return n;
}
unsigned cell_candidates(const cell_plateau_t *p,unsigned np,uint32_t first,uint32_t last,
                         uint32_t *out,unsigned max)
{
    static const uint32_t occupied[]={1080000,2700000,4500000,9000000,13500000,18000000};
    static const uint32_t half[]={1350000,2250000,4500000,6750000,9000000};
    if(!p || !np || !out || !max || last<first) return 0;
    unsigned order[CELL_PLATEAU_MAX],n=0;
    if(np>CELL_PLATEAU_MAX) np=CELL_PLATEAU_MAX;
    for(unsigned i=0;i<np;i++) {unsigned j=n++;while(j && p[order[j-1]].level<p[i].level){order[j]=order[j-1];j--;}order[j]=i;}
    unsigned count=0;
    for(unsigned o=0;o<np && count<max;o++) {
        const cell_plateau_t *pl=&p[order[o]];
        uint32_t width=pl->hi-pl->lo,mid=pl->lo+width/2;
        bool fit=false;
        for(unsigned k=0;k<6;k++) {
            uint32_t tol=occupied[k]/16;
            if(tol<250000)tol=250000;
            uint32_t d=width>occupied[k]?width-occupied[k]:occupied[k]-width;
            if(d<=tol) fit=true;
        }
        count=add_near(out,count,max,mid,first,last);
        if(!fit) {
            /* Merged or odd-sized: a carrier's edge sits half its occupied
             * width from its centre. */
            for(unsigned k=0;k<5;k++) {
                if(width+400000<2*half[k]) continue;
                count=add_near(out,count,max,pl->lo+half[k],first,last);
                count=add_near(out,count,max,pl->hi-half[k],first,last);
            }
        }
    }
    for(unsigned a=1;a<count;a++) {uint32_t t=out[a];unsigned b=a;while(b && out[b-1]>t){out[b]=out[b-1];b--;}out[b]=t;}
    return count;
}
