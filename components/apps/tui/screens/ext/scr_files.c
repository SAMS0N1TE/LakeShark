/* FILES: what is on the card, and what can be done with it.

   A directory list first. A .sub opens as a capture - where it sits in the
   band, what its pulses look like over the whole recording, how long they
   are, and what it decodes as - with the things that can be done to it:
   send it to a Flipper, send it on air, delete it. Anything readable as text
   opens as text. */
#include "../../ls_tui_screen.h"
#include "../../ls_tui_ui.h"
#include "../../ls_picker.h"
#include "../../ls_motion.h"
#include "../../ls_rec_replay.h"
#include "../../ls_keyboard.h"
#include "rec_state.h"
#include "rec_watch.h"
#include "subghz_file.h"
#include "subghz_pwm.h"
#include "subghz_nrz.h"
#include "esp_attr.h"
#include "esp_timer.h"
#include "esp_vfs_fat.h"
#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define ROOT "/sdcard"
#define MAX_ENTRIES 256
#define MAX_EDGES 8192
#define TEXT_MAX 8192

typedef struct { char name[64]; uint32_t size; time_t mtime; bool dir; } entry_t;

typedef enum { SORT_NAME, SORT_NEWEST, SORT_OLDEST, SORT_LARGEST, SORT_TYPE, SORT__COUNT } sort_t;
static const char *const SORT_NAME_TEXT[SORT__COUNT] = {"NAME", "NEWEST", "OLDEST", "LARGEST", "TYPE"};
static sort_t s_sort = SORT_NEWEST;

typedef enum { FILTER_ALL, FILTER_SUB, FILTER_IMAGE, FILTER_TEXT, FILTER__COUNT } filter_t;
static const char *const FILTER_LABEL[FILTER__COUNT] = {"ALL", ".SUB", "IMAGES", "TEXT"};
static filter_t s_filter = FILTER_ALL;

/* Where the cursor was in each folder above this one, so coming back up
   lands on the folder that was opened rather than at the top. */
#define DEPTH_MAX 16
static int s_sel_stack[DEPTH_MAX];
static int s_depth;

static EXT_RAM_BSS_ATTR entry_t s_entry[MAX_ENTRIES];
static int s_count;
static bool s_truncated, s_read_failed;
static char s_path[160] = ROOT;
static int s_selected;
static char s_feedback[96];
static int button_focus = -1, button_slot;
static tui_rect s_list;
static bool s_touch_nav;

typedef enum { VIEW_LIST, VIEW_SUB, VIEW_TEXT } view_t;
static view_t s_view;
static char s_open_path[240];

static EXT_RAM_BSS_ATTR int32_t s_edges[MAX_EDGES];
static EXT_RAM_BSS_ATTR subghz_file_t s_sub;
static EXT_RAM_BSS_ATTR char s_line[6144];
static EXT_RAM_BSS_ATTR char s_text[TEXT_MAX + 1];
static int s_scroll, s_scroll_max;
/* ---------------------------------------------------------------- listing */

static const char *ext_of(const char *name);
static bool is_text(const char *name);

/* Folders first in every order: they are where you go, not what you look
   at, and mixing them into a date sort buries them. */
static int entry_cmp(const void *a, const void *b)
{
    const entry_t *x = a, *y = b;
    if (x->dir != y->dir) return x->dir ? -1 : 1;
    int c = 0;
    switch (s_sort) {
    case SORT_NEWEST:  c = x->mtime < y->mtime ? 1 : x->mtime > y->mtime ? -1 : 0; break;
    case SORT_OLDEST:  c = x->mtime < y->mtime ? -1 : x->mtime > y->mtime ? 1 : 0; break;
    case SORT_LARGEST: c = x->size < y->size ? 1 : x->size > y->size ? -1 : 0; break;
    case SORT_TYPE:    c = strcasecmp(ext_of(x->name), ext_of(y->name)); break;
    default: break;
    }
    return c ? c : strcasecmp(x->name, y->name);
}

static bool is_image(const char *name)
{
    const char *e = ext_of(name);
    return !strcasecmp(e, "bmp") || !strcasecmp(e, "png") || !strcasecmp(e, "jpg");
}

static bool passes_filter(const entry_t *e)
{
    if (e->dir) return true;
    switch (s_filter) {
    case FILTER_SUB:   return !strcasecmp(ext_of(e->name), "sub");
    case FILTER_IMAGE: return is_image(e->name);
    case FILTER_TEXT:  return is_text(e->name);
    default:           return true;
    }
}

