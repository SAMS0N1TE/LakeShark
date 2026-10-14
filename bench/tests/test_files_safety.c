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
static int test_rename(const char *from, const char *to);
#define stat(path, st) test_stat(path, st)
#define fopen test_fopen
#define rec_file_open_new test_open_new
#define unlink test_unlink
#define ferror test_ferror
#define rename test_rename
#include "../../components/apps/tui/screens/ext/scr_files.c"
#undef stat
#undef fopen
#undef rec_file_open_new
#undef unlink
#undef ferror
#undef rename

static struct dirent entries[310];
static int entry_count, decoder_calls, picker_cursor, picker_count, unlinks;
static char loaded[512], copied[512], deleted[512];
static bool collision, read_failed, names_full;
static int keyboard_limit, rename_calls;
static char keyboard_initial[768];
static const char *text_data;
static size_t text_data_len;
static void (*picker_parent)(void);
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
        if (text_data) fwrite(text_data, 1, text_data_len, f);
        else fputs("capture", f);
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
static int test_rename(const char *from, const char *to) { rename_calls++; return 0; }
bool ls_sdcard_mounted(void) { return true; }
esp_err_t ls_sdcard_mount(void) { return ESP_OK; }
esp_err_t esp_vfs_fat_info(const char *p, uint64_t *t, uint64_t *f) { return ESP_OK; }
int ls_tui_corner_pad(int row) { return 0; }
bool ls_tui_keyboard_mode(void) { return false; }
bool ls_scr_rec_replay_file(const char *p, const subghz_file_t *f, const int32_t *e) { return false; }
static int shown_screen,returned;
void ls_tui_screen_show(int id) {shown_screen=id; if (id == 1) enter();}
int ls_tui_screen_current(void){return 0;}
int ls_tui_screen_count(void){return 2;}
const char *ls_tui_screen_name(int id){return id==1?"FILES":"P25";}
const char *ls_rsel_name(ls_rsel_radio_t r) { return "LoRa"; }
void ls_keyboard_open(const char *t, const char *s, int n, ls_keyboard_done_t cb) {
    keyboard_limit = n; snprintf(keyboard_initial, sizeof(keyboard_initial), "%.*s", n, s);
}
void ls_picker_back(void (*cb)(void)) { picker_parent = cb; }
void ls_picker_stepper(int i, void (*cb)(int, int)) {}
void ls_picker_set_detail(int i, const char *d) {}
void ls_picker_open(const char *t, ls_picker_done_t cb) { picker_cursor = 0; picker_count = 0; picker_done = cb; }
bool ls_picker_add(const char *l, const char *d) { picker_count++; return true; }
void ls_picker_select(int i) { picker_cursor = i; }
void subghz_file_begin(subghz_file_t *f) { memset(f, 0, sizeof(*f)); }
void subghz_file_line(subghz_file_t *f, const char *line, int32_t *e, int cap) {
    snprintf(loaded, sizeof(loaded), "%s", s_open_path); f->filetype_ok = true;
}
bool subghz_file_load(const char *p, subghz_file_t *f, int32_t *e, int n, char *line, size_t cap)
{
    snprintf(loaded, sizeof(loaded), "%s", p);
    memset(f, 0, sizeof(*f));
    return true;
}
bool subghz_file_is_fsk(const subghz_file_t *f) { return false; }
bool subghz_file_is_cc_fsk(const subghz_file_t *f) { return false; }
bool subghz_file_is_ook(const subghz_file_t *f) { return false; }
bool subghz_file_is_iq(const subghz_file_t *f) { return false; }
const char *subghz_tx_refusal(uint32_t hz, int dbm, size_t edges, uint64_t span_us)
{ (void)hz; (void)dbm; (void)edges; (void)span_us; return NULL; }
bool subghz_pwm_decode(const int32_t *p, int n, subghz_pwm_t *o) { decoder_calls++; return false; }
bool subghz_nrz_decode(const int32_t *p, int n, subghz_nrz_t *o) { decoder_calls++; return false; }
bool rec_decode_ook24(const int32_t *p, int n, rec_ook24_t *o) { decoder_calls++; return false; }
size_t subghz_pwm_format(const subghz_pwm_t *p, char *o, size_t n) { return 0; }
size_t subghz_nrz_format(const subghz_nrz_t *p, char *o, size_t n) { return 0; }

