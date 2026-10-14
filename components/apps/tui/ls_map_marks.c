/* See ls_map_marks.h.

   The file is lines of text:
     M <lat> <lon> <icon> <name>
     S <hue> <name>
     P <lat> <lon>        (points of the S line above)
   Anything else is ignored, so a comment or a blank line is harmless. */
#include "ls_map_marks.h"

#include <ctype.h>
#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdatomic.h>
#include <time.h>

#include "esp_attr.h"

#include "ls_geo.h"

EXT_RAM_BSS_ATTR static ls_mark_t   s_marks[LS_MARKS_MAX];
EXT_RAM_BSS_ATTR static ls_sketch_t s_sketch[LS_SKETCH_MAX];
EXT_RAM_BSS_ATTR static ls_sketch_t s_open;
static int  s_nmarks, s_nsketch;
static uint32_t s_next_id;
static bool s_drawing;
static bool s_loaded;
static int  s_serial = 1;           /* the number the next unnamed marker gets */
static const char *s_error;
static char s_path[96] = "/sdcard/maps/marks.txt";

/* UI owns live marks. Worker sees only PSRAM snapshots, never live arrays. */
typedef struct {
    ls_mark_t marks[LS_MARKS_MAX]; ls_sketch_t sketch[LS_SKETCH_MAX];
    int nmarks,nsketch,serial; unsigned generation,revision; char path[96]; const char *error;
    atomic_int state; /* 0 free, 1 queued, 2 worker-owned, 3 complete */
} marks_io;
static EXT_RAM_BSS_ATTR marks_io read_job,write_job[2];
static bool async_io;
static unsigned revision, saved_revision, saved_generation;
static unsigned write_generation; /* UI only */
static void adopt(const marks_io *j) {
    memcpy(s_marks,j->marks,sizeof(s_marks));memcpy(s_sketch,j->sketch,sizeof(s_sketch));
    for(int i=0;i<j->nmarks;i++) s_marks[i].id=++s_next_id;
    for(int i=0;i<j->nsketch;i++) s_sketch[i].id=++s_next_id;
    s_nmarks=j->nmarks;s_nsketch=j->nsketch;s_serial=j->serial;s_error=j->error;s_loaded=true;revision=saved_revision;
}
void ls_marks_defer_io(void) {
    async_io=true;
    if(!s_loaded && atomic_load(&read_job.state)==0) {
        snprintf(read_job.path,sizeof(read_job.path),"%s",s_path);
        atomic_store(&read_job.state,1);
    }
}

static const char ICON_CHAR[LS_MARK__COUNT] = { '*', 'F', '!', 'X', 'A', 'W', 'P', '+' };
static const char *const ICON_NAME[LS_MARK__COUNT] = {
    "place", "meeting point", "hazard", "target", "camp", "water", "vehicle", "first aid" };

char ls_mark_icon_char(int icon)
{
    return (icon >= 0 && icon < LS_MARK__COUNT) ? ICON_CHAR[icon] : '*';
}

const char *ls_mark_icon_name(int icon)
{
    return (icon >= 0 && icon < LS_MARK__COUNT) ? ICON_NAME[icon] : ICON_NAME[0];
}

void ls_marks_use_file(const char *path)
{
    revision=saved_revision=0;
    snprintf(s_path, sizeof(s_path), "%s", path ? path : "");
    ls_marks_reload();
}

const char *ls_marks_file(void) { return s_path; }
const char *ls_marks_error(void) {
    for(int i=0;i<2;i++) if(atomic_load(&write_job[i].state)==3 &&
        write_job[i].generation==write_generation) return write_job[i].error;
    return s_error;
}

static bool valid_place(double lat, double lon)
{
    return isfinite(lat) && isfinite(lon) && fabs(lat) <= 85.0 && fabs(lon) <= 180.0;
}

/* A name as stored: printable, no line breaks, trimmed. */
static void clean_name(char *dst, size_t cap, const char *src)
{
    size_t n = 0;
    while (src && *src == ' ') src++;
    for (; src && *src && n + 1 < cap; src++)
        dst[n++] = (*src >= 32 && *src < 127) ? *src : ' ';
    while (n > 0 && dst[n - 1] == ' ') n--;
    dst[n] = 0;
}