static void load_dir(void)
{
    s_count = 0;
    s_truncated = false;
    s_read_failed = false;
    DIR *d = opendir(s_path);
    if (!d) { s_read_failed = true; return; }
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        if (e->d_name[0] == '.') continue;
        if (s_count >= MAX_ENTRIES) { s_truncated = true; break; }
        entry_t *t = &s_entry[s_count];
        snprintf(t->name, sizeof(t->name), "%s", e->d_name);
        char full[240];
        snprintf(full, sizeof(full), "%s/%s", s_path, e->d_name);
        struct stat st;
        if (stat(full, &st) == 0) {
            t->dir = S_ISDIR(st.st_mode);
            t->size = (uint32_t)st.st_size;
            t->mtime = st.st_mtime;
        } else {
            t->dir = e->d_type == DT_DIR;
            t->size = 0;
            t->mtime = 0;
        }
        if (passes_filter(t)) s_count++;
    }
    closedir(d);
    qsort(s_entry, (size_t)s_count, sizeof(s_entry[0]), entry_cmp);
    if (s_selected >= s_count) s_selected = s_count ? s_count - 1 : 0;
}

/* Reload, keeping the cursor on the entry it was on - after a sort, a
   refresh or a rename the thing you were looking at should still be under
   your finger. */
static void reload_keep(const char *keep)
{
    char name[64];
    snprintf(name, sizeof(name), "%s", keep ? keep : "");
    load_dir();
    if (!name[0]) return;
    for (int i = 0; i < s_count; i++)
        if (!strcmp(s_entry[i].name, name)) { s_selected = i; return; }
}
static const char *selected_name(void)
{
    return s_selected < s_count ? s_entry[s_selected].name : NULL;
}

static bool at_root(void) { return !strcmp(s_path, ROOT); }

static void go_up(void)
{
    if (s_view != VIEW_LIST) { s_view = VIEW_LIST; return; }
    if (at_root()) return;
    char *slash = strrchr(s_path, '/');
    if (slash && slash != s_path) *slash = 0;
    load_dir();
    s_selected = s_depth > 0 ? s_sel_stack[--s_depth] : 0;
    if (s_selected >= s_count) s_selected = s_count ? s_count - 1 : 0;
}

static const char *ext_of(const char *name)
{
    const char *dot = strrchr(name, '.');
    return dot ? dot + 1 : "";
}

static bool is_text(const char *name)
{
    static const char *const T[] = {"txt","csv","gpx","json","log","md","ini","cfg","nmea","sub"};
    const char *e = ext_of(name);
    for (unsigned i = 0; i < sizeof(T) / sizeof(T[0]); i++)
        if (!strcasecmp(e, T[i])) return true;
    return false;
}

/* FAT keeps a modification time; one before 2000 means the clock was not
   set when it was written, and printing 1980 would look like a fact. */
static void date_text(time_t t, char *out, size_t len)
{
    if (t < 946684800) { snprintf(out, len, "no date"); return; }
    struct tm tm;
    gmtime_r(&t, &tm);
    strftime(out, len, "%Y-%m-%d %H:%M", &tm);
}

/* A name too long for its row loses its middle, not its end: the end is
   where the extension and the timestamp are. */
static void fit_name(const char *name, int width, char *out, size_t len)
{
    const int n = (int)strlen(name);
    if (width < 8 || n <= width) { snprintf(out, len, "%s", name); return; }
    const int tail = (width - 1) / 2, head = width - 1 - tail;
    snprintf(out, len, "%.*s~%s", head, name, name + n - tail);
}

static void size_text(uint32_t n, char *out, size_t len)
{
    if (n < 1024) snprintf(out, len, "%lu B", (unsigned long)n);
    else if (n < 1024 * 1024) snprintf(out, len, "%.1f K", n / 1024.0);
    else snprintf(out, len, "%.1f M", n / 1048576.0);
}

/* ------------------------------------------------------------------ open */

static void open_text(void)
{
    s_text[0] = 0;
    FILE *f = fopen(s_open_path, "r");
    if (!f) { snprintf(s_feedback, sizeof(s_feedback), "Could not open the file"); return; }
    const size_t n = fread(s_text, 1, TEXT_MAX, f);
    fclose(f);
    s_text[n] = 0;
    s_scroll = 0;
    s_view = VIEW_TEXT;
}

static void open_selected(void)
{
    if (s_selected >= s_count) return;
    const entry_t *e = &s_entry[s_selected];
    if (e->dir) {
        if (strlen(s_path) + strlen(e->name) + 2 >= sizeof(s_path)) {
            snprintf(s_feedback, sizeof(s_feedback), "Path too long");
            return;
        }
        if (s_depth < DEPTH_MAX) s_sel_stack[s_depth++] = s_selected;
        strcat(s_path, "/");
        strcat(s_path, e->name);
        s_selected = 0;
        load_dir();
        return;
    }
    snprintf(s_open_path, sizeof(s_open_path), "%s/%s", s_path, e->name);
    s_feedback[0] = 0;
    if (!strcasecmp(ext_of(e->name), "sub")) {
        if (subghz_file_load(s_open_path, &s_sub, s_edges, MAX_EDGES, s_line, sizeof(s_line))) {
            s_scroll = 0;
            s_view = VIEW_SUB;
            return;
        }
        snprintf(s_feedback, sizeof(s_feedback), "Not a SubGhz file - showing it as text");
    }
    if (is_text(e->name)) { open_text(); return; }
    char sz[16];
    size_text(e->size, sz, sizeof(sz));
    snprintf(s_feedback, sizeof(s_feedback), "%s, %s - no viewer for .%s", e->name, sz, ext_of(e->name));
}

