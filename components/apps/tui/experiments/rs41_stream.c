#include "rs41_stream.h"
#include <string.h>
void rs41_stream_reset(rs41_stream_t *s, bool after_sync)
{
    memset(s,0,sizeof(*s));
    if (after_sync) {
        for (int i = 0; i < 8; ++i) s->wire[i] = rs41_header[i]^rs41_mask[i];
        s->used = 8; s->collecting = true;
    }
}
void rs41_stream_feed(rs41_stream_t *s, rs41_decoder_t *d, const uint8_t *msb,
                      size_t n, bool inverted, rs41_frame_fn fn, void *arg)
{
    for (size_t i = 0; i < n; ++i) for (int j = 7; j >= 0; --j) {
        unsigned bit = ((msb[i]>>j)&1)^inverted;
        s->sync = (s->sync<<1)|bit;
        if (s->sync == UINT64_C(0x086d53884469481f)) {
            for (int k = 0; k < 8; ++k) s->wire[k] = rs41_header[k]^rs41_mask[k];
            s->used = 8; s->bits = 0; s->byte = 0; s->collecting = true;
            continue;
        }
        if (!s->collecting) continue;
        s->byte |= (uint8_t)(bit<<s->bits);
        if (++s->bits != 8) continue;
        s->wire[s->used++] = s->byte; s->byte = 0; s->bits = 0;
        if (s->used == 320 || s->used == 518) {
            rs41_report_t r;
            if (rs41_decode(d,s->wire,s->used,&r)) {
                if (fn) fn(&r,arg);
                s->collecting = false;
            } else if (s->used == 518) s->collecting = false;
        }
    }
}
