/* Firmware modules: built-in apps that live in the internal app storage
   (/spiffs/apps/<id>.lsm) instead of the app slot.

   A module is the app's own source files, linked as a shared ELF against the
   firmware's internal symbols. That is only valid for the firmware it was
   built with, so the firmware carries the SHA-256 of each of its modules and
   loads nothing else. The app's screen symbol (ls_scr_<x>) stays in the
   firmware as a proxy: HOME, the app table and the router see no difference.
   The first time it is shown the module is read into PSRAM, linked and kept
   for the rest of the run, exactly as long-lived as the built-in it was.

   After a firmware update the stored modules no longer match; they are
   fetched again by hash (ls_mod_poll) or taken from /sdcard/apps.

   components/apps/modules/modules.json lists the modules;
   build_modules.py builds them and generates the tables. */
#ifndef LS_MOD_H
#define LS_MOD_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define LS_MOD_MAGIC 0x314d534cu /* "LSM1" */

typedef struct {
    const char *name;
    void *addr;
} ls_mod_sym_t;

/* What a module's entry point returns. */
typedef struct {
    uint32_t magic;
    const void *const *screens; /* const ls_tui_screen_t *[] */
    uint32_t nscreens;
    void (*init)(void);         /* once, right after linking; may be NULL */
    const ls_mod_sym_t *syms;   /* what the firmware may call by name */
    uint32_t nsyms;
} ls_mod_desc_t;

/* A function (or object) of module `id` by name. With `load` the module is
   brought in first if it is on the board (blocks for the read: console
   verbs); without, NULL until the user has opened the app. The address stays
   valid for the rest of the run. */
void *ls_mod_sym(const char *id, const char *name, bool load);

/* Whether module `id` is loaded. */
bool ls_mod_loaded(const char *id);

/* Once a UI tick: fetches modules that are missing or stale (after a
   firmware update) when the network is up. Cheap when there is nothing to do. */
void ls_mod_poll(void);

/* `mod` console verb: list, load <id>, fetch. */
int ls_mod_command(int argc, char **argv);

#ifdef __cplusplus
}
#endif
#endif /* LS_MOD_H */