/* --------------------------------------------------------------- actions */

static const char *open_name(void)
{
    const char *slash = strrchr(s_open_path, '/');
    return slash ? slash + 1 : s_open_path;
}

/* The Flipper's LakeShark app lists the REC folder and pulls a file from it
   with REC LOAD and REC GET. Putting a copy there is what sends it. */
static void send_to_flipper(void)
{
    const char *dir = rec_dir();
    if (!dir || strncmp(dir, ROOT, strlen(ROOT))) {
        snprintf(s_feedback, sizeof(s_feedback), "No card folder for the Flipper to read");
        return;
    }
    char to[240];
    snprintf(to, sizeof(to), "%s/%s", dir, open_name());
    if (!strcmp(to, s_open_path)) {
        snprintf(s_feedback, sizeof(s_feedback), "Already there: Flipper > LakeShark > FILES");
        return;
    }
    FILE *in = fopen(s_open_path, "rb");
    if (!in) { snprintf(s_feedback, sizeof(s_feedback), "Could not read the file"); return; }
    FILE *out = fopen(to, "wb");
    if (!out) { fclose(in); snprintf(s_feedback, sizeof(s_feedback), "Could not write %s", dir); return; }
    bool ok = true;
    size_t n;
    while ((n = fread(s_line, 1, sizeof(s_line), in)) > 0)
        if (fwrite(s_line, 1, n, out) != n) { ok = false; break; }
    fclose(in);
    if (fclose(out) != 0) ok = false;
    if (!ok) unlink(to);
    snprintf(s_feedback, sizeof(s_feedback), "%s",
             ok ? "Copied. On the Flipper: LakeShark > FILES" : "Copy failed - card full?");
}

static void replay_open(void)
{
    if(!ls_scr_rec_replay_file(s_open_path,&s_sub,s_edges))
        snprintf(s_feedback,sizeof(s_feedback),"Replay busy or file unsupported");
}

static void delete_done(int i)
{
    if (i != 0) return;
    struct stat st;
    const bool dir = stat(s_open_path, &st) == 0 && S_ISDIR(st.st_mode);
    if (dir && rmdir(s_open_path) != 0) {
        snprintf(s_feedback, sizeof(s_feedback), "%s",
                 errno == ENOTEMPTY || errno == EEXIST || errno == EACCES
                     ? "Only an empty folder can be deleted" : "Delete failed");
        return;
    }
    if (dir || unlink(s_open_path) == 0) {
        snprintf(s_feedback, sizeof(s_feedback), "Deleted %s", open_name());
        s_view = VIEW_LIST;
        load_dir();
    } else {
        snprintf(s_feedback, sizeof(s_feedback), "Delete failed (%d)", errno);
    }
}

static char s_rename_from[240];
static void rename_done(const char *name)
{
    if (!name || !name[0]) return;
    if (strchr(name, '/') || !strcmp(name, ".") || !strcmp(name, "..")) {
        snprintf(s_feedback, sizeof(s_feedback), "A name cannot contain /");
        return;
    }
    char to[240];
    snprintf(to, sizeof(to), "%s/%s", s_path, name);
    struct stat st;
    if (stat(to, &st) == 0) { snprintf(s_feedback, sizeof(s_feedback), "%s already exists", name); return; }
    if (rename(s_rename_from, to) == 0) {
        snprintf(s_feedback, sizeof(s_feedback), "Renamed to %s", name);
        s_view = VIEW_LIST;
        reload_keep(name);
    } else {
        snprintf(s_feedback, sizeof(s_feedback), "Rename failed (%d)", errno);
    }
}
static void info_done(int i) { (void)i; }
static void info_open(void)
{
    struct stat st;
    if (stat(s_open_path, &st) != 0) { snprintf(s_feedback, sizeof(s_feedback), "Could not read it"); return; }
    char size[24], date[24];
    size_text((uint32_t)st.st_size, size, sizeof(size));
    date_text(st.st_mtime, date, sizeof(date));
    ls_picker_open(open_name(), info_done);
    ls_picker_add("TYPE", S_ISDIR(st.st_mode) ? "folder" : ext_of(s_open_path)[0] ? ext_of(s_open_path) : "file");
    if (!S_ISDIR(st.st_mode)) {
        char exact[32];
        snprintf(exact, sizeof(exact), "%s (%lu bytes)", size, (unsigned long)st.st_size);
        ls_picker_add("SIZE", exact);
    }
    ls_picker_add("MODIFIED", date);
    ls_picker_add("FOLDER", s_path);
}

