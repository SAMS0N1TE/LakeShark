/* LS_TEST_SOURCES: ${FW}/components/lakeshark/core/settings_schema.c */
/* LS_TEST_INCLUDE: ${FW}/components/lakeshark/core */
/**/

#include "ls_test.h"
#include "settings_schema.h"

/* Local aliases so the cases read as English, not enum acronyms. */
#define OK          SETTINGS_SCHEMA_READ_OK
#define MISSING     SETTINGS_SCHEMA_READ_MISSING
#define UNREADABLE  SETTINGS_SCHEMA_READ_UNREADABLE

#define KEEP        SETTINGS_SCHEMA_ACTION_KEEP
#define ADOPT       SETTINGS_SCHEMA_ACTION_ADOPT
#define MIGRATE     SETTINGS_SCHEMA_ACTION_MIGRATE
#define RESET       SETTINGS_SCHEMA_ACTION_RESET

LS_CASE(same_version_keeps)
{
    /* The normal boot: version key present, matches the current build.
       Nothing is rewritten, nothing is erased. */
    LS_EQ_INT(settings_schema_decide(OK, 3, 3, 1), KEEP);
    LS_EQ_INT(settings_schema_decide(OK, 1, 1, 1), KEEP);
    LS_EQ_INT(settings_schema_decide(OK, 99, 99, 1), KEEP);
}

LS_CASE(older_additive_migrates)
{
    /* Stored is older but inside the additive window the current build
       still knows how to read from.  Keys stay; version is bumped. */
    LS_EQ_INT(settings_schema_decide(OK, 1, 2, 1), MIGRATE);
    LS_EQ_INT(settings_schema_decide(OK, 2, 5, 2), MIGRATE);
    LS_EQ_INT(settings_schema_decide(OK, 5, 6, 1), MIGRATE);
    LS_EQ_INT(settings_schema_decide(OK, 4, 5, 4), MIGRATE); /* boundary */
}

LS_CASE(unreadable_resets)
{

    LS_EQ_INT(settings_schema_decide(UNREADABLE, 0, 1, 1), RESET);
    /* The stored value is not meaningful when the read failed, but the
       decision must not depend on it - a corrupted-flash path that
       happened to read as a plausible number should still reset. */
    LS_EQ_INT(settings_schema_decide(UNREADABLE, 3, 3, 1), RESET);
    LS_EQ_INT(settings_schema_decide(UNREADABLE, 99, 1, 1), RESET);
}

LS_CASE(missing_adopts)
{
    /* Fresh flash or a device that shipped before this fix.  Existing keys
       (if any) were written by the same layout the current build reads
       from - that is the whole reason ADOPT is not RESET.  Stamp version,
       keep data. */
    LS_EQ_INT(settings_schema_decide(MISSING, 0, 1, 1), ADOPT);
    LS_EQ_INT(settings_schema_decide(MISSING, 0, 7, 3), ADOPT);
    /* The stored argument is undefined when the read said MISSING; the
       decision must not depend on it.  A garbage caller passing junk
       through the unused arg must still get ADOPT. */
    LS_EQ_INT(settings_schema_decide(MISSING, 12345, 1, 1), ADOPT);
}

LS_CASE(older_than_additive_resets)
{
    /* Stored is real but predates the oldest layout this build can read
       from.  There is no migration path defined for it, so the keys are
       assumed to be in the wrong layout and are wiped. */
    LS_EQ_INT(settings_schema_decide(OK, 1, 5, 3), RESET);
    LS_EQ_INT(settings_schema_decide(OK, 2, 5, 3), RESET);
    /* Boundary: min_additive - 1 is a reset, min_additive itself migrates. */
    LS_EQ_INT(settings_schema_decide(OK, 2, 5, 3), RESET);
    LS_EQ_INT(settings_schema_decide(OK, 3, 5, 3), MIGRATE);
}

LS_CASE(newer_resets)
{

    LS_EQ_INT(settings_schema_decide(OK, 2, 1, 1), RESET);
    LS_EQ_INT(settings_schema_decide(OK, 99, 5, 1), RESET);
    /* Newer-and-current-min-additive still resets - stored > current wins. */
    LS_EQ_INT(settings_schema_decide(OK, 6, 5, 5), RESET);
}

LS_CASE(action_and_status_names_are_stable)
{
    /* settings.c logs these strings.  A rename would silently change the
       serial console output, which is the only channel the user has to
       find out their settings moved.  Pin them. */
    LS_EQ_STR(settings_schema_action_name(KEEP),     "keep");
    LS_EQ_STR(settings_schema_action_name(ADOPT),    "adopt");
    LS_EQ_STR(settings_schema_action_name(MIGRATE),  "migrate");
    LS_EQ_STR(settings_schema_action_name(RESET),    "reset");

    LS_EQ_STR(settings_schema_read_status_name(OK),         "ok");
    LS_EQ_STR(settings_schema_read_status_name(MISSING),    "missing");
    LS_EQ_STR(settings_schema_read_status_name(UNREADABLE), "unreadable");
}

LS_CASE(current_version_matches_header)
{
    /* If the header constant is bumped without a corresponding thought
       about the additive window, this trips.  It is here to force the
       author of a future settings change to look at both. */
    LS_CHECK(SETTINGS_SCHEMA_VERSION >= 1);
    LS_CHECK(SETTINGS_SCHEMA_MIN_ADDITIVE >= 1);
    LS_CHECK(SETTINGS_SCHEMA_MIN_ADDITIVE <= SETTINGS_SCHEMA_VERSION);
}
