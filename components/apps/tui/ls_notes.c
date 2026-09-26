#include "ls_notes.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "esp_attr.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "core/ls_time.h"
#ifdef ESP_PLATFORM
#include "ls_tui_png.h"
#endif

#define QUEUE_CAP   6
#define TEXT_SLOTS  3

typedef enum { OP_SCAN, OP_SAVE, OP_TRASH, OP_PNG } op_kind_t;
typedef struct {
    op_kind_t kind;
    char stem[LS_NOTE_STEM];
    int slot;
    int w, h;
} op_t;

EXT_RAM_BSS_ATTR static ls_note_info_t s_index[LS_NOTES_MAX];
EXT_RAM_BSS_ATTR static char s_slots[TEXT_SLOTS][LS_NOTE_TEXT_MAX];
EXT_RAM_BSS_ATTR static char s_scan_buf[LS_NOTE_TEXT_MAX];
EXT_RAM_BSS_ATTR static uint16_t s_png[LS_NOTE_PNG_MAX_PX];
EXT_RAM_BSS_ATTR static op_t s_queue[QUEUE_CAP];
static bool s_slot_busy[TEXT_SLOTS], s_png_busy;
static int s_count, s_qhead, s_qcount;
static uint32_t s_next_seq = 1, s_generation;
static bool s_started, s_scanning;
EXT_RAM_BSS_ATTR static char s_status[80];
EXT_RAM_BSS_ATTR static char s_dir[96];
static StaticSemaphore_t s_lock_memory;
static SemaphoreHandle_t s_lock;

static void defaults(void)
{
    if (!s_dir[0]) snprintf(s_dir, sizeof(s_dir), "/sdcard/notes");
    if (!s_status[0]) snprintf(s_status, sizeof(s_status), "Notes not opened yet");
}

static void lock(void) { xSemaphoreTake(s_lock, portMAX_DELAY); }
static void unlock(void) { xSemaphoreGive(s_lock); }
static void status(const char *text) { lock(); snprintf(s_status, sizeof(s_status), "%s", text); unlock(); }

const char *ls_notes_directory(void) { defaults(); return s_dir; }

void ls_notes_use_directory(const char *dir)
{
    if (dir && dir[0]) snprintf(s_dir, sizeof(s_dir), "%s", dir);
}

/* ------------------------------------------------------------ parsing -- */

static const char *skip_space(const char *p) { while (*p == ' ') p++; return p; }

bool ls_notes_parse_place(const char *line, double *lat, double *lon)
{
    if (!line || line[0] != '>') return false;
    const char *p = skip_space(line + 1);
    const bool bearing = !strncmp(p, "BEARING ", 8);
    if (bearing) {
        /* A bearing names where it was taken from after "from". */
        const char *from = strstr(p, " from ");
        if (!from) return false;
        p = from + 6;
    } else if (!strncmp(p, "GPS ", 4) || !strncmp(p, "MAP ", 4) || !strncmp(p, "FIX ", 4)) {
        p += 4;
    } else return false;
    char *end;
    double a = strtod(p, &end);
    if (end == p) return false;
    p = skip_space(end);
    if (*p == ',') p++;
    const char *q = skip_space(p);
    double b = strtod(q, &end);
    if (end == q) return false;
    if (a < -90 || a > 90 || b < -180 || b > 180) return false;
    if (lat) *lat = a;
    if (lon) *lon = b;
    return true;
}

bool ls_notes_parse_map(const char *line, double *lat, double *lon, int *zoom,
                        char *file, size_t file_cap)
{
    if (!line || strncmp(line, "> MAP ", 6)) return false;
    double a, b;
    if (!ls_notes_parse_place(line, &a, &b)) return false;
    int z = 0;
    const char *zp = strstr(line, " z");
    if (zp) z = atoi(zp + 2);
    if (file && file_cap) {
        file[0] = 0;
        const char *png = strstr(line, ".png");
        if (png) {
            const char *start = png;
            while (start > line && start[-1] != ' ') start--;
            size_t n = (size_t)(png + 4 - start);
            if (n < file_cap) { memcpy(file, start, n); file[n] = 0; }
        }
    }
    if (lat) *lat = a;
    if (lon) *lon = b;
    if (zoom) *zoom = z;
    return true;
}