/* The next free "MARK n" after everything already named that way. */
static void note_serial(const char *name)
{
    int k = 0;
    if (sscanf(name, "MARK %d", &k) == 1 && k >= s_serial) s_serial = k + 1;
}

static void clear_all(void)
{
    s_nmarks = s_nsketch = 0;
    s_drawing = false;
    s_serial = 1;
}

void ls_marks_reload(void)
{
    ls_marks_save_status();
    if(async_io && atomic_load(&read_job.state)!=0) return;
    s_loaded = false;
    if(async_io && revision!=saved_revision) { s_loaded=true;return; }
    clear_all();
    ls_marks_load();
}

static void read_marks(marks_io *j) {
    j->nmarks=j->nsketch=0;j->serial=1;
    j->error = NULL;
    FILE *f = j->path[0] ? fopen(j->path, "r") : NULL;
    if (!f && j->path[0]) {
        char backup[104];snprintf(backup,sizeof(backup),"%s.bak",j->path);
        f=fopen(backup,"r");
    }
    if (!f) return;                 /* nothing saved yet is not an error */
    char line[160];
    ls_sketch_t *cur = NULL;
    ls_mark_origin_t *origin = NULL;
    while (fgets(line, sizeof(line), f)) {
        line[strcspn(line, "\r\n")] = 0;
        double lat, lon;
        int icon, hue, used = 0;
        if (line[0] == 'M' && sscanf(line + 1, "%lf %lf %d %n", &lat, &lon, &icon, &used) == 3) {
            cur = NULL;origin = NULL;
            if (j->nmarks >= LS_MARKS_MAX || !valid_place(lat, lon)) continue;
            ls_mark_t *m = &j->marks[j->nmarks++];
            memset(m,0,sizeof(*m));origin=&m->origin;
            m->lat = lat; m->lon = lon;
            m->icon = (uint8_t)((icon >= 0 && icon < LS_MARK__COUNT) ? icon : 0);
            clean_name(m->name, sizeof(m->name), line + 1 + used);
            if (!m->name[0]) snprintf(m->name, sizeof(m->name), "MARK %d", j->serial);
            int serial=0; if(sscanf(m->name,"MARK %d",&serial)==1 && serial>=j->serial) j->serial=serial+1;
        } else if (line[0] == 'S' && sscanf(line + 1, "%d %n", &hue, &used) == 1) {
            cur = NULL;origin = NULL;
            if (j->nsketch >= LS_SKETCH_MAX) continue;
            cur = &j->sketch[j->nsketch++];
            memset(cur, 0, sizeof(*cur));origin=&cur->origin;
            cur->hue = (uint8_t)(hue & 7);
            clean_name(cur->name, sizeof(cur->name), line + 1 + used);
        } else if (line[0] == 'T' && origin) {
            unsigned stamp=0;
            if(sscanf(line+1,"%u %n",&stamp,&used)==1) {
                origin->created=stamp;clean_name(origin->sender,sizeof(origin->sender),line+1+used);
            }
        } else if (line[0] == 'P' && cur && sscanf(line + 1, "%lf %lf", &lat, &lon) == 2) {
            if (cur->n < LS_SKETCH_PTS && valid_place(lat, lon)) {
                cur->lat[cur->n] = lat;
                cur->lon[cur->n] = lon;
                cur->n++;
            }
        }
    }
    fclose(f);
    /* A line that lost its points to a bad edit is not a line. */
    for (int i = j->nsketch - 1; i >= 0; i--)
        if (j->sketch[i].n < 2) { memmove(&j->sketch[i],&j->sketch[i+1],(size_t)(j->nsketch-i-1)*sizeof(j->sketch[0]));j->nsketch--; }
}
void ls_marks_load(void) {
    if(s_loaded) return;
    if(async_io) {
        if(atomic_load(&read_job.state)==3) { adopt(&read_job);atomic_store(&read_job.state,0); }
        else ls_marks_defer_io();
        return;
    }
    snprintf(read_job.path,sizeof(read_job.path),"%s",s_path);
    read_marks(&read_job);adopt(&read_job);
}

