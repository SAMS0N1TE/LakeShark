/* NOTES driven through its real screen, router and storage worker. */
#include "ls_test.h"
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
#ifdef _WIN32
#include <direct.h>
#include <process.h>
#define getpid _getpid
#define getcwd _getcwd
#define chdir _chdir
#define popen _popen
#define pclose _pclose
#else
#include <unistd.h>
#endif

static char s_base[1024], s_out[64 * 1024];

static void fresh(const char *text)
{
    static int seq;
    if (!s_base[0]) getcwd(s_base, sizeof(s_base));
    chdir(s_base);
    char dir[64]; snprintf(dir, sizeof(dir), "notes-ui-%ld-%d", (long)getpid(), ++seq);
    ls_test_mkdir(dir); LS_CHECK(chdir(dir) == 0);
    ls_test_mkdir("fixtures"); ls_test_mkdir("fixtures/notes");
    FILE *f = fopen("fixtures/notes/9000_test.md", "wb"); LS_CHECK(f != NULL);
    fputs(text, f); fclose(f);
}

static void run(const char *opts, const char *image)
{
    char cmd[2048];
    snprintf(cmd, sizeof(cmd), "\"%s\" notes -d -o %s %s 2>&1", LSSIM_EXE, image, opts);
    FILE *p = popen(cmd, "r"); LS_CHECK(p != NULL);
    size_t n = 0; char line[512];
    while (fgets(line, sizeof(line), p)) {
        size_t k = strlen(line);
        if (n + k + 1 < sizeof(s_out)) { memcpy(s_out + n, line, k); n += k; }
    }
    s_out[n] = 0;
    LS_EQ_INT(pclose(p), 0);
    FILE *f = fopen("grid.txt", "wb"); if (f) { fputs(s_out, f); fclose(f); }
}

static bool said(const char *text) { return strstr(s_out, text) != NULL; }

LS_CASE(failed_done_keeps_the_editor_and_original_file)
{
    fresh("# Original\nBody\n");
    ls_test_mkdir("build"); ls_test_mkdir("build/lssim_notes"); ls_test_mkdir("build/lssim_notes/9000_test.tmp");
    run("-x +o,+e,+LOST_CHANGE,@esc,~2", "failed.bmp");
    LS_CHECK_MSG(said("SAVE FAILED") && said("LOST CHANGE"), "%s", s_out);
    FILE *f = fopen("build/lssim_notes/9000_test.md", "rb"); LS_CHECK(f != NULL);
    char text[100] = {0}; fread(text, 1, sizeof(text) - 1, f); fclose(f);
    LS_EQ_STR(text, "# Original\nBody\n");
}

LS_CASE(done_returns_to_the_reader_only_after_the_write_succeeds)
{
    fresh("# Original\nBody\n");
    run("-x +o,+e,+EDITED,@esc,~2", "done.bmp");
    LS_CHECK_MSG(said("EDITED") && !said("DONE"), "%s", s_out);
    FILE *f = fopen("build/lssim_notes/9000_test.md", "rb"); LS_CHECK(f != NULL);
    char text[100] = {0}; fread(text, 1, sizeof(text) - 1, f); fclose(f);
    LS_CHECK(strstr(text, "EDITED") != NULL);
}

LS_CASE(oversized_note_cannot_enter_the_editor)
{
    static char text[17010]; memset(text, 'a', sizeof(text) - 1); text[sizeof(text) - 1] = 0;
    memcpy(text, "# Large\n", 8); fresh(text);
    run("-x +o,+e", "oversize.bmp");
    LS_CHECK_MSG(said("note too large"), "%s", s_out);
    struct stat st; LS_CHECK(stat("build/lssim_notes/9000_test.md", &st) == 0);
    LS_EQ_UINT(st.st_size, sizeof(text) - 1);
}

