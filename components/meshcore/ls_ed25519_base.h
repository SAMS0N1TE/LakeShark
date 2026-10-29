/* The ed25519 base-point table, computed on first use (ls_ed25519_base.c). */
#pragma once

#include "ge.h"

#ifdef __cplusplus
extern "C" {
#endif

/* base[i][j] = (j+1) * 256^i * B as (y+x, y-x, 2dxy). Built on the first
   call (a few milliseconds), then the same 32 x 8 table every time. */
const ge_precomp (*ls_ed25519_base_table(void))[8];

#ifdef __cplusplus
}
#endif