static bool write_marks(marks_io *j) {
    if (!j->path[0]) { j->error = "no file to save to"; return false; }
    char tmp[104],backup[104];
    snprintf(backup,sizeof(backup),"%s.bak",j->path);
    snprintf(tmp, sizeof(tmp), "%s.new", j->path);
    FILE *f = fopen(tmp, "w");
    if (!f) { j->error = "not saved: no SD card or no maps folder"; return false; }
    bool ok = fprintf(f, "# LakeShark map marks\n") > 0;
    for (int i = 0; i < j->nmarks && ok; i++) {
        if(j->marks[i].origin.received) continue;
        ok = fprintf(f, "M %.6f %.6f %d %s\n", j->marks[i].lat, j->marks[i].lon,
                     j->marks[i].icon, j->marks[i].name) > 0;
        if(ok) ok=fprintf(f,"T %u %s\n",(unsigned)j->marks[i].origin.created,j->marks[i].origin.sender)>0;
    }
    for (int i = 0; i < j->nsketch && ok; i++) {
        if(j->sketch[i].origin.received) continue;
        ok = fprintf(f, "S %d %s\n", j->sketch[i].hue, j->sketch[i].name) > 0;
        if(ok) ok=fprintf(f,"T %u %s\n",(unsigned)j->sketch[i].origin.created,j->sketch[i].origin.sender)>0;
        for (int k = 0; k < j->sketch[i].n && ok; k++)
            ok = fprintf(f, "P %.6f %.6f\n", j->sketch[i].lat[k], j->sketch[i].lon[k]) > 0;
    }
    if (fclose(f) != 0) ok = false;
    if (!ok) { j->error = "not saved: the card refused the write"; return false; }
    FILE *old=fopen(j->path,"r");
    if(old) {
        if(fclose(old)!=0 || (remove(backup)!=0 && errno!=ENOENT) || rename(j->path,backup)!=0) {
            j->error="not saved: could not keep previous file";return false;
        }
    } else if(errno!=ENOENT) { j->error="not saved: could not read previous file";return false; }
    if (rename(tmp, j->path) != 0) { j->error = "not saved: could not replace the file"; return false; }
    j->error = NULL;
    return true;
}
bool ls_marks_save(void) {
    ls_marks_load();
    if(!s_loaded) { s_error="opening marks...";return false; }
    for(int i=0;i<2;i++) {
        marks_io *j=&write_job[i];int state=atomic_load(&j->state);
        if(state==2 || !atomic_compare_exchange_strong(&j->state,&state,2)) continue;
        memcpy(j->marks,s_marks,sizeof(s_marks));memcpy(j->sketch,s_sketch,sizeof(s_sketch));
        j->nmarks=s_nmarks;j->nsketch=s_nsketch;j->generation=++write_generation;j->revision=revision;
        snprintf(j->path,sizeof(j->path),"%s",s_path);
        if(async_io) { s_error=NULL;atomic_store(&j->state,1);return true; }
        bool ok=write_marks(j);if(ok) saved_revision=j->revision;s_error=j->error;atomic_store(&j->state,0);return ok;
    }
    s_error="marks save busy";return false;
}
void ls_marks_io_step(void) {
    static unsigned last_write;
    if(!async_io) return;
    int state=1;
    if(atomic_compare_exchange_strong(&read_job.state,&state,2)) {
        read_marks(&read_job);atomic_store(&read_job.state,3);
    }
    for(int i=0;i<2;i++) {
        state=1;
        if(atomic_compare_exchange_strong(&write_job[i].state,&state,2)) {
            if(write_job[i].generation>last_write) {
                write_marks(&write_job[i]);last_write=write_job[i].generation;
            }
            atomic_store(&write_job[i].state,3);
        }
    }
}

