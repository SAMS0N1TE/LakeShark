/* LS_TEST_SOURCES: ${FW}/components/apps/file_browser/ls_dialog_slot.c */
#include "ls_test.h"
#include "ls_dialog_slot.h"

#include <stdbool.h>
#include <string.h>

/**/

/* --- mock LVGL object ---------------------------------------------------- */

typedef struct {
    int  id;      /* stable identity so a use-after-free is spottable        */
    bool alive;   /* the del hook flips this to false                        */
} fake_obj_t;

static int s_del_count;

static void reset_counters(void)
{
    s_del_count = 0;
}

static void fake_del(void *obj)
{
    fake_obj_t *o = (fake_obj_t *)obj;
    s_del_count++;
    /* Real LVGL frees the object here; the "gone" state is is_valid() -> false.
       Emulate that by flipping alive so a follow-up close cannot re-delete. */
    if (o) o->alive = false;
}

static bool fake_is_valid(const void *obj)
{
    const fake_obj_t *o = (const fake_obj_t *)obj;
    return o && o->alive;
}

static void wire_mocks(void)
{
    reset_counters();
    ls_dialog_slot_hooks_t h = { fake_del, fake_is_valid };
    ls_dialog_slot_configure(&h);
}

/* Helper: fresh "object" plus store it in slot the way showFileDialog does
   (close-then-set - see the comments in FileBrowser::showFileDialog). */
static void open_into(ls_dialog_slot_t *slot, fake_obj_t *o, int id)
{
    o->id    = id;
    o->alive = true;
    ls_dialog_slot_close(slot);
    ls_dialog_slot_set(slot, o);
}

/* --- cases --------------------------------------------------------------- */

LS_CASE(open_records_a_live_object)
{
    wire_mocks();
    ls_dialog_slot_t slot = {0};
    fake_obj_t o;

    open_into(&slot, &o, 1);

    LS_CHECK(ls_dialog_slot_is_open(&slot));
    LS_CHECK(o.alive);
    LS_EQ_INT(s_del_count, 0);          /* nothing to sweep on the first open */
    LS_CHECK(ls_dialog_slot_obj(&slot) == &o);
}

LS_CASE(close_deletes_and_clears)
{
    /* This is the exact regression: the old close() nulled the pointer and
       left the object dangling.  The test insists the del hook fires and the
       slot ends empty in one call. */
    wire_mocks();
    ls_dialog_slot_t slot = {0};
    fake_obj_t o;

    open_into(&slot, &o, 2);
    ls_dialog_slot_close(&slot);

    LS_EQ_INT(s_del_count, 1);
    LS_CHECK(!o.alive);
    LS_CHECK(!ls_dialog_slot_is_open(&slot));
    LS_CHECK(ls_dialog_slot_obj(&slot) == NULL);
}

LS_CASE(close_on_empty_slot_is_a_no_op)
{
    /* pause() and close() both call slot_close; when a user never opened a
       dialog neither call must double-delete or crash. */
    wire_mocks();
    ls_dialog_slot_t slot = {0};

    ls_dialog_slot_close(&slot);
    ls_dialog_slot_close(&slot);

    LS_EQ_INT(s_del_count, 0);
    LS_CHECK(!ls_dialog_slot_is_open(&slot));
}

LS_CASE(reopen_deletes_the_previous_object)
{
    /* showFileDialog can be called while a dialog is already up (e.g. the
       user opens a second file from the list without dismissing the first).
       The slot must destroy the old one before it accepts the new; otherwise
       every reopen leaks a live overlay. */
    wire_mocks();
    ls_dialog_slot_t slot = {0};
    fake_obj_t a, b;

    open_into(&slot, &a, 10);
    open_into(&slot, &b, 11);          /* implicit close of a before set of b */

    LS_EQ_INT(s_del_count, 1);
    LS_CHECK(!a.alive);
    LS_CHECK(b.alive);
    LS_CHECK(ls_dialog_slot_obj(&slot) == &b);
    LS_CHECK(ls_dialog_slot_is_open(&slot));
}

LS_CASE(pause_after_open_leaves_no_live_dialog)
{
    /* The done-when clause the task calls out: switching away from Files must
       not leave the dialog visible.  Model pause() as slot_close and assert
       the object is destroyed and the slot is empty. */
    wire_mocks();
    ls_dialog_slot_t slot = {0};
    fake_obj_t o;

    open_into(&slot, &o, 20);
    /* pause() body from FileBrowser.cpp: ls_dialog_slot_close(&_dialog). */
    ls_dialog_slot_close(&slot);

    LS_EQ_INT(s_del_count, 1);
    LS_CHECK(!o.alive);
    LS_CHECK(!ls_dialog_slot_is_open(&slot));
}

