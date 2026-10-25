#include "ls_lap_signature.h"
#include <stdio.h>
#include <string.h>
bool ls_lap_signature_layout(const ls_lap_reader_t *r, size_t *elf_size, bool *present, char *why,
                             size_t cap) {
    uint8_t tail[6];
    const char *err = NULL;
    if (!r || !r->read || r->size < 52 || r->size > LS_LAP_MAX_BYTES || !elf_size || !present)
        err = "invalid app size";
    else if (!r->read(r->ctx, r->size - 6, tail, 6))
        err = "cannot read signature trailer";
    if (err)
        goto bad;
    if (memcmp(tail + 2, "LSG1", 4)) {
        *elf_size = r->size;
        *present = false;
        return true;
    }
    size_t n = (size_t)tail[0] + ((size_t)tail[1] << 8);
    if (n < 8 || n > 72 || n + 6 > r->size - 52) {
        err = "invalid signature trailer";
        goto bad;
    }
    *elf_size = r->size - 6 - n;
    *present = true;
    return true;
bad:
    if (why && cap)
        snprintf(why, cap, "%s", err);
    return false;
}
bool ls_lap_signature(const ls_lap_reader_t *r, ls_lap_verify_fn verify, void *ctx,
                      size_t *elf_size, bool *is_signed, char *why, size_t cap) {
    size_t size;
    bool present;
    if (!ls_lap_signature_layout(r, &size, &present, why, cap))
        return false;
    if (present) {
        uint8_t der[72];
        size_t n = r->size - size - 6;
        if (!r->read(r->ctx, size, der, n) || !verify || !verify(ctx, r, size, der, n)) {
            if (why && cap)
                snprintf(why, cap, "bad app signature");
            return false;
        }
    }
    *elf_size = size;
    *is_signed = present;
    return true;
}

#ifdef ESP_PLATFORM
#include "core/ls_signature.h"
#include "esp_heap_caps.h"
#include "ls_lap_keys.h"
#include "mbedtls/sha256.h"
bool ls_lap_verify_release(void *ctx, const ls_lap_reader_t *r, size_t size, const uint8_t *der,
                           size_t n) {
    (void)ctx;
    uint8_t *buf = heap_caps_malloc(1024, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!buf)
        return false;
    mbedtls_sha256_context sha;
    uint8_t hash[32];
    mbedtls_sha256_init(&sha);
    bool ok = mbedtls_sha256_starts(&sha, 0) == 0;
    for (size_t off = 0; ok && off < size;) {
        size_t chunk = size - off;
        if (chunk > 1024)
            chunk = 1024;
        ok = r->read(r->ctx, off, buf, chunk) && mbedtls_sha256_update(&sha, buf, chunk) == 0;
        off += chunk;
    }
    ok = ok && mbedtls_sha256_finish(&sha, hash) == 0;
    mbedtls_sha256_free(&sha);
    heap_caps_free(buf);
    if (!ok)
        return false;
    for (size_t i = 0; i < LS_LAP_PUBLIC_KEY_COUNT; i++)
        if (ls_signature_verify(LS_LAP_PUBLIC_KEYS[i], hash, der, n))
            return true;
    return false;
}
#endif
