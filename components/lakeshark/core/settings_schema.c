/* - see settings_schema.h for what this exists to catch. */

#include "settings_schema.h"

settings_schema_action_t settings_schema_decide(
    settings_schema_read_status_t status,
    uint32_t stored,
    uint32_t current,
    uint32_t min_additive)
{
    /* An unreadable version key means either flash corruption or a byte where
       a number should be.  Nothing downstream can be trusted, so wipe rather
       than guess - a silent bad value is exactly what the schema was added
       to prevent. */
    if (status == SETTINGS_SCHEMA_READ_UNREADABLE)
        return SETTINGS_SCHEMA_ACTION_RESET;

    /* No key at all is the expected shape on a fresh flash and on any device
       that shipped before this fix landed.  Keep the existing keys - they
       were written by the same layout the current build reads - and stamp
       the current version so the next boot takes the KEEP path. */
    if (status == SETTINGS_SCHEMA_READ_MISSING)
        return SETTINGS_SCHEMA_ACTION_ADOPT;

    if (stored == current)
        return SETTINGS_SCHEMA_ACTION_KEEP;

    /* stored < current: was written by an older build.  If it is inside the
       additive-only window this build knows about, keep the keys and stamp
       the new version.  Older than that means the layout crossed a breaking
       cutover we no longer know how to migrate - wipe. */
    if (stored < current) {
        if (stored >= min_additive)
            return SETTINGS_SCHEMA_ACTION_MIGRATE;
        return SETTINGS_SCHEMA_ACTION_RESET;
    }

    /* stored > current: written by a newer build than we are.  A downgrade
       has no defined behaviour, and quietly reading whatever keys the newer
       build wrote is the exact silent-nonsense trap.  Wipe. */
    return SETTINGS_SCHEMA_ACTION_RESET;
}

const char *settings_schema_action_name(settings_schema_action_t a)
{
    switch (a) {
    case SETTINGS_SCHEMA_ACTION_KEEP:    return "keep";
    case SETTINGS_SCHEMA_ACTION_ADOPT:   return "adopt";
    case SETTINGS_SCHEMA_ACTION_MIGRATE: return "migrate";
    case SETTINGS_SCHEMA_ACTION_RESET:   return "reset";
    }
    return "?";
}

const char *settings_schema_read_status_name(settings_schema_read_status_t s)
{
    switch (s) {
    case SETTINGS_SCHEMA_READ_OK:         return "ok";
    case SETTINGS_SCHEMA_READ_MISSING:    return "missing";
    case SETTINGS_SCHEMA_READ_UNREADABLE: return "unreadable";
    }
    return "?";
}
