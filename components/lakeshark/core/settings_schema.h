#ifndef SETTINGS_SCHEMA_H
#define SETTINGS_SCHEMA_H

/* LS-800 - Every value in the `sdr-tool` NVS namespace is written key-per-
   field, so a new key added by the firmware does not physically overwrite an
   older key.  What can still bite is a key whose type or units changed between
   releases: `nvs_get_u8` on a `u32`-typed key returns ESP_ERR_NVS_TYPE_MISMATCH
   and the getter quietly returns its default, and a range/encoding change (a
   volume that used to be 0..255 and is now 0..100, a gain in tenths that is
   now dB) reads back a real number that means the wrong thing.  Six queued
   tasks are about to add settings, so the flash needs an anchor point that
   says which layout wrote it.

   The module is a pure decision function so the bench can drive every branch
   without NVS.  settings.c wraps it with a real nvs_get_u32 for the version
   key and a namespace-wide erase for the RESET path. */

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Bump SETTINGS_SCHEMA_VERSION whenever a key in `sdr-tool` changes type or
   semantics.  A pure additive change (new key, existing keys untouched) still
   bumps it - the point is that the version stamp says "this build wrote
   this".  Bump SETTINGS_SCHEMA_MIN_ADDITIVE only when the oldest version this
   build can still read from without wiping crosses a breaking cutover. */
#define SETTINGS_SCHEMA_VERSION       3U
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
