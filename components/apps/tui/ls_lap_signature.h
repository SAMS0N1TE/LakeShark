/* Portable signature-trailer validation; verification callback must hash exactly elf_size bytes. */
#ifndef LS_LAP_SIGNATURE_H
#define LS_LAP_SIGNATURE_H
#include "ls_lap_manifest.h"
typedef bool (*ls_lap_verify_fn)(void *ctx, const ls_lap_reader_t *r, size_t elf_size,
                                 const uint8_t *der, size_t der_size);
/* Metadata-only scan: no payload hashing and no claim of cryptographic trust. */
bool ls_lap_signature_layout(const ls_lap_reader_t *r, size_t *elf_size, bool *present, char *why,
                             size_t cap);
bool ls_lap_signature(const ls_lap_reader_t *r, ls_lap_verify_fn verify, void *ctx,
                      size_t *elf_size, bool *is_signed, char *why, size_t cap);
#ifdef ESP_PLATFORM
bool ls_lap_verify_release(void *ctx, const ls_lap_reader_t *r, size_t elf_size, const uint8_t *der,
                           size_t der_size);
#endif
#endif
