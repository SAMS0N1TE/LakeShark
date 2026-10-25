#include "ls_lap_api.h"
#include "ls_lap_storage.h"
#include "ls_lap_sys.h"
#include "ls_value.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#ifdef _WIN32
#include <direct.h>
#define MAKE_DIR(p) _mkdir(p)
#else
#define MAKE_DIR(p) mkdir(p, 0777)
#endif
#include <limits.h>
#include <unistd.h>
static unsigned image_calls, image_clears;
void ls_tui_image(tui_rect r, const uint16_t *p, int w, int h, uint32_t serial) {
    if (!p) {
        assert(!r.w && !r.h);
        image_clears++;
        return;
    }
    (void)serial;
    assert(r.x >= 3 && r.y >= 2 && r.x + r.w <= 13 && r.y + r.h <= 10);
    assert(p && w == 2 && h == 2);
    image_calls++;
}
void ls_tui_image_ring(tui_rect r, const uint16_t *p, int w, int h, int x0, int y0, int dx, int dy,
                       uint32_t serial) {
    assert(!x0 && !y0 && dx == dy);
    ls_tui_image(r, p, w, h, serial);
}
bool ls_value_read(const char *path, ls_val_t *out, const char **unit) {
    *unit = NULL;
    if (strcmp(path, "test.value"))
        return false;
    *out = (ls_val_t){.kind = LS_VAL_FLOAT, .f = 1.25f};
    return true;
}
static void write_file(const char *p, const char *text) {
    FILE *f = fopen(p, "wb");
    assert(f);
    assert(fwrite(text, 1, strlen(text), f) == strlen(text));
    assert(!fclose(f));
}
int main(void) {
    unsigned checks = 0;
    ls_lap_api_begin("probe", 1, 0);
    size_t count;
    const ls_lap_export_t *e = ls_lap_exports(&count);
    assert(count >= 65);
    for (size_t i = 0; i < count; i++) {
        assert(e[i].address && ls_lap_resolve(e[i].name) == e[i].address);
        if (i)
            assert(strcmp(e[i - 1].name, e[i].name) < 0);
        checks++;
    }
    assert(!ls_lap_resolve("socket") && !strcmp(ls_lap_missing_symbol(), "needs socket"));
    checks++;
    void *p = ls_lap_calloc(10, 4);
    assert(p);
    for (int i = 0; i < 40; i++)
        assert(!((char *)p)[i]);
    checks++;
    assert(!ls_lap_calloc(SIZE_MAX, 2));
    assert(!ls_lap_malloc(LS_LAP_MAX_MEMORY));
    assert(strstr(ls_lap_api_error(), "4 MB"));
    checks++;
    int foreign;
    ls_lap_free(&foreign);
    assert(!ls_lap_realloc(&foreign, 8));
    assert(ls_lap_api_bad_frees() == 2);
    checks++;
    void *q = ls_lap_realloc(p, 100);
    assert(q);
    ls_lap_free(q);
    ls_lap_free(q);
    assert(ls_lap_api_bad_frees() == 3);
    assert(!ls_lap_api_bytes());
    checks++;
    tui_cell back[20 * 12], front[20 * 12];
    tui_surface sf;
    tui_surface_setup(&sf, back, front, 20, 12);
    tui_frame_begin(&sf);
    tui_cell before[20 * 12];
    memcpy(before, back, sizeof(back));
    lap_clear(7);
    lap_put_cell(1, 1, 'X', 7);
    assert(!memcmp(before, back, sizeof(back)));
    checks++;
    ls_lap_api_draw_begin(&sf, tui_rect_make(3, 2, 10, 8));
    ls_lap_api_callback(true);
    assert(lap_cols() == 10 && lap_rows() == 8);
    lap_fill(INT_MIN, INT_MIN, INT_MAX, INT_MAX, 'x', 0);
    lap_fill(-8, -8, INT_MAX, INT_MAX, 'A', 0x12);
    lap_put_str(-1, 0, "hello", 7);
    lap_box(-2, -1, 8, 6, "clip", 7);
    lap_big_char(-1, 3, 'A', 7);
    int16_t legal[] = {32, 126, 128, 159, 192, 255, 0x2800, 0x28ff};
    for (unsigned i = 0; i < sizeof(legal) / sizeof(legal[0]); i++) {
        lap_put_cell((int)i, 1, legal[i], 7);
        assert(back[3 * 20 + 3 + i].ch == legal[i]);
        checks++;
    }
    int16_t bad[] = {0, 31, 127, 160, 191, 256, 0x27ff, 0x2900, -1};
    for (unsigned i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        lap_put_cell((int)i, 2, bad[i], 7);
        assert(back[4 * 20 + 3 + i].ch == '?');
        checks++;
    }
    uint16_t image[4] = {0};
    lap_image(1, 1, 2, 2, image, 2, 2, 1);
    assert(image_calls == 1);
    checks++;
    for (int y = 0; y < 12; y++)
        for (int x = 0; x < 20; x++)
            if (x < 3 || x >= 13 || y < 2 || y >= 10)
                assert(!memcmp(&back[y * 20 + x], &before[y * 20 + x], sizeof(tui_cell)));
    checks++;
    float v;
    char text[20];
    assert(lap_value_float("test.value", &v) && v == 1.25f);
    assert(lap_value_text("test.value", text, sizeof(text)) && !strcmp(text, "1.25"));
    checks++;
    size_t before_log = ls_lap_api_bytes();
    for (int i = 0; i < 9; i++)
        lap_log("bounded log %d\n", i);
    assert(strstr(ls_lap_api_error(), "log queue full") && ls_lap_api_bytes() == before_log);
    ls_lap_api_log_service();
    checks++;
    ls_lap_api_draw_end();
    ls_lap_api_draw_begin(&sf, tui_rect_make(3, 2, 10, 8));
    ls_lap_api_draw_end();
    assert(image_clears == 1);
    checks++;
    assert(!lap_cols());
    memcpy(before, back, sizeof(back));
    lap_clear(4);
    assert(!memcmp(before, back, sizeof(back)));
    checks++;
    ls_lap_api_callback(false);
    assert(!lap_value_float("test.value", &v));
    assert(lap_file_open("note", "w") < 0);
    checks++;
    const char *root = "lap-api-data";
    MAKE_DIR(root);
    assert(ls_lap_api_files_load(root));
    ls_lap_api_callback(true);
    const char *bad_names[] = {"../x", ".hidden", "x/y", "x\\y", "a..b", "", "x y"};
    for (unsigned i = 0; i < 7; i++) {
        assert(lap_file_open(bad_names[i], "w") < 0);
        checks++;
    }
    assert(lap_file_open("note", "rb") < 0);
    assert(lap_file_open("note", "r+") < 0);
    checks++;
    int fd = lap_file_open("note", "w");
    assert(fd >= 0);
    assert(lap_file_write(fd, "hello", 5) == 5);
    lap_file_close(fd);
    fd = lap_file_open("note", "a");
    assert(lap_file_write(fd, "!", 1) == 1);
    lap_file_close(fd);
    assert(lap_file_size("note") == 6);
    checks++;
    int handles[4];
    for (int i = 0; i < 4; i++) {
        handles[i] = lap_file_open("note", "r");
        assert(handles[i] >= 0);
    }
    assert(lap_file_open("note", "r") < 0);
    assert(!lap_file_remove("note"));
    checks++;
    char read[8] = {0};
    assert(lap_file_read(handles[0], read, sizeof(read)) == 6 && !strcmp(read, "hello!"));
    assert(lap_file_read(handles[0], read, 8) == 0);
    checks++;
    for (int i = 0; i < 4; i++)
        lap_file_close(handles[i]);
    ls_lap_api_callback(false);
    assert(ls_lap_api_files_flush());
    ls_lap_api_cleanup();
    assert(!ls_lap_api_bytes());
    checks++;
    ls_lap_api_begin("probe", 2, 0);
    assert(ls_lap_api_files_load(root));
    ls_lap_api_callback(true);
    fd = lap_file_open("note", "r");
    memset(read, 0, sizeof(read));
    assert(lap_file_read(fd, read, 8) == 6 && !strcmp(read, "hello!"));
    lap_file_close(fd);
    assert(lap_file_remove("note"));
    assert(lap_file_size("note") == -1);
    ls_lap_api_callback(false);
    assert(ls_lap_api_files_flush());
    ls_lap_api_cleanup();
    assert(!ls_lap_api_bytes());
    checks++;
    write_file("lap-api-data/probe/.lap-old-note", "old");
    assert(ls_lap_recover_dir("lap-api-data/probe"));
    write_file("lap-api-data/probe/.lap-write", "new");
    assert(ls_lap_replace("lap-api-data/probe/.lap-write", "lap-api-data/probe/note"));
    write_file("lap-api-data/probe/.lap-old-note", "old");
    assert(ls_lap_recover_dir("lap-api-data/probe"));
    FILE *f = fopen("lap-api-data/probe/note", "rb");
    assert(f);
    assert(fread(read, 1, 3, f) == 3 && !memcmp(read, "new", 3));
    fclose(f);
    checks++;
    assert(!ls_lap_replace("lap-api-data/probe/missing", "lap-api-data/probe/note"));
    f = fopen("lap-api-data/probe/note", "rb");
    assert(f);
    fclose(f);
    checks++;
    remove("lap-api-data/probe/note");
    rmdir("lap-api-data/probe");
    rmdir(root);
    /* API 1.1 ------------------------------------------------------------ */
    {
        ls_lap_api_begin("probe", 7, 0);
        tui_cell back2[20 * 12], front2[20 * 12];
        tui_surface sf2;
        tui_surface_setup(&sf2, back2, front2, 20, 12);
        tui_frame_begin(&sf2);
        ls_lap_sys_begin();
        ls_lap_sys_frame(tui_rect_make(0, 0, 20, 12));
        ls_lap_api_draw_begin(&sf2, tui_rect_make(0, 0, 20, 12));
        ls_lap_api_callback(true);
        /* lap_dots: one cell, ink the commoner colour, paper the other. */
        const uint8_t dots[6] = {LAP_RED, LAP_RED, LAP_RED, LAP_DOT_CLEAR, LAP_RED, LAP_DOT_CLEAR};
        lap_dots(2, 3, 1, 1, dots);
        tui_cell c = back2[3 * 20 + 2];
        assert((uint8_t)c.ch == (0xC0 | 1 | 2 | 4 | 16));
        assert(c.attr == LAP_ATTR(LAP_RED, LAP_BLACK));
        const uint8_t flat[6] = {LAP_GREEN, LAP_GREEN, LAP_GREEN, LAP_GREEN, LAP_GREEN, LAP_GREEN};
        lap_dots(-1, 0, 1, 1, flat); /* clipped, no write */
        lap_dots(4, 0, 1, 1, flat);
        assert((uint8_t)back2[4].ch == 0xFF && back2[4].attr == LAP_ATTR(LAP_GREEN, LAP_BLACK));
        checks++;
        /* Notes: in range only, and the queue holds 32. */
        assert(!lap_tone(-1, 10, 50, LAP_WAVE_SINE) && !lap_tone(440, 0, 50, 0) && !lap_tone(440, 10, 101, 0) &&
               !lap_tone(440, 10, 50, 9));
        int queued = 0;
        while (lap_tone(440, 10, 50, LAP_WAVE_SQUARE))
            queued++;
        assert(queued == 32 && lap_tone_busy());
        lap_tone_stop();
        assert(!lap_tone_busy());
        checks++;
        /* Touch and swipes come from the sensor stand-in, swipes once each. */
        ls_lap_sys_host_t *h = ls_lap_sys_host();
        lap_touch_t tc;
        assert(!lap_touch(&tc));
        h->down = true;
        h->x = 30;
        h->y = 40;
        h->x0 = 10;
        h->y0 = 12;
        h->held_ms = 80;
        h->swipe = LAP_SWIPE_LEFT;
        ls_lap_api_callback(false);
        ls_lap_sys_frame(tui_rect_make(0, 0, 20, 12));
        ls_lap_api_callback(true);
        assert(lap_touch(&tc) && tc.down && tc.x == 30 && tc.y0 == 12 && tc.held_ms == 80);
        assert(lap_swipe() == LAP_SWIPE_LEFT && lap_swipe() == LAP_SWIPE_NONE);
        checks++;
        /* The card: paths, then a write, a read back, a list and a remove, as jobs. */
        MAKE_DIR("lap-sd");
        assert(lap_sd_size("/etc/passwd") == LAP_SD_EINVAL && lap_sd_remove("lap-sd") == LAP_SD_EINVAL && lap_sd_mkdir("lap-sd/") == LAP_SD_EINVAL);
        assert(lap_sd_size("lap-sd/../x") == LAP_SD_EINVAL && lap_sd_size("lap-sd/a/../../x") == LAP_SD_EINVAL);
        assert(lap_sd_write("lap-sd/apps/evil.lap", 0, "x", 1) == LAP_SD_EINVAL);
        assert(lap_sd_remove("lap-sd/apps") == LAP_SD_EINVAL);
        char stack_buf[8];
        assert(lap_sd_read("lap-sd/f.txt", 0, stack_buf, 4) == LAP_SD_EINVAL); /* not the app's memory */
        int j = lap_sd_write("lap-sd/f.txt", 0, "hello card", 10);
        assert(j >= 0 && lap_sd_poll(j) == LAP_SD_PENDING);
        assert(ls_lap_sys_service() && lap_sd_poll(j) == 10);
        assert(lap_sd_poll(j) == LAP_SD_EINVAL); /* collected once */
        j = lap_sd_write("lap-sd/f.txt", LAP_SD_APPEND, "!", 1);
        ls_lap_sys_service();
        assert(lap_sd_poll(j) == 1);
        char *buf = ls_lap_malloc(32);
        j = lap_sd_read("lap-sd/f.txt", 6, buf, 32);
        ls_lap_sys_service();
        assert(lap_sd_poll(j) == 5 && !memcmp(buf, "card!", 5));
        lap_sd_entry_t *list = ls_lap_calloc(8, sizeof(lap_sd_entry_t));
        j = lap_sd_list("lap-sd", list, 8);
        ls_lap_sys_service();
        {
            const int r = lap_sd_poll(j);
            assert(r == 1 && !strcmp(list[0].name, "f.txt") && !list[0].dir && list[0].size == 11);
        }
        j = lap_sd_size("lap-sd/nope");
        ls_lap_sys_service();
        assert(lap_sd_poll(j) == LAP_SD_ENOENT);
        /* A read whose buffer is freed before it is collected copies nothing. */
        j = lap_sd_read("lap-sd/f.txt", 0, buf, 32);
        ls_lap_free(buf);
        ls_lap_sys_service();
        assert(lap_sd_poll(j) == LAP_SD_EINVAL);
        j = lap_sd_remove("lap-sd/f.txt");
        ls_lap_sys_service();
        assert(lap_sd_poll(j) == 0);
        /* Five jobs do not fit in four slots; closing drops what is queued. */
        for (int i = 0; i < 4; i++)
            assert(lap_sd_size("lap-sd/x") >= 0);
        assert(lap_sd_size("lap-sd/x") == LAP_SD_EBUSY);
        ls_lap_api_callback(false);
        ls_lap_sys_end();
        assert(!ls_lap_sys_service());
        ls_lap_api_callback(true);
        assert(lap_sd_size("lap-sd/x") >= 0);
        ls_lap_api_callback(false);
        ls_lap_sys_end();
        ls_lap_api_draw_end();
        ls_lap_api_cleanup();
        rmdir("lap-sd");
        checks++;
    }
    printf("test_lap_api: %u cases passed\n", checks);
    return 0;
}
