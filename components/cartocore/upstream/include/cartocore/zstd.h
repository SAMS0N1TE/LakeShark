#ifndef CC_ZSTD_H
#define CC_ZSTD_H
#include "core.h"
/* One independent frame, no dictionaries, <=64 KiB window, <=4 MiB output.
   Context and retained output are caller-arena allocations (PSRAM is suitable).
   Bufferless block streaming uses retained output as history, no history copy. */
#define CC_ZSTD_MAX_OUTPUT (4u*1024u*1024u)
size_t cc_zstd_context_bytes(void);
int cc_zstd_header(cc_str tile,size_t *decoded);
int cc_zstd_decode(cc_str tile,cc_arena *arena,cc_str *decoded);
/* Callback block stream: no whole compressed tile staging. <=64 KiB reads. */
int cc_zstd_decode_read(int (*read)(void *,uint64_t,void *,size_t),void *ctx,uint64_t offset,
                         size_t bytes,cc_arena *arena,cc_str *decoded);
#endif