LS_CASE(close_after_pause_does_nothing_extra)
{
    /* closeAll() runs close() after the shell has already paused Files.  The
       second sweep must not double-delete and must not fabricate a phantom
       delete on nothing. */
    wire_mocks();
    ls_dialog_slot_t slot = {0};
    fake_obj_t o;

    open_into(&slot, &o, 30);
    ls_dialog_slot_close(&slot);   /* pause() */
    ls_dialog_slot_close(&slot);   /* close() */

    LS_EQ_INT(s_del_count, 1);
    LS_CHECK(!o.alive);
    LS_CHECK(!ls_dialog_slot_is_open(&slot));
}

LS_CASE(is_open_returns_false_when_hooks_say_stale)
{
    /* If the app container was torn down under LVGL (e.g. by closeAll() on a
       different app path), lv_obj_is_valid returns false even though the
       pointer is still stored.  is_open must trust the hook, not the pointer,
       or dialog-open callbacks would re-enter with a dead handle. */
    wire_mocks();
    ls_dialog_slot_t slot = {0};
    fake_obj_t o = { .id = 40, .alive = true };
    ls_dialog_slot_set(&slot, &o);
    LS_CHECK(ls_dialog_slot_is_open(&slot));

    o.alive = false;               /* something else invalidated the object */
    LS_CHECK(!ls_dialog_slot_is_open(&slot));

    /* close() on a stale handle must not call del on it. */
    reset_counters();
    ls_dialog_slot_close(&slot);
    LS_EQ_INT(s_del_count, 0);
    LS_CHECK(!ls_dialog_slot_is_open(&slot));
}

/* --- re-entry from inside a del hook ------------------------------------ */

static ls_dialog_slot_t *s_reentry_slot;

static void reentry_del(void *obj)
{
    fake_obj_t *o = (fake_obj_t *)obj;
    s_del_count++;
    if (o) o->alive = false;
    /* Real LVGL fires pending click callbacks during teardown; those can call
       back into ls_dialog_slot_close on the same slot.  The module clears
       the slot BEFORE invoking del, so this reentry must see an empty slot
       and be a no-op.  If it double-deletes, s_del_count goes to 2. */
    if (s_reentry_slot) {
        ls_dialog_slot_close(s_reentry_slot);
    }
}

LS_CASE(reentry_from_del_hook_does_not_double_delete)
{
    ls_dialog_slot_hooks_t h = { reentry_del, fake_is_valid };
    ls_dialog_slot_configure(&h);
    reset_counters();

    ls_dialog_slot_t slot = {0};
    fake_obj_t o = { .id = 50, .alive = true };
    s_reentry_slot = &slot;

    ls_dialog_slot_set(&slot, &o);
    ls_dialog_slot_close(&slot);

    LS_EQ_INT(s_del_count, 1);
    LS_CHECK(!o.alive);
    LS_CHECK(!ls_dialog_slot_is_open(&slot));

    s_reentry_slot = NULL;
}

LS_CASE(full_files_teardown_sequence_leaves_no_live_dialog)
{
    /* End-to-end walk of the sequence the shell puts Files through when the
       user opens a properties dialog and then taps a different app in the
       status bar:

         run()             -> establishes the container
         showFileDialog()  -> slot_close (no-op), then slot_set on a new obj
         pause()           -> slot_close on the live obj
         close()           -> slot_close on an already-empty slot

       At the end: exactly one del call, the object is gone, the slot is
       empty.  This case pins the entire done-when: "switching away from
       Files cannot leave its dialog visible or callable". */
    wire_mocks();
    ls_dialog_slot_t slot = {0};
    fake_obj_t o;

    /* run() does not touch the slot except to forget its prior value, which
       is what a fresh {0} slot already reflects. */

    /* showFileDialog: close-then-set. */
    ls_dialog_slot_close(&slot);
    o.id = 60; o.alive = true;
    ls_dialog_slot_set(&slot, &o);
    LS_CHECK(ls_dialog_slot_is_open(&slot));

    /* pause(). */
    ls_dialog_slot_close(&slot);
    LS_CHECK(!ls_dialog_slot_is_open(&slot));
    LS_CHECK(!o.alive);

    /* close(). */
    ls_dialog_slot_close(&slot);

    LS_EQ_INT(s_del_count, 1);
    LS_CHECK(!ls_dialog_slot_is_open(&slot));
}
