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
#include "ls_textpack.h"
#ifdef ESP_PLATFORM
#include "ls_tui_png.h"
#include <unistd.h>
#endif

#define QUEUE_CAP   6
#define TEXT_SLOTS  3

typedef enum { OP_SCAN, OP_SAVE, OP_TRASH, OP_PNG } op_kind_t;
typedef struct {
    op_kind_t kind;
    char stem[LS_NOTE_STEM];
    int slot;
    int w, h;
    uint32_t ticket;
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
static uint32_t s_ticket;
static struct { uint32_t ticket; int result; } s_results[QUEUE_CAP + 2];
EXT_RAM_BSS_ATTR static char s_status[80];
EXT_RAM_BSS_ATTR static char s_dir[96];
static StaticSemaphore_t s_lock_memory;
static SemaphoreHandle_t s_lock;

static void defaults(void)
{
    if (!s_dir[0]) snprintf(s_dir, sizeof(s_dir), "/sdcard/notes");
    if (!s_status[0]) LS_SNPUTS(t_ls_notes_notes_not_opened_yet, s_status, sizeof(s_status), "Notes not opened yet");
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
        if (n && line[n - 1] == '\r') n--;
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
    buf[0] = 0;
    FILE *f = fopen(path, "rb");
    if (!f) return false;
    size_t n = fread(buf, 1, cap - 1, f);
    int extra = fgetc(f);
    bool ok = extra == EOF && !ferror(f);
    if (fclose(f) != 0) ok = false;
    buf[n] = 0;
    /* Windows text files use CRLF; the editor and reader use LF. */
    size_t out = 0;
    for (size_t i = 0; i < n; i++) {
        if (buf[i] == '\r' && i + 1 < n && buf[i + 1] == '\n') continue;
        buf[out++] = buf[i];
    }
    buf[out] = 0;
    return ok;
}

bool ls_notes_load(const char *stem, char *buf, size_t cap)
{
    char path[160];
    if (!stem || !buf || cap < 2 || !path_of(path, sizeof(path), stem, ".md")) return false;
    bool ok = read_file(path, buf, cap);
    if (!ok) { buf[0] = 0; status(LS_TEXT(t_ls_notes_cannot_open_note_too, "Cannot open: note too large or SD read failed")); }
    return ok;
}

/* Keep a backup until the replacement has a committed name. On restart,
   a backup with no .md is the last good version, never a partial .tmp. */
static bool recover_note(const char *stem)
{
    char path[160], backup[160]; struct stat st;
    if (!path_of(path, sizeof(path), stem, ".md") ||
        !path_of(backup, sizeof(backup), stem, ".bak")) return false;
    if (stat(backup, &st) != 0) return true;
    if (!S_ISREG(st.st_mode)) return false;
    if (stat(path, &st) == 0) return S_ISREG(st.st_mode) && remove(backup) == 0;
    return rename(backup, path) == 0;
}

static bool write_note(const char *stem, const char *text)
{
    char path[160], temp[160], backup[160]; struct stat st;
    if (!path_of(path, sizeof(path), stem, ".md") || !path_of(temp, sizeof(temp), stem, ".tmp") ||
        !path_of(backup, sizeof(backup), stem, ".bak")) return false;
    ensure_dir(s_dir);
    if (!recover_note(stem)) return false;
    FILE *f = fopen(temp, "wb");
    if (!f) return false;
    size_t n = strlen(text);
    bool ok = fwrite(text, 1, n, f) == n;
#ifdef ESP_PLATFORM
    ok = fflush(f) == 0 && ok;
    if (ok) ok = fsync(fileno(f)) == 0;
#endif
    ok = fclose(f) == 0 && ok;
    if (!ok) { remove(temp); return false; }
    bool had = stat(path, &st) == 0;
    if (had && !S_ISREG(st.st_mode)) return false;
    if (had && rename(path, backup) != 0) return false;
    if (rename(temp, path) != 0) {
        if (had) rename(backup, path);
        return false;
    }
    if (had) remove(backup);
    return true;
}

/* Called only by the journal I/O worker, after its disk load. Stable IDs
   make each confirmed file the migration cursor, including after reset. */
bool ls_notes_import(uint32_t id, const char *text)
{
    if (!s_started || !id || !text || strlen(text) >= LS_NOTE_TEXT_MAX) return false;
    char stem[LS_NOTE_STEM], path[160]; struct stat st;
    snprintf(stem, sizeof(stem), "journal-%lu", (unsigned long)id);
    if (!recover_note(stem) || !path_of(path, sizeof(path), stem, ".md")) return false;
    if (stat(path, &st) == 0) return S_ISREG(st.st_mode);
    snprintf(path, sizeof(path), "%s/trash/%s.md", s_dir, stem);
    if (stat(path, &st) == 0) return S_ISREG(st.st_mode);
    if (!write_note(stem, text)) return false;
    ls_note_info_t info = {0};
    snprintf(info.stem, sizeof(info.stem), "%s", stem);
    ls_notes_summarize(text, &info); upsert(&info);
    return true;
}

static void scan(void)
{
    lock(); s_scanning = true; unlock();
    ensure_dir(s_dir);
    DIR *d = opendir(s_dir);
    if (!d) { status(LS_TEXT(t_ls_notes_no_sd_card_or, "No SD card or notes folder")); lock(); s_scanning = false; unlock(); return; }
    struct dirent *entry;
    while ((entry = readdir(d))) {
        size_t n = strlen(entry->d_name);
        if (n > 4 && n - 4 < LS_NOTE_STEM && !strcmp(entry->d_name + n - 4, ".bak")) {
            char stem[LS_NOTE_STEM]; memcpy(stem, entry->d_name, n - 4); stem[n - 4] = 0;
            recover_note(stem);
        }
    }
    rewinddir(d);
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
        if (!path_of(path, sizeof(path), info.stem, ".md")) continue;
        if (!read_file(path, s_scan_buf, sizeof(s_scan_buf)) && !s_scan_buf[0]) continue;
        ls_notes_summarize(s_scan_buf, &info);
        upsert(&info);
        found++;
    }
    closedir(d);
    char line[80];
    LS_SNPRINTF(t_ls_notes_d_note_s_on, line, sizeof(line), "%d note%s on the card", found, found == 1 ? "" : "s");
    status(line);
    lock(); s_scanning = false; s_generation++; unlock();
}

