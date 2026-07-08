#include "p25_qual.h"
#include "freertos/FreeRTOS.h"
#include <string.h>

#define QUAL_CAP 32

static p25_qual_rec_t s_recs[QUAL_CAP];
static int s_head = 0;
static int s_n    = 0;
static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;

void p25_qual_push(const p25_qual_rec_t *r)
{
    if (!r) return;
    portENTER_CRITICAL(&s_mux);
    s_recs[s_head] = *r;
    s_head = (s_head + 1) % QUAL_CAP;
    if (s_n < QUAL_CAP) s_n++;
    portEXIT_CRITICAL(&s_mux);
}

int p25_qual_count(void) { return s_n; }

bool p25_qual_get(int i, p25_qual_rec_t *out)
{
    bool ok = false;
    portENTER_CRITICAL(&s_mux);
    if (out && i >= 0 && i < s_n) {
        int oldest = (s_head - s_n + QUAL_CAP) % QUAL_CAP;
        *out = s_recs[(oldest + i) % QUAL_CAP];
        ok = true;
    }
    portEXIT_CRITICAL(&s_mux);
    return ok;
}

void p25_qual_clear(void)
{
    portENTER_CRITICAL(&s_mux);
    s_head = 0;
    s_n    = 0;
    portEXIT_CRITICAL(&s_mux);
}

void p25_qual_aggregate(int *calls, int *ok, int *fail, int *ok_pct,
                        int *vox, uint32_t *under, uint32_t *drop)
{
    int c = 0, to = 0, tf = 0, tv = 0;
    uint32_t tu = 0, td = 0;
    portENTER_CRITICAL(&s_mux);
    int oldest = (s_head - s_n + QUAL_CAP) % QUAL_CAP;
    for (int i = 0; i < s_n; i++) {
        const p25_qual_rec_t *r = &s_recs[(oldest + i) % QUAL_CAP];
        c++;
        to += r->bch_ok;
        tf += r->bch_fail;
        tv += r->vox;
        tu += r->under;
        td += r->drop;
    }
    portEXIT_CRITICAL(&s_mux);
    int tot = to + tf;
    if (calls)  *calls  = c;
    if (ok)     *ok     = to;
    if (fail)   *fail   = tf;
    if (ok_pct) *ok_pct = tot > 0 ? (to * 100) / tot : 0;
    if (vox)    *vox    = tv;
    if (under)  *under  = tu;
    if (drop)   *drop   = td;
}
