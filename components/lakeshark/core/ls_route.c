#include "ls_route.h"
#include "ls_track.h"
#include "ls_rlog.h"
#include <ctype.h>
#include <float.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#define RAD 0.017453292519943295
#define EARTH 6371008.8
#define XML_DEPTH 16
#define XML_NAME 48
#define XML_TAG 384

static bool coordinate(double lat, double lon)
{
    return isfinite(lat) && isfinite(lon) && fabs(lat) <= 90 && fabs(lon) <= 180;
}
static double angle(double a, double b, double c, double d)
{
    double x = sin((c-a)*RAD/2), y = sin((d-b)*RAD/2);
    double h = x*x + cos(a*RAD)*cos(c*RAD)*y*y;
    return 2*asin(sqrt(fmin(1, fmax(0, h))));
}
static double bearing(double a, double b, double c, double d)
{
    double dl = (d-b)*RAD;
    return atan2(sin(dl)*cos(c*RAD), cos(a*RAD)*sin(c*RAD)-sin(a*RAD)*cos(c*RAD)*cos(dl));
}
void ls_route_clear(ls_route_t *r)
{
    ls_route_point_t *p = r->point;
    size_t cap = r->cap;
    double off = r->off_m, arrival = r->arrival_m;
    memset(r, 0, sizeof(*r));
    r->point = p; r->cap = cap; r->off_m = off; r->arrival_m = arrival;
    r->bearing = NAN;
}
void ls_route_init(ls_route_t *r, ls_route_point_t *p, size_t cap)
{
    memset(r, 0, sizeof(*r));
    r->point = p; r->cap = cap < LS_ROUTE_CAP ? cap : LS_ROUTE_CAP;
    r->off_m = 40; r->arrival_m = 15; r->bearing = NAN;
}
static bool finish(ls_route_t *r)
{
    if (r->n < 2) return false;
    double sum = 0;
    r->point[0].start = true;
    for (size_t i = 0; i < r->n; i++) {
        if (i && !r->point[i].start)
            sum += EARTH*angle(r->point[i-1].lat, r->point[i-1].lon, r->point[i].lat, r->point[i].lon);
        r->point[i].along_m = sum;
    }
    if (sum < 0.01) return false;
    r->length_m = r->remaining_m = sum;
    r->valid = true;
    return true;
}
/* The last input point is always selected, even when the count exceeds the cap. */
static size_t selected(size_t slot, size_t total, size_t kept)
{
    return (size_t)((uint64_t)slot*(total-1)/(kept-1));
}
static const char *local(const char *s)
{
    const char *p = strchr(s, ':');
    return p ? p+1 : s;
}
static bool name_char(int c)
{
    return isalnum((unsigned char)c) || c == '_' || c == ':' || c == '-' || c == '.';
}
/* Parse all attributes, rejecting unquoted, duplicate or non-finite coordinates. */
static bool attributes(char *s, double *lat, double *lon, bool point)
{
    bool have_lat = false, have_lon = false;
    while (*s) {
        while (isspace((unsigned char)*s)) s++;
        if (!*s) break;
        char *key = s;
        while (name_char(*s)) s++;
        if (key == s) return false;
        char *end = s;
        while (isspace((unsigned char)*s)) s++;
        if (*s++ != '=') return false;
        while (isspace((unsigned char)*s)) s++;
        char quote = *s++;
        if (quote != '\'' && quote != '"') return false;
        char *value = s;
        while (*s && *s != quote) { if (*s == '<') return false; s++; }
        if (!*s) return false;
        *end = 0; *s++ = 0;
        if (point && (!strcmp(key, "lat") || !strcmp(key, "lon"))) {
            char *tail;
            double v = strtod(value, &tail);
            if (tail == value || *tail || !isfinite(v) || strchr(value,'x') || strchr(value,'X')) return false;
            if (!strcmp(key, "lat")) { if (have_lat) return false; *lat = v; have_lat = true; }
            else { if (have_lon) return false; *lon = v; have_lon = true; }
        }
        if (*s && !isspace((unsigned char)*s)) return false;
    }
    return !point || (have_lat && have_lon && coordinate(*lat, *lon));
}
/* A small XML tokenizer: bounded tags/depth/bytes, no DTDs or entity expansion.
   Exact qualified names balance the tree; local names identify GPX elements. */
