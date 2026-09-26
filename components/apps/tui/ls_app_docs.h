/* What each app is for, what it keeps, and what it does with a position.
 *
 * Separated from the APPS[] table in compact_ui.cpp for two reasons. The
 * table is a layout - id, tile colour, icon, screen - and this is prose; and
 * the bench does not compile compact_ui.cpp, so a contract declared only
 * there could not be tested. ls_app_docs_all[] exists so test_app_contract
 * can walk every one of these without a UI.
 *
 * See ls_app_doc_t in ls_app.h for what the three answers mean. An app that
 * keeps nothing says so; that is an answer, not a blank. */

#ifndef LS_APP_DOCS_H
#define LS_APP_DOCS_H

#include "ls_app.h"

#ifdef __cplusplus
extern "C" {
#endif

extern const ls_app_doc_t ls_doc_home;
extern const ls_app_doc_t ls_doc_p25;
extern const ls_app_doc_t ls_doc_fm;
extern const ls_app_doc_t ls_doc_adsb;
extern const ls_app_doc_t ls_doc_falls;
extern const ls_app_doc_t ls_doc_cell;
extern const ls_app_doc_t ls_doc_mesh;
extern const ls_app_doc_t ls_doc_labs;
extern const ls_app_doc_t ls_doc_journal;
extern const ls_app_doc_t ls_doc_notes;
extern const ls_app_doc_t ls_doc_compass;
extern const ls_app_doc_t ls_doc_rec;
extern const ls_app_doc_t ls_doc_subghz;
extern const ls_app_doc_t ls_doc_files;
extern const ls_app_doc_t ls_doc_mixrf;
extern const ls_app_doc_t ls_doc_diag;
extern const ls_app_doc_t ls_doc_settings;
extern const ls_app_doc_t ls_doc_map;
extern const ls_app_doc_t ls_doc_gps;
extern const ls_app_doc_t ls_doc_radios;
extern const ls_app_doc_t ls_doc_link;

typedef struct {
    const char         *id;
    const ls_app_doc_t *doc;
} ls_app_doc_row_t;

extern const ls_app_doc_row_t ls_app_docs_all[];
extern const int              ls_app_docs_count;

#ifdef __cplusplus
}
#endif

#endif /* LS_APP_DOCS_H */
