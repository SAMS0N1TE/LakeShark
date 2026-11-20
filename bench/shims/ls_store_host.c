/* lssim: the STORE backend's platform calls. Catalogs come from
   bench/fixtures/store (catalog.json, community.json); "installed" apps are
   a small table; downloads and installs succeed at once.
     LSSIM_STORE_OFFLINE=1      no network (the kept-list / no Wi-Fi views)
     LSSIM_STORE_JOB=id:got:total   a download under way, to look at it */
#include "ls_fetch.h"
#include "ls_lap.h"
#include "ls_store.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef LS_STORE_FIXTURES
#define LS_STORE_FIXTURES "bench/fixtures/store"
#endif

static struct { char id[32], ver[16]; } inst[16] = {
    {"dice", "2.0"}, {"pinball", "1.1"}, {"meshtastic", "1.0"}, {"net_diag", "1.1.0"}, {"lan_scan", "1.0.0"},
};

int ls_store_host_installed(const char *id, char *have, int cap)
{
    for (int i = 0; i < 16; i++)
        if (inst[i].id[0] && !strcmp(inst[i].id, id)) { snprintf(have, cap, "%s", inst[i].ver); return 1; }
    return 0;
}

void ls_store_host_opened(void)
{
    const char *j = getenv("LSSIM_STORE_JOB");
    char id[32];
    unsigned long got = 0, total = 0;
    if (j && sscanf(j, "%31[^:]:%lu:%lu", id, &got, &total) == 3) ls_store_sim_job(id, (uint32_t)got, (uint32_t)total);
}

bool ls_fetch_online(void) { return !getenv("LSSIM_STORE_OFFLINE"); }

ls_fetch_result_t ls_fetch_run(const ls_fetch_req_t *q, uint32_t *got, int *http)
{
    if (http) *http = 200;
    if (!q->buf) {
        if (q->progress) q->progress(q->bytes, q->bytes, q->ctx);
        return LS_FETCH_OK;
    }
    char path[512];
    snprintf(path, sizeof(path), "%s/%s", LS_STORE_FIXTURES, strstr(q->url, "apps.terminalbay") ? "community.json" : "catalog.json");
    FILE *f = fopen(path, "rb");
    if (!f) return LS_FETCH_UNREACHABLE;
    const size_t n = fread(q->buf, 1, q->cap - 1, f);
    fclose(f);
    q->buf[n] = 0;
    if (got) *got = (uint32_t)n;
    return LS_FETCH_OK;
}

const char *ls_fetch_result_text(ls_fetch_result_t r) { return r == LS_FETCH_OK ? "ok" : "failed"; }

bool ls_lap_install_file(const char *src, const char *dest, size_t size, const uint8_t sha256[32], bool need_signed,
                         char *why, size_t cap)
{
    (void)src; (void)size; (void)sha256; (void)need_signed;
    const char *b = strrchr(dest, '/');
    char id[32];
    snprintf(id, sizeof(id), "%s", b ? b + 1 : dest);
    id[strcspn(id, ".")] = 0;
    const int i = ls_store_find(id);
    for (int k = 0; k < 16; k++)
        if (!inst[k].id[0] || !strcmp(inst[k].id, id)) {
            snprintf(inst[k].id, sizeof(inst[k].id), "%s", id);
            snprintf(inst[k].ver, sizeof(inst[k].ver), "%s", i >= 0 ? ls_store_at(i)->ver : "1");
            why[0] = 0;
            return true;
        }
    snprintf(why, cap, "sim table full");
    return false;
}

bool ls_lap_remove(const char *id, bool data, char *why, size_t cap)
{
    (void)data;
    snprintf(why, cap, "%s", "");
    for (int k = 0; k < 16; k++)
        if (!strcmp(inst[k].id, id)) { inst[k].id[0] = 0; return true; }
    snprintf(why, cap, "not installed");
    return false;
}