static char s_menu[8];
static int s_menu_n;
static void menu_add(char id, const char *label, const char *detail)
{
    if (s_menu_n < (int)sizeof(s_menu) && ls_picker_add(label, detail)) s_menu[s_menu_n++] = id;
}
static void actions_done(int i)
{
    if (i < 0 || i >= s_menu_n) return;
    switch (s_menu[i]) {
    case 'F': send_to_flipper(); break;
    case 'R': replay_open(); break;
    case 'T': open_text(); break;
    case 'I': info_open(); break;
    case 'N':
        snprintf(s_rename_from, sizeof(s_rename_from), "%s", s_open_path);
        ls_keyboard_open("RENAME", open_name(), 63, rename_done);
        break;
    case 'D': {
        char title[48];
        snprintf(title, sizeof(title), "DELETE %s?", open_name());
        ls_picker_open(title, delete_done);
        ls_picker_add("DELETE", "It cannot be brought back");
        ls_picker_add("KEEP", "Leave it on the card");
        break;
    }
    default: break;
    }
}
static void actions_open(void)
{
    bool dir = false;
    if (s_view == VIEW_LIST) {
        if (s_selected >= s_count) {
            snprintf(s_feedback, sizeof(s_feedback), "Nothing selected");
            return;
        }
        dir = s_entry[s_selected].dir;
        snprintf(s_open_path, sizeof(s_open_path), "%s/%s", s_path, s_entry[s_selected].name);
    }
    const bool sub = !dir && !strcasecmp(ext_of(open_name()), "sub");
    char title[48];
    snprintf(title, sizeof(title), "%s", open_name());
    s_menu_n = 0;
    ls_picker_open(title, actions_done);
    if (sub) {
        menu_add('F', "SEND TO FLIPPER", "Copy where the Flipper app reads");
        if (s_view == VIEW_SUB && subghz_file_is_fsk(&s_sub))
            menu_add('R', "REPLAY", "Open in RECORD / SX1262");
        else if (s_view == VIEW_SUB && subghz_file_is_ook(&s_sub))
            menu_add('R', "REPLAY", "Open in RECORD / CC1101");
        else if (s_view == VIEW_SUB)
            menu_add('.', "REPLAY UNAVAILABLE", "Incomplete or unsupported RAW/modulation");
    }
    if (!dir && is_text(open_name()) && s_view != VIEW_TEXT) menu_add('T', "VIEW AS TEXT", "The file as written");
    menu_add('I', "INFO", "Size, date, where it is");
    menu_add('N', "RENAME", "Type a new name");
    menu_add('D', "DELETE", dir ? "Only if the folder is empty" : "Remove it from the card");
}

static void sort_done(int i)
{
    if (i < 0 || i >= SORT__COUNT) return;
    s_sort = (sort_t)i;
    reload_keep(selected_name());
    snprintf(s_feedback, sizeof(s_feedback), "Sorted by %s", SORT_NAME_TEXT[i]);
}
static void sort_open(void)
{
    static const char *const WHY[SORT__COUNT] = {"A to Z", "Most recent at the top",
        "Oldest at the top", "Biggest at the top", "Grouped by extension"};
    ls_picker_open("SORT", sort_done);
    for (int i = 0; i < SORT__COUNT; i++)
        ls_picker_add(SORT_NAME_TEXT[i], i == (int)s_sort ? "in use" : WHY[i]);
}
static void filter_done(int i)
{
    if (i < 0 || i >= FILTER__COUNT) return;
    s_filter = (filter_t)i;
    reload_keep(selected_name());
}
static void goto_path(const char *path)
{
    snprintf(s_path, sizeof(s_path), "%s", path);
    s_depth = 0;
    s_selected = 0;
    s_view = VIEW_LIST;
    load_dir();
    if (s_read_failed) snprintf(s_feedback, sizeof(s_feedback), "%s is not on this card", path);
}
static const char *const GOTO_PATH[] = {ROOT, ROOT "/lakeshark", ROOT "/subghz", ROOT "/journal", ROOT "/maps"};
static void more_done(int i)
{
    if (i < 0 || i >= s_menu_n) return;
    const char id = s_menu[i];
    if (id >= '0' && id <= '4') { goto_path(GOTO_PATH[id - '0']); return; }
    if (id == 'F') {
        ls_picker_open("SHOW", filter_done);
        static const char *const WHY[FILTER__COUNT] = {"Everything", "Sub-GHz captures",
            "Screenshots and pictures", "Notes, logs, tracks"};
        for (int k = 0; k < FILTER__COUNT; k++)
            ls_picker_add(FILTER_LABEL[k], k == (int)s_filter ? "in use" : WHY[k]);
        return;
    }
    if (id == 'R') { s_view = VIEW_LIST; reload_keep(selected_name()); snprintf(s_feedback, sizeof(s_feedback), "Refreshed"); return; }
    if (id == 'C') {
        uint64_t total = 0, free_b = 0;
        if (esp_vfs_fat_info(ROOT, &total, &free_b) == ESP_OK)
            snprintf(s_feedback, sizeof(s_feedback), "Card: %.1f GB free of %.1f GB",
                     free_b / 1073741824.0, total / 1073741824.0);
        else snprintf(s_feedback, sizeof(s_feedback), "No card mounted");
    }
}
static void more_open(void)
{
    s_menu_n = 0;
    ls_picker_open("MORE", more_done);
    menu_add('1', "CAPTURES", "REC recordings - what the Flipper reads");
    menu_add('2', "SUB-GHZ", "Watch exports and archives");
    menu_add('3', "JOURNAL", "Field notes and tracks");
    menu_add('0', "CARD ROOT", ROOT);
    menu_add('F', "SHOW", FILTER_LABEL[s_filter]);
    menu_add('R', "REFRESH", "Read the folder again");
    menu_add('C', "CARD SPACE", "How full it is");
}