void ls_notes_summarize(const char *text, ls_note_info_t *info)
{
    char stem[LS_NOTE_STEM]; uint32_t seq = info->seq, bytes;
    memcpy(stem, info->stem, sizeof(stem));
    bytes = text ? (uint32_t)strlen(text) : 0;
    memset(info, 0, sizeof(*info));
    memcpy(info->stem, stem, sizeof(stem));
    info->seq = seq; info->bytes = bytes;
    if (!text) return;
    bool titled = false;
    for (const char *line = text; *line;) {
        const char *eol = strchr(line, '\n');
        size_t n = eol ? (size_t)(eol - line) : strlen(line);
        char buf[160];
        size_t k = n < sizeof(buf) - 1 ? n : sizeof(buf) - 1;
        memcpy(buf, line, k); buf[k] = 0;
        const char *s = skip_space(buf);
        if (!titled && s[0] == '#' && s[1] == ' ') {
            snprintf(info->title, sizeof(info->title), "%.*s", LS_NOTE_TITLE - 1, skip_space(s + 2));
            titled = true;
        } else if (!titled && !info->title[0] && *s && s[0] != '>') {
            snprintf(info->title, sizeof(info->title), "%.*s", LS_NOTE_TITLE - 1, s);
        }
        /* The first line that starts with a date is when the note is from. */
        if (!info->when[0] && s[0] == '2' && s[1] == '0' && strlen(s) >= 16 && s[4] == '-' && s[7] == '-' && s[10] == 'T')
            snprintf(info->when, sizeof(info->when), "%.10s %.5s", s, s + 11);
        if (s[0] == '>') {
            const char *t = skip_space(s + 1);
            if (!strncmp(t, "GPS", 3)) info->badges |= LS_NOTE_HAS_GPS;
            else if (!strncmp(t, "MAP", 3)) info->badges |= LS_NOTE_HAS_MAP;
            else if (!strncmp(t, "RADIO", 5)) info->badges |= LS_NOTE_HAS_RADIO;
            else if (!strncmp(t, "BEARING", 7) || !strncmp(t, "FIX", 3)) info->badges |= LS_NOTE_HAS_BEARING;
            else if (!strncmp(t, "SENSORS", 7) || !strncmp(t, "HEADING", 7)) info->badges |= LS_NOTE_HAS_SENSORS;
            double lat, lon;
            if (!info->has_place && ls_notes_parse_place(s, &lat, &lon)) {
                info->has_place = true; info->lat = lat; info->lon = lon;
            }
        }
        if (!strncmp(s, "- [ ]", 5)) { info->checks_open++; info->badges |= LS_NOTE_HAS_CHECK; }
        else if (!strncmp(s, "- [x]", 5) || !strncmp(s, "- [X]", 5)) { info->checks_done++; info->badges |= LS_NOTE_HAS_CHECK; }
        if (!eol) break;
        line = eol + 1;
    }
    /* Trailing '#' and spaces off a title typed as "# Title #". */
    size_t n = strlen(info->title);
    while (n && (info->title[n - 1] == ' ' || info->title[n - 1] == '#')) info->title[--n] = 0;
    if (!info->title[0]) snprintf(info->title, sizeof(info->title), "Untitled");
}

/* -------------------------------------------------------------- index -- */

static uint32_t seq_of(const char *stem)
{
    return isdigit((unsigned char)stem[0]) ? (uint32_t)strtoul(stem, NULL, 10) : 0;
}

/* Newest first: by sequence, then by name for notes from elsewhere. */
static int index_cmp(const void *a, const void *b)
{
    const ls_note_info_t *x = a, *y = b;
    if (x->seq != y->seq) return x->seq > y->seq ? -1 : 1;
    return -strcmp(x->stem, y->stem);
}

static void sort_locked(void) { qsort(s_index, (size_t)s_count, sizeof(s_index[0]), index_cmp); }

static int find_locked(const char *stem)
{
    for (int i = 0; i < s_count; i++) if (!strcmp(s_index[i].stem, stem)) return i;
    return -1;
}

static void upsert(const ls_note_info_t *info)
{
    lock();
    int i = find_locked(info->stem);
    if (i < 0) {
        if (s_count == LS_NOTES_MAX) {
            /* Full: the oldest drops out of the list, not off the card. */
            sort_locked(); i = s_count - 1;
        } else i = s_count++;
    }
    s_index[i] = *info;
    if (info->seq >= s_next_seq) s_next_seq = info->seq + 1;
    sort_locked(); s_generation++;
    unlock();
}

