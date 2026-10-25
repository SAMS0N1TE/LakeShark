/* Bounded, allocation-free ELF32/LAP metadata and loader preflight. No ESP dependencies. */
#ifndef LS_LAP_MANIFEST_H
#define LS_LAP_MANIFEST_H
#include "../../../app-sdk/include/lakeshark_app.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#define LS_LAP_MAX_BYTES (512u * 1024u)
#define LS_LAP_MANIFEST_BYTES 198u
/* The whole lap_app_t on the RV32 target: manifest, five callbacks, radio. */
#define LS_LAP_DESCRIPTOR_BYTES 228u
#define LS_LAP_MAX_MEMORY (4u * 1024u * 1024u)
/* Return exactly n bytes or false. The parser never requests outside [0,size). */
typedef bool (*ls_lap_read_fn)(void *ctx, size_t offset, void *out, size_t n);
typedef struct {
    ls_lap_read_fn read;
    void *ctx;
    size_t size;
} ls_lap_reader_t;
/* out is unchanged on failure; callbacks are always zero, including on a 64-bit host. */
bool ls_lap_manifest_read(const ls_lap_reader_t *r, lap_app_t *out, char *why, size_t cap);
bool ls_lap_manifest(const void *bytes, size_t size, lap_app_t *out, char *why, size_t cap);
/* Full loader preflight: symbol/string indices, mapped entry and relocation writes.
 * Metadata scans need only manifest_read; call preflight before esp_elf_relocate. */
bool ls_lap_preflight(const void *bytes, size_t size, char *why, size_t cap);
typedef uintptr_t (*ls_lap_resolve_fn)(const char *name);
bool ls_lap_preflight_resolved(const void *bytes, size_t size, ls_lap_resolve_fn resolve, char *why,
                               size_t cap);
/* Shared SD name policy: ASCII, no leading dot or '..', 1..max characters (max<=48). */
bool ls_lap_plain_name(const char *name, size_t max);
bool ls_lap_name_equal(const char *a, const char *b); /* FAT ASCII case folding */
bool ls_lap_fields_valid(const lap_app_t *app, char *why, size_t cap);
/* The optional .lap_icon section (LAP_ICON_PX) as colours 0-15, 0xFF clear.
   False when the app has none or it is malformed; out is then all clear.
   Call after ls_lap_manifest_read accepted the same reader. */
bool ls_lap_icon_read(const ls_lap_reader_t *r, uint8_t out[LAP_PX_H][LAP_PX_W]);
#endif