static void action(char c)
{
    s_feedback[0] = 0;
    switch (c) {
    case 'u': go_up(); break;
    case 'o': if (s_view == VIEW_LIST) open_selected(); break;
    case 'a': actions_open(); break;
    case 's': sort_open(); break;
    case 'm': more_open(); break;
    default: break;
    }
}

/* ------------------------------------------------------------------ draw */

/* The whole recording, row after row like lines of text, each cell showing
   how much of its slice of time the carrier was on. Bursts, gaps and
   repeats show up as shape before any number is read. */
static void sub_overview(tui_surface *sf, tui_rect r)
{
    if (r.w < 8 || r.h < 1 || !s_sub.edges || !s_sub.span_us) return;
    const uint64_t cells = (uint64_t)r.w * (uint64_t)r.h;
    const double dt = (double)s_sub.span_us / (double)cells;
    int e = 0;
    double edge_start = 0, edge_end = s_edges[0] < 0 ? -s_edges[0] : s_edges[0];
    for (uint64_t c = 0; c < cells; c++) {
        const double a = c * dt, b = a + dt;
        double on = 0;
        while (e < s_sub.edges) {
            const double lo = edge_start > a ? edge_start : a;
            const double hi = edge_end < b ? edge_end : b;
            if (hi > lo && s_edges[e] > 0) on += hi - lo;
            if (edge_end > b) break;
            e++;
            if (e < s_sub.edges) {
                edge_start = edge_end;
                edge_end += s_edges[e] < 0 ? -s_edges[e] : s_edges[e];
            }
        }
        const double frac = on / dt;
        const char g = frac > 0.85 ? '#' : frac > 0.4 ? '=' : frac > 0.02 ? '-' : '_';
        const uint8_t at = frac > 0.02 ? TUI_ATTR(TUI_GREEN | TUI_BRIGHT, TUI_BLACK)
                                       : TUI_ATTR(TUI_BLUE, TUI_BLACK);
        tui_put_char(sf, r, r.x + (int)(c % (uint64_t)r.w), r.y + (int)(c / (uint64_t)r.w), g, at);
    }
}

/* How long the pulses are, on a doubling scale, marks and spaces apart.
   Two tall bars an octave or so apart is pulse-width keying; one bar is a
   carrier being switched at a fixed rate. */
static int sub_histogram(tui_surface *sf, tui_rect r)
{
    enum { BINS = 14 };
    uint32_t hi[BINS] = {0}, lo[BINS] = {0}, top = 1;
    for (int i = 0; i < s_sub.edges; i++) {
        uint32_t d = (uint32_t)(s_edges[i] < 0 ? -s_edges[i] : s_edges[i]);
        int b = 0;
        while (b < BINS - 1 && d >= (32u << b)) b++;
        if (s_edges[i] > 0) hi[b]++; else lo[b]++;
    }
    for (int b = 0; b < BINS; b++) { if (hi[b] > top) top = hi[b]; if (lo[b] > top) top = lo[b]; }
    const int label_w = 13, half = (r.w - label_w - 2) / 2;
    if (half < 4) return 0;
    tui_put_str(sf, r, r.x, r.y, "PULSE LENGTH", LS_ATTR_DIM);
    tui_put_str(sf, r, r.x + label_w, r.y, "MARK", TUI_ATTR(TUI_GREEN | TUI_BRIGHT, TUI_BLACK));
    tui_put_str(sf, r, r.x + label_w + half + 1, r.y, "SPACE", TUI_ATTR(TUI_CYAN | TUI_BRIGHT, TUI_BLACK));
    int y = r.y + 1;
    for (int b = 0; b < BINS && y < r.y + r.h; b++) {
        if (!hi[b] && !lo[b]) continue;
        char label[16];
        const uint32_t from = b ? (32u << (b - 1)) : 0;
        if (from < 1000) snprintf(label, sizeof(label), "%5lu us", (unsigned long)from);
        else snprintf(label, sizeof(label), "%5.1f ms", from / 1000.0);
        tui_put_str(sf, r, r.x, y, label, LS_ATTR_DIM);
        const int wh = (int)((uint64_t)hi[b] * (uint64_t)(half - 1) / top);
        const int wl = (int)((uint64_t)lo[b] * (uint64_t)(half - 1) / top);
        for (int x = 0; x < wh; x++)
            tui_put_char(sf, r, r.x + label_w + x, y, '#', TUI_ATTR(TUI_GREEN | TUI_BRIGHT, TUI_BLACK));
        if (hi[b] && !wh) tui_put_char(sf, r, r.x + label_w, y, '.', TUI_ATTR(TUI_GREEN, TUI_BLACK));
        for (int x = 0; x < wl; x++)
            tui_put_char(sf, r, r.x + label_w + half + 1 + x, y, '#', TUI_ATTR(TUI_CYAN | TUI_BRIGHT, TUI_BLACK));
        if (lo[b] && !wl) tui_put_char(sf, r, r.x + label_w + half + 1, y, '.', TUI_ATTR(TUI_CYAN, TUI_BLACK));
        y++;
    }
    return y - r.y;
}

