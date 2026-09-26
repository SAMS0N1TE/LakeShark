#include "ls_test.h"
#include "ls_notes.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

static char dir[128];
static char buf[LS_NOTE_TEXT_MAX];

static void fresh(void)
{
    static int n;
    snprintf(dir, sizeof(dir), "build_notes_test_%ld_%d", (long)time(NULL), ++n);
#ifdef _WIN32
    mkdir(dir);
#else
    mkdir(dir, 0775);
#endif
    ls_notes_test_reset(dir);
    LS_CHECK(ls_notes_start());
}

static void drain(void) { for (int i = 0; i < 20; i++) ls_notes_io_step(); }

static void put(const char *leaf, const char *text)
{
    char path[256]; snprintf(path, sizeof(path), "%s/%s", dir, leaf);
    FILE *f = fopen(path, "wb"); LS_CHECK(f != NULL);
    fputs(text, f); fclose(f);
}

LS_CASE(summary_reads_title_badges_checklist_and_first_place)
{
    ls_note_info_t info = { .stem = "0007_x", .seq = 7 };
    ls_notes_summarize(
        "# Ridge survey #\n2026-09-25T14:00:00Z\n\nWalked the ridge.\n"
        "> GPS 43.200000, -71.650000  alt 120 m\n"
        "> RADIO P25 154.7850 MHz NAC 527 -71 dBm\n"
        "> MAP 43.210000, -71.660000  z15  0007_x-map1.png\n"
        "- [ ] check tower\n- [x] photos\n- [ ] batteries\n", &info);
    LS_EQ_STR(info.title, "Ridge survey");
    LS_EQ_STR(info.when, "2026-09-25 14:00");
    LS_EQ_STR(info.stem, "0007_x");
    LS_EQ_UINT(info.seq, 7);
    LS_EQ_INT(info.checks_open, 2);
    LS_EQ_INT(info.checks_done, 1);
    LS_CHECK(info.badges & LS_NOTE_HAS_GPS);
    LS_CHECK(info.badges & LS_NOTE_HAS_RADIO);
    LS_CHECK(info.badges & LS_NOTE_HAS_MAP);
    LS_CHECK(info.badges & LS_NOTE_HAS_CHECK);
    LS_CHECK(!(info.badges & LS_NOTE_HAS_BEARING));
    LS_CHECK(info.has_place);
    LS_NEAR(info.lat, 43.2, 1e-9); LS_NEAR(info.lon, -71.65, 1e-9);
}

LS_CASE(an_untitled_note_is_named_after_its_first_line)
{
    ls_note_info_t info = {0};
    ls_notes_summarize("\n  tower lights out on the north side\nmore", &info);
    LS_EQ_STR(info.title, "tower lights out on the north side");
    ls_notes_summarize("", &info);
    LS_EQ_STR(info.title, "Untitled");
}

LS_CASE(places_come_from_gps_map_fix_and_the_origin_of_a_bearing)
{
    double lat, lon;
    LS_CHECK(ls_notes_parse_place("> GPS -33.5, 151.25 alt 3 m", &lat, &lon));
    LS_NEAR(lat, -33.5, 1e-9); LS_NEAR(lon, 151.25, 1e-9);
    LS_CHECK(ls_notes_parse_place("> BEARING 212 T  MESH -88 dBm  from 43.1, -71.2", &lat, &lon));
    LS_NEAR(lat, 43.1, 1e-9);
    LS_CHECK(!ls_notes_parse_place("> BEARING 212 T  no origin", &lat, &lon));
    LS_CHECK(!ls_notes_parse_place("> RADIO 154.785 MHz", &lat, &lon));
    LS_CHECK(!ls_notes_parse_place("> GPS 95, 10", &lat, &lon));
    LS_CHECK(!ls_notes_parse_place("GPS 43, 10", &lat, &lon));
    int zoom; char file[64];
    LS_CHECK(ls_notes_parse_map("> MAP 43.21, -71.66  z15  0007_x-map1.png", &lat, &lon, &zoom, file, sizeof(file)));
    LS_EQ_INT(zoom, 15); LS_EQ_STR(file, "0007_x-map1.png");
}