static bool gpx_pass(ls_route_t *r, FILE *f, size_t total, size_t *count)
{
    char stack[XML_DEPTH][XML_NAME], tag[XML_TAG];
    unsigned depth = 0;
    size_t bytes = 0, seen = 0, kept = total < r->cap ? total : r->cap;
    bool root = false, closed = false, gap = true;
    int c;
    /* UTF-8 BOMs are common in GPX exported by desktop tools. */
    c=fgetc(f);
    if (c==0xef) {
        if (fgetc(f)!=0xbb || fgetc(f)!=0xbf) return false;
        bytes=3;
    } else if (c!=EOF) ungetc(c,f);
    while ((c = fgetc(f)) != EOF) {
        if (++bytes > LS_ROUTE_FILE_MAX) return false;
        if (c != '<') {
            if (!depth && !isspace((unsigned char)c)) return false;
            continue;
        }
        size_t n = 0;
        int quote = 0;
        while ((c = fgetc(f)) != EOF) {
            if (++bytes > LS_ROUTE_FILE_MAX || n+1 >= sizeof(tag)) return false;
            if (c == '>' && !quote) break;
            if (c == '<' && !quote) return false;
            if (c == '\'' || c == '"') { if (!quote) quote = c; else if (quote == c) quote = 0; }
            tag[n++] = (char)c;
            if (n==3 && !memcmp(tag,"!--",3)) {
                int a=0,b=0;
                while ((c=fgetc(f))!=EOF) {
                    if (++bytes>LS_ROUTE_FILE_MAX) return false;
                    if (a=='-' && b=='-' && c=='>') break;
                    a=b;b=c;
                }
                if (c==EOF) return false;
                memcpy(tag,"!-- --",6);n=6;quote=0;
                break;
            }
        }
        if (c == EOF || quote) return false;
        tag[n] = 0;
        if (tag[0] == '?') { if (n < 2 || tag[n-1] != '?' || root) return false; continue; }
        if (!strncmp(tag, "!--", 3)) {
            if (n < 5 || strcmp(tag+n-2, "--")) return false;
            continue;
        }
        if (tag[0] == '!') return false;
        bool close = tag[0] == '/', self = n && tag[n-1] == '/';
        if (self) tag[--n] = 0;
        char *name = tag + (close ? 1 : 0), *s = name;
        while (name_char(*s)) s++;
        size_t namelen = (size_t)(s-name);
        if (!namelen || namelen >= XML_NAME) return false;
        char qname[XML_NAME]; memcpy(qname, name, namelen); qname[namelen] = 0;
        if (*s && !isspace((unsigned char)*s)) return false;
        if (close) {
            while (isspace((unsigned char)*s)) s++;
            if (*s || self || !depth || strcmp(stack[depth-1], qname)) return false;
            depth--;
            if (!depth) closed = true;
            continue;
        }
        const char *ln = local(qname), *parent = depth ? local(stack[depth-1]) : "";
        if (!depth) {
            if (root || closed || strcmp(ln, "gpx")) return false;
            root = true;
        }
        if ((!strcmp(ln,"trk") && strcmp(parent,"gpx")) ||
            (!strcmp(ln,"rte") && strcmp(parent,"gpx")) ||
            (!strcmp(ln,"trkseg") && strcmp(parent,"trk")) ||
            (!strcmp(ln,"trkpt") && strcmp(parent,"trkseg")) ||
            (!strcmp(ln,"rtept") && strcmp(parent,"rte"))) return false;
        bool point = (!strcmp(ln,"trkpt") && !strcmp(parent,"trkseg")) ||
                     (!strcmp(ln,"rtept") && !strcmp(parent,"rte"));
        if ((!strcmp(ln,"trkseg") && !strcmp(parent,"trk")) ||
            (!strcmp(ln,"rte") && !strcmp(parent,"gpx"))) gap = true;
        double lat = 0, lon = 0;
        if (!attributes(s, &lat, &lon, point)) return false;
        if (point) {
            if (++seen > LS_ROUTE_INPUT_MAX) return false;
            if (total && r->n < kept && seen-1 == selected(r->n,total,kept)) {
                r->point[r->n++] = (ls_route_point_t){ .lat=lat, .lon=lon, .start=gap };
                gap = false;
            }
        }
        if (!self) {
            if (depth == XML_DEPTH) return false;
            strcpy(stack[depth++], qname);
        } else if (!depth) closed = true;
    }
    *count = seen;
    return !ferror(f) && root && closed && !depth;
}
bool ls_route_gpx(ls_route_t *r, FILE *f)
{
    ls_route_clear(r);
    size_t count = 0, second = 0;
    if (!f || !r->point || r->cap < 2 || fseek(f,0,SEEK_SET) ||
        !gpx_pass(r,f,0,&count) || count < 2 || fseek(f,0,SEEK_SET) ||
        !gpx_pass(r,f,count,&second) || count != second || !finish(r)) {
        ls_route_clear(r); return false;
    }
    return true;
}
bool ls_route_track(ls_route_t *r, FILE *f)
{
    ls_route_clear(r);
    ls_rlog_header_t h, after;
    if (!f || !r->point || r->cap < 2 || fseek(f,0,SEEK_SET) || fread(&h,sizeof(h),1,f)!=1 ||
        h.magic!=LS_RLOG_MAGIC || h.version!=LS_RLOG_VERSION || h.rec_size!=sizeof(ls_track_pt_t) ||
        h.capacity!=LS_TRACK_CAPACITY || h.count<2 || h.count>h.capacity || h.head>=h.capacity) return false;
    size_t kept = h.count < r->cap ? h.count : r->cap;
    for (size_t i=0; i<h.count; i++) {
        uint32_t slot = (h.head+h.capacity-h.count+(uint32_t)i)%h.capacity;
        ls_track_pt_t p;
        if (fseek(f,(long)(sizeof(h)+slot*sizeof(p)),SEEK_SET) || fread(&p,sizeof(p),1,f)!=1 ||
            !coordinate(p.lat_e7/1e7,p.lon_e7/1e7)) goto fail;
        if (r->n<kept && i==selected(r->n,h.count,kept))
            r->point[r->n++] = (ls_route_point_t){ .lat=p.lat_e7/1e7, .lon=p.lon_e7/1e7 };
    }
    /* Reject a ring which changed while reading instead of mixing two walks. */
    if (fseek(f,0,SEEK_SET) || fread(&after,sizeof(after),1,f)!=1 || memcmp(&h,&after,sizeof(h)) || !finish(r)) goto fail;
    ls_route_reverse(r);
    return true;
fail:
    ls_route_clear(r); return false;
}
void ls_route_reverse(ls_route_t *r)
{
    if (!r->valid) return;
    for (size_t i=0;i<r->n/2;i++) {
        ls_route_point_t p=r->point[i]; r->point[i]=r->point[r->n-1-i]; r->point[r->n-1-i]=p;
    }
    for (size_t i=r->n-1;i>0;i--) r->point[i].start=r->point[i-1].start;
    r->progress_m=r->walked_m=0; r->outside=r->inside=r->near=0;
    r->off_route=r->arrived=r->located=false; r->bearing=NAN;
    r->reversed=!r->reversed;
    finish(r);
}
ls_route_event_t ls_route_update(ls_route_t *r, bool fresh, double lat, double lon)
{
    if (!r->valid) return LS_ROUTE_NONE;
    if (!fresh || !coordinate(lat,lon)) {
        r->located=false; r->bearing=NAN;
        r->outside=r->inside=r->near=0;
        return LS_ROUTE_NONE;
    }
    if (r->arrived) { r->located=true; return LS_ROUTE_NONE; }
    double best=DBL_MAX, along=0;
    size_t next=1;
    for (size_t i=1;i<r->n;i++) {
        const ls_route_point_t *a=&r->point[i-1], *b=&r->point[i];
        if (b->start) continue;
        double len=b->along_m-a->along_m;
        if (len<0.01) continue;
        double d=angle(a->lat,a->lon,lat,lon);
        double delta=bearing(a->lat,a->lon,lat,lon)-bearing(a->lat,a->lon,b->lat,b->lon);
        double at=atan2(sin(d)*cos(delta),cos(d))*EARTH;
        double cross;
        if (at<0) {at=0;cross=d*EARTH;}
        else if (at>len) {at=len;cross=angle(b->lat,b->lon,lat,lon)*EARTH;}
        else cross=fabs(asin(fmax(-1,fmin(1,sin(d)*sin(delta)))))*EARTH;
        double progress=a->along_m+at;
        /* At crossings, prefer the branch nearest the previous progress. */
        if (cross<best-0.5 || (fabs(cross-best)<=0.5 &&
            fabs(progress-r->progress_m)<fabs(along-r->progress_m))) {
            best=cross;along=progress;next=i;
        }
    }
    if (best==DBL_MAX) return LS_ROUTE_NONE;
    r->located=true;r->cross_m=best;r->progress_m=along;r->remaining_m=r->length_m-along;
    /* A reached vertex points at the following waypoint, without skipping gaps. */
    if (next+1<r->n && !r->point[next+1].start && r->point[next].along_m-along<1) next++;
    r->next=next;
    r->bearing=fmod(bearing(lat,lon,r->point[next].lat,r->point[next].lon)/RAD+360,360);
    double off=isfinite(r->off_m) && r->off_m>=5 ? r->off_m : 40;
    double arrival=isfinite(r->arrival_m) && r->arrival_m>=1 ? r->arrival_m : 15;
    if (best<=off && along>r->walked_m) r->walked_m=along;
    ls_route_event_t event=LS_ROUTE_NONE;
    if (!r->off_route) {
        r->outside=best>off ? r->outside+1 : 0;
        if (r->outside>=3) {r->off_route=true;r->outside=0;event=LS_ROUTE_OFF;}
    } else {
        r->inside=best<off*0.7 ? r->inside+1 : 0;
        if (r->inside>=2) {r->off_route=false;r->inside=0;event=LS_ROUTE_REJOIN;}
    }
    const ls_route_point_t *end=&r->point[r->n-1];
    bool near=!r->off_route && r->remaining_m<=arrival && angle(lat,lon,end->lat,end->lon)*EARTH<=arrival;
    r->near=near ? r->near+1 : 0;
    if (!r->arrived && r->near>=2) {r->arrived=true;event=LS_ROUTE_ARRIVE;}
    if (r->arrived) {r->near=0;r->bearing=NAN;return event==LS_ROUTE_ARRIVE ? event : LS_ROUTE_NONE;}
    return event;
}
