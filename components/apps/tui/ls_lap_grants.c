/* See ls_lap_grants.h. One line an install: "id<TAB>version<TAB>size<TAB>caps". */
#include "ls_lap_grants.h"
#include "ls_lap_storage.h"
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef ESP_PLATFORM
#include "esp_attr.h"
#endif
#ifndef EXT_RAM_BSS_ATTR
#define EXT_RAM_BSS_ATTR
#endif

#define GRANTS 32
typedef struct {
    char id[32], version[16];
    uint32_t size;
    uint8_t caps;
} grant_t;
EXT_RAM_BSS_ATTR static grant_t table[GRANTS];
static atomic_uint changes, saved;      /* the worker writes when they differ */
static atomic_int count;
static atomic_bool loaded;

static bool same(const grant_t *g, const ls_lap_info_t *a) {
    return !strcmp(g->id, a->manifest.id) && !strcmp(g->version, a->manifest.version) &&
           g->size == (uint32_t)a->size;
}
bool ls_lap_grant_has(const ls_lap_info_t *a, uint8_t caps) {
    caps &= LS_LAP_GRANT_CAPS;
    if (!a || !caps) return true;
    const int n = atomic_load(&count);
    for (int i = 0; i < n; i++)
        if (same(&table[i], a)) return (table[i].caps & caps) == caps;
    return false;
}
void ls_lap_grant_set(const ls_lap_info_t *a, uint8_t caps) {
    caps &= LS_LAP_GRANT_CAPS;
    if (!a || !caps) return;
    int n = atomic_load(&count), slot = -1;
    for (int i = 0; i < n && slot < 0; i++)
        if (!strcmp(table[i].id, a->manifest.id)) slot = i;   /* one install per id: an update replaces it */
    if (slot < 0) {
        if (n >= GRANTS) {                       /* full: forget the oldest */
            memmove(table, table + 1, sizeof(table[0]) * (GRANTS - 1));
            n = GRANTS - 1;
        }
        slot = n;
    }
    grant_t g = {0};
    snprintf(g.id, sizeof(g.id), "%s", a->manifest.id);
    snprintf(g.version, sizeof(g.version), "%s", a->manifest.version);
    g.size = (uint32_t)a->size;
    g.caps = (uint8_t)((slot < n && same(&table[slot], a) ? table[slot].caps : 0) | caps);
    table[slot] = g;
    if (slot == n) atomic_store(&count, n + 1);
    atomic_fetch_add(&changes, 1u);
}
void ls_lap_grants_load(const char *path) {
    if (atomic_exchange(&loaded, true) || !path) return;
    FILE *f = fopen(path, "r");
    if (!f) return;
    char line[128];
    int n = 0;
    while (n < GRANTS && fgets(line, sizeof(line), f)) {
        char *id = line, *ver = strchr(id, '\t'), *size = ver ? strchr(ver + 1, '\t') : NULL,
             *caps = size ? strchr(size + 1, '\t') : NULL;
        if (!caps) continue;
        *ver++ = 0; *size++ = 0; *caps++ = 0;
        if (!*id || strlen(id) >= sizeof(table[0].id) || strlen(ver) >= sizeof(table[0].version)) continue;
        grant_t *g = &table[n];
        snprintf(g->id, sizeof(g->id), "%.31s", id);
        snprintf(g->version, sizeof(g->version), "%.15s", ver);
        g->size = (uint32_t)strtoul(size, NULL, 10);
        g->caps = (uint8_t)(strtoul(caps, NULL, 10) & LS_LAP_GRANT_CAPS);
        if (g->caps) n++;
    }
    fclose(f);
    atomic_store(&count, n);
}
bool ls_lap_grants_save(const char *path) {
    const unsigned want = atomic_load(&changes);
    if (want == atomic_load(&saved) || !path) return true;
    char tmp[200];
    snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    FILE *f = fopen(tmp, "w");
    if (!f) return false;
    bool ok = true;
    const int n = atomic_load(&count);
    for (int i = 0; i < n && ok; i++) {
        /* Versions are free text: a tab or newline in one would break the line. */
        if (strpbrk(table[i].version, "\t\r\n")) continue;
        ok = fprintf(f, "%s\t%s\t%u\t%u\n", table[i].id, table[i].version, (unsigned)table[i].size,
                     (unsigned)table[i].caps) > 0;
    }
    ok = (fclose(f) == 0) && ok && ls_lap_replace(tmp, path);
    if (!ok) remove(tmp);
    else atomic_store(&saved, want);
    return ok;
}
