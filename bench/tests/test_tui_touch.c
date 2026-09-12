/* LS_TEST_SOURCES: ls_tui_touch.c with the controller and the grid faked */

#include "ls_test.h"
#include "ls_tui_touch.h"

#include <string.h>
#include <stdbool.h>
#include <stdint.h>

/* ---------------------------------------------------------------- fakes -- */

#define CELL_W 10
#define CELL_H 17
#define COLS   115
#define ROWS   27

static uint16_t s_x, s_y;
static bool     s_pressed;
static bool     s_read_ok = true;
static int      s_reads;

/* Modelled on the real driver, including the part that matters: the
   controller's report carries no coordinates once the finger is up, so the
   driver holds the last ones and reports those.

   An earlier version of this fake wrote the current coordinates on every
   read, press or release. That is more generous than the hardware and it hid
   a real defect: the driver left the caller's locals untouched on release,
   the caller zeroes them before every read, and every release therefore
   resolved to pixel 0,0 - inside the inset margin, so no cell, so no tap ever
   completed. Touch did not work at all and these tests passed. */
static uint16_t s_held_x, s_held_y;

bool ls_touch_read(uint16_t *x, uint16_t *y, bool *pressed)
{
    s_reads++;
    if (!s_read_ok) return false;
    if (s_pressed) { s_held_x = s_x; s_held_y = s_y; }
    if (x) *x = s_held_x;
    if (y) *y = s_held_y;
    if (pressed) *pressed = s_pressed;
    return true;
}

bool ls_tui_pixel_to_cell(int nx, int ny, int *col, int *row)
{
    if (nx < 0 || ny < 0) return false;
    int c = nx / CELL_W, r = ny / CELL_H;
    if (c >= COLS || r >= ROWS) return false;
    if (col) *col = c;
    if (row) *row = r;
    return true;
}

/* ------------------------------------------------------------- helpers -- */

/* Put the finger on the centre of a cell. */
static void finger_at(int col, int row, bool down)
{
    s_x = (uint16_t)(col * CELL_W + CELL_W / 2);
    s_y = (uint16_t)(row * CELL_H + CELL_H / 2);
    s_pressed = down;
}

static bool poll(ls_tui_touch_t *t)
{
    ls_tui_touch_t scratch;
    return ls_tui_touch_poll(t ? t : &scratch);
}

/* Drain any latched state so each case starts from the same place. */
static void settle(void)
{
    s_read_ok = true;
    finger_at(0, 0, false);
    for (int i = 0; i < 4; i++) poll(NULL);
}

/* ---------------------------------------------------------------- cases -- */

LS_CASE(a_press_alone_fires_nothing)
{
    settle();
    finger_at(10, 5, true);
    LS_CHECK(!poll(NULL));
    /* And staying down keeps firing nothing, however long it is held. */
    for (int i = 0; i < 20; i++) LS_CHECK(!poll(NULL));
}

LS_CASE(a_press_and_release_on_one_cell_fires_that_cell)
{
    settle();
    finger_at(10, 5, true);
    poll(NULL);

    finger_at(10, 5, false);
    ls_tui_touch_t t = { -1, -1 };
    LS_CHECK(poll(&t));
    LS_EQ_INT(10, t.col);
    LS_EQ_INT(5, t.row);
}

LS_CASE(a_release_fires_once_and_not_again)
{
    settle();
    finger_at(3, 3, true);
    poll(NULL);
    finger_at(3, 3, false);
    LS_CHECK(poll(NULL));
    /* The finger is still off; nothing more may come of it. */
    for (int i = 0; i < 10; i++) LS_CHECK(!poll(NULL));
}

LS_CASE(a_drag_across_cells_fires_nothing)
{
    /* The reason this layer exists. Firing the control under the finger at
       release is how a list activates the row you scrolled past. */
    settle();
    finger_at(10, 5, true);
    poll(NULL);
    for (int c = 11; c <= 20; c++) { finger_at(c, 5, true); LS_CHECK(!poll(NULL)); }
    finger_at(20, 5, false);
    LS_CHECK_MSG(!poll(NULL), "a drag from 10,5 to 20,5 fired");
}

LS_CASE(a_drag_that_returns_to_the_first_cell_still_fires)
{

    settle();
    finger_at(10, 5, true);
    poll(NULL);
    finger_at(14, 5, true); poll(NULL);
    finger_at(10, 5, true); poll(NULL);
    s_pressed = false;
    LS_CHECK(poll(NULL));
}

LS_CASE(a_finger_dragged_off_the_grid_and_lifted_fires_nothing)
{
    /* The finger has to be seen off the grid while still down for that to be
       where it ended - a release carries no position of its own. Dragging
       onto the bezel and lifting is a cancelled tap. */
    settle();
    finger_at(10, 5, true);
    poll(NULL);

    s_x = (uint16_t)(COLS * CELL_W + 50);
    s_y = 20;
    s_pressed = true;                /* still down, now off the grid */
    poll(NULL);

    s_pressed = false;
    LS_CHECK_MSG(!poll(NULL), "a tap cancelled onto the bezel still fired");
}

