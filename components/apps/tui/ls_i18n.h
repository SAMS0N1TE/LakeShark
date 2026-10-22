/* The interface language: English, or Ukrainian.

   A string is looked up by its English text, so English is the code as it
   is and costs one branch: ls_tr() hands back the very pointer it was given.
   A Ukrainian translation comes back in the TUI's cell encoding, one byte a
   cell (tui_utf8_to_cells), so code that measures and cuts text by bytes
   keeps working. Text with no entry stays English.

   The table is built in (ls_i18n_uk.h): the interface has to read before any
   SD card is mounted. The language itself is a setting (Settings > Device),
   stored by whoever installs a saver; this module touches no storage. */

#ifndef LS_I18N_H
#define LS_I18N_H

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    LS_LANG_EN = 0,
    LS_LANG_UK = 1,
    LS_LANG_COUNT
} ls_lang_t;

ls_lang_t ls_i18n_lang(void);
bool ls_i18n_lang_valid(int lang);

/* Change the language and hand it to the saver, if one is installed. */
void ls_i18n_set_lang(ls_lang_t lang);
/* The stored language at boot: applied, not saved again. Anything not a
   language is English. */
void ls_i18n_load(int stored);
void ls_i18n_set_saver(void (*save)(int lang));

/* The next language, for a setting that cycles. */
ls_lang_t ls_i18n_next(ls_lang_t lang);
/* A language's own name in its own script, in cell bytes. */
const char *ls_i18n_lang_name(ls_lang_t lang);

/* `en` in the interface language. Never NULL for a non-NULL argument. */
const char *ls_tr(const char *en);

/* The same English word can need two Ukrainian ones (HOME the screen,
   HOME the place the receiver is). Such text is keyed "context|English":
   English is `en` itself, Ukrainian is the entry for `key`, else `en`. */
const char *ls_tr_ctx(const char *key, const char *en);

/* For tests: the table, and how many entries could not be stored. */
int ls_i18n_entries(void);
const char *ls_i18n_entry_en(int i);
const char *ls_i18n_entry_uk(int i);   /* UTF-8, as written in the table */
int ls_i18n_dropped(void);
/* For the simulator: told of every string ls_tr() had no entry for. */
void ls_i18n_set_miss_hook(void (*miss)(const char *en));

#ifdef __cplusplus
}
#endif

#endif /* LS_I18N_H */