LS_CASE(create_lists_at_once_and_lands_on_the_card_after_the_worker_runs)
{
    fresh();
    drain();
    LS_EQ_INT(ls_notes_count(), 0);
    char stem[LS_NOTE_STEM];
    LS_CHECK(ls_notes_create("# First\nhello\n", stem, sizeof(stem)));
    LS_EQ_INT(ls_notes_count(), 1);
    LS_CHECK(!strncmp(stem, "0001_", 5));
    LS_CHECK(!ls_notes_load(stem, buf, sizeof(buf)));   /* not written yet */
    drain();
    LS_CHECK(ls_notes_load(stem, buf, sizeof(buf)));
    LS_EQ_STR(buf, "# First\nhello\n");
    LS_EQ_STR(ls_notes_status(), "Saved to the SD card");
}

LS_CASE(saving_updates_the_summary_and_a_rescan_finds_the_same_notes)
{
    fresh();
    char a[LS_NOTE_STEM], b[LS_NOTE_STEM];
    LS_CHECK(ls_notes_create("# Alpha\n", a, sizeof(a)));
    LS_CHECK(ls_notes_create("# Beta\n", b, sizeof(b)));
    drain();
    LS_CHECK(ls_notes_save(a, "# Alpha two\n> GPS 1, 2\n"));
    drain();
    ls_note_info_t info;
    LS_CHECK(ls_notes_at(0, &info)); LS_EQ_STR(info.title, "Beta");   /* newest first */
    LS_CHECK(ls_notes_at(1, &info)); LS_EQ_STR(info.title, "Alpha two");
    LS_CHECK(info.badges & LS_NOTE_HAS_GPS);
    put("from-a-computer.md", "# Typed on a laptop\n");
    ls_notes_refresh(); drain();
    LS_EQ_INT(ls_notes_count(), 3);
    LS_CHECK(ls_notes_find("from-a-computer") >= 0);
    /* A new note after a rescan still sorts first. */
    char c[LS_NOTE_STEM];
    LS_CHECK(ls_notes_create("# Gamma\n", c, sizeof(c)));
    LS_CHECK(!strncmp(c, "0003_", 5));
    LS_CHECK(ls_notes_at(0, &info)); LS_EQ_STR(info.title, "Gamma");
}

LS_CASE(trash_moves_the_note_and_its_pictures_and_deletes_nothing)
{
    fresh();
    char stem[LS_NOTE_STEM];
    LS_CHECK(ls_notes_create("# Doomed\n", stem, sizeof(stem)));
    drain();
    char leaf[80];
    ls_notes_picture_leaf(stem, leaf, sizeof(leaf));
    static uint16_t px[4 * 3];
    LS_CHECK(ls_notes_save_png(leaf, px, 4, 3));
    drain();
    LS_CHECK(ls_notes_trash(stem));
    drain();
    LS_EQ_INT(ls_notes_count(), 0);
    char path[256]; struct stat st;
    snprintf(path, sizeof(path), "%s/trash/%s.md", dir, stem);
    LS_CHECK(stat(path, &st) == 0);
    snprintf(path, sizeof(path), "%s/trash/%s.png", dir, leaf);
    LS_CHECK(stat(path, &st) == 0);
    snprintf(path, sizeof(path), "%s/%s.md", dir, stem);
    LS_CHECK(stat(path, &st) != 0);
}

LS_CASE(a_second_picture_while_one_is_pending_is_refused_not_mixed)
{
    fresh();
    static uint16_t px[16];
    LS_CHECK(ls_notes_save_png("x-map1", px, 4, 4));
    LS_CHECK(!ls_notes_save_png("x-map2", px, 4, 4));
    drain();
    LS_CHECK(ls_notes_save_png("x-map2", px, 4, 4));
    LS_CHECK(!ls_notes_save_png("big", px, 1000, 1000));
}

LS_CASE(marks_from_other_apps_become_titled_notes)
{
    fresh();
    LS_CHECK(ls_notes_mark("P25 talkgroup", "> RADIO P25 NAC 527 TG 8501"));
    drain();
    ls_note_info_t info;
    LS_CHECK(ls_notes_at(0, &info));
    LS_EQ_STR(info.title, "P25 talkgroup");
    LS_CHECK(info.badges & LS_NOTE_HAS_RADIO);
}

LS_CASE(a_note_made_before_the_first_scan_still_numbers_after_the_card)
{
    fresh();
    put("0041_2026-01-01T00-00-00Z.md", "# Old\n");
    /* No drain: the queued scan has not run. */
    char stem[LS_NOTE_STEM];
    LS_CHECK(ls_notes_create("# New\n", stem, sizeof(stem)));
    LS_CHECK_MSG(!strncmp(stem, "0042_", 5), "got %s", stem);
    drain();
    ls_note_info_t info;
    LS_CHECK(ls_notes_at(0, &info));
    LS_EQ_STR(info.title, "New");
}
