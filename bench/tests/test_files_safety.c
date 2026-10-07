#include "ls_test.h"
#include <errno.h>
#include <sys/stat.h>
#include <unistd.h>
#include <time.h>
#ifdef _WIN32
#define gmtime_r(t, out) (gmtime_s(out, t) == 0 ? (out) : NULL)
#endif
#include "dirent.h"
#include "rec_file_open.h"

static int test_stat(const char *path, struct stat *st);
static FILE *test_fopen(const char *path, const char *mode);
static FILE *test_open_new(const char *path);
static int test_unlink(const char *path);
static int test_ferror(FILE *file);
#define stat(path, st) test_stat(path, st)
#define fopen test_fopen
#define rec_file_open_new test_open_new
#define unlink test_unlink
#define ferror test_ferror
#include "../../components/apps/tui/screens/ext/scr_files.c"
#undef stat
#undef fopen
#undef rec_file_open_new
#undef unlink
#undef ferror

static struct dirent entries[310];
static int entry_count, decoder_calls, picker_cursor, picker_count, unlinks;
static char loaded[512], copied[512], deleted[512];
static bool collision, read_failed, names_full;
static ls_picker_done_t picker_done;

DIR *opendir(const char *path) { static DIR d; d.cursor = 0; return &d; }
struct dirent *readdir(DIR *d) { return d->cursor < entry_count ? &entries[d->cursor++] : NULL; }
int closedir(DIR *d) { return 0; }
static int test_stat(const char *path, struct stat *st)
{
    memset(st, 0, sizeof(*st));
    st->st_mode = S_IFREG;
    const char *name = strrchr(path, '/');
    for (int i = 0; i < entry_count; i++) {
        if (name && !strcmp(name + 1, entries[i].d_name)) {
            st->st_mtime = 1000 + i;
            st->st_size = i;
            if (entries[i].d_type == DT_DIR) st->st_mode = S_IFDIR;
            return 0;
        }
    }
    errno = ENOENT;
    return -1;
}
static FILE *test_fopen(const char *path, const char *mode)
{
    FILE *f = ls_test_tmpfile();
    if (mode[0] == 'w') { snprintf(copied, sizeof(copied), "%s", path); collision = false; }
    else if (f) {
        fputs("capture", f);
        rewind(f);
    }
    return f;
}
static FILE *test_open_new(const char *path)
{
    if (names_full || (collision && !strcmp(path, ROOT "/rec/capture.sub"))) { errno = EEXIST; return NULL; }
    snprintf(copied, sizeof(copied), "%s", path);
    return ls_test_tmpfile();
}
static int test_unlink(const char *path) { unlinks++; snprintf(deleted, sizeof(deleted), "%s", path); return 0; }
static int test_ferror(FILE *file) { return read_failed || ferror(file); }
const char *rec_dir(void) { return ROOT "/rec"; }
bool ls_scr_rec_replay_file(const char *p, const subghz_file_t *f, const int32_t *e) { return false; }
void ls_tui_screen_show(int id) {}
const char *ls_rsel_name(ls_rsel_radio_t r) { return "LoRa"; }
void ls_keyboard_open(const char *t, const char *s, int n, ls_keyboard_done_t cb) {}
void ls_picker_open(const char *t, ls_picker_done_t cb) { picker_cursor = 0; picker_count = 0; picker_done = cb; }
bool ls_picker_add(const char *l, const char *d) { picker_count++; return true; }
void ls_picker_select(int i) { picker_cursor = i; }
bool subghz_file_load(const char *p, subghz_file_t *f, int32_t *e, int n, char *line, size_t cap)
{
    snprintf(loaded, sizeof(loaded), "%s", p);
    memset(f, 0, sizeof(*f));
    return true;
}
bool subghz_file_is_fsk(const subghz_file_t *f) { return false; }
bool subghz_file_is_cc_fsk(const subghz_file_t *f) { return false; }
bool subghz_file_is_ook(const subghz_file_t *f) { return false; }
const char *subghz_tx_refusal(uint32_t hz, int dbm, size_t edges, uint64_t span_us)
{ (void)hz; (void)dbm; (void)edges; (void)span_us; return NULL; }
bool subghz_pwm_decode(const int32_t *p, int n, subghz_pwm_t *o) { decoder_calls++; return false; }
bool subghz_nrz_decode(const int32_t *p, int n, subghz_nrz_t *o) { decoder_calls++; return false; }
bool rec_decode_ook24(const int32_t *p, int n, rec_ook24_t *o) { decoder_calls++; return false; }
size_t subghz_pwm_format(const subghz_pwm_t *p, char *o, size_t n) { return 0; }
size_t subghz_nrz_format(const subghz_nrz_t *p, char *o, size_t n) { return 0; }

