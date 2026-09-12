#ifndef SETTINGS_SCHEMA_H
#define SETTINGS_SCHEMA_H

/* - Every value in the `sdr-tool` NVS namespace is written key-per- field, so a new key added by the firmware does not physically overwrite an older key. */

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Bump SETTINGS_SCHEMA_VERSION whenever a key in `sdr-tool` changes type or
   semantics.  A pure additive change (new key, existing keys untouched) still
   bumps it - the point is that the version stamp says "this build wrote
   this".  Bump SETTINGS_SCHEMA_MIN_ADDITIVE only when the oldest version this
   build can still read from without wiping crosses a breaking cutover. */
#define SETTINGS_SCHEMA_VERSION       4U
#define SETTINGS_SCHEMA_MIN_ADDITIVE  1U

typedef enum {
    SETTINGS_SCHEMA_READ_OK,          /* version key was read successfully */
    SETTINGS_SCHEMA_READ_MISSING,     /* no version key (fresh flash or legacy device) */
    SETTINGS_SCHEMA_READ_UNREADABLE,  /* read failed for any other reason */
} settings_schema_read_status_t;

typedef enum {
    SETTINGS_SCHEMA_ACTION_KEEP,      /* stored version matches current; nothing to do */
    SETTINGS_SCHEMA_ACTION_ADOPT,     /* no version key present; keep existing data, stamp current */
    SETTINGS_SCHEMA_ACTION_MIGRATE,   /* older but still readable; keep data, stamp current */
    SETTINGS_SCHEMA_ACTION_RESET,     /* unreadable, too old, or newer/unknown; wipe namespace */
} settings_schema_action_t;

settings_schema_action_t settings_schema_decide(
    settings_schema_read_status_t status,
    uint32_t stored,
    uint32_t current,
    uint32_t min_additive);

const char *settings_schema_action_name(settings_schema_action_t a);
const char *settings_schema_read_status_name(settings_schema_read_status_t s);

#ifdef __cplusplus
}
#endif

#endif