LS_CASE(a_press_off_the_grid_cannot_be_completed_on_it)
{
    /* A press that began on the bezel is not a tap on whatever cell the
       finger happens to be over when it lifts. */
    settle();
    s_x = (uint16_t)(COLS * CELL_W + 50);
    s_y = 20;
    s_pressed = true;
    poll(NULL);

    finger_at(4, 4, false);
    LS_CHECK_MSG(!poll(NULL), "a press off the grid completed on a cell");
}

LS_CASE(a_failed_read_reports_nothing)
{
    settle();
    s_read_ok = false;
    for (int i = 0; i < 5; i++) LS_CHECK(!poll(NULL));
    s_read_ok = true;
}

LS_CASE(a_read_that_fails_across_a_release_still_fires_the_pressed_cell)
{
    /* The touch controller shares an I2C bus that has NACKed before, so
       ls_touch_read can fail. A failure that swallows the release does not
       lose the tap: the driver holds the last position it was given, which is
       where the press was, so when the bus recovers the release is attributed
       to the cell the finger was actually on.

       The important half is which cell. Moving the finger while the bus is
       down reports nothing, so it cannot drag the tap somewhere the user
       never pressed. */
    settle();
    finger_at(10, 5, true);
    poll(NULL);

    s_read_ok = false;
    s_pressed = false;
    for (int i = 0; i < 3; i++) LS_CHECK(!poll(NULL));

    s_x = 40 * CELL_W;
    s_y = 12 * CELL_H;
    s_read_ok = true;

    ls_tui_touch_t t = { -1, -1 };
    LS_CHECK_MSG(poll(&t), "a late release lost the tap entirely");
    LS_EQ_INT(10, t.col);
    LS_EQ_INT(5, t.row);
}

LS_CASE(the_poll_survives_a_null_output)
{
    settle();
    finger_at(2, 2, true);
    ls_tui_touch_poll(NULL);
    finger_at(2, 2, false);
    /* Returns true with nowhere to put the result, and must not write through
       the null. */
    LS_CHECK(ls_tui_touch_poll(NULL));
}

LS_CASE(a_release_seen_late_on_the_same_cell_still_counts)
{
    /* The user did press and release that cell. Observing it a few polls
       later does not make it a different gesture. */
    settle();
    finger_at(7, 9, true);
    poll(NULL);

    s_read_ok = false;
    finger_at(7, 9, false);
    for (int i = 0; i < 3; i++) LS_CHECK(!poll(NULL));

    s_read_ok = true;
    ls_tui_touch_t t = { -1, -1 };
    LS_CHECK_MSG(poll(&t), "a late release on the pressed cell was dropped");
    LS_EQ_INT(7, t.col);
    LS_EQ_INT(9, t.row);
}

LS_CASE(a_gesture_lost_to_the_bus_cannot_fire_a_cell_that_was_never_pressed)
{

    settle();
    finger_at(10, 5, true);
    poll(NULL);

    s_read_ok = false;
    finger_at(10, 5, false); poll(NULL);   /* A's release, unseen */
    finger_at(40, 20, true); poll(NULL);   /* B's press, unseen   */
    s_read_ok = true;

    s_pressed = false;
    ls_tui_touch_t t = { -1, -1 };
    if (poll(&t))
        LS_CHECK_MSG(t.col == 10 && t.row == 5,
                     "fired cell %d,%d, which was never reported pressed",
                     t.col, t.row);
}

LS_CASE(a_tap_that_wobbles_a_cell_between_press_and_release_still_fires)
{
    settle();
    finger_at(10, 5, true);
    poll(NULL);

    /* One cell right and one down: 10 px and 17 px, well inside a fingertip
       and nothing like a gesture. */
    finger_at(11, 6, true);
    poll(NULL);
    s_pressed = false;

    ls_tui_touch_t t = { -1, -1 };
    LS_CHECK_MSG(poll(&t), "a one-cell wobble cancelled the tap");
    LS_CHECK_MSG(t.col == 10 && t.row == 5,
                 "fired %d,%d - a tap belongs to the cell it was pressed on, "
                 "not the one the finger drifted to", t.col, t.row);
}

LS_CASE(a_wobble_past_the_slop_is_still_a_drag)
{
    /* Far enough that no finger roll explains it. The guard has to keep
       refusing these or 's whole reason for existing is gone. */
    settle();
    finger_at(10, 5, true);
    poll(NULL);
    finger_at(15, 5, true);          /* 50 px, past the 32 px slop */
    poll(NULL);
    s_pressed = false;
    LS_CHECK_MSG(!poll(NULL), "a 50 px drag fired a tap");
}

LS_CASE(a_release_carries_the_position_the_finger_was_last_at)
{

    settle();
    finger_at(9, 4, true);
    poll(NULL);

    /* Release with the controller reporting nothing new, as it does. */
    s_pressed = false;
    ls_tui_touch_t t = { -1, -1 };
    LS_CHECK_MSG(poll(&t), "a release with no fresh coordinates fired nothing");
    LS_EQ_INT(9, t.col);
    LS_EQ_INT(4, t.row);
}
