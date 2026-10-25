/* Runs the actual firmware P-256 verifier against a test-generated public key. */
#include "core/ls_signature.h"
#include "ls_lap_signature.h"
#include "mbedtls/sha256.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static bool memory_read(void *ctx, size_t off, void *out, size_t n) {
    memcpy(out, (unsigned char *)ctx + off, n);
    return true;
}
static bool verify(void *ctx, const ls_lap_reader_t *r, size_t n, const uint8_t *sig, size_t sn) {
    uint8_t hash[32];
    if (mbedtls_sha256(r->ctx, n, hash, 0))
        return false;
    return ls_signature_verify(ctx, hash, sig, sn);
}
static void *read_file(const char *name, size_t *n) {
    FILE *f = fopen(name, "rb");
    if (!f)
        return NULL;
    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    rewind(f);
    if (len < 0 || (uint64_t)len > LS_LAP_MAX_BYTES) {
        fclose(f);
        return NULL;
    }
    char *p = calloc((size_t)len + 1, 1);
    if (!p) {
        fclose(f);
        return NULL;
    }
    *n = fread(p, 1, (size_t)len, f);
    fclose(f);
    if (*n != (size_t)len) {
        free(p);
        return NULL;
    }
    return p;
}
int main(int argc, char **argv) {
    if (argc != 3)
        return 2;
    size_t size, pem_size;
    void *bytes = read_file(argv[1], &size), *pem = read_file(argv[2], &pem_size);
    if (!bytes || !pem) {
        free(bytes);
        free(pem);
        return 2;
    }
    ls_lap_reader_t r = {memory_read, bytes, size};
    size_t elf_size;
    bool signed_app;
    char why[128];
    bool ok = ls_lap_signature(&r, verify, pem, &elf_size, &signed_app, why, sizeof(why));
    if (ok) {
        lap_app_t m;
        ok = ls_lap_manifest(bytes, elf_size, &m, why, sizeof(why));
    }
    puts(ok ? (signed_app ? "signed" : "unsigned") : why);
    free(bytes);
    free(pem);
    return ok ? 0 : 1;
}
