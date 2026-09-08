
#include "acars_resample.h"

#include <string.h>

void acars_rs_init(acars_rs_t *r)
{
    if (!r) return;
    memset(r, 0, sizeof(*r));
}

int acars_rs_5to3(acars_rs_t *r, const float *in, int n_in,
                  float *out, int cap)
{
    if (!r || !in || n_in <= 0 || !out || cap <= 0) return 0;

    int n_out = 0;
    while (n_out < cap) {
        int pos = r->pos_num;

        /* Floor division by 3 that rounds toward -infinity, so a negative
           pos (an output falling in the seam between blocks) lands on
           i = -1 and correctly uses the previous block's last sample. */
        int i;
        if (pos >= 0) i = pos / 3;
        else          i = -((-pos + 2) / 3);
        int rem = pos - i * 3;                 /* 0, 1 or 2 */
        float f = (float)rem * (1.0f / 3.0f);

        if (i >= n_in - 1) break;              /* need in[i+1] */

        float s0 = (i < 0) ? (r->primed ? r->last : in[0]) : in[i];
        float s1 = in[i + 1];
        out[n_out++] = s0 * (1.0f - f) + s1 * f;
        r->pos_num += 5;                       /* advance by 5/3 input samples */
    }

    /* Fold pos back into the next block's coordinate space. */
    r->pos_num -= n_in * 3;
    r->last    = in[n_in - 1];
    r->primed  = true;
    return n_out;
}