static void reset(void)
{
    memset(entries, 0, sizeof(entries));
    entry_count = 0;
    decoder_calls = unlinks = 0;
    collision = read_failed = names_full = false;
    loaded[0] = copied[0] = deleted[0] = 0;
    strcpy(s_path, ROOT);
    s_selected = 0;
    s_view = VIEW_LIST;
    s_sort = SORT_NEWEST;
    s_filter = FILTER_ALL;
}
LS_CASE(long_names_never_alias_their_short_prefix)
{
    reset();
    memset(entries[0].d_name, 'a', 63);
    memset(entries[1].d_name, 'a', 251);
    strcpy(entries[1].d_name + 251, ".sub");
    entry_count = 2;
    load_dir();
    LS_EQ_STR(s_entry[0].name, entries[1].d_name);
    open_selected();
    char want[512];
    snprintf(want, sizeof(want), "%s/%s", ROOT, entries[1].d_name);
    LS_EQ_STR(loaded, want);
    s_view = VIEW_LIST;
    actions_open();
    LS_EQ_STR(s_open_path, want);
    reload_keep(entries[1].d_name);
    LS_EQ_STR(selected_name(), entries[1].d_name);
    s_menu[0] = 'N'; s_menu_n = 1;
    actions_done(0);
    LS_EQ_STR(s_rename_from, want);
    delete_done(0);
    LS_EQ_STR(deleted, want);
}
LS_CASE(path_overflow_is_refused_before_use)
{
    reset();
    char tiny[12];
    LS_CHECK(!join_path(tiny, sizeof(tiny), ROOT, "capture.sub"));
    LS_EQ_STR(tiny, "");
    LS_EQ_STR(s_feedback, "Path too long");
}
LS_CASE(newest_entry_after_the_cap_is_retained)
{
    reset();
    entry_count = 300;
    for (int i = 0; i < entry_count; i++) snprintf(entries[i].d_name, 256, "capture%03d.sub", i);
    load_dir();
    LS_EQ_INT(s_count, 256);
    LS_CHECK(s_truncated);
    LS_EQ_STR(s_entry[0].name, "capture299.sub");
    LS_EQ_STR(s_entry[255].name, "capture044.sub");
    s_sort = SORT_OLDEST;
    load_dir();
    LS_EQ_STR(s_entry[0].name, "capture000.sub");
    LS_EQ_STR(s_entry[255].name, "capture255.sub");
}
LS_CASE(flipper_copy_preserves_an_existing_capture)
{
    reset();
    strcpy(s_open_path, ROOT "/capture.sub");
    collision = true;
    send_to_flipper();
    LS_CHECK(collision);
    LS_EQ_STR(copied, ROOT "/rec/capture-1.sub");
    LS_EQ_INT(unlinks, 0);
}
LS_CASE(delete_opens_on_cancel)
{
    reset();
    strcpy(s_open_path, ROOT "/capture.sub");
    s_menu[0] = 'D'; s_menu_n = 1;
    actions_done(0);
    LS_EQ_INT(picker_cursor, 1);
    picker_done(picker_cursor);
    LS_EQ_INT(unlinks, 0);
}
LS_CASE(failed_copy_only_removes_its_new_file)
{
    reset();
    strcpy(s_open_path, ROOT "/capture.sub");
    collision = read_failed = true;
    send_to_flipper();
    LS_CHECK(collision);
    LS_EQ_INT(unlinks, 1);
    LS_EQ_STR(deleted, ROOT "/rec/capture-1.sub");
}
LS_CASE(exhausted_copy_names_leave_existing_files_alone)
{
    reset();
    strcpy(s_open_path, ROOT "/capture.sub");
    collision = names_full = true;
    send_to_flipper();
    LS_CHECK(collision);
    LS_EQ_INT(unlinks, 0);
    LS_EQ_STR(copied, "");
}
LS_CASE(capture_decodes_once_per_open_including_no_match)
{
    reset();
    strcpy(entries[0].d_name, "capture.sub"); entry_count = 1;
    load_dir(); open_selected();
    LS_EQ_INT(decoder_calls, 3);
    tui_cell back[80 * 40], front[80 * 40];
    tui_surface sf;
    tui_surface_setup(&sf, back, front, 80, 40);
    for (int i = 0; i < 5; i++) { tui_frame_begin(&sf); draw_sub(&sf, tui_rect_make(0, 0, 80, 40)); }
    LS_EQ_INT(decoder_calls, 3);
    s_view = VIEW_LIST; open_selected();
    LS_EQ_INT(decoder_calls, 6);
}