static void draw_sub(tui_surface *sf, tui_rect p)
{
    char line[120];
    const int x = p.x + 2, w = p.w - 4;
    int y = p.y + 1;
    tui_rect in = tui_rect_make(x, p.y + 1, w, p.h - 2);
    char name[80];
    fit_name(open_name(), w, name, sizeof(name));
    tui_put_str(sf, in, x, y++, name, TUI_ATTR(TUI_YELLOW | TUI_BRIGHT, TUI_BLACK));
    snprintf(line, sizeof(line), "%.4f MHz   %s", s_sub.freq_hz / 1e6,
             s_sub.protocol[0] ? s_sub.protocol : "?");
    tui_put_str(sf, in, x, y++, line, TUI_ATTR(TUI_WHITE | TUI_BRIGHT, TUI_BLACK));
    if (subghz_file_is_fsk(&s_sub))
        snprintf(line, sizeof(line), "2-FSK %lu baud, %.1f kHz dev, sync %08lX",
                 (unsigned long)s_sub.bitrate, s_sub.deviation_hz / 1000.0,
                 (unsigned long)s_sub.sync_word);
    else
        snprintf(line, sizeof(line), "%s", strstr(s_sub.preset, "Ook") ? "OOK (amplitude keyed)" :
                 s_sub.preset[0] ? s_sub.preset : "no preset");
    tui_put_str(sf, in, x, y++, line, LS_ATTR_DIM);
    snprintf(line, sizeof(line), "%d edges%s  %.1f ms", s_sub.edges_total,
             s_sub.edges < s_sub.edges_total ? " (first shown)" : "", s_sub.span_us / 1000.0);
    tui_put_str(sf, in, x, y++, line, LS_ATTR_DIM);

    /* What it decodes as, where the encoding is one this board knows. */
    char what[64] = "";
    subghz_pwm_t pwm;
    subghz_nrz_t nrz;
    rec_ook24_t ook;
    if (subghz_pwm_decode(s_edges, s_sub.edges, &pwm)) subghz_pwm_format(&pwm, what, sizeof(what));
    if (!what[0] && subghz_nrz_decode(s_edges, s_sub.edges, &nrz)) subghz_nrz_format(&nrz, what, sizeof(what));
    if (!what[0] && rec_decode_ook24(s_edges, s_sub.edges, &ook))
        snprintf(what, sizeof(what), "OOK24 %06lX x%u", (unsigned long)ook.value, ook.repeats);
    snprintf(line, sizeof(line), "DECODE  %s", what[0] ? what : "no known encoding");
    tui_put_str(sf, in, x, y++, line, what[0] ? TUI_ATTR(TUI_CYAN | TUI_BRIGHT, TUI_BLACK) : LS_ATTR_DIM);
    y++;

    const int left = p.y + p.h - 1 - y;
    if (left < 6) return;
    /* Overview gets what the histogram does not need, capped so a tall
       portrait pane does not stretch a short capture into dust. */
    const int hist_rows = left > 24 ? 10 : 6;
    int ov_rows = left - hist_rows - 3;
    if (ov_rows > 24) ov_rows = 24;
    if (ov_rows >= 2) {
        snprintf(line, sizeof(line), "WHOLE CAPTURE  %.2f ms a row   # on  _ off",
                 s_sub.span_us / 1000.0 / ov_rows);
        tui_put_str(sf, in, x, y++, line, LS_ATTR_DIM);
        sub_overview(sf, tui_rect_make(x, y, w, ov_rows));
        y += ov_rows + 1;
    }
    sub_histogram(sf, tui_rect_make(x, y, w, p.y + p.h - 1 - y));
}

static void draw_text(tui_surface *sf, tui_rect a)
{
    int row = 0, col = 0;
    const int width = a.w;
    if (width < 1) return;
    int lines = 1, used = 0;
    for (const char *p = s_text; *p; p++) {
        if (*p == '\n' || used >= width) { lines++; used = 0; if (*p == '\n') continue; }
        used++;
    }
    s_scroll_max = lines > a.h ? lines - a.h : 0;
    if (s_scroll > s_scroll_max) s_scroll = s_scroll_max;
    for (const char *p = s_text; *p; p++) {
        if (*p == '\r') continue;
        if (*p == '\n' || col >= width) { row++; col = 0; if (*p == '\n') continue; }
        if (row >= s_scroll && row < s_scroll + a.h)
            tui_put_char(sf, a, a.x + col, a.y + row - s_scroll,
                         (*p >= 32 && *p < 127) ? *p : '.', LS_ATTR_DIM);
        col++;
    }
}

