/* Bounded state packs and signed catalog policy; no device I/O. */
#pragma once
#include "p25_profile.h"
#include "ls_ota_core.h"
#ifdef __cplusplus
extern "C" {
#endif
#define P25_PACK_RECORD_MAX 256u
#define P25_PACK_STATE_MAX 51u
#define P25_PACK_MAX_BYTES (1024u * 1024u)
#define P25_PACK_CATALOG_URL "https://ota.terminalbay.com/p25/catalog.txt"
typedef struct {
    p25_profile_t records[P25_PACK_RECORD_MAX];
    unsigned count, packs, sites;
} p25_pack_db_t;
typedef struct {
    char state[3], name[48], file[48], version[24];
    uint32_t bytes;
    uint8_t sha256[32];
    bool plus;
} p25_pack_entry_t;
typedef struct {
    p25_pack_entry_t entries[P25_PACK_STATE_MAX];
    unsigned count;
} p25_pack_catalog_t;
typedef bool (*p25_pack_verify_fn)(const char *, const ls_ota_manifest_t *);
bool p25_pack_state_valid(const char *state);
/* Append only complete v2 records, each beginning version=2. On failure the
 * prior database remains usable. Caller supplies PSRAM database and scratch. */
bool p25_pack_parse(p25_pack_db_t *db, p25_profile_parse_scratch_t *scratch,
                    const char *text, size_t len, const p25_profile_parse_config_t *cfg,
                    p25_profile_diagnostic_t *diag);
/* Caller must use staging: failed parse clears count. Verification is required. */
bool p25_pack_catalog_parse(p25_pack_catalog_t *out, const char *text, size_t len,
                            p25_pack_verify_fn verify);
bool p25_pack_allowed(const p25_pack_entry_t *entry, const char *token);
bool p25_pack_digest_ok(const p25_pack_entry_t *entry, size_t bytes, const uint8_t digest[32]);
#ifdef __cplusplus
}
#endif
