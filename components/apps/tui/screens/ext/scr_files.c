/* FILES: what is on the card, and what can be done with it.

   A directory list first. A .sub opens as a capture - where it sits in the
   band, what its pulses look like over the whole recording, how long they
   are, and what it decodes as - with the things that can be done to it:
   send it to a Flipper, send it on air, delete it. Anything readable as text
   opens as text. */
#include "../../ls_tui_screen.h"
#include "../../ls_tui_ui.h"
#include "../../ls_picker.h"
#include "../../ls_radio_select.h"
#include "../../ls_motion.h"
#include "../../ls_media_widgets.h"
#include "../../ls_rec_replay.h"
#include "../../ls_keyboard.h"
#include "rec_state.h"
#include "rec_watch.h"
#include "rec_file_open.h"
#include "subghz_file.h"
#include "subghz_pwm.h"
#include "subghz_nrz.h"
#include "esp_attr.h"
#include "esp_timer.h"
#include "esp_vfs_fat.h"
#include "ls_sdcard.h"
#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include "ls_textpack.h"

#define ROOT "/sdcard"
#define MAX_ENTRIES 256
#define MAX_EDGES 8192
#define TEXT_MAX 8192
#define PATH_MAX_LEN 416

typedef struct { char name[256]; uint32_t size; time_t mtime; bool dir; } entry_t;

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
static EXT_RAM_BSS_ATTR char s_open_path[PATH_MAX_LEN];
static char s_decode[64];

static EXT_RAM_BSS_ATTR int32_t s_edges[MAX_EDGES];
static EXT_RAM_BSS_ATTR subghz_file_t s_sub;
static EXT_RAM_BSS_ATTR char s_line[SUBGHZ_FILE_LINE_BYTES];
static EXT_RAM_BSS_ATTR char s_text[TEXT_MAX + 1];
static int s_scroll, s_scroll_max;
static size_t s_text_len;
static long s_text_offset, s_text_total;
static int s_text_rows = 1;
#define JOB_MAX_BYTES (1024 * 1024)
typedef enum { JOB_NONE, JOB_DIR, JOB_SUB, JOB_COPY } job_t;
static job_t s_job;
static DIR *s_dir;
static FILE *s_in, *s_out;
static EXT_RAM_BSS_ATTR char s_copy_path[PATH_MAX_LEN], s_copy_dir[PATH_MAX_LEN];
static const char *s_copy_note;
static unsigned long s_copy_limit;
static char s_keep[256];
static unsigned long s_job_bytes;
static bool s_replay_menu;
static int s_copy_suffix;
static void job_cancel(void);
static void job_step(void);
/* ---------------------------------------------------------------- listing */

static const char *ext_of(const char *name);
static bool is_text(const char *name);
static bool is_audio(const char *name);
static void play_audio(void);

/* A shortened path must never address a different entry on the card. */
static bool join_path(char *out, size_t len, const char *dir, const char *name)
{
    const int n = snprintf(out, len, "%s/%s", dir, name);
    if (n >= 0 && (size_t)n < len) return true;
    out[0] = 0;
    snprintf(s_feedback, sizeof(s_feedback), "Path too long");
    return false;
}

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
    job_cancel();
    s_count = 0;
    s_truncated = false;
    s_read_failed = false;
    DIR *d = opendir(s_path);
    if (!d) { s_read_failed = true; return; }
    s_dir = d;
    s_job = JOB_DIR;
    s_job_bytes = 0;
    s_keep[0] = 0;
}

/* Reload, keeping the cursor on the entry it was on - after a sort, a
   refresh or a rename the thing you were looking at should still be under
   your finger. */
static void reload_keep(const char *keep)
{
    char name[256];
    snprintf(name, sizeof(name), "%s", keep ? keep : "");
    load_dir();
    snprintf(s_keep, sizeof(s_keep), "%s", name);
}
static const char *selected_name(void)
{
    return s_selected < s_count ? s_entry[s_selected].name : NULL;
}

static bool at_root(void) { return !strcmp(s_path, ROOT); }

/* The screen that sent the operator here to pick a file, or -1. BACK at the
   top of the card goes there rather than nowhere: arriving from REC's
   BROWSE and then having no way back except through the home is how this
   screen was first reported. */
static int s_return_to = -1;
static void (*s_on_return)(void);
void ls_scr_files_return_to(int screen) { s_return_to = screen; }
static bool can_go_up(void)
{
    return s_view != VIEW_LIST || !at_root() || s_return_to >= 0 || s_on_return;
}

/* False at the top of the card with nobody to return to, so the router
   takes ESC home as it does on every other screen. */