static void reset(void)
{
    job_cancel();
    memset(entries, 0, sizeof(entries));
    s_depth = 0; s_return_to = -1; s_on_return = NULL;
    text_data = NULL; text_data_len = 0;
    rename_calls = 0;
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
static void drain(void) { int budget = 10000; while (s_job != JOB_NONE && budget--) job_step(); LS_EQ_INT(s_job, JOB_NONE); }
static void load_all(void) { load_dir(); drain(); }
static void open_all(void) { open_selected(); drain(); }
static void copy_all(void) {
    if (!entry_count) { snprintf(entries[0].d_name, 256, "%s", open_name()); entry_count = 1; }
    send_to_flipper(); drain();
}
LS_CASE(long_names_never_alias_their_short_prefix)
{
    reset();
    memset(entries[0].d_name, 'a', 63);
    memset(entries[1].d_name, 'a', 251);
    strcpy(entries[1].d_name + 251, ".sub");
    entry_count = 2;
    load_all();
    LS_EQ_STR(s_entry[0].name, entries[1].d_name);
    open_all();
    char want[512];
    snprintf(want, sizeof(want), "%s/%s", ROOT, entries[1].d_name);
    LS_EQ_STR(loaded, want);
    s_view = VIEW_LIST;
    actions_open();
    LS_EQ_STR(s_open_path, want);
    reload_keep(entries[1].d_name); drain();
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
    load_all();
    LS_EQ_INT(s_count, 256);
    LS_CHECK(s_truncated);
    LS_EQ_STR(s_entry[0].name, "capture299.sub");
    LS_EQ_STR(s_entry[255].name, "capture044.sub");
    s_sort = SORT_OLDEST;
    load_all();
    LS_EQ_STR(s_entry[0].name, "capture000.sub");
    LS_EQ_STR(s_entry[255].name, "capture255.sub");
}
LS_CASE(flipper_copy_preserves_an_existing_capture)
{
    reset();
    strcpy(s_open_path, ROOT "/capture.sub");
    collision = true;
    copy_all();
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
    copy_all();
    LS_CHECK(collision);
    LS_EQ_INT(unlinks, 1);
    LS_EQ_STR(deleted, ROOT "/rec/capture-1.sub");
}
LS_CASE(exhausted_copy_names_leave_existing_files_alone)
{
    reset();
    strcpy(s_open_path, ROOT "/capture.sub");
    collision = names_full = true;
    copy_all();
    LS_CHECK(collision);
    LS_EQ_INT(unlinks, 0);
    LS_EQ_STR(copied, "");
}
LS_CASE(capture_decodes_once_per_open_including_no_match)
{
    reset();
    strcpy(entries[0].d_name, "capture.sub"); entry_count = 1;
    load_all(); open_all();
    LS_EQ_INT(decoder_calls, 3);
    tui_cell back[80 * 40], front[80 * 40];
    tui_surface sf;
    tui_surface_setup(&sf, back, front, 80, 40);
    for (int i = 0; i < 5; i++) { tui_frame_begin(&sf); draw_sub(&sf, tui_rect_make(0, 0, 80, 40)); }
    LS_EQ_INT(decoder_calls, 3);
    s_view = VIEW_LIST; open_all();
    LS_EQ_INT(decoder_calls, 6);
}

static void returned_to_calls(void){returned++;}
LS_CASE(explicit_archive_file_action_selects_file_and_back_returns_to_caller) {
    reset();entry_count=1;snprintf(entries[0].d_name,sizeof(entries[0].d_name),"120000_42.wav");
    returned=0;LS_CHECK(ls_scr_files_show_path("/sdcard/lakeshark/calls/20261007/120000_42.wav",returned_to_calls));
    drain(); LS_EQ_INT(shown_screen,1);LS_EQ_STR(s_path,"/sdcard/lakeshark/calls/20261007");LS_EQ_STR(selected_name(),"120000_42.wav");
    LS_CHECK(go_up());LS_EQ_INT(shown_screen,0);LS_EQ_INT(returned,1);
    LS_CHECK(!ls_scr_files_show_path("relative.wav",returned_to_calls));
}

LS_CASE(rename_keeps_all_255_characters_and_no_edit_is_noop) {
    reset(); memset(entries[0].d_name, 'a', 251); strcpy(entries[0].d_name + 251, ".sub"); entry_count = 1;
    load_all(); actions_open(); s_menu[0] = 'N'; s_menu_n = 1; actions_done(0);
    LS_EQ_INT(keyboard_limit, 255); LS_EQ_STR(keyboard_initial, entries[0].d_name);
    rename_done(keyboard_initial); LS_EQ_INT(rename_calls, 0);
    rename_done("bad?.sub"); LS_EQ_INT(rename_calls, 0);
    rename_done("bad\\name.sub"); LS_EQ_INT(rename_calls, 0);
    rename_done("trailing."); LS_EQ_INT(rename_calls, 0);
    char oversized[257]; memset(oversized, 'a', 256); oversized[256] = 0;
    rename_done(oversized); LS_EQ_INT(rename_calls, 0);
}
LS_CASE(text_touch_pages_in_both_orientations) {
    reset(); s_view = VIEW_TEXT;
    memset(s_text, 'x', 2000); s_text_len = s_text_total = 2000; s_text_offset = 0;
    static tui_cell back[120 * 64], front[120 * 64]; tui_surface sf;
    const int widths[] = {120, 42, 24}, heights[] = {24, 64, 14};
    for (int i = 0; i < 3; i++) {
        tui_surface_setup(&sf, back, front, widths[i], heights[i]); s_scroll = 0;
        draw(&sf, tui_rect_make(0, 0, widths[i], heights[i]));
        draw(&sf, tui_rect_make(0, 0, widths[i], heights[i]));
        LS_CHECK(s_touch_nav); LS_CHECK(s_scroll_max > 0);
        if (getenv("LS_FILES_RENDER")) {
            char path[128]; snprintf(path, sizeof(path), "bench/build/files-text-%d.cells", i);
            FILE *dump = fopen(path, "wb");
            if (dump) { fprintf(dump, "%d %d\n", widths[i], heights[i]); fwrite(back, sizeof(*back), widths[i] * heights[i], dump); fclose(dump); }
        }
        bool hit = false;
        for (int y = heights[i] - 5; y < heights[i] && !hit; y++)
            for (int x = 0; x < widths[i] && !hit; x++)
                if (ls_btn_hit_slot(x, y, LS_BTN_SLOT_WATERFALL) == 1) { touch(x, y); hit = true; }
        LS_CHECK(hit); LS_CHECK(s_scroll > 0);
        for (int n = 0; n < 200; n++) page(1);
        LS_EQ_INT(s_scroll, s_scroll_max);
    }
}
LS_CASE(text_crlf_tabs_nul_and_large_file_ranges) {
    reset(); memset(s_text, 'x', 52); memcpy(s_text + 52, "\r\n", 2); s_text_len = 54;
    LS_EQ_INT(text_rows(NULL, tui_rect_make(0, 0, 52, 2)), 2);
    memcpy(s_text, "a\tB\0C", 5); s_text_len = 5;
    LS_EQ_INT(text_rows(NULL, tui_rect_make(0, 0, 4, 2)), 2);
    static char large[TEXT_MAX + 20]; memset(large, 'z', sizeof(large));
    text_data = large; text_data_len = sizeof(large); strcpy(s_open_path, ROOT "/large.txt");
    open_text(); LS_EQ_INT(s_text_len, TEXT_MAX); LS_EQ_INT(s_text_total, sizeof(large));
    s_scroll = s_scroll_max; page(1); LS_EQ_INT(s_text_offset, TEXT_MAX); LS_EQ_INT(s_text_len, 20);
    page(-1); LS_EQ_INT(s_text_offset, 0);
}
LS_CASE(caller_back_and_browser_reentry) {
    reset(); s_view = VIEW_TEXT; enter(); drain(); LS_EQ_INT(s_view, VIEW_LIST);
    ls_scr_files_return_to(0); LS_CHECK(can_go_up()); LS_CHECK(go_up()); LS_EQ_INT(shown_screen, 0);
}
LS_CASE(case_rename_and_depth_limit) {
    reset(); strcpy(s_rename_from, ROOT "/Capture.sub"); rename_done("capture.sub"); drain(); LS_EQ_INT(rename_calls, 2);
    strcpy(entries[0].d_name, "d"); entries[0].d_type = DT_DIR; entry_count = 1; load_all();
    s_depth = DEPTH_MAX; open_selected(); LS_EQ_STR(s_path, ROOT); LS_EQ_INT(s_depth, DEPTH_MAX);
}
LS_CASE(options_child_back_and_steppers) {
    reset(); more_open(); LS_EQ_INT(s_menu_n, 8); option_step(0, 1); LS_EQ_INT(s_sort, SORT_OLDEST);
    more_done(5); LS_CHECK(picker_parent == more_open); picker_parent(); LS_EQ_INT(s_menu_n, 8);
}
LS_CASE(card_jobs_are_bounded_and_copy_cancel_removes_only_new_file) {
    reset(); entry_count = 30;
    for (int i = 0; i < entry_count; i++) snprintf(entries[i].d_name, 256, "f%d", i);
    load_dir(); LS_EQ_INT(s_count, 0); job_step(); LS_EQ_INT(s_count, 8);
    LS_CHECK(key(LS_TK_ESC, 0)); LS_EQ_INT(s_job, JOB_NONE);
    strcpy(s_open_path, ROOT "/f0"); send_to_flipper(); LS_EQ_INT(s_job, JOB_COPY);
    job_step(); touch(0, 0); LS_EQ_INT(s_job, JOB_NONE); LS_EQ_INT(unlinks, 1); LS_EQ_STR(deleted, ROOT "/rec/f0");
}
