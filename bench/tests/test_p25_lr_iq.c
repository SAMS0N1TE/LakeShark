#include "ls_test.h"
#include "p25_lr_iq.h"
#include <string.h>
LS_CASE(iq_bridge_rejects_partial_and_oversized_windows_without_writing)
{
    uint8_t in[8] = {0}, out[16]; memset(out, 0xA5, sizeof(out));
    LS_EQ_UINT(p25_lr_iq_convert(in, 7, out, sizeof(out)), 0);
    LS_EQ_UINT(p25_lr_iq_convert(in, 125260, out, sizeof(out)), 0);
    LS_EQ_UINT(p25_lr_iq_convert(in, 8, out, 3), 0);
    LS_EQ_UINT(p25_lr_iq_convert(NULL, 8, out, sizeof(out)), 0);
    for (unsigned i=0; i<sizeof(out); i++) LS_EQ_UINT(out[i], 0xA5);
}
LS_CASE(iq_bridge_preserves_iq_order_signed_extrema_and_output_boundary)
{
    uint8_t in[] = {0,128,255,127, 0,128,255,127};
    uint8_t out[8]; memset(out, 0xA5, sizeof(out));
    LS_EQ_UINT(p25_lr_iq_convert(in, sizeof(in), out, sizeof(out)), 4);
    LS_EQ_UINT(out[0], 18); LS_EQ_UINT(out[1], 237);
    LS_EQ_UINT(out[2], 18); LS_EQ_UINT(out[3], 237);
    LS_EQ_UINT(out[4], 0xA5);
    memset(in, 0, sizeof(in));
    LS_EQ_UINT(p25_lr_iq_convert(in, sizeof(in), out, sizeof(out)), 4);
    for(unsigned i=0;i<4;i++) LS_EQ_UINT(out[i],128);
}