static int trash(const char *stem)
{
    char from[160], dir[128], to[200];
    snprintf(dir, sizeof(dir), "%s/trash", s_dir);
    ensure_dir(dir);
    if (!path_of(from, sizeof(from), stem, ".md")) return -1;
    snprintf(to, sizeof(to), "%s/%s.md", dir, stem);
    remove(to);
    bool ok = rename(from, to) == 0, pictures = true;
    /* Its pictures go with it: they are named after the note. */
    DIR *d = opendir(s_dir);
    if (d && ok) {
        struct dirent *e; size_t sn = strlen(stem);
        while ((e = readdir(d)))
            if (!strncmp(e->d_name, stem, sn) && e->d_name[sn] == '-' && strstr(e->d_name, ".png")) {
                char a[400], b[480];
                snprintf(a, sizeof(a), "%s/%s", s_dir, e->d_name);
                snprintf(b, sizeof(b), "%s/%s", dir, e->d_name);
                remove(b); if (rename(a, b) != 0) pictures = false;
            }
        closedir(d);
    }
    if (d && !ok) closedir(d);
    if (ok) forget(stem);
    status(ok ? pictures ? "Moved to notes/trash" : "Note moved; some pictures stayed behind" : "Could not move the note");
    return ok ? pictures ? 1 : 2 : -1;
}

/* ---------------------------------------------------------------- queue -- */

static bool enqueue_tracked(op_t *op, uint32_t *ticket)
{
    if (!s_started) return false;
    lock();
    bool ok = s_qcount < QUEUE_CAP;
    int result = -1;
    if (ticket) {
        for (int i = 0; i < QUEUE_CAP + 2; i++) if (!s_results[i].ticket) { result = i; break; }
        ok = ok && result >= 0;
        if (ok) {
            op->ticket = ++s_ticket; if (!op->ticket) op->ticket = ++s_ticket;
            s_results[result].ticket = op->ticket; s_results[result].result = 0;
            *ticket = op->ticket;
        }
    }
    if (ok) s_queue[(s_qhead + s_qcount++) % QUEUE_CAP] = *op;
    unlock();
    return ok;
}

static bool enqueue(const op_t *op)
{
    op_t copy = *op; return enqueue_tracked(&copy, NULL);
}

/* Pending is zero. Reading a completed result releases its reserved slot. */
int ls_notes_result(uint32_t ticket)
{
    lock(); int result = -1;
    for (int i = 0; i < QUEUE_CAP + 2; i++) if (s_results[i].ticket == ticket) {
        result = s_results[i].result;
        if (result) s_results[i].ticket = 0;
        break;
    }
    unlock(); return result;
}

