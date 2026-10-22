/* Firmware parser entry point used by pack production and host tests. */
#include "p25_pack.h"
#include "p25_controls.h"
#include <stdio.h>
#include <stdlib.h>
int main(int argc, char **argv)
{
    if (argc != 2) { fprintf(stderr, "usage: p25pack_validate pack.txt\n"); return 2; }
    FILE *f = fopen(argv[1], "rb"); if (!f) return 2;
    char *text = malloc(P25_PACK_MAX_BYTES + 1);
    p25_pack_db_t *db = calloc(1, sizeof(*db));
    p25_profile_parse_scratch_t *scratch = malloc(sizeof(*scratch));
    if (!text || !db || !scratch) return 2;
    size_t len = fread(text, 1, P25_PACK_MAX_BYTES + 1, f);
    bool read_ok = !ferror(f); fclose(f);
    const ls_radio_range_t ranges[] = {{P25_CONTROL_TUNER_MIN_HZ, P25_CONTROL_TUNER_MAX_HZ}};
    const p25_profile_parse_config_t cfg = {ranges, 1};
    p25_profile_diagnostic_t diag = {0};
    bool ok = read_ok && p25_pack_parse(db, scratch, text, len, &cfg, &diag);
    if (ok) printf("valid: %u records, %u sites; db=%zu scratch=%zu profile=%zu bytes\n", db->count, db->sites, sizeof(*db), sizeof(*scratch), sizeof(p25_profile_t));
    else fprintf(stderr, "invalid pack: record line %zu, %s\n", diag.line, diag.reason ? diag.reason : "pack version, capacity or geometry");
    free(text); free(db); free(scratch); return ok ? 0 : 1;
}