LS_CASE(reader_shows_the_tail_of_a_long_line_in_both_orientations)
{
    static char text[800];
    snprintf(text, sizeof(text), "# Long line\n");
    for (int i = 0; i < 55; i++) strcat(text, "paragraph ");
    strcat(text, "LONG_LINE_TAIL\n");
    fresh(text); run("-x +o", "long-p.bmp");
    LS_CHECK_MSG(said("LONG_LINE_TAIL"), "%s", s_out);
    run("-l -x +o", "long-l.bmp");
    LS_CHECK_MSG(said("LONG_LINE_TAIL"), "%s", s_out);
}

LS_CASE(scrolling_preserves_the_visible_continuation_of_a_wrapped_paragraph)
{
    static char text[3000]; text[0] = 0;
    for (int i = 0; i < 150; i++) strcat(text, "VISIBLE ");
    strcat(text, "\nEND\n"); fresh(text);
    run("-x +o,@down,@down", "scroll-p.bmp");
    LS_CHECK_MSG(said("VISIBLE"), "%s", s_out);
    run("-l -x +o,@down,@down", "scroll-l.bmp");
    LS_CHECK_MSG(said("VISIBLE"), "%s", s_out);
}

LS_CASE(reader_touch_pages_to_the_end_and_keyboard_ticks_a_checklist)
{
    static char text[2000]; snprintf(text, sizeof(text), "# Checklist\n- [ ] First\n");
    for (int i = 0; i < 70; i++) strcat(text, "row\n");
    strcat(text, "READER_END\n"); fresh(text);
    run("-x +o,20:60,20:60,20:60", "touch-read.bmp");
    LS_CHECK_MSG(said("READER_END"), "%s", s_out);
    run("-x +o,+_,@enter,~2", "check.bmp");
    LS_CHECK_MSG(said("[x]"), "%s", s_out);
}

LS_CASE(list_touch_can_reach_notes_beyond_the_initial_page)
{
    fresh("# Top\n");
    for (int i = 1; i <= 25; i++) {
        char leaf[100]; snprintf(leaf, sizeof(leaf), "fixtures/notes/%04d_test.md", 9000 - i);
        FILE *f = fopen(leaf, "wb"); LS_CHECK(f != NULL); fprintf(f, "# Item %d\n", i); fclose(f);
    }
    run("-x 48:60,48:60,48:60", "touch-list.bmp");
    LS_CHECK_MSG(said("Item 25"), "%s", s_out);
}

static void center(double *lat, double *lon)
{
    const char *z = strstr(s_out, "  z12");
    LS_CHECK_MSG(z != NULL, "%s", s_out);
    if (!z) return;
    const char *line = z;
    while (line > s_out && line[-1] != '\n') line--;
    int row; LS_EQ_INT(sscanf(line, "%d %lf, %lf", &row, lat, lon), 3);
}

LS_CASE(map_four_arrows_pan_and_tab_keeps_toolbar_navigation)
{
    fresh("# Map\n");
    double lat = 0, lon = 0;
    run("-x +o,+e,@f1,@down,@down,@down,@enter,@up,@down", "map-base.bmp");
    center(&lat, &lon); double base_lat = lat, base_lon = lon;
    run("-x +o,+e,@f1,@down,@down,@down,@enter,@right", "map-east.bmp");
    center(&lat, &lon); LS_CHECK(lon > base_lon); LS_NEAR(lat, base_lat, .00001);
    run("-x +o,+e,@f1,@down,@down,@down,@enter,@left", "map-west.bmp");
    center(&lat, &lon); LS_CHECK(lon < base_lon);
    run("-x +o,+e,@f1,@down,@down,@down,@enter,@up", "map-north.bmp");
    center(&lat, &lon); LS_CHECK(lat > base_lat);
    run("-x +o,+e,@f1,@down,@down,@down,@enter,@down,@tab", "map-p.bmp");
    center(&lat, &lon); LS_CHECK(lat < base_lat);
    LS_CHECK_MSG(said("CANCEL") && said("PICTURE"), "%s", s_out);
    run("-l -x +o,+e,@f1,@down,@down,@down,@enter,90:18", "map-touch-l.bmp");
    center(&lat, &lon); LS_CHECK(lon > base_lon);
    LS_CHECK_MSG(said("CANCEL") && said("PICTURE"), "%s", s_out);
}
