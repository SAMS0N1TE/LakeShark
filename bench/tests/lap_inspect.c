/* Tiny host cross-check tool used by the Python SDK tests. */
#include "core/ls_signature.h"
#include "ls_lap_manifest.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define EXPORT(name, fn) name
static const char *const exports[] = {
#include "ls_lap_exports.inc"
};
#undef EXPORT
static uintptr_t allowed(const char *name) {
    for (size_t i = 0; i < sizeof(exports) / sizeof(exports[0]); i++)
        if (!strcmp(name, exports[i]))
            return 1;
    return 0;
}
int main(int argc, char **argv) {
    if (argc != 2)
        return 2;
    FILE *f = fopen(argv[1], "rb");
    if (!f)
        return 2;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    rewind(f);
    if (n < 0 || (uint64_t)n > LS_LAP_MAX_BYTES) {
        fclose(f);
        return 1;
    }
    void *b = malloc((size_t)n);
    if (!b) {
        fclose(f);
        return 2;
    }
    if (fread(b, 1, (size_t)n, f) != (size_t)n) {
        free(b);
        fclose(f);
        return 2;
    }
    fclose(f);
    lap_app_t m;
    char why[128];
    bool ok = ls_lap_manifest(b, (size_t)n, &m, why, sizeof(why)) &&
              ls_lap_preflight_resolved(b, (size_t)n, allowed, why, sizeof(why));
    if (ok)
        printf("%s\n%s\n%s\n", m.id, m.name, m.version);
    else
        puts(why);
    free(b);
    return ok ? 0 : 1;
}