static void completed(uint32_t ticket, int result)
{
    if (!ticket) return;
    lock();
    for (int i = 0; i < QUEUE_CAP + 2; i++) if (s_results[i].ticket == ticket) s_results[i].result = result;
    unlock();
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
        completed(op.ticket, ok ? 1 : -1);
        give_slot(op.slot);
        status(ok ? "Saved to the SD card" : "SAVE FAILED: check the SD card");
    } else if (op.kind == OP_TRASH) completed(op.ticket, trash(op.stem));
    else if (op.kind == OP_PNG) {
        char path[160];
        bool ok = false;
        ensure_dir(s_dir);
#ifdef ESP_PLATFORM
        ok = path_of(path, sizeof(path), op.stem, ".png") && ls_tui_png_write(s_png, op.w, op.h, path) > 0;
#else
        FILE *f = path_of(path, sizeof(path), op.stem, ".png") ? fopen(path, "wb") : NULL;
        if (f) { ok = fwrite(s_png, 2, (size_t)op.w * (size_t)op.h, f) == (size_t)op.w * (size_t)op.h; if (fclose(f) != 0) ok = false; }
#endif
        completed(op.ticket, ok ? 1 : -1);
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

bool ls_notes_save_tracked(const char *stem, const char *text, uint32_t *ticket)
{
    if (!s_started || !stem || !stem[0] || !text || strlen(text) >= LS_NOTE_TEXT_MAX) return false;
    int slot = take_slot(text);
    if (slot < 0) { status(LS_TEXT(t_ls_notes_still_saving_try_again, "Still saving; try again in a moment")); return false; }
    op_t op = { .kind = OP_SAVE, .slot = slot };
    snprintf(op.stem, sizeof(op.stem), "%s", stem);
    if (!enqueue_tracked(&op, ticket)) { give_slot(slot); return false; }
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

bool ls_notes_create_tracked(const char *text, char *stem_out, size_t cap, uint32_t *ticket)
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
    if (!ls_notes_save_tracked(stem, text, ticket)) { lock(); if (s_next_seq == seq + 1) s_next_seq = seq; unlock(); return false; }
    upsert(&info);
    if (stem_out && cap) snprintf(stem_out, cap, "%s", stem);
    return true;
}

bool ls_notes_trash_tracked(const char *stem, uint32_t *ticket)
{
    if (!stem || !stem[0]) return false;
    op_t op = { .kind = OP_TRASH };
    snprintf(op.stem, sizeof(op.stem), "%s", stem);
    return enqueue_tracked(&op, ticket);
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

bool ls_notes_save_png_tracked(const char *leaf, const uint16_t *px, int w, int h, uint32_t *ticket)
{
    if (!s_started || !leaf || !px || w <= 0 || h <= 0 || (long)w * h > LS_NOTE_PNG_MAX_PX) return false;
    lock(); bool busy = s_png_busy; if (!busy) s_png_busy = true; unlock();
    if (busy) return false;
    memcpy(s_png, px, (size_t)w * (size_t)h * sizeof(uint16_t));
    op_t op = { .kind = OP_PNG, .w = w, .h = h };
    snprintf(op.stem, sizeof(op.stem), "%s", leaf);
    if (!enqueue_tracked(&op, ticket)) { lock(); s_png_busy = false; unlock(); return false; }
    return true;
}

bool ls_notes_save(const char *stem, const char *text) { return ls_notes_save_tracked(stem, text, NULL); }
bool ls_notes_create(const char *text, char *stem, size_t cap) { return ls_notes_create_tracked(text, stem, cap, NULL); }
bool ls_notes_trash(const char *stem) { return ls_notes_trash_tracked(stem, NULL); }
bool ls_notes_save_png(const char *leaf, const uint16_t *px, int w, int h) { return ls_notes_save_png_tracked(leaf, px, w, h, NULL); }

#ifdef LS_NOTES_TEST
void ls_notes_test_reset(const char *directory)
{
    if (!s_lock) s_lock = xSemaphoreCreateMutexStatic(&s_lock_memory);
    snprintf(s_dir, sizeof(s_dir), "%s", directory);
    s_count = s_qhead = s_qcount = 0; s_next_seq = 1; s_generation = 0;
    memset(s_slot_busy, 0, sizeof(s_slot_busy)); s_png_busy = false;
    memset(s_results, 0, sizeof(s_results)); s_ticket = 0;
    s_started = false; s_scanning = false; s_seq_known = false;
}
#endif