static void forget(const char *stem)
{
    lock();
    int i = find_locked(stem);
    if (i >= 0) { memmove(s_index + i, s_index + i + 1, sizeof(s_index[0]) * (size_t)(s_count - i - 1)); s_count--; s_generation++; }
    unlock();
}

int ls_notes_count(void) { if (!s_lock) return 0; lock(); int n = s_count; unlock(); return n; }
bool ls_notes_at(int i, ls_note_info_t *out)
{
    if (!s_lock || !out) return false;
    lock(); bool ok = i >= 0 && i < s_count; if (ok) *out = s_index[i]; unlock();
    return ok;
}
int ls_notes_find(const char *stem) { if (!s_lock || !stem) return -1; lock(); int i = find_locked(stem); unlock(); return i; }
bool ls_notes_scanning(void) { if (!s_lock) return false; lock(); bool b = s_scanning || s_qcount; unlock(); return b; }
uint32_t ls_notes_generation(void) { if (!s_lock) return 0; lock(); uint32_t g = s_generation; unlock(); return g; }
const char *ls_notes_status(void) { defaults(); return s_status; }

/* ---------------------------------------------------------------- files -- */

static bool path_of(char *out, size_t cap, const char *leaf, const char *ext)
{
    int n = snprintf(out, cap, "%s/%s%s", s_dir, leaf, ext);
    return n > 0 && (size_t)n < cap;
}

static void ensure_dir(const char *dir)
{
#ifdef _WIN32
    mkdir(dir);
#else
    mkdir(dir, 0775);
#endif
}

static bool read_file(const char *path, char *buf, size_t cap)
{
    FILE *f = fopen(path, "rb");
    if (!f) return false;
    size_t n = fread(buf, 1, cap - 1, f);
    fclose(f);
    buf[n] = 0;
    return true;
}

bool ls_notes_load(const char *stem, char *buf, size_t cap)
{
    char path[160];
    if (!stem || !buf || cap < 2 || !path_of(path, sizeof(path), stem, ".md")) return false;
    return read_file(path, buf, cap);
}

/* Written beside the note and renamed over it, so a pulled card or a flat
   battery mid-save leaves the previous version, never half of the new one. */
static bool write_note(const char *stem, const char *text)
{
    char path[160], temp[160];
    if (!path_of(path, sizeof(path), stem, ".md") || !path_of(temp, sizeof(temp), stem, ".tmp")) return false;
    ensure_dir(s_dir);
    FILE *f = fopen(temp, "wb");
    if (!f) return false;
    size_t n = strlen(text);
    bool ok = fwrite(text, 1, n, f) == n;
    ok = fclose(f) == 0 && ok;
    if (!ok) { remove(temp); return false; }
    remove(path);
    return rename(temp, path) == 0;
}

static void scan(void)
{
    lock(); s_scanning = true; unlock();
    ensure_dir(s_dir);
    DIR *d = opendir(s_dir);
    if (!d) { status("No SD card or notes folder"); lock(); s_scanning = false; unlock(); return; }
    int found = 0;
    lock(); s_count = 0; unlock();
    struct dirent *e;
    while ((e = readdir(d))) {
        size_t n = strlen(e->d_name);
        if (n < 4 || n - 3 >= LS_NOTE_STEM || strcasecmp(e->d_name + n - 3, ".md")) continue;
        ls_note_info_t info = {0};
        memcpy(info.stem, e->d_name, n - 3); info.stem[n - 3] = 0;
        info.seq = seq_of(info.stem);
        char path[160];
        if (!path_of(path, sizeof(path), info.stem, ".md") || !read_file(path, s_scan_buf, sizeof(s_scan_buf))) continue;
        ls_notes_summarize(s_scan_buf, &info);
        upsert(&info);
        found++;
    }
    closedir(d);
    char line[80];
    snprintf(line, sizeof(line), "%d note%s on the card", found, found == 1 ? "" : "s");
    status(line);
    lock(); s_scanning = false; s_generation++; unlock();
}

