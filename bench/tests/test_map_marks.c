/* The map's hand-placed markers and lines: naming, drawing a line point
   by point, and the text file they are kept in. */
#include "ls_test.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ls_map_marks.h"

static const char *scratch(void)
{
    static char path[512];
    const char *tmp = getenv("TEMP");
    snprintf(path, sizeof(path), "%s/test_map_marks.txt", tmp ? tmp : ".");
    return path;
}

static void fresh(void)
{
    remove(scratch());
    ls_marks_use_file(scratch());
}

LS_CASE(unnamed_markers_number_themselves_after_the_highest_so_far)
{
    fresh();
    LS_EQ_INT(ls_marks_add(43.0, -71.0, LS_MARK_STAR, NULL), 0);
    LS_EQ_STR(ls_marks_at(0)->name, "MARK 1");
    LS_EQ_INT(ls_marks_add(43.1, -71.1, LS_MARK_CAMP, "MARK 7"), 1);
    LS_EQ_INT(ls_marks_add(43.2, -71.2, LS_MARK_STAR, ""), 2);
    LS_EQ_STR(ls_marks_at(2)->name, "MARK 8");
}

LS_CASE(markers_off_the_map_are_refused)
{
    fresh();
    LS_EQ_INT(ls_marks_add(91.0, 0.0, 0, NULL), -1);
    LS_EQ_INT(ls_marks_add(0.0, 181.0, 0, NULL), -1);
    LS_EQ_INT(ls_marks_count(), 0);
}

LS_CASE(a_line_needs_two_points_and_undo_takes_the_last)
{
    fresh();
    ls_sketch_begin();
    LS_CHECK(ls_sketch_drawing());
    LS_CHECK(ls_sketch_add_point(43.0, -71.0));
    LS_EQ_INT(ls_sketch_finish(), -1);
    LS_CHECK(!ls_sketch_drawing());
    LS_EQ_INT(ls_sketch_count(), 0);

    ls_sketch_begin();
    ls_sketch_add_point(43.0, -71.0);
    ls_sketch_add_point(43.1, -71.0);
    ls_sketch_add_point(44.0, -71.0);
    LS_CHECK(ls_sketch_undo());
    LS_EQ_INT(ls_sketch_open()->n, 2);
    LS_EQ_INT(ls_sketch_finish(), 0);
    LS_EQ_STR(ls_sketch_at(0)->name, "LINE 1");
    /* A tenth of a degree of latitude is six nautical miles. */
    LS_NEAR(ls_sketch_length_m(ls_sketch_at(0)) / 1852.0, 6.0, 0.05);
}

LS_CASE(everything_survives_a_save_and_a_reload)
{
    fresh();
    ls_marks_add(43.4520, -71.6620, LS_MARK_HAZARD, "WASHOUT  ");
    ls_marks_add(43.4300, -71.6750, LS_MARK_WATER, NULL);
    ls_sketch_begin();
    ls_sketch_add_point(43.45, -71.66);
    ls_sketch_add_point(43.44, -71.64);
    ls_sketch_finish();
    LS_CHECK(ls_marks_save());

    ls_marks_reload();
    LS_EQ_INT(ls_marks_count(), 2);
    LS_EQ_STR(ls_marks_at(0)->name, "WASHOUT");
    LS_EQ_INT(ls_marks_at(0)->icon, LS_MARK_HAZARD);
    LS_NEAR(ls_marks_at(0)->lat, 43.4520, 1e-6);
    LS_EQ_STR(ls_marks_at(1)->name, "MARK 1");
    LS_EQ_INT(ls_sketch_count(), 1);
    LS_EQ_INT(ls_sketch_at(0)->n, 2);
    LS_NEAR(ls_sketch_at(0)->lon[1], -71.64, 1e-5);
    /* Numbering carries on from what was loaded. */
    ls_marks_add(43.0, -71.0, 0, NULL);
    LS_EQ_STR(ls_marks_at(2)->name, "MARK 2");
}

LS_CASE(a_hand_edited_file_with_junk_loads_what_it_can)
{
    FILE *f = fopen(scratch(), "w");
    LS_CHECK(f != NULL);
    fputs("# notes\nM 43.1 -71.1 2 CAMP ONE\nM nonsense\nM 99 0 0 OFF MAP\n"
          "S 2 LINE A\nP 43.0 -71.0\nS 1 SHORT\nP 43.0 -71.0\nP 43.1 -71.0\n", f);
    fclose(f);
    ls_marks_use_file(scratch());
    LS_EQ_INT(ls_marks_count(), 1);
    LS_EQ_STR(ls_marks_at(0)->name, "CAMP ONE");
    /* LINE A lost its second point, so it is not a line. */
    LS_EQ_INT(ls_sketch_count(), 1);
    LS_EQ_STR(ls_sketch_at(0)->name, "SHORT");
}

LS_CASE(delete_and_rename_keep_the_list_in_order)
{
    fresh();
    ls_marks_add(43.0, -71.0, 0, "A");
    ls_marks_add(43.1, -71.0, 0, "B");
    ls_marks_add(43.2, -71.0, 0, "C");
    LS_CHECK(ls_marks_delete(1));
    LS_EQ_STR(ls_marks_at(1)->name, "C");
    LS_CHECK(!ls_marks_rename(0, "   "));
    LS_CHECK(ls_marks_rename(0, "ALPHA"));
    LS_EQ_STR(ls_marks_at(0)->name, "ALPHA");
    LS_CHECK(!ls_marks_set_icon(0, LS_MARK__COUNT));
}
