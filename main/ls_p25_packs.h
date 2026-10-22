/* Signed P25 catalog/download worker, registry and console adapter. */
#pragma once
#include "p25_pack.h"
#ifdef __cplusplus
extern "C" {
#endif
typedef struct {
    p25_pack_entry_t pack;
    bool installed;
    char installed_version[24];
} ls_p25_pack_info_t;
void ls_p25_packs_publish(void);
/* Cached per-entry metadata, no I/O. Returns catalog count. */
unsigned ls_p25_pack_entry(unsigned index, ls_p25_pack_info_t *out);
int ls_p25_packs_command(int argc, char **argv);
#ifdef __cplusplus
}
#endif