static void trash(const char *stem)
{
    char from[160], dir[128], to[200];
    snprintf(dir, sizeof(dir), "%s/trash", s_dir);
    ensure_dir(dir);
    if (!path_of(from, sizeof(from), stem, ".md")) return;
    snprintf(to, sizeof(to), "%s/%s.md", dir, stem);
    remove(to);
    bool ok = rename(from, to) == 0;
    /* Its pictures go with it: they are named after the note. */
    DIR *d = opendir(s_dir);
    if (d) {
        struct dirent *e; size_t sn = strlen(stem);
        while ((e = readdir(d)))
            if (!strncmp(e->d_name, stem, sn) && e->d_name[sn] == '-' && strstr(e->d_name, ".png")) {
                char a[400], b[480];
                snprintf(a, sizeof(a), "%s/%s", s_dir, e->d_name);
                snprintf(b, sizeof(b), "%s/%s", dir, e->d_name);
                remove(b); rename(a, b);
            }
        closedir(d);
    }
    if (ok) forget(stem);
    status(ok ? "Moved to notes/trash" : "Could not move the note");
}

/* ---------------------------------------------------------------- queue -- */

static bool enqueue(const op_t *op)
{
    if (!s_started) return false;
    lock();
    bool ok = s_qcount < QUEUE_CAP;
    if (ok) s_queue[(s_qhead + s_qcount++) % QUEUE_CAP] = *op;
    unlock();
    return ok;
}

static int take_slot(const char *text)
{
    lock();
    int slot = -1;
    for (int i = 0; i < TEXT_SLOTS; i++) if (!s_slot_busy[i]) { slot = i; s_slot_busy[i] = true; break; }
    unlock();
    if (slot >= 0) snprintf(s_slots[slot], LS_NOTE_TEXT_MAX, "%s", text);
    return slot;
}

static void give_slot(int slot) { lock(); s_slot_busy[slot] = false; unlock(); }

void ls_notes_io_step(void)
{
    if (!s_started) return;
    lock();
    bool have = s_qcount > 0;
    op_t op;
    if (have) { op = s_queue[s_qhead]; s_qhead = (s_qhead + 1) % QUEUE_CAP; s_qcount--; }
    unlock();
    if (!have) return;
    if (op.kind == OP_SCAN) scan();
    else if (op.kind == OP_SAVE) {
        bool ok = write_note(op.stem, s_slots[op.slot]);
        if (ok) {
            ls_note_info_t info = {0};
            snprintf(info.stem, sizeof(info.stem), "%s", op.stem);
            info.seq = seq_of(op.stem);
            ls_notes_summarize(s_slots[op.slot], &info);
            upsert(&info);
        }
        give_slot(op.slot);
        status(ok ? "Saved to the SD card" : "SAVE FAILED: check the SD card");
    } else if (op.kind == OP_TRASH) trash(op.stem);
    else if (op.kind == OP_PNG) {
        char path[160];
        bool ok = false;
        ensure_dir(s_dir);
#ifdef ESP_PLATFORM
        ok = path_of(path, sizeof(path), op.stem, ".png") && ls_tui_png_write(s_png, op.w, op.h, path) > 0;
#else
        FILE *f = path_of(path, sizeof(path), op.stem, ".png") ? fopen(path, "wb") : NULL;
        if (f) { ok = fwrite(s_png, 2, (size_t)op.w * (size_t)op.h, f) == (size_t)op.w * (size_t)op.h; fclose(f); }
#endif
        lock(); s_png_busy = false; unlock();
        status(ok ? "Map picture saved" : "MAP PICTURE FAILED: check the SD card");
    }
}

/* ------------------------------------------------------------------ API -- */

bool ls_notes_start(void)
{
    if (!s_lock) s_lock = xSemaphoreCreateMutexStatic(&s_lock_memory);
    if (!s_lock) return false;
    defaults();
    s_started = true;
    ls_notes_refresh();
    return true;
}

void ls_notes_refresh(void)
{
    const op_t op = { .kind = OP_SCAN };
    lock();
    for (int i = 0; i < s_qcount; i++) if (s_queue[(s_qhead + i) % QUEUE_CAP].kind == OP_SCAN) { unlock(); return; }
    unlock();
    enqueue(&op);
}

bool ls_notes_save(const char *stem, const char *text)
{
    if (!s_started || !stem || !stem[0] || !text) return false;
    int slot = take_slot(text);
    if (slot < 0) { status("Still saving; try again in a moment"); return false; }
    op_t op = { .kind = OP_SAVE, .slot = slot };
    snprintf(op.stem, sizeof(op.stem), "%s", stem);
    if (!enqueue(&op)) { give_slot(slot); return false; }
    status("Saving...");
    return true;
}