static bool go_up(void)
{
    if (s_view != VIEW_LIST) { s_view = VIEW_LIST; return true; }
    if(s_on_return) {
        void (*done)(void)=s_on_return;int to=s_return_to;
        s_on_return=NULL;s_return_to=-1;
        if(to>=0)ls_tui_screen_show(to);
        done();return true;
    }
    if (at_root()) {
        if (s_return_to < 0) return false;
        const int to = s_return_to;
        s_return_to = -1;
        ls_tui_screen_show(to);
        return true;
    }
    char *slash = strrchr(s_path, '/');
    if (slash && slash != s_path) *slash = 0;
    load_dir();
    s_selected = s_depth > 0 ? s_sel_stack[--s_depth] : 0;
    return true;
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

/* Cache even a failed decode: an unchanged capture has the same answer. */
static void decode_sub(void)
{
    subghz_pwm_t pwm;
    subghz_nrz_t nrz;
    rec_ook24_t ook;
    s_decode[0] = 0;
    if (subghz_pwm_decode(s_edges, s_sub.edges, &pwm))
        subghz_pwm_format(&pwm, s_decode, sizeof(s_decode));
    if (!s_decode[0] && subghz_nrz_decode(s_edges, s_sub.edges, &nrz))
        subghz_nrz_format(&nrz, s_decode, sizeof(s_decode));
    if (!s_decode[0] && rec_decode_ook24(s_edges, s_sub.edges, &ook))
        snprintf(s_decode, sizeof(s_decode), "OOK24 %06lX x%u", (unsigned long)ook.value, ook.repeats);
}

static bool text_read(long offset)
{
    FILE *f = fopen(s_open_path, "rb");
    if (!f) { LS_SNPUTS(t_scr_files_could_not_open_the, s_feedback, sizeof(s_feedback), "Could not open the file"); return false; }
    long total;
    if (fseek(f, 0, SEEK_END) != 0 || (total = ftell(f)) < 0 || fseek(f, offset, SEEK_SET) != 0) {
        fclose(f); LS_SNPUTS(t_scr_files_could_not_seek_in, s_feedback, sizeof(s_feedback), "Could not seek in the file"); return false;
    }
    const size_t n = fread(s_text, 1, TEXT_MAX, f);
    const bool ok = !ferror(f);
    fclose(f);
    if (!ok) { s_text_len = 0; snprintf(s_feedback, sizeof(s_feedback), "Text read failed"); return false; }
    s_text_total = total;
    s_text_len = n;
    s_text_offset = offset;
    s_text[n] = 0;
    s_scroll = 0;
    s_scroll_max = 0;
    return true;
}
static void open_text(void)
{
    if (!text_read(0)) return;
    s_view = VIEW_TEXT;
}

static void open_selected(void)
{
    if (s_selected >= s_count) return;
    const entry_t *e = &s_entry[s_selected];
    if (e->dir) {
        if (s_depth == DEPTH_MAX) {
            LS_SNPUTS(t_scr_files_folder_depth_limit_reached, s_feedback, sizeof(s_feedback), "Folder depth limit reached");
            return;
        }
        if (strlen(s_path) + strlen(e->name) + 2 >= sizeof(s_path)) {
            snprintf(s_feedback, sizeof(s_feedback), "Path too long");
            return;
        }
        s_sel_stack[s_depth++] = s_selected;
        strcat(s_path, "/");
        strcat(s_path, e->name);
        s_selected = 0;
        load_dir();
        return;
    }
    if (!join_path(s_open_path, sizeof(s_open_path), s_path, e->name)) return;
    s_feedback[0] = 0;
    if (!strcasecmp(ext_of(e->name), "sub")) {
        struct stat st;
        if (stat(s_open_path, &st) != 0 || st.st_size > JOB_MAX_BYTES) {
            LS_SNPUTS(t_scr_files_capture_too_large_or, s_feedback, sizeof(s_feedback), "Capture too large or unreadable (limit 1 MB)"); return;
        }
        s_in = fopen(s_open_path, "r");
        if (!s_in) { LS_SNPUTS(t_scr_files_could_not_open_capture, s_feedback, sizeof(s_feedback), "Could not open capture"); return; }
        subghz_file_begin(&s_sub);
        s_job = JOB_SUB;
        s_job_bytes = 0;
        return;
    }
    if (is_text(e->name)) { open_text(); return; }
    if (is_audio(e->name)) { play_audio(); return; }
    char sz[16];
    size_text(e->size, sz, sizeof(sz));
    LS_SNPRINTF(t_scr_files_s_s_no_viewer, s_feedback, sizeof(s_feedback), "%s, %s - no viewer for .%s", e->name, sz, ext_of(e->name));
}

/* --------------------------------------------------------------- actions */

static const char *open_name(void)
{
    const char *slash = strrchr(s_open_path, '/');
    return slash ? slash + 1 : s_open_path;
}

/* Copy the open file to `to`, a block at a time from job_step. A name taken
   there already gets -1, -2, ... rather than being written over. */
static void copy_begin(const char *dir, const char *to, unsigned long limit, const char *note)
{
    FILE *in = fopen(s_open_path, "rb");
    if (!in) { LS_SNPUTS(t_scr_files_could_not_read_the, s_feedback, sizeof(s_feedback), "Could not read the file"); return; }
    s_in = in;
    s_out = NULL;
    s_copy_suffix = 0;
    snprintf(s_copy_dir, sizeof(s_copy_dir), "%s", dir);
    snprintf(s_copy_path, sizeof(s_copy_path), "%s", to);
    s_copy_limit = limit;
    s_copy_note = note;
    s_job_bytes = 0;
    s_job = JOB_COPY;
}

/* The Flipper's LakeShark app lists the REC folder and pulls a file from it
   with REC LOAD and REC GET. Putting a copy there is what sends it. */
static void send_to_flipper(void)
{
    const char *dir = rec_dir();
    if (!dir || strncmp(dir, ROOT, strlen(ROOT))) {
        LS_SNPUTS(t_scr_files_no_card_folder_for, s_feedback, sizeof(s_feedback), "No card folder for the Flipper to read");
        return;
    }
    char to[PATH_MAX_LEN];
    if (!join_path(to, sizeof(to), dir, open_name())) return;
    if (!strcmp(to, s_open_path)) {
        LS_SNPUTS(t_scr_files_already_there_flipper_lakeshark, s_feedback, sizeof(s_feedback), "Already there: Flipper > LakeShark > FILES");
        return;
    }
    struct stat st;
    if (stat(s_open_path, &st) != 0 || st.st_size > JOB_MAX_BYTES) {
        LS_SNPUTS(t_scr_files_copy_too_large_or, s_feedback, sizeof(s_feedback), "Copy too large or unreadable (limit 1 MB)"); return;
    }
    copy_begin(dir, to, JOB_MAX_BYTES, LS_TEXT(t_scr_files_copied_on_the_flipper, "Copied. On the Flipper: LakeShark > FILES"));
}

/* ---------------------------------------------------------------- music */

/* The SD folder MUSIC lists. A P25 or FM call, a mic recording kept
   somewhere else: PLAY opens it in the player where it is; COPY and MOVE put
   it in the list for good. */
#define MUSIC_DIR ROOT "/music"
#define MUSIC_MAX_BYTES (512UL * 1024 * 1024)
extern bool ls_scr_music_play_file(const char *path);
extern bool ls_media_supported_extension(const char *name);   /* media_playlist.h */

static bool is_audio(const char *name) { return ls_media_supported_extension(name); }
static bool in_music(void)
{
    const char *slash = strrchr(s_open_path, '/');
    return slash && (size_t)(slash - s_open_path) == strlen(MUSIC_DIR) &&
           !strncmp(s_open_path, MUSIC_DIR, strlen(MUSIC_DIR));
}
static bool music_dir_ready(void)
{
    struct stat st;
    if (stat(MUSIC_DIR, &st) == 0 && S_ISDIR(st.st_mode)) return true;
    if (mkdir(MUSIC_DIR, 0777) == 0) return true;
    snprintf(s_feedback, sizeof(s_feedback), "Could not make %s", MUSIC_DIR);
    return false;
}
static void play_audio(void)
{
    if (!ls_scr_music_play_file(s_open_path))
        LS_SNPUTS(t_scr_files_music_is_not_on, s_feedback, sizeof(s_feedback), "MUSIC is not on this build");
}
static void copy_to_music(void)
{
    if (in_music()) { snprintf(s_feedback, sizeof(s_feedback), "Already in MUSIC"); return; }
    struct stat st;
    if (stat(s_open_path, &st) != 0 || (unsigned long)st.st_size > MUSIC_MAX_BYTES) {
        LS_SNPUTS(t_scr_files_too_large_or_unreadable, s_feedback, sizeof(s_feedback), "Too large or unreadable (limit 512 MB)"); return;
    }
    uint64_t total = 0, free_b = 0;
    if (esp_vfs_fat_info(ROOT, &total, &free_b) == ESP_OK && free_b && free_b < (uint64_t)st.st_size + 65536) {
        LS_SNPUTS(t_scr_files_not_enough_card_space, s_feedback, sizeof(s_feedback), "Not enough card space for a copy"); return;
    }
    if (!music_dir_ready()) return;
    char to[PATH_MAX_LEN];
    if (!join_path(to, sizeof(to), MUSIC_DIR, open_name())) return;
    copy_begin(MUSIC_DIR, to, MUSIC_MAX_BYTES, "Copied to MUSIC");
}
/* A rename on the same card: instant, whatever the size. A call's .meta and
   .keep belong to the call list, which no longer has the call, so they go. */
static void move_to_music(void)
{
    if (in_music()) { snprintf(s_feedback, sizeof(s_feedback), "Already in MUSIC"); return; }
    if (!music_dir_ready()) return;
    const char *base = open_name(), *dot = strrchr(base, '.');
    const int stem = dot ? (int)(dot - base) : (int)strlen(base);
    char to[PATH_MAX_LEN];
    struct stat st;
    int suffix;
    for (suffix = 0; suffix < 1000; suffix++) {
        char name[256];
        const int n = suffix ? snprintf(name, sizeof(name), "%.*s-%d%s", stem, base, suffix, dot ? dot : "")
                             : snprintf(name, sizeof(name), "%s", base);
        if (n < 0 || (size_t)n >= sizeof(name) || !join_path(to, sizeof(to), MUSIC_DIR, name)) {
            LS_SNPUTS(t_scr_files_name_too_long_for, s_feedback, sizeof(s_feedback), "Name too long for MUSIC"); return;
        }
        if (stat(to, &st) != 0 && errno == ENOENT) break;
    }
    if (suffix == 1000) { LS_SNPUTS(t_scr_files_no_free_name_in, s_feedback, sizeof(s_feedback), "No free name in MUSIC"); return; }
    if (rename(s_open_path, to) != 0) { snprintf(s_feedback, sizeof(s_feedback), "Move failed (%d)", errno); return; }
    static const char *const SIDECARS[] = {".keep", ".meta"};
    for (unsigned i = 0; i < sizeof(SIDECARS) / sizeof(SIDECARS[0]); i++) {
        char side[PATH_MAX_LEN + 8];
        snprintf(side, sizeof(side), "%s%s", s_open_path, SIDECARS[i]);
        if (stat(side, &st) == 0) unlink(side);
    }
    const char *slash = strrchr(to, '/');
    LS_SNPRINTF(t_scr_files_moved_to_music_60s, s_feedback, sizeof(s_feedback), "Moved to MUSIC: %.60s", slash ? slash + 1 : to);
    s_view = VIEW_LIST;
    load_dir();
}

static void replay_open(void)
{
    if(ls_scr_rec_replay_file(s_open_path,&s_sub,s_edges))return;
    const char *why=subghz_tx_refusal(s_sub.freq_hz,0,(size_t)s_sub.edges_total,s_sub.span_us);
    if(why)snprintf(s_feedback,sizeof(s_feedback),"Replay refused: %s",why);
    else LS_SNPUTS(t_scr_files_replay_busy_or_file, s_feedback, sizeof(s_feedback), "Replay busy or file unsupported");
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

static EXT_RAM_BSS_ATTR char s_rename_from[PATH_MAX_LEN];
static void rename_done(const char *name)
{
    if (!name || !name[0]) return;
    const size_t len = strlen(name);
    if (len > 255 || strpbrk(name, "\\/:*?\"<>|") || name[len - 1] == '.' || name[len - 1] == ' ') {
        LS_SNPUTS(t_scr_files_invalid_fat_filename_1, s_feedback, sizeof(s_feedback), "Invalid FAT filename (1-255 characters)");
        return;
    }
    for (const unsigned char *p = (const unsigned char *)name; *p; p++)
        if (*p < 32 || *p == 127) { LS_SNPUTS(t_scr_files_invalid_filename_character, s_feedback, sizeof(s_feedback), "Invalid filename character"); return; }
    char to[PATH_MAX_LEN];
    if (!join_path(to, sizeof(to), s_path, name)) return;
    if (!strcmp(to, s_rename_from)) return;
    struct stat st;
    int result;
    if (!strcasecmp(to, s_rename_from)) {
        char temp[PATH_MAX_LEN];
        int suffix;
        for (suffix = 0; suffix < 1000; suffix++) {
            LS_SNPRINTF(t_scr_files_s_ls_rename_d, temp, sizeof(temp), "%s/.ls-rename-%d.tmp", s_path, suffix);
            FILE *reserved = rec_file_open_new(temp);
            if (reserved) {
                if (fclose(reserved) != 0) { unlink(temp); LS_SNPUTS(t_scr_files_could_not_reserve_rename, s_feedback, sizeof(s_feedback), "Could not reserve rename path"); return; }
                break;
            }
            if (errno != EEXIST) { LS_SNPUTS(t_scr_files_could_not_reserve_rename, s_feedback, sizeof(s_feedback), "Could not reserve rename path"); return; }
        }
        if (suffix == 1000) { snprintf(s_feedback, sizeof(s_feedback), "No free rename path"); return; }
        /* The reserved destination belongs to this rename, never another file. */
        if (rename(s_rename_from, temp) != 0) {
            unlink(temp); LS_SNPUTS(t_scr_files_could_not_stage_case, s_feedback, sizeof(s_feedback), "Could not stage case rename"); return;
        }
        result = rename(temp, to);
        if (result != 0 && rename(temp, s_rename_from) != 0) {
            LS_SNPRINTF(t_scr_files_rename_recovery_file_ls, s_feedback, sizeof(s_feedback), "Rename recovery file: .ls-rename-%d.tmp", suffix); return;
        }
    } else {
        if (stat(to, &st) == 0 || errno != ENOENT) {
            LS_SNPUTS(t_scr_files_destination_exists_or_cannot, s_feedback, sizeof(s_feedback), "Destination exists or cannot be checked"); return;
        }
        result = rename(s_rename_from, to);
    }
    if (result == 0) {
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
static void actions_open(void);
static void more_open(void);
static void menu_add(char id, const char *label, const char *detail)
{
    if (s_menu_n < (int)sizeof(s_menu) && ls_picker_add(label, detail)) s_menu[s_menu_n++] = id;
}
static void actions_done(int i)
{
    if (i < 0 || i >= s_menu_n) return;
    switch (s_menu[i]) {
    case 'O': s_replay_menu = true; open_selected(); break;
    case 'F': send_to_flipper(); break;
    case 'R': replay_open(); break;
    case 'T': open_text(); break;
    case 'P': play_audio(); break;
    case 'C': copy_to_music(); break;
    case 'M': move_to_music(); break;
    case 'I': info_open(); break;
    case 'N':
        snprintf(s_rename_from, sizeof(s_rename_from), "%s", s_open_path);
        ls_keyboard_open("RENAME", open_name(), 255, rename_done);
        break;
    case 'D': {
        char title[48];
        snprintf(title, sizeof(title), "DELETE %s?", open_name());
        ls_picker_open(title, delete_done);
        ls_picker_add("DELETE", LS_TEXT(t_scr_files_it_cannot_be_brought, "It cannot be brought back"));
        ls_picker_add("CANCEL", LS_TEXT(t_scr_files_leave_it_on_the, "Leave it on the card"));
        ls_picker_select(1);
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
        if (!join_path(s_open_path, sizeof(s_open_path), s_path, s_entry[s_selected].name)) return;
    }
    const bool sub = !dir && !strcasecmp(ext_of(open_name()), "sub");
    char title[48];
    snprintf(title, sizeof(title), "%s", open_name());
    s_menu_n = 0;
    ls_picker_open(title, actions_done);
    if (sub) {
        menu_add('F', "SEND TO FLIPPER", LS_TEXT(t_scr_files_copy_where_the_flipper, "Copy where the Flipper app reads"));
        if (s_view == VIEW_LIST) menu_add('O', LS_TEXT(t_scr_files_open_capture_replay, "OPEN CAPTURE / REPLAY"), LS_TEXT(t_scr_files_load_and_validate_before, "Load and validate before replay"));
        if (s_view == VIEW_SUB && subghz_file_is_iq(&s_sub))
            menu_add('R', "REPLAY", LS_TEXT(t_scr_files_open_in_rec_lr2021, "Open in REC / LR2021 I/Q"));
        else if (s_view == VIEW_SUB && subghz_file_is_cc_fsk(&s_sub))
            menu_add('R', "REPLAY", LS_TEXT(t_scr_files_open_in_record_cc1101, "Open in RECORD / CC1101 FSK"));
        else if (s_view == VIEW_SUB && subghz_file_is_fsk(&s_sub)) {
            char where[40];
            snprintf(where, sizeof(where), "Open in RECORD / %s", ls_rsel_name(LS_RSEL_LORA));
            menu_add('R', "REPLAY", where);
        }
        else if (s_view == VIEW_SUB && subghz_file_is_ook(&s_sub))
            menu_add('R', "REPLAY", LS_TEXT(t_scr_files_open_in_record_cc1101_2, "Open in RECORD / CC1101"));
        else if (s_view == VIEW_SUB) {
            const char *why = subghz_tx_refusal(s_sub.freq_hz, 0,
                (size_t)s_sub.edges_total, s_sub.span_us);
            menu_add('.', why ? "REPLAY REFUSED" : "REPLAY UNAVAILABLE",
                     why ? why : "Incomplete or unsupported RAW/modulation");
        }
    }
    if (!dir && is_audio(open_name())) {
        menu_add('P', "PLAY", LS_TEXT(t_scr_files_open_it_in_music, "Open it in MUSIC, from here"));
        if (!in_music()) {
            menu_add('C', "COPY TO MUSIC", LS_TEXT(t_scr_files_a_copy_in_the, "A copy in the MUSIC track list"));
            menu_add('M', "MOVE TO MUSIC", LS_TEXT(t_scr_files_take_it_out_of, "Take it out of here, into MUSIC"));
        }
    }
    if (!dir && is_text(open_name()) && s_view != VIEW_TEXT) menu_add('T', "VIEW AS TEXT", "The file as written");
    menu_add('I', "INFO", LS_TEXT(t_scr_files_size_date_where_it, "Size, date, where it is"));
    menu_add('N', "RENAME", "Type a new name");
    menu_add('D', "DELETE", dir ? "Only if the folder is empty" : "Remove it from the card");
}

static void sort_done(int i)
{
    if (i < 0 || i >= SORT__COUNT) return;
    char keep[256];
    snprintf(keep, sizeof(keep), "%s", selected_name() ? selected_name() : "");
    s_sort = (sort_t)i;
    if (!s_truncated && !s_read_failed) {
        qsort(s_entry, s_count, sizeof(s_entry[0]), entry_cmp);
        for (int k = 0; k < s_count; k++)
            if (!strcmp(s_entry[k].name, keep)) { s_selected = k; break; }
    } else reload_keep(keep);
    snprintf(s_feedback, sizeof(s_feedback), "Sorted by %s", SORT_NAME_TEXT[i]);
}
static void sort_open(void)
{
    static const char *const WHY[SORT__COUNT] = {"A to Z", "Most recent at the top",
        "Oldest at the top", "Biggest at the top", "Grouped by extension"};
    ls_picker_open("SORT", sort_done);
    ls_picker_back(more_open);
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
    if (s_read_failed) LS_SNPRINTF(t_scr_files_s_is_not_on, s_feedback, sizeof(s_feedback), "%s is not on this card", path);
}
static const char *const GOTO_PATH[] = {ROOT, ROOT "/lakeshark", ROOT "/subghz", ROOT "/journal", ROOT "/maps"};
static void more_done(int i)
{
    if (i < 0 || i >= s_menu_n) return;
    const char id = s_menu[i];
    if (id == 'S') { sort_open(); return; }
    if (id >= '0' && id <= '4') { goto_path(GOTO_PATH[id - '0']); return; }
    if (id == 'F') {
        ls_picker_open("SHOW", filter_done);
        ls_picker_back(more_open);
        static const char *const WHY[FILTER__COUNT] = {"Everything", "Sub-GHz captures",
            "Image files (no preview)", "Notes, logs, tracks"};
        for (int k = 0; k < FILTER__COUNT; k++)
            ls_picker_add(FILTER_LABEL[k], k == (int)s_filter ? "in use" : WHY[k]);
        return;
    }
    if (id == 'R') {
        s_view = VIEW_LIST;
        if (!ls_sdcard_mounted() && ls_sdcard_mount() != ESP_OK) {
            snprintf(s_feedback, sizeof(s_feedback), "Card mount failed"); return;
        }
        reload_keep(selected_name());
        snprintf(s_feedback, sizeof(s_feedback), "%s", s_read_failed ? "Refresh failed - card or folder unreadable" : "Refreshing"); return;
    }
    if (id == 'C') {
        uint64_t total = 0, free_b = 0;
        if (esp_vfs_fat_info(ROOT, &total, &free_b) == ESP_OK)
            LS_SNPRINTF(t_scr_files_card_1f_gb_free, s_feedback, sizeof(s_feedback), "Card: %.1f GB free of %.1f GB",
                     free_b / 1073741824.0, total / 1073741824.0);
        else snprintf(s_feedback, sizeof(s_feedback), "No card mounted");
    }
}
static void option_step(int i, int dir)
{
    if (i < 0 || i >= s_menu_n) return;
    if (s_menu[i] == 'S') {
        sort_done((s_sort + dir + SORT__COUNT) % SORT__COUNT);
        ls_picker_set_detail(i, SORT_NAME_TEXT[s_sort]);
    } else if (s_menu[i] == 'F') {
        filter_done((s_filter + dir + FILTER__COUNT) % FILTER__COUNT);
        ls_picker_set_detail(i, FILTER_LABEL[s_filter]);
    }
}
static void more_open(void)
{
    s_menu_n = 0;
    ls_picker_open("OPTIONS", more_done);
    menu_add('S', "SORT", SORT_NAME_TEXT[s_sort]);
    ls_picker_stepper(0, option_step);
    menu_add('1', "CAPTURES", LS_TEXT(t_scr_files_rec_recordings_what_the, "REC recordings - what the Flipper reads"));
    menu_add('2', "SUB-GHZ", LS_TEXT(t_scr_files_watch_exports_and_archives, "Watch exports and archives"));
    menu_add('3', "JOURNAL", LS_TEXT(t_scr_files_field_notes_and_tracks, "Field notes and tracks"));
    menu_add('0', "CARD ROOT", ROOT);
    menu_add('F', "SHOW", FILTER_LABEL[s_filter]);
    ls_picker_stepper(s_menu_n - 1, option_step);
    menu_add('R', "REFRESH", LS_TEXT(t_scr_files_read_the_folder_again, "Read the folder again"));
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

/* One bounded batch per frame leaves input available between card reads. */
static void job_cancel(void)
{
    if (s_dir) { closedir(s_dir); s_dir = NULL; s_count = 0; s_read_failed = true; }
    if (s_in) { fclose(s_in); s_in = NULL; }
    if (s_out) { fclose(s_out); s_out = NULL; unlink(s_copy_path); }
    s_job = JOB_NONE;
    s_replay_menu = false;
}
static void job_step(void)
{
    if (s_job == JOB_DIR) {
        for (int n = 0; n < 8; n++) {
            errno = 0;
            struct dirent *e = readdir(s_dir);
            if (!e) {
                s_read_failed = errno != 0;
                closedir(s_dir); s_dir = NULL; s_job = JOB_NONE;
                if (s_selected >= s_count) s_selected = s_count ? s_count - 1 : 0;
                for (int i = 0; i < s_count; i++)
                    if (!strcmp(s_entry[i].name, s_keep)) { s_selected = i; break; }
                if (s_read_failed) snprintf(s_feedback, sizeof(s_feedback), "Folder read failed");
                else if (!strcmp(s_feedback, "Refreshing")) snprintf(s_feedback, sizeof(s_feedback), "Refreshed");
                return;
            }
            s_job_bytes++;
            if (e->d_name[0] == '.') continue;
            entry_t candidate = {0};
            entry_t *t = &candidate;
            if (strlen(e->d_name) >= sizeof(t->name)) continue;
            strcpy(t->name, e->d_name);
            char full[PATH_MAX_LEN];
            if (!join_path(full, sizeof(full), s_path, e->d_name)) continue;
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
            if (!passes_filter(t)) continue;
            /* Retain the first page of the active order across the whole folder,
               including entries encountered after the storage limit. */
            if (s_count == MAX_ENTRIES) s_truncated = true;
            ls_media_insert(s_entry,&s_count,MAX_ENTRIES,sizeof(s_entry[0]),&candidate,entry_cmp);
        }
    } else if (s_job == JOB_SUB) {
        for (int n = 0; n < 4; n++) {
            if (!fgets(s_line, sizeof(s_line), s_in)) {
                const bool ok = !ferror(s_in) && s_sub.filetype_ok;
                fclose(s_in); s_in = NULL; s_job = JOB_NONE;
                if (ok) {
                    decode_sub(); s_scroll = 0; s_view = VIEW_SUB;
                    if (s_replay_menu) actions_open();
                } else LS_SNPUTS(t_scr_files_not_a_readable_subghz, s_feedback, sizeof(s_feedback), "Not a readable SubGhz file - use VIEW AS TEXT");
                s_replay_menu = false;
                return;
            }
            s_job_bytes += strlen(s_line);
            if (s_job_bytes > JOB_MAX_BYTES) { job_cancel(); LS_SNPUTS(t_scr_files_capture_exceeds_1_mb, s_feedback, sizeof(s_feedback), "Capture exceeds 1 MB limit"); return; }
            if (!strchr(s_line, '\n') && !feof(s_in)) s_sub.invalid = true;
            subghz_file_line(&s_sub, s_line, s_edges, MAX_EDGES);
        }
    } else if (s_job == JOB_COPY) {
        if (!s_out) {
            for (int attempt = 0; attempt < 8 && s_copy_suffix <= 999; attempt++, s_copy_suffix++) {
                if (s_copy_suffix) {
                    char name[256];
                    const char *base = open_name(), *dot = strrchr(base, '.');
                    const int stem = dot ? (int)(dot - base) : (int)strlen(base);
                    const int n = snprintf(name, sizeof(name), "%.*s-%d%s", stem, base, s_copy_suffix, dot ? dot : "");
                    if (n < 0 || (size_t)n >= sizeof(name) || !join_path(s_copy_path, sizeof(s_copy_path), s_copy_dir, name)) {
                        job_cancel(); snprintf(s_feedback, sizeof(s_feedback), "Copy name too long"); return;
                    }
                }
                /* Reserve only a new name, including against concurrent writers. */
                s_out = rec_file_open_new(s_copy_path);
                if (s_out) break;
                if (errno != EEXIST) { job_cancel(); LS_SNPUTS(t_scr_files_could_not_create_copy, s_feedback, sizeof(s_feedback), "Could not create copy"); return; }
            }
            if (!s_out && s_copy_suffix > 999) { job_cancel(); snprintf(s_feedback, sizeof(s_feedback), "No free copy name"); }
            return;     /* the name is reserved; the copy starts next frame */
        }
        /* A few blocks a frame: a song is megabytes, and input still gets a
           turn between them. */
        for (int block = 0; block < 8; block++) {
            const size_t n = fread(s_line, 1, sizeof(s_line), s_in);
            s_job_bytes += n;
            if (ferror(s_in) || s_job_bytes > s_copy_limit || (n && fwrite(s_line, 1, n, s_out) != n)) {
                job_cancel(); snprintf(s_feedback, sizeof(s_feedback), "Copy failed"); return;
            }
            if (!n) {
                fclose(s_in); s_in = NULL;
                const bool ok = fclose(s_out) == 0; s_out = NULL; s_job = JOB_NONE;
                if (!ok) unlink(s_copy_path);
                snprintf(s_feedback, sizeof(s_feedback), "%s", ok ? s_copy_note : "Copy failed");
                return;
            }
        }
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
    if(!strcmp(s_sub.protocol,"LR2021_IQ")) {
        LS_SNPRINTF(t_scr_files_iq16le_lu_s_s, line,sizeof(line),"IQ16LE / %lu S/s / %lu pairs / %.2f ms",
            (unsigned long)s_sub.iq_rate,(unsigned long)s_sub.iq_samples,s_sub.iq_samples*0.008);
        tui_put_str(sf,in,x,y++,line,LS_ATTR_DIM);
        tui_put_str(sf,in,x,y++,subghz_file_is_iq(&s_sub)?"LR2021 I/Q replay; requires matching sidecar":"Invalid or truncated I/Q recording",LS_ATTR_DIM);
        return;
    }
    /* What the file is, whether or not it may be replayed. */
    if (s_sub.bitrate && s_sub.deviation_hz && !strstr(s_sub.preset, "Ook"))
        LS_SNPRINTF(t_scr_files_2_fsk_lu_baud, line, sizeof(line), "2-FSK %lu baud, %.1f kHz dev, sync %08lX",
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
    snprintf(line, sizeof(line), "DECODE  %s", s_decode[0] ? s_decode : "no known encoding");
    tui_put_str(sf, in, x, y++, line, s_decode[0] ? TUI_ATTR(TUI_CYAN | TUI_BRIGHT, TUI_BLACK) : LS_ATTR_DIM);
    y++;

    const int left = p.y + p.h - 1 - y;
    if (left < 6) return;
    /* Overview gets what the histogram does not need, capped so a tall
       portrait pane does not stretch a short capture into dust. */
    const int hist_rows = left > 24 ? 10 : 6;
    int ov_rows = left - hist_rows - 3;
    if (ov_rows > 24) ov_rows = 24;
    if (ov_rows >= 2) {
        LS_SNPRINTF(t_scr_files_whole_capture_2f_ms, line, sizeof(line), "WHOLE CAPTURE  %.2f ms a row   # on  _ off",
                 s_sub.span_us / 1000.0 / ov_rows);
        tui_put_str(sf, in, x, y++, line, LS_ATTR_DIM);
        sub_overview(sf, tui_rect_make(x, y, w, ov_rows));
        y += ov_rows + 1;
    }
    sub_histogram(sf, tui_rect_make(x, y, w, p.y + p.h - 1 - y));
}

static int text_rows(tui_surface *sf, tui_rect a)
{
    int row = 0, col = 0;
    const int width = a.w;
    if (width < 1) return 0;
    for (size_t i = 0; i < s_text_len; i++) {
        unsigned char ch = (unsigned char)s_text[i];
        if (ch == '\r') continue;
        if (ch == '\n') { row++; col = 0; continue; }
        int repeat = ch == '\t' ? 4 - col % 4 : 1;
        if (ch >= 0xc2 && ch <= 0xf4) {
            int bytes = ch < 0xe0 ? 2 : ch < 0xf0 ? 3 : 4;
            int n = 1;
            while (n < bytes && i + n < s_text_len && ((unsigned char)s_text[i+n] & 0xc0) == 0x80) n++;
            if (n == bytes) i += bytes - 1;
            ch = '?';
        }
        while (repeat--) {
            if (col >= width) { row++; col = 0; }
            if (sf && row >= s_scroll && row < s_scroll + a.h)
                tui_put_char(sf, a, a.x + col, a.y + row - s_scroll,
                             ch == '\t' ? ' ' : ch >= 32 && ch < 127 ? (char)ch : '.', LS_ATTR_DIM);
            col++;
        }
    }
    return row + 1;
}
static void draw_text(tui_surface *sf, tui_rect a)
{
    s_text_rows = a.h > 0 ? a.h : 1;
    const int lines = text_rows(NULL, a);
    s_scroll_max = lines > a.h ? lines - a.h : 0;
    if (s_scroll > s_scroll_max) s_scroll = s_scroll_max;
    text_rows(sf, a);
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
        tui_fill(sf, tui_rect_make(p.x + p.w - n - 3, p.y + 1, n + 1, 1), ' ', LS_ATTR_DIM);
        tui_put_str(sf, p, p.x + p.w - n - 2, p.y + 1, how, LS_ATTR_DIM);
    }
    if (s_read_failed) {
        tui_put_str(sf, p, s_list.x, s_list.y + 1, LS_TEXT(t_scr_files_no_card_or_this, "No card, or this folder cannot be read."), LS_ATTR_DIM);
        tui_put_str(sf, p, s_list.x, s_list.y + 2, LS_TEXT(t_scr_files_refresh_tries_again, "REFRESH tries again."), LS_ATTR_DIM);
        return;
    }
    if (!s_count) {
        char empty[64];
        snprintf(empty, sizeof(empty), s_filter ? "No matches for %s. OPTIONS > SHOW ALL" : "Empty folder.", FILTER_LABEL[s_filter]);
        tui_put_str(sf, p, s_list.x, s_list.y + 1, empty, LS_ATTR_DIM);
        return;
    }
    const int rows = list_rows();
    const int first = s_selected / rows * rows;
    const int rh = row_h();
    for (int i = 0; i < rows && first + i < s_count; i++) {
        const entry_t *e = &s_entry[first + i];
        const int y = s_list.y + i * rh;
        const bool sel = first + i == s_selected;

        char full[257], name[80];
        snprintf(full, sizeof(full), "%s%s", e->name, e->dir ? "/" : "");
        fit_name(full, rh == 2 ? s_list.w - 2 : s_list.w - 30, name, sizeof(name));
        const bool sub = !e->dir && !strcasecmp(ext_of(e->name), "sub");
        const uint8_t hue = e->dir ? TUI_YELLOW : sub ? TUI_GREEN : TUI_WHITE;
        const char *columns[]={name}; int widths[]={s_list.w-2};
        ls_media_row_tinted(sf,tui_rect_make(s_list.x+1,y,s_list.w-2,1),columns,widths,1,sel,hue|TUI_BRIGHT);
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
    if (s_job != JOB_NONE) {
        char progress[80];
        LS_SNPRINTF(t_scr_files_s_lu_s_back, progress, sizeof(progress), "%s %lu %s - BACK or tap to cancel",
                 s_job == JOB_DIR ? "Reading folder" : s_job == JOB_SUB ? "Loading capture" : "Copying",
                 s_job_bytes, s_job == JOB_DIR ? "entries" : "bytes");
        ls_panel_box(sf, a, "FILES", TUI_CYAN);
        tui_put_str(sf, a, a.x + 1, a.y + 2, progress, LS_ATTR_DIM);
        job_step();
        return;
    }
    if (a.h < 14 || a.w < 24) {
        ls_panel_box(sf, a, "FILES", TUI_CYAN);
        tui_put_str(sf, a, a.x + 1, a.y + 1, "Enlarge the pane", LS_ATTR_DIM);
        return;
    }
    s_touch_nav = s_view == VIEW_TEXT || (a.w < 90 && a.h > 35);
    if (s_touch_nav) {
        const bool list = s_view == VIEW_LIST;
        const int rows = list_rows();
        ls_btn_t nav[] = {
            {"PREV", "PAGE", 0, false, list ? s_selected < rows : s_scroll == 0 && !s_text_offset},
            {"NEXT", "PAGE", 0, false, list ? s_selected / rows * rows + rows >= s_count
                                            : s_scroll >= s_scroll_max && s_text_offset + (long)s_text_len >= s_text_total},
            {"BACK", list ? "FOLDER" : "LIST", 0, false, !can_go_up()}};
        ls_btn_bar_raised_slot(sf, tui_rect_make(a.x, a.y + a.h - 5, a.w, 5), nav, 3, -1,
                               LS_BTN_SLOT_WATERFALL);
        a.h -= 5;
    }
    const bool list = s_view == VIEW_LIST;
    const bool sel_file = list && s_selected < s_count && !s_entry[s_selected].dir;
    ls_btn_t btn[] = {
        {list && at_root() ? "BACK" : "UP", list ? "FOLDER" : "LIST", 'u', false, !can_go_up()},
        {sel_file && is_audio(s_entry[s_selected].name) ? "PLAY" : "OPEN",
         list && s_selected < s_count && s_entry[s_selected].dir ? "FOLDER" : "FILE", 'o', false,
         !list || s_selected >= s_count},
        {"ACTIONS", list && s_selected < s_count && s_entry[s_selected].dir ? "FOLDER" : "FILE", 'a',
         false, list && s_selected >= s_count},
        {"SORT", SORT_NAME_TEXT[s_sort], 's', false, !list},
        {"OPTIONS", "SHOW / GO TO", 'm', false, false}};
    (void)sel_file;
    const int bar_h = ls_btn_raised_height(a, 5);
    ls_btn_bar_raised(sf, tui_rect_make(a.x, a.y, a.w, bar_h), btn, 5, button_focus);
    tui_rect panel = tui_rect_make(a.x, a.y + bar_h, a.w, a.h - bar_h - 1);
    char title[80];
    if (list) snprintf(title, sizeof(title), "FILES  %s%d", s_truncated ? "first " : "", s_count);
    else if (s_view == VIEW_TEXT) LS_SNPRINTF(t_scr_files_text_ld_ld_ld, title, sizeof(title), "TEXT %ld-%ld/%ld bytes", s_text_offset, s_text_offset + (long)s_text_len, s_text_total);
    else snprintf(title, sizeof(title), "CAPTURE");
    ls_panel_box(sf, panel, title, TUI_CYAN);
    if (s_view == VIEW_SUB) draw_sub(sf, panel);
    else if (s_view == VIEW_TEXT)
        draw_text(sf, tui_rect_make(panel.x + 2, panel.y + 1, panel.w - 4, panel.h - 2));
    else draw_list(sf, panel);
    ls_safe_line(sf, a, a.y + a.h - 1,
                 s_feedback[0] ? s_feedback :
                 list ? "Tap to select, tap again to open" : s_view == VIEW_TEXT ? "ASCII / UTF-8: unsupported glyphs shown as ?" : "ACTIONS: send, replay, delete",
                 LS_ATTR_DIM);
}

/* ----------------------------------------------------------------- input */

static void enter(void)
{
    button_focus = -1;
    button_slot = 0;
    s_feedback[0] = 0;
    s_view = VIEW_LIST;
    load_dir();
}

static void page(int dir)
{
    if (s_view != VIEW_LIST) {
        if (s_view == VIEW_TEXT && dir > 0 && s_scroll >= s_scroll_max && s_text_offset + (long)s_text_len < s_text_total) {
            text_read(s_text_offset + TEXT_MAX); return;
        }
        if (s_view == VIEW_TEXT && dir < 0 && !s_scroll && s_text_offset) {
            if (text_read(s_text_offset - TEXT_MAX)) s_scroll = TEXT_MAX;
            return;
        }
        const int step = s_text_rows;
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
    if (s_job != JOB_NONE) {
        if (k == LS_TK_ESC || k == LS_TK_BACKSPACE) { job_cancel(); snprintf(s_feedback, sizeof(s_feedback), "Cancelled"); }
        return true;
    }
    if (ch >= 'A' && ch <= 'Z') ch += 'a' - 'A';
    if (k == LS_TK_LEFT || k == LS_TK_RIGHT || k == LS_TK_TAB)
        return ls_btn_navigate(k, &button_slot, &button_focus, false);
    if (k == LS_TK_ENTER && button_focus >= 0) {
        if (ls_btn_enabled(0, button_focus)) action("uoasm"[button_focus]);
        return true;
    }
    if (k == LS_TK_UP || k == LS_TK_DOWN || k == LS_TK_BACKSPACE) button_focus = -1;
    if (k == LS_TK_BACKSPACE || k == LS_TK_ESC) return go_up();
    if (k == LS_TK_UP) {
        if (s_view == VIEW_LIST) { if (s_selected) s_selected--; }
        else if (s_view == VIEW_TEXT && !s_scroll) page(-1);
        else if (s_scroll) s_scroll--;
        return true;
    }
    if (k == LS_TK_DOWN) {
        if (s_view == VIEW_LIST) { if (s_selected + 1 < s_count) s_selected++; }
        else if (s_view == VIEW_TEXT && s_scroll >= s_scroll_max) page(1);
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
    if (s_job != JOB_NONE) { job_cancel(); snprintf(s_feedback, sizeof(s_feedback), "Cancelled"); return true; }
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

/* Leaving by any other route - the home, another app - forgets the caller,
   so opening FILES later from the directory does not jump somewhere odd. */
static void leave(void) { job_cancel(); s_return_to = -1; s_on_return=NULL; }

const ls_tui_screen_t ls_scr_files = {
    .name = "FILES", .hint = "ENTER open  BS up  A actions  S sort  M options",
    .enter = enter, .leave = leave, .draw = draw, .key = key, .touch = touch};

bool ls_scr_files_show_path(const char *path,void (*on_return)(void)) {
    const char *slash=path?strrchr(path,'/'):NULL;
    if(!slash || (size_t)(slash-path)>=sizeof(s_path))return false;
    for(int i=0;i<ls_tui_screen_count();i++)if(!strcmp(ls_tui_screen_name(i),"FILES")) {
        int from=ls_tui_screen_current();s_depth=0;s_view=VIEW_LIST;
        snprintf(s_path,sizeof(s_path),"%.*s",(int)(slash-path),path);
        ls_tui_screen_show(i);s_return_to=from;s_on_return=on_return;reload_keep(slash+1);return true;
    }
    return false;
}
