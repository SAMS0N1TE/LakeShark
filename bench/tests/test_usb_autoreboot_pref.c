/* LS_TEST_SOURCES: ${FW}/components/lakeshark/core/usb_autoreboot_pref.c */
/* LS_TEST_INCLUDE: ${FW}/components/lakeshark/core */
/**/
/* Before this fix, app_set_usb_autoreboot only wrote to a process-static
   bool in app_registry initialized to false, so USB AUTO-REBOOT sat in the
   Settings and P25 CONFIG rows as if it were a persistent device setting
   but flipped back to OFF on every power cycle.  The state was extracted
   into usb_autoreboot_pref so that:

     - a persist hook fires on real transitions (Settings and P25 both
       route through app_set_usb_autoreboot, so they can no longer drift),
     - init takes the value that was loaded from NVS at boot without
       firing that hook, so a first boot after factory-erase does not
       commit its own default,
     - a reboot is simulated here by calling init again with whatever the
       spy recorded, and the value has to come back the same. */

#include "ls_test.h"
#include "usb_autoreboot_pref.h"

#include <stdbool.h>

/* Persist spy.  Records the last-seen value and how many times the hook
   was fired, so tests can prove init did not persist and a redundant set
   did not either. */
static bool s_persisted_value;
static int  s_persist_calls;
static bool s_stored;

static void spy_persist(bool en)
{
    s_persisted_value = en;
    s_persist_calls++;
    s_stored = en;
}

static void reset_spy(void)
{
    s_persisted_value = false;
    s_persist_calls   = 0;
}

LS_CASE(init_takes_initial_value_without_persisting)
{
    /* Boot path: settings_get_usb_autoreboot() has just returned whatever
       was in NVS.  Passing it through init must not turn around and write
       it back - that would burn an NVS commit on every boot and, worse,
       would erase the "have I ever been set?" distinction the reboot
       recovery could grow to care about. */
    reset_spy();
    s_stored = false;
    usb_autoreboot_pref_init(spy_persist, false);
    LS_CHECK(!usb_autoreboot_pref_get());
    LS_EQ_INT(s_persist_calls, 0);

    reset_spy();
    s_stored = true;
    usb_autoreboot_pref_init(spy_persist, true);
    LS_CHECK(usb_autoreboot_pref_get());
    LS_EQ_INT(s_persist_calls, 0);
}

LS_CASE(set_updates_value_and_fires_persist)
{
    /* The whole point of the fix: a toggle from Settings or P25 CONFIG
       must reach the persistence layer.  Before the fix this call only
       touched a static bool and returned. */
    reset_spy();
    s_stored = false;
    usb_autoreboot_pref_init(spy_persist, false);

    usb_autoreboot_pref_set(true);
    LS_CHECK(usb_autoreboot_pref_get());
    LS_EQ_INT(s_persist_calls, 1);
    LS_CHECK(s_persisted_value);
    LS_CHECK(s_stored);

    usb_autoreboot_pref_set(false);
    LS_CHECK(!usb_autoreboot_pref_get());
    LS_EQ_INT(s_persist_calls, 2);
    LS_CHECK(!s_persisted_value);
    LS_CHECK(!s_stored);
}

LS_CASE(set_to_same_value_does_not_re_persist)
{
    /* AppSettings::timerCb and AppP25::updateSettings run once a second
       and re-read the value to relabel the row; they do not call set().
       But other callers can, and an NVS write per tick would be a real
       cost.  Pin the no-op behaviour so nobody drops the guard later. */
    reset_spy();
    s_stored = false;
    usb_autoreboot_pref_init(spy_persist, false);

    usb_autoreboot_pref_set(false);
    LS_EQ_INT(s_persist_calls, 0);

    usb_autoreboot_pref_set(true);
    LS_EQ_INT(s_persist_calls, 1);

    usb_autoreboot_pref_set(true);
    LS_EQ_INT(s_persist_calls, 1);
}

LS_CASE(reboot_restores_the_stored_value)
{
    /* The actual defect the queue task describes: enable the toggle, cut
       power, come back up, find it still ON.  The spy stands in for the
       NVS blob; reinit is the "power on and hand the loaded value back"
       part of the boot sequence. */
    reset_spy();
    s_stored = false;
    usb_autoreboot_pref_init(spy_persist, false);

    usb_autoreboot_pref_set(true);
    LS_CHECK(s_stored);

    usb_autoreboot_pref_init(spy_persist, s_stored);
    LS_CHECK(usb_autoreboot_pref_get());

    usb_autoreboot_pref_set(false);
    LS_CHECK(!s_stored);

    usb_autoreboot_pref_init(spy_persist, s_stored);
    LS_CHECK(!usb_autoreboot_pref_get());
}

LS_CASE(init_without_persist_hook_is_still_safe)
{
    /* Defensive: if the backend ever forgets to wire the persist callback
       (or a future test doubles as a caller), a set() must not crash and
       the read-back must still reflect the write.  Volatile-only mode,
       basically - the presentation the alternative done-when clause
       described. */
    usb_autoreboot_pref_init(NULL, false);
    usb_autoreboot_pref_set(true);
    LS_CHECK(usb_autoreboot_pref_get());
    usb_autoreboot_pref_set(false);
    LS_CHECK(!usb_autoreboot_pref_get());
}