/* The next number must follow every note on the card, and a note can be
   created before the first scan has read them all. The names alone are
   enough, and listing them costs no file reads. */
static bool s_seq_known;
static void learn_sequence(void)
{
    if (s_seq_known) return;
    DIR *d = opendir(s_dir);
    if (!d) return;
    uint32_t top = 0;
    struct dirent *e;
    while ((e = readdir(d))) { uint32_t s = seq_of(e->d_name); if (s > top) top = s; }
    closedir(d);
    lock(); if (top + 1 > s_next_seq) s_next_seq = top + 1; unlock();
    s_seq_known = true;
}

bool ls_notes_create(const char *text, char *stem_out, size_t cap)
{
    if (!s_started || !text) return false;
    learn_sequence();
    char stamp[LS_TIME_STAMP_MAX], stem[LS_NOTE_STEM];
    ls_time_render_filename_stamp(stamp, sizeof(stamp));
    lock(); uint32_t seq = s_next_seq++; unlock();
    snprintf(stem, sizeof(stem), "%04lu_%s", (unsigned long)seq, stamp);
    /* Listed at once, so the note is there when the screen returns to the
       list, whether or not the card has caught up. */
    ls_note_info_t info = {0};
    snprintf(info.stem, sizeof(info.stem), "%s", stem);
    info.seq = seq;
    ls_notes_summarize(text, &info);
    if (!ls_notes_save(stem, text)) { lock(); if (s_next_seq == seq + 1) s_next_seq = seq; unlock(); return false; }
    upsert(&info);
    if (stem_out && cap) snprintf(stem_out, cap, "%s", stem);
    return true;
}

bool ls_notes_trash(const char *stem)
{
    if (!stem || !stem[0]) return false;
    op_t op = { .kind = OP_TRASH };
    snprintf(op.stem, sizeof(op.stem), "%s", stem);
    return enqueue(&op);
}

bool ls_notes_mark(const char *title, const char *body)
{
    if (!s_started) ls_notes_start();
    /* Marks come from the UI task, whose stack is internal RAM. */
    EXT_RAM_BSS_ATTR static char text[1400];
    char stamp[LS_TIME_STAMP_MAX];
    ls_time_render_stamp(stamp, sizeof(stamp));
    snprintf(text, sizeof(text), "# %s\n%s\n\n%s\n", title && title[0] ? title : "Mark", stamp, body ? body : "");
    return ls_notes_create(text, NULL, 0);
}

void ls_notes_picture_leaf(const char *stem, char *out, size_t cap)
{
    /* Named after the note so they travel with it, numbered so two maps in
       one note never collide. */
    char path[200];
    for (unsigned n = 1; n < 1000; n++) {
        snprintf(out, cap, "%s-map%u", stem ? stem : "note", n);
        struct stat st;
        if (!path_of(path, sizeof(path), out, ".png") || stat(path, &st) != 0) return;
    }
}

bool ls_notes_save_png(const char *leaf, const uint16_t *px, int w, int h)
{
    if (!s_started || !leaf || !px || w <= 0 || h <= 0 || (long)w * h > LS_NOTE_PNG_MAX_PX) return false;
    lock(); bool busy = s_png_busy; if (!busy) s_png_busy = true; unlock();
    if (busy) return false;
    memcpy(s_png, px, (size_t)w * (size_t)h * sizeof(uint16_t));
    op_t op = { .kind = OP_PNG, .w = w, .h = h };
    snprintf(op.stem, sizeof(op.stem), "%s", leaf);
    if (!enqueue(&op)) { lock(); s_png_busy = false; unlock(); return false; }
    return true;
}

#ifdef LS_NOTES_TEST
void ls_notes_test_reset(const char *directory)
{
    if (!s_lock) s_lock = xSemaphoreCreateMutexStatic(&s_lock_memory);
    snprintf(s_dir, sizeof(s_dir), "%s", directory);
    s_count = s_qhead = s_qcount = 0; s_next_seq = 1; s_generation = 0;
    memset(s_slot_busy, 0, sizeof(s_slot_busy)); s_png_busy = false;
    s_started = false; s_scanning = false; s_seq_known = false;
}
#endif