const char *ls_marks_save_status(void) {
    for(int i=0;i<2;i++) {
        marks_io *j=&write_job[i];int state=atomic_load(&j->state);
        if(j->generation!=write_generation || !write_generation) continue;
        if(state==1 || state==2) return "MAP SAVING";
        if(state==3 && j->generation>saved_generation) {
            saved_generation=j->generation;s_error=j->error;
            if(!j->error) saved_revision=j->revision;
        }
    }
    if(!write_generation && !s_error) return revision!=saved_revision ? "MAP UNSAVED" : NULL;
    if(revision!=saved_revision || s_error) return "MAP NOT SAVED: retry in OPTIONS";
    return write_generation ? "MAP SAVED" : NULL;
}

int ls_marks_count(void) { ls_marks_load(); return s_nmarks; }

const ls_mark_t *ls_marks_at(int i)
{
    ls_marks_load();
    return (i >= 0 && i < s_nmarks) ? &s_marks[i] : NULL;
}

int ls_marks_add(double lat, double lon, int icon, const char *name)
{
    ls_marks_load();
    if (!s_loaded || s_nmarks >= LS_MARKS_MAX || !valid_place(lat, lon)) return -1;
    ls_mark_t *m = &s_marks[s_nmarks];
    memset(m,0,sizeof(*m));
    time_t now=time(NULL);
    m->origin.created=now>=1577836800 ? (uint32_t)now : 0;
    m->id=++s_next_id;
    m->lat = lat; m->lon = lon;
    m->icon = (uint8_t)((icon >= 0 && icon < LS_MARK__COUNT) ? icon : 0);
    char clean[LS_MARK_NAME];
    clean_name(clean, sizeof(clean), name);
    if (clean[0]) snprintf(m->name, sizeof(m->name), "%s", clean);
    else          snprintf(m->name, sizeof(m->name), "MARK %d", s_serial);
    note_serial(m->name);
    revision++;return s_nmarks++;
}

bool ls_marks_rename(int i, const char *name)
{
    if (i < 0 || i >= s_nmarks) return false;
    char clean[LS_MARK_NAME];
    clean_name(clean, sizeof(clean), name);
    if (!clean[0]) return false;
    revision++;
    snprintf(s_marks[i].name, sizeof(s_marks[i].name), "%s", clean);
    return true;
}

bool ls_marks_set_icon(int i, int icon)
{
    if (i < 0 || i >= s_nmarks || icon < 0 || icon >= LS_MARK__COUNT) return false;
    revision++;
    s_marks[i].icon = (uint8_t)icon;
    return true;
}

bool ls_marks_delete(int i)
{
    if (i < 0 || i >= s_nmarks) return false;
    memmove(&s_marks[i], &s_marks[i + 1], (size_t)(s_nmarks - i - 1) * sizeof(s_marks[0]));
    s_nmarks--;revision++;
    return true;
}

int ls_sketch_count(void) { ls_marks_load(); return s_nsketch; }

const ls_sketch_t *ls_sketch_at(int i)
{
    ls_marks_load();
    return (i >= 0 && i < s_nsketch) ? &s_sketch[i] : NULL;
}

bool ls_sketch_delete(int i)
{
    if (i < 0 || i >= s_nsketch) return false;
    memmove(&s_sketch[i], &s_sketch[i + 1], (size_t)(s_nsketch - i - 1) * sizeof(s_sketch[0]));
    s_nsketch--;revision++;
    return true;
}

bool ls_sketch_rename(int i, const char *name)
{
    if (i < 0 || i >= s_nsketch) return false;
    char clean[LS_MARK_NAME];
    clean_name(clean, sizeof(clean), name);
    if (!clean[0]) return false;
    revision++;
    snprintf(s_sketch[i].name, sizeof(s_sketch[i].name), "%s", clean);
    return true;
}

void ls_sketch_begin(void)
{
    ls_marks_load();
    if(!s_loaded) return;
    memset(&s_open, 0, sizeof(s_open));
    /* Each new line in the next colour, clear of the mesh's magenta and
       the rings' cyan. */
    static const uint8_t HUES[] = { 3, 1, 2, 4, 7 };
    s_open.hue = HUES[s_nsketch % (int)sizeof(HUES)];
    s_drawing = true;
}

