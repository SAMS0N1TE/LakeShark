#ifndef LS_VERSION_H
#define LS_VERSION_H

/* One place that says which build is running. */

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Enough for "LakeShark v<32>  board=<32>  built <16> <16>  IDF <32>" plus
   the DIRTY marker.  Callers reach for a stack buffer; keep it small. */
#define LS_VERSION_LINE_MAX 192

typedef struct {
    const char *version;    /* esp_app_desc.version, e.g. "v0.1-3-g1a2b3c-dirty" */
    const char *board;      /* LS_BOARD_NAME */
    const char *idf;        /* esp_get_idf_version() */
    const char *date;       /* esp_app_desc.date */
    const char *time;       /* esp_app_desc.time */
} ls_version_info_t;

/* True when `version` was produced by `git describe --dirty` on a working
   tree with uncommitted changes.  The suffix `git describe` writes is
   `-dirty`, but be forgiving about case and about future variants that
   might spell it differently. */
bool ls_version_is_dirty(const char *version);

/* Assemble one line.  Always NUL-terminates when len > 0.  Returns the
   length written (not counting the terminator), truncated to len-1.  The
   layout is:

       LakeShark <version>  [DIRTY]  board=<board>  built <date> <time>  IDF <idf>

   [DIRTY] is inserted only when ls_version_is_dirty(version) is true, so a
   build from uncommitted changes cannot pass for a tagged release. */
int ls_version_format(char *out, size_t len, const ls_version_info_t *v);

/* Device-only.  Fills v from esp_app_get_description() + esp_get_idf_version()
   + LS_BOARD_NAME.  The strings live in .rodata so the pointers are stable
   for the lifetime of the process. */
void ls_version_get(ls_version_info_t *v);

/* Device-only shortcut: ls_version_get() + ls_version_format() in one call. */
int ls_version_line(char *out, size_t len);

#ifdef __cplusplus
}
#endif
#endif