/* Portrait rows are two lines: the name on its own line has room, the
   date and size go under it, and a two-line row is a target a thumb can
   hit. Landscape has the width for one line with a date column. */
static bool s_tall_rows;
static int row_h(void) { return s_tall_rows ? 2 : 1; }
static int list_rows(void)
{
    const int n = s_list.h / row_h();
    return n > 0 ? n : 1;
}

static void draw_list(tui_surface *sf, tui_rect p)
{
    s_list = tui_rect_make(p.x + 2, p.y + 2, p.w - 4, p.h - 3);
    s_tall_rows = s_list.w < 70;
    tui_put_str(sf, p, p.x + 2, p.y + 1, s_path, TUI_ATTR(TUI_YELLOW | TUI_BRIGHT, TUI_BLACK));
    {
        char how[40];
        snprintf(how, sizeof(how), "%s%s%s", SORT_NAME_TEXT[s_sort],
                 s_filter ? "  " : "", s_filter ? FILTER_LABEL[s_filter] : "");
        const int n = (int)strlen(how);
        if ((int)strlen(s_path) + n + 4 < p.w)
            tui_put_str(sf, p, p.x + p.w - n - 2, p.y + 1, how, LS_ATTR_DIM);
    }
    if (s_read_failed) {
        tui_put_str(sf, p, s_list.x, s_list.y + 1, "No card, or this folder cannot be read.", LS_ATTR_DIM);
        tui_put_str(sf, p, s_list.x, s_list.y + 2, "REFRESH tries again.", LS_ATTR_DIM);
        return;
    }
    if (!s_count) {
        tui_put_str(sf, p, s_list.x, s_list.y + 1, "Empty folder.", LS_ATTR_DIM);
        return;
    }
    const int rows = list_rows();
    const int first = s_selected / rows * rows;
    const int rh = row_h();
    for (int i = 0; i < rows && first + i < s_count; i++) {
        const entry_t *e = &s_entry[first + i];
        const int y = s_list.y + i * rh;
        const bool sel = first + i == s_selected;
        if (sel) ls_fill_dither(sf, tui_rect_make(s_list.x, y, s_list.w, rh), LS_DITHER_LIGHT, TUI_CYAN);
        char full[80], name[80];
        snprintf(full, sizeof(full), "%s%s", e->name, e->dir ? "/" : "");
        fit_name(full, rh == 2 ? s_list.w - 2 : s_list.w - 30, name, sizeof(name));
        const bool sub = !e->dir && !strcasecmp(ext_of(e->name), "sub");
        const uint8_t hue = e->dir ? TUI_YELLOW : sub ? TUI_GREEN : TUI_WHITE;
        tui_put_str(sf, s_list, s_list.x + 1, y, name, TUI_ATTR(hue | TUI_BRIGHT, TUI_BLACK));
        char sz[16] = "", date[24], meta[48];
        if (!e->dir) size_text(e->size, sz, sizeof(sz));
        date_text(e->mtime, date, sizeof(date));
        if (rh == 2) {
            snprintf(meta, sizeof(meta), "%s%s%s", date, sz[0] ? "   " : "", sz);
            tui_put_str(sf, s_list, s_list.x + 1, y + 1, meta, LS_ATTR_DIM);
        } else {
            snprintf(meta, sizeof(meta), "%-16s %9s", date, sz);
            const int n = (int)strlen(meta);
            if ((int)strlen(name) + n + 3 < s_list.w)
                tui_put_str(sf, s_list, s_list.x + s_list.w - n - 1, y, meta, LS_ATTR_DIM);
        }
    }
    if (s_count > rows) {
        char pos[24];
        snprintf(pos, sizeof(pos), " %d/%d ", s_selected + 1, s_count);
        tui_put_str(sf, p, p.x + p.w - (int)strlen(pos) - 2, p.y + p.h - 1, pos, LS_ATTR_DIM);
    }
}