bool ls_sketch_drawing(void) { return s_drawing; }

bool ls_sketch_add_point(double lat, double lon)
{
    if (!s_drawing || s_open.n >= LS_SKETCH_PTS || !valid_place(lat, lon)) return false;
    s_open.lat[s_open.n] = lat;
    s_open.lon[s_open.n] = lon;
    s_open.n++;
    return true;
}

bool ls_sketch_undo(void)
{
    if (!s_drawing || s_open.n <= 0) return false;
    s_open.n--;
    return true;
}

const ls_sketch_t *ls_sketch_open(void) { return s_drawing ? &s_open : NULL; }

int ls_sketch_finish(void)
{
    if (!s_drawing) return -1;
    if (s_open.n < 2) return -1;
    if (s_nsketch >= LS_SKETCH_MAX) return -2;
    s_drawing = false;
    int k = 1;
    for (int i = 0; i < s_nsketch; i++) {
        int m = 0;
        if (sscanf(s_sketch[i].name, "LINE %d", &m) == 1 && m >= k) k = m + 1;
    }
    snprintf(s_open.name, sizeof(s_open.name), "LINE %d", k);
    time_t now=time(NULL);
    s_open.origin.created=now>=1577836800 ? (uint32_t)now : 0;
    s_open.id=++s_next_id;
    s_sketch[s_nsketch] = s_open;
    revision++;return s_nsketch++;
}

void ls_sketch_cancel(void) { s_drawing = false; }

double ls_sketch_length_m(const ls_sketch_t *s)
{
    if (!s) return 0.0;
    double m = 0.0;
    for (int i = 1; i < s->n; i++) {
        double b = 0, d = 0;
        ls_geo_bearing_range(s->lat[i - 1], s->lon[i - 1], s->lat[i], s->lon[i], &b, &d);
        m += d;
    }
    return m;
}

/* All editing goes through the same revision and snapshot path. */
bool ls_marks_move(int i, double lat, double lon)
{
    if(!ls_marks_at(i) || !valid_place(lat,lon)) return false;
    s_marks[i].lat=lat;s_marks[i].lon=lon;revision++;return true;
}
int ls_marks_next(uint32_t id)
{
    if(!ls_marks_count()) return -1;
    for(int i=0;i<s_nmarks;i++) if(s_marks[i].id==id) return (i+1)%s_nmarks;
    return 0;
}
void ls_marks_clear(void) { ls_marks_load();s_nmarks=0;revision++; }
bool ls_marks_received(bool line, int i, const char *sender)
{
    ls_mark_origin_t *o=NULL;
    if(line && ls_sketch_at(i)) o=&s_sketch[i].origin;
    if(!line && ls_marks_at(i)) o=&s_marks[i].origin;
    if(!o) return false;
    o->received=true;clean_name(o->sender,sizeof(o->sender),sender);revision++;return true;
}
bool ls_marks_accept(bool line, int i)
{
    ls_mark_origin_t *o=NULL;
    if(line && ls_sketch_at(i)) o=&s_sketch[i].origin;
    if(!line && ls_marks_at(i)) o=&s_marks[i].origin;
    if(!o) return false;
    o->received=false;revision++;return true;
}

int ls_sketch_import(const double *lat,const double *lon,int n,const char *name)
{
    ls_marks_load();
    if(!s_loaded || !lat || !lon || n<2 || n>LS_SKETCH_PTS || s_nsketch>=LS_SKETCH_MAX) return -1;
    for(int i=0;i<n;i++) if(!valid_place(lat[i],lon[i])) return -1;
    ls_sketch_t *s=&s_sketch[s_nsketch];memset(s,0,sizeof(*s));
    s->id=++s_next_id;s->n=n;s->hue=3;clean_name(s->name,sizeof(s->name),name);
    memcpy(s->lat,lat,(size_t)n*sizeof(*lat));memcpy(s->lon,lon,(size_t)n*sizeof(*lon));
    time_t now=time(NULL);s->origin.created=now>=1577836800 ? (uint32_t)now : 0;
    revision++;return s_nsketch++;
}
