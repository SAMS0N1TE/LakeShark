
#include "acars_msk.h"
#include "acars.h"

#include "esp_heap_caps.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/* Samples per bit at ACARS_SAMP_RATE / ACARS_BAUD = 19200 / 2400 = 8. */
#define SPB          8
#define BIT_RING     512
#define N_PHASES     SPB

/* Sync pattern - two SYN characters (0x16) with odd parity, LSB bit first,
   shifted into a 16-bit register with the newest bit at the LSB.  Duplicated
   here because the MSK slicer needs to find this pattern too, to establish
   which of the 8 sample offsets per bit period is the correct bit boundary.
   Once locked, the framer above sees these same bits and locks again. */
#define ACARS_SYNC_16  0x6868u

/* Cached tone tables.  cos/sin evaluated at k=0..SPB-1 for both mark and
   space, so the correlator is a dot product with no runtime trig. */
static float MARK_COS [SPB], MARK_SIN [SPB];
static float SPACE_COS[SPB], SPACE_SIN[SPB];
static int   TABLES_READY = 0;

static void build_tables(void)
{
    if (TABLES_READY) return;
    for (int k = 0; k < SPB; k++) {
        double tm = 2.0 * M_PI * (double)ACARS_TONE_MARK  * (double)k /
                    (double)ACARS_SAMP_RATE;
        double ts = 2.0 * M_PI * (double)ACARS_TONE_SPACE * (double)k /
                    (double)ACARS_SAMP_RATE;
        MARK_COS [k] = (float)cos(tm);  MARK_SIN [k] = (float)sin(tm);
        SPACE_COS[k] = (float)cos(ts);  SPACE_SIN[k] = (float)sin(ts);
    }
    TABLES_READY = 1;
}

/* Per-phase state - one of these per candidate bit-clock alignment. */
typedef struct {
    float    mi, mq;              /* correlator against mark tone  */
    float    si, sq;              /* correlator against space tone */
    uint16_t hist;                /* last 16 emitted bits          */
} phase_t;

struct acars_msk {
    float    dc;                  /* leaky DC tracker              */
    int      pos;                 /* global sample position mod SPB */
    phase_t  ph[N_PHASES];

    /* -1 while searching, 0..SPB-1 once one phase has found sync.
       In the locked state, only that phase's accumulators run. */
    int      locked_phase;

    uint8_t  bits[BIT_RING];
    int      bh, bt;
    int      bcount;
};

static void bit_push(acars_msk_t *m, int bit)
{
    if (m->bcount >= BIT_RING) {
        m->bt = (m->bt + 1) % BIT_RING;
        m->bcount--;
    }
    m->bits[m->bh] = (uint8_t)(bit & 1);
    m->bh = (m->bh + 1) % BIT_RING;
    m->bcount++;
}

static void phase_reset_accum(phase_t *p)
{
    p->mi = p->mq = p->si = p->sq = 0.0f;
}

/* Accumulate one sample into phase p's correlator.  k is the position of
   this sample within phase p's current bit period, 0..SPB-1. */
static inline void phase_accum(phase_t *p, float x, int k)
{
    p->mi += x * MARK_COS [k];
    p->mq += x * MARK_SIN [k];
    p->si += x * SPACE_COS[k];
    p->sq += x * SPACE_SIN[k];
}

/* Slice one bit from phase p: which tone has more energy. */
static inline int phase_slice(const phase_t *p)
{
    float me = p->mi * p->mi + p->mq * p->mq;
    float se = p->si * p->si + p->sq * p->sq;
    return me > se ? 1 : 0;
}

acars_msk_t *acars_msk_create(void)
{
    build_tables();
    /* the 512-bit ring and eight correlator phases are bulk decoder
       state, with no DMA, ISR or cache-off access.  Keep them out of the
       internal heap when PSRAM is available. */
    acars_msk_t *m = (acars_msk_t *)heap_caps_calloc(
        1, sizeof(*m), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!m) m = (acars_msk_t *)heap_caps_calloc(
        1, sizeof(*m), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!m) return NULL;
    acars_msk_reset(m);
    return m;
}

void acars_msk_destroy(acars_msk_t *m) { if (m) heap_caps_free(m); }

void acars_msk_reset(acars_msk_t *m)
{
    m->dc = 0.0f;
    m->pos = 0;
    for (int i = 0; i < N_PHASES; i++) {
        m->ph[i].mi = m->ph[i].mq = m->ph[i].si = m->ph[i].sq = 0.0f;
        m->ph[i].hist = 0;
    }
    m->locked_phase = -1;
    m->bh = m->bt = 0;
    m->bcount = 0;
}

int acars_msk_available(const acars_msk_t *m) { return m ? m->bcount : 0; }

int acars_msk_process(acars_msk_t *m, const float *audio, int n)
{
    if (!m || !audio) return 0;
    int emitted = 0;

    for (int i = 0; i < n; i++) {
        float x = audio[i];
        /* Slow leaky DC tracker.  MSK audio averages to nonzero over
           short spans (bit '1' is a half sine, mean ~2/pi), so the
           coefficient here is small - fast enough to absorb tuning
           offset, slow enough that a run of one bit does not track. */
        m->dc += (x - m->dc) * (1.0f / 512.0f);
        float y = x - m->dc;

        if (m->locked_phase < 0) {
            /* Not locked: run all SPB phases in parallel, each with
               its own bit-period alignment.  When one finds the sync
               pattern (0x6868 or its complement, for inverted audio),
               lock to that phase and emit the sync bits so the framer
               above can lock as well. */
            for (int p = 0; p < N_PHASES; p++) {
                int k = (m->pos - p + SPB) % SPB;
                if (k == 0) phase_reset_accum(&m->ph[p]);
                phase_accum(&m->ph[p], y, k);
                if (k == SPB - 1) {
                    int bit = phase_slice(&m->ph[p]);
                    m->ph[p].hist = (uint16_t)((m->ph[p].hist << 1) | (uint32_t)bit);

                    uint16_t h = m->ph[p].hist;
                    uint16_t inv = (uint16_t)~h;
                    if (h == ACARS_SYNC_16 || inv == ACARS_SYNC_16) {
                        m->locked_phase = p;
                        /* Emit the 16 sync bits so the framer above
                           locks as well.  Bit 15 was received first. */
                        for (int b = 15; b >= 0; b--)
                            bit_push(m, (h >> b) & 1);
                        emitted += 16;
                        /* Clear other phases so they don't fire again. */
                        for (int q = 0; q < N_PHASES; q++)
                            if (q != p) m->ph[q].hist = 0;
                        break;
                    }
                }
            }
        } else {
            int p = m->locked_phase;
            int k = (m->pos - p + SPB) % SPB;
            if (k == 0) phase_reset_accum(&m->ph[p]);
            phase_accum(&m->ph[p], y, k);
            if (k == SPB - 1) {
                int bit = phase_slice(&m->ph[p]);
                bit_push(m, bit);
                emitted++;
            }
        }

        m->pos = (m->pos + 1) % SPB;
    }
    return emitted;
}

int acars_msk_read_bits(acars_msk_t *m, uint8_t *out, int cap)
{
    if (!m || !out) return 0;
    int n = 0;
    while (n < cap && m->bcount > 0) {
        out[n++] = m->bits[m->bt];
        m->bt = (m->bt + 1) % BIT_RING;
        m->bcount--;
    }
    return n;
}