static void draw(tui_surface *sf, tui_rect a)
{
    if (a.h < 14 || a.w < 24) {
        ls_panel_box(sf, a, "FILES", TUI_CYAN);
        tui_put_str(sf, a, a.x + 1, a.y + 1, "Enlarge the pane", LS_ATTR_DIM);
        return;
    }
    s_touch_nav = a.w < 90 && a.h > 35;
    if (s_touch_nav) {
        const bool list = s_view == VIEW_LIST;
        const int rows = list_rows();
        ls_btn_t nav[] = {
            {"PREV", "PAGE", 0, false, list ? s_selected < rows : s_scroll == 0},
            {"NEXT", "PAGE", 0, false, list ? s_selected / rows * rows + rows >= s_count
                                            : s_scroll >= s_scroll_max},
            {"BACK", list ? "FOLDER" : "LIST", 0, false, list && at_root()}};
        ls_btn_bar_raised_slot(sf, tui_rect_make(a.x, a.y + a.h - 5, a.w, 5), nav, 3, -1,
                               LS_BTN_SLOT_WATERFALL);
        a.h -= 5;
    }
    const bool list = s_view == VIEW_LIST;
    const bool sel_file = list && s_selected < s_count && !s_entry[s_selected].dir;
    ls_btn_t btn[] = {
        {"UP", list ? "FOLDER" : "LIST", 'u', false, list && at_root()},
        {"OPEN", list && s_selected < s_count && s_entry[s_selected].dir ? "FOLDER" : "FILE", 'o', false,
         !list || s_selected >= s_count},
        {"ACTIONS", list && s_selected < s_count && s_entry[s_selected].dir ? "FOLDER" : "FILE", 'a',
         false, list && s_selected >= s_count},
        {"SORT", SORT_NAME_TEXT[s_sort], 's', false, !list},
        {"MORE", "GO TO", 'm', false, false}};
    (void)sel_file;
    const int bar_h = ls_btn_raised_height(a, 5);
    ls_btn_bar_raised(sf, tui_rect_make(a.x, a.y, a.w, bar_h), btn, 5, button_focus);
    tui_rect panel = tui_rect_make(a.x, a.y + bar_h, a.w, a.h - bar_h - 1);
    char title[40];
    if (list) snprintf(title, sizeof(title), "FILES  %d%s", s_count, s_truncated ? "+" : "");
    else snprintf(title, sizeof(title), "%s", s_view == VIEW_SUB ? "CAPTURE" : "TEXT");
    ls_panel_box(sf, panel, title, TUI_CYAN);
    if (s_view == VIEW_SUB) draw_sub(sf, panel);
    else if (s_view == VIEW_TEXT)
        draw_text(sf, tui_rect_make(panel.x + 2, panel.y + 1, panel.w - 4, panel.h - 2));
    else draw_list(sf, panel);
    ls_safe_line(sf, a, a.y + a.h - 1,
                 s_feedback[0] ? s_feedback :
                 list ? "Tap to select, tap again to open" : "ACTIONS: send, replay, delete",
                 LS_ATTR_DIM);
}

/* ----------------------------------------------------------------- input */

static void enter(void)
{
    button_focus = -1;
    button_slot = 0;
    s_feedback[0] = 0;
    if (s_view == VIEW_LIST) load_dir();
}

static void page(int dir)
{
    if (s_view != VIEW_LIST) {
        const int step = 10;
        s_scroll += dir * step;
        if (s_scroll < 0) s_scroll = 0;
        if (s_scroll > s_scroll_max) s_scroll = s_scroll_max;
        return;
    }
    const int rows = list_rows();
    s_selected += dir * rows;
    if (s_selected < 0) s_selected = 0;
    if (s_selected >= s_count) s_selected = s_count ? s_count - 1 : 0;
}

static bool key(ls_tk_t k, char ch)
{
    if (ch >= 'A' && ch <= 'Z') ch += 'a' - 'A';
    if (k == LS_TK_LEFT || k == LS_TK_RIGHT || k == LS_TK_TAB)
        return ls_btn_navigate(k, &button_slot, &button_focus, false);
    if (k == LS_TK_ENTER && button_focus >= 0) {
        if (ls_btn_enabled(0, button_focus)) action("uoasm"[button_focus]);
        return true;
    }
    if (k == LS_TK_UP || k == LS_TK_DOWN || k == LS_TK_BACKSPACE) button_focus = -1;
    if (k == LS_TK_BACKSPACE || k == LS_TK_ESC) { go_up(); return true; }
    if (k == LS_TK_UP) {
        if (s_view == VIEW_LIST) { if (s_selected) s_selected--; }
        else if (s_scroll) s_scroll--;
        return true;
    }
    if (k == LS_TK_DOWN) {
        if (s_view == VIEW_LIST) { if (s_selected + 1 < s_count) s_selected++; }
        else if (s_scroll < s_scroll_max) s_scroll++;
        return true;
    }
    if (k == LS_TK_ENTER) { if (s_view == VIEW_LIST) open_selected(); return true; }
    if (k != LS_TK_CHAR || !strchr("uoasm", ch)) return false;
    action(ch);
    return true;
}

static bool touch(int col, int row)
{
    if (s_touch_nav) {
        const int i = ls_btn_hit_slot(col, row, LS_BTN_SLOT_WATERFALL);
        if (i == 0) { page(-1); return true; }
        if (i == 1) { page(1); return true; }
        if (i == 2) { go_up(); return true; }
    }
    const int b = ls_btn_hit(col, row);
    if (b >= 0) { if (ls_btn_enabled(0, b)) action("uoasm"[b]); return true; }
    if (s_view != VIEW_LIST) return true;
    if (col < s_list.x || col >= s_list.x + s_list.w || row < s_list.y || row >= s_list.y + s_list.h)
        return true;
    const int rows = list_rows();
    const int index = s_selected / rows * rows + (row - s_list.y) / row_h();
    if (index < s_count) {
        if (index == s_selected) open_selected();
        else s_selected = index;
    }
    return true;
}

const ls_tui_screen_t ls_scr_files = {
    .name = "FILES", .hint = "ENTER open  BS up  A actions  S sort  M more",
    .enter = enter, .draw = draw, .key = key, .touch = touch};
