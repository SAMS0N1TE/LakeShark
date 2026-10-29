#ifndef LR_DDC_H
#define LR_DDC_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
/* LR2021 wideband I/Q -> one channel at 125 kS/s (a digital down-converter).
 * The LO is tuned off the wanted channel so its DC leakage and centre spurs
 * fall outside it; this block mixes the channel to 0 Hz and decimates.
 * 500k and 1M divide by 4 and 8. 576k, 666,667 and 800k reach exactly 125k
 * through a fractional step in the last stage, so the output is a drop-in
 * for every consumer that reads the native 125k stream.
 *
 *   in -> [DC block] -> mix -> CIC5 /R -> polyphase channel filter
 *         (CIC droop corrected), rational step Fm/out -> int16 out
 *
 *   in       R  Fm      step       channel taps x rows
 *   500000   2  250000  2          29 x 1
 *   576000   2  288000  2+38/125   33 x 125
 *   666667   2  333333  2+2/3      37 x 3
 *   800000   3  266667  2+2/15     31 x 15
 *   1000000  4  250000  2          29 x 1
 *
 * R is the largest integer leaving Fm >= 2*out. The channel filter is flat
 * to 0.16*out (20 kHz) and 60+ dB down from out/2 (62.5 kHz), so a tone
 * 50 kHz past a 12.5 kHz channel cannot alias in. Coefficients are designed
 * at init (Kaiser windowed sinc), one row per phase of the rational step, so
 * no output blends rows (rates whose step denominator is too large for the
 * table fall back to 17 rows blended linearly). No allocation, no waits and
 * no libm calls in process.
 *
 * Mixing. An offset of exactly +-in/4 (576k: +-144 kHz, 500k: +-125 kHz,
 * 1M: +-250 kHz) mixes by sign and swap: no oscillator, no multiplies. Any
 * other offset runs a recursive float oscillator re-anchored to the exact
 * phase every 32 input samples.
 *
 * DC (LO leakage) is mixed to -offset. When |offset| >= out that is deep in
 * the stopband and the filters remove it; nearer the channel a one-pole DC
 * blocker (about 20..40 Hz) runs on the input as well.
 *
 * Structure for speed on the P4 (rv32 with a single-precision FPU):
 * R = 2 runs the CIC5 as its exact 6-tap form [1 5 10 10 5 1] in float, four
 * input pairs per step; the channel filter computes two outputs per pass over
 * the data so each sample load feeds four multiply-adds. By instruction count
 * at 576k: about 10 per input pair for the in/4 front (20 with the
 * oscillator) and about 40 for the channel filter (33 taps, 125k outputs).
 * `lora lrddc bench` measures it on the board.
 *
 * The state is caller-owned, about 22 KB (mostly the coefficient table),
 * read per block: put it in PSRAM (EXT_RAM_BSS_ATTR or heap_caps
 * MALLOC_CAP_SPIRAM) unless internal RAM is free. One owner per state; not
 * thread safe. */

#define LR_DDC_OUT_HZ    125000u /* the rate every LR I/Q consumer expects */
#define LR_DDC_CHUNK     256     /* input pairs per internal pass */
#define LR_DDC_CIC_N     5       /* CIC order */
#define LR_DDC_PHASES    16      /* fallback rows (blended) for large step denominators */
#define LR_DDC_NT_MAX    56      /* channel filter taps per row */
#define LR_DDC_FIR_MAX   5120    /* coefficient table, floats (rows x padded taps) */

typedef struct { float i, q; } lr_ddc_cf;

typedef struct {
    /* Configuration, set by lr_ddc_init. Input rate is in_num/in_den Hz. */
    uint32_t in_num, in_den, out_hz;
    int32_t offset_hz;
    uint32_t decim;             /* CIC R */
    /* Channel filter step, CIC outputs per output: int + num/den. */
    uint32_t step_int, step_num, step_den;
    uint32_t nco_step;
    float nco_wr, nco_wi, cic_gain;
    bool dc_on;
    bool exact;                 /* one row per step phase; else 17 blended rows */
    int8_t quarter;             /* +1: offset +in/4, -1: -in/4, 0: oscillator */
    uint8_t nt;                 /* channel filter taps */
    uint8_t pad;                /* zero taps ahead of each row */
    uint16_t stride, rows;      /* floats per row (padded), rows in fir */
    float fir[LR_DDC_FIR_MAX];  /* includes the CIC gain 1/R^5 */
    /* Running state, cleared by lr_ddc_reset. nco_phase must stay first. */
    uint32_t nco_phase, n_abs, cic_left, facc, fpos;
    uint32_t dropped;           /* outputs that did not fit the caller's cap */
    float nco_zr, nco_zi;
    int32_t dc_i, dc_q;         /* input DC estimate, Q14 */
    uint32_t integ[2][LR_DDC_CIC_N], comb[2][LR_DDC_CIC_N]; /* R > 2 */
    float hist[2][LR_DDC_CIC_N]; /* R = 2: last mixed inputs, newest first */
    lr_ddc_cf fin_buf[LR_DDC_NT_MAX + LR_DDC_CHUNK / 2 + 8];
} lr_ddc_t;

/* in_hz: the driver's pair rate. 666666 and 666667 both mean the exact
   2,000,000/3. offset_hz: wanted channel minus LO (LO 150 kHz below the
   channel -> +150000). out_hz: normally LR_DDC_OUT_HZ. False unless
   2*out_hz <= in_hz < 16*out_hz and the channel's +-out/2 fits inside the
   input band (|offset| <= in/2 - out/2). */
bool lr_ddc_init(lr_ddc_t *s, uint32_t in_hz, int32_t offset_hz, uint32_t out_hz);
/* Retune the NCO only. Phase continuous; filter history is kept. */
bool lr_ddc_set_offset(lr_ddc_t *s, int32_t offset_hz);
/* Clear history and NCO phase, keep configuration (call on a stream gap). */
void lr_ddc_reset(lr_ddc_t *s);
/* in: `pairs` interleaved signed I,Q. out: up to `cap` interleaved I,Q pairs
   at lr_ddc_out_hz, same scale as the input (a tone of amplitude A comes out
   at amplitude A), saturated to int16. Returns pairs written; any beyond cap
   are counted in s->dropped. Output is independent of how the input is cut
   into blocks. Both buffers are little-endian int16 on the P4, so `out` can
   go straight to a consumer taking (const uint8_t *)out, 4 bytes a pair. */
size_t lr_ddc_process(lr_ddc_t *s, const int16_t *in, size_t pairs,
                      int16_t *out, size_t cap);
/* Exact output rate: always the out_hz given to init. */
uint32_t lr_ddc_out_hz(const lr_ddc_t *s);
/* Upper bound on what one lr_ddc_process call of in_pairs can return. */
size_t lr_ddc_out_max(const lr_ddc_t *s, size_t in_pairs);
#ifdef __cplusplus
}
#endif
#endif
