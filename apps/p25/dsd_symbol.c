#include "dsd.h"

/*
 * ──────────────────────────────────────────────────────────────
 *  Session 9: median-of-5 for C4FM symbol extraction
 * ──────────────────────────────────────────────────────────────
 *
 * Replaces the mean-of-4 averaging used by stock DSD at 10 sps. Rationale:
 * our pipeline now runs at 24 kHz / 5 sps after narrowing the LPF to fc=6 kHz.
 * At this rate a single envelope null produces a 1-2 sample phase-wrap spike
 * that the FM discriminator reports as a legitimate ±π rad/sample step
 * (~±28 k counts). The mean-of-4 at 10 sps used to catch these inside the
 * averaging window and collapse the dibit. Median of 5 is immune to any
 * two-sample pulse: after sorting, the middle value is guaranteed to come
 * from the plateau, not the pulse.
 *
 * The network below is the optimal 7-comparison median-of-5 sorter
 * (branchless on most toolchains; compiles to a straight line of min/max
 * pairs). Verified over 100k random inputs to match sort()[2]. The spec
 * R[18] sample window [-339, -4177, -12863, -1635, +884] returns -1635,
 * exactly as a textbook sort would give.
 */
static inline short median_of_5(short p0, short p1, short p2, short p3, short p4)
{
    short a = p0, b = p1, c = p2, d = p3, e = p4, t;
    #define SORT2(x, y) do { if ((x) > (y)) { t = (x); (x) = (y); (y) = t; } } while (0)
    SORT2(a, b);
    SORT2(d, e);
    SORT2(a, d);   /* a = min of {a,b,d,e}; discardable */
    SORT2(b, e);   /* e = max of {a,b,d,e}; discardable */
    SORT2(b, c);   /* now sort the middle 3 {b,c,d} */
    SORT2(c, d);
    SORT2(b, c);
    #undef SORT2
    return c;      /* median */
}

/*
 * ──────────────────────────────────────────────────────────────
 *  TIMING DIAGNOSTIC — flight recorder
 * ──────────────────────────────────────────────────────────────
 *  Captures jitter index and raw symbol value for each getSymbol call
 *  during a frame. processFrame() in dsd_frame.c calls reset() just
 *  before the 33-dibit read and dump() after BCH check.
 */
#define TIMING_TRACE_LEN 40
static int timing_trace_jit[TIMING_TRACE_LEN];
static int timing_trace_sym[TIMING_TRACE_LEN];
static int timing_trace_idx = 0;
static int timing_trace_armed = 0;

/*
 * Session 8 RAW-sample flight recorder. (Updated session 9 for 5 sps.)
 *
 * For each symbol collected during an NID read, stash all 5 raw samples
 * from the symbol window. When BCH fails, dsd_frame.c can call
 * dsd_raw_trace_dump() to emit RAW[i] lines for ALL 32 dibits of the NID.
 *
 * Storage cost: 40 * 5 * 2 = 400 bytes static.
 */
#define RAW_TRACE_LEN TIMING_TRACE_LEN
#define RAW_TRACE_SAMPLES 5
static short raw_trace_samples[RAW_TRACE_LEN][RAW_TRACE_SAMPLES];
static int   raw_trace_sum[RAW_TRACE_LEN];
static int   raw_trace_symbol[RAW_TRACE_LEN];
static int   raw_trace_idx = 0;
static int   raw_trace_valid[RAW_TRACE_LEN];  /* 1 if samples were captured */

void dsd_raw_trace_reset(void)
{
    raw_trace_idx = 0;
    for (int i = 0; i < RAW_TRACE_LEN; i++) {
        raw_trace_valid[i] = 0;
    }
}

void dsd_timing_trace_reset(void)
{
    timing_trace_idx = 0;
    timing_trace_armed = 1;
    for (int i = 0; i < TIMING_TRACE_LEN; i++) {
        timing_trace_jit[i] = -99;
        timing_trace_sym[i] = 0;
    }
}

extern void sys_log(unsigned char color, const char *fmt, ...);

void dsd_timing_trace_dump(const char *tag)
{
    int n = timing_trace_idx;
    if (n <= 0) return;
    if (n > TIMING_TRACE_LEN) n = TIMING_TRACE_LEN;

    /* Jitter histogram */
    int hist[11];
    for (int k = 0; k < 11; k++) hist[k] = 0;
    int valid = 0;
    for (int i = 0; i < n; i++) {
        int j = timing_trace_jit[i];
        if (j >= 0 && j <= 10) { hist[j]++; valid++; }
    }

    /* Segment means */
    int seg_sum[4] = {0,0,0,0};
    int seg_cnt[4] = {0,0,0,0};
    for (int i = 0; i < n && i < 33; i++) {
        int j = timing_trace_jit[i];
        if (j < 0 || j > 10) continue;
        int seg = (i < 6) ? 0 : (i < 9) ? 1 : (i < 29) ? 2 : 3;
        seg_sum[seg] += j;
        seg_cnt[seg]++;
    }

    sys_log(3, "%s jit hist 0:%d 1:%d 2:%d 3:%d 4:%d 5:%d 6:%d 7:%d 8:%d 9:%d v=%d",
            tag, hist[0], hist[1], hist[2], hist[3], hist[4], hist[5],
            hist[6], hist[7], hist[8], hist[9], valid);

    int m0 = seg_cnt[0] ? (seg_sum[0] * 10 / seg_cnt[0]) : -1;
    int m1 = seg_cnt[1] ? (seg_sum[1] * 10 / seg_cnt[1]) : -1;
    int m2 = seg_cnt[2] ? (seg_sum[2] * 10 / seg_cnt[2]) : -1;
    int m3 = seg_cnt[3] ? (seg_sum[3] * 10 / seg_cnt[3]) : -1;
    sys_log(3, "%s jit*10 NAC=%d DUID=%d BCH=%d tail=%d (good=edges, bad=4-6)",
            tag, m0, m1, m2, m3);

    sys_log(3, "%s sym0-7   %+6d %+6d %+6d %+6d %+6d %+6d %+6d %+6d", tag,
            timing_trace_sym[0], timing_trace_sym[1], timing_trace_sym[2],
            timing_trace_sym[3], timing_trace_sym[4], timing_trace_sym[5],
            timing_trace_sym[6], timing_trace_sym[7]);
    sys_log(3, "%s sym8-15  %+6d %+6d %+6d %+6d %+6d %+6d %+6d %+6d", tag,
            timing_trace_sym[8], timing_trace_sym[9], timing_trace_sym[10],
            timing_trace_sym[11], timing_trace_sym[12], timing_trace_sym[13],
            timing_trace_sym[14], timing_trace_sym[15]);
    sys_log(3, "%s sym24-32 %+6d %+6d %+6d %+6d %+6d %+6d %+6d %+6d %+6d", tag,
            timing_trace_sym[24], timing_trace_sym[25], timing_trace_sym[26],
            timing_trace_sym[27], timing_trace_sym[28], timing_trace_sym[29],
            timing_trace_sym[30], timing_trace_sym[31], timing_trace_sym[32]);

    /* Ideal thresholds derived from observed symbols */
    int smax = timing_trace_sym[0], smin = timing_trace_sym[0];
    for (int i = 0; i < n && i < 33; i++) {
        if (timing_trace_sym[i] > smax) smax = timing_trace_sym[i];
        if (timing_trace_sym[i] < smin) smin = timing_trace_sym[i];
    }
    int icenter = (smax + smin) / 2;
    int iumid   = (smax + icenter) / 2;
    int ilmid   = (smin + icenter) / 2;
    sys_log(3, "%s ideal  min=%+d lmid=%+d center=%+d umid=%+d max=%+d (compare to THR)",
            tag, smin, ilmid, icenter, iumid, smax);

    timing_trace_armed = 0;
}

/*
 * Session 8: Dump stored RAW samples for all captured dibits on BCH failure.
 * Prints one line per dibit showing all 5 samples of the symbol window
 * (session 9: was 10 when pipeline ran at 48 kHz).
 */
void dsd_raw_trace_dump(const char *tag)
{
    int n = raw_trace_idx;
    if (n <= 0) return;
    if (n > RAW_TRACE_LEN) n = RAW_TRACE_LEN;

    for (int i = 0; i < n; i++) {
        if (!raw_trace_valid[i]) continue;
        sys_log(3, "%s R[%02d] %+6d %+6d %+6d %+6d %+6d med=%+6d",
                tag, i,
                raw_trace_samples[i][0], raw_trace_samples[i][1],
                raw_trace_samples[i][2], raw_trace_samples[i][3],
                raw_trace_samples[i][4],
                raw_trace_symbol[i]);
    }
}

static inline void timing_trace_record(int jitter, int symbol)
{
    if (!timing_trace_armed) return;
    if (timing_trace_idx >= TIMING_TRACE_LEN) return;
    timing_trace_jit[timing_trace_idx] = jitter;
    timing_trace_sym[timing_trace_idx] = symbol;
    timing_trace_idx++;
}

/* Existing DSD 1.7 code follows ─────────────────────────────── */


/*
 * ──────────────────────────────────────────────────────────────
 *  Session 10: C4FM symbol-timing clock assist (TED)
 *  Ported from DSD-Neo src/dsp/dsd_symbol.c (maybe_c4fm_clock).
 * ──────────────────────────────────────────────────────────────
 *
 *  Stock DSD 1.7 has no symbol timing recovery — it uses a fixed
 *  symbolCenter=2 and prays that samples fall on plateaus. When RTL
 *  clock drift or sync-detection offset puts the center off by
 *  ±1 sample, we sit on transitions. This is the closed-loop
 *  correction that makes symbolCenter track the true symbol phase.
 *
 *  Two modes:
 *    1 = Early-Late  — non-data-aided; uses |late|² − |early|² as
 *                      timing error. Works even when decisions are
 *                      unreliable. More chatter, less stable center.
 *    2 = Mueller-Müller — data-aided; error is
 *                      (late − early) × slice(mid). Requires correct
 *                      symbol decisions; more stable once center and
 *                      min/max refs are populated, and strictly better
 *                      once sync is being acquired.
 *
 *  After computing the sign of the error, we only nudge symbolCenter
 *  when the same direction has been observed for 4 consecutive symbols.
 *  Cooldown of 12 symbols after a nudge prevents chatter.
 *
 *  Called once per symbol from getSymbol() after the median symbol
 *  value has been computed. See call site below for wiring of
 *  early/mid/late from the 5-sample saved_samples[] buffer.
 */

/* Slice a single sample onto the nominal C4FM constellation {-3,-1,+1,+3}
 * using state->center (mid reference) and ((minref+center)/2, (maxref+center)/2)
 * as the outer-to-inner boundaries. Returns one of {-3,-1,+1,+3}. */
static inline int slice_c4fm_level(int x, const dsd_state *s)
{
    float c  = (float)s->center;
    float lo = ((float)s->minref + c) * 0.5f;
    float hi = ((float)s->maxref + c) * 0.5f;
    if ((float)x >= hi)      return  3;
    else if ((float)x >= c)  return  1;
    else if ((float)x >= lo) return -1;
    else                     return -3;
}

/* Run one step of the C4FM clock TED. Call after each symbol is decided.
 *  early = sample at (symbolCenter - 1)
 *  mid   = sample at (symbolCenter)
 *  late  = sample at (symbolCenter + 1)
 *
 * Mutates: state->symbolCenter (bounded [1, samplesPerSymbol-2]),
 *          state->c4fm_clk_{prev_dec,run_dir,run_len,cooldown,nudges}.
 *
 * Deliberately stripped of DSD-Neo's runtime gating (audio_in_type,
 * have_sync policy, cfg lookups). Our pipeline is always RTL-SDR C4FM;
 * calling this during sync is desired — drift doesn't pause during acquisition.
 */
static inline void maybe_c4fm_clock(dsd_state *state, int mode,
                                    int early, int mid, int late)
{
    if (mode <= 0) return;

    /* C4FM only. rf_mod 0=C4FM, 1=QPSK, 2=GFSK. */
    if (state->rf_mod != 0) return;

    /* Require valid neighborhood around center inside the symbol window.
     * At samplesPerSymbol=5 with symbolCenter=2, early=1, late=3 — fine.
     * If a future nudge pushes symbolCenter to 1 or 3, the bound-check in
     * the nudge block keeps us inside [1, sps-2] so this stays safe. */
    if (state->symbolCenter < 1 ||
        state->symbolCenter + 1 >= state->samplesPerSymbol) {
        return;
    }

    long long e = 0;
    if (mode == 1) {
        /* Early-Late: timing error ∝ |late|² − |early|². Sign tells us
         * which side of the symbol has more energy; positive means the
         * late sample is further from zero → we sampled too early → move
         * center toward late → dir = +1. */
        long long er = (long long)early;
        long long lr = (long long)late;
        e = (lr * lr) - (er * er);
    } else if (mode == 2) {
        /* Mueller-Müller: e = (late − early) × slice(mid).
         * Needs one symbol of history before it produces output. */
        int a_prev = state->c4fm_clk_prev_dec;
        int a_k = slice_c4fm_level(mid, state);
        if (a_prev == 0) {
            state->c4fm_clk_prev_dec = a_k;
            return;
        }
        long long diff = (long long)late - (long long)early;
        e = diff * (long long)a_k;
        state->c4fm_clk_prev_dec = a_k;
        (void)a_prev;   /* DSD-Neo stores prev but the classical M&M using
                         * only (late−early)*a_k here; kept for parity. */
    } else {
        return;
    }

    int dir;
    if      (e > 0) dir = +1;   /* sample early → shift center right */
    else if (e < 0) dir = -1;   /* sample late  → shift center left */
    else {
        /* Exact zero: reset run; don't penalize persistence otherwise. */
        state->c4fm_clk_run_dir = 0;
        state->c4fm_clk_run_len = 0;
        return;
    }

    if (state->c4fm_clk_cooldown > 0) {
        state->c4fm_clk_cooldown--;
        return;
    }

    if (dir == state->c4fm_clk_run_dir) {
        state->c4fm_clk_run_len++;
    } else {
        state->c4fm_clk_run_dir = dir;
        state->c4fm_clk_run_len = 1;
    }

    /* Nudge once we've seen 4 consecutive same-direction errors. */
    if (state->c4fm_clk_run_len >= 4) {
        int c = state->symbolCenter + dir;
        int min_c = 1;
        int max_c = state->samplesPerSymbol - 2;
        if (c < min_c) c = min_c;
        if (c > max_c) c = max_c;
        if (c != state->symbolCenter) {
            state->symbolCenter = c;
            state->c4fm_clk_nudges++;
        }
        state->c4fm_clk_cooldown = 12;
        state->c4fm_clk_run_len = 0;
    }
}


int dsd_ring_available(dsd_sample_ring_t *r)
{
    int w = r->write_idx;
    int rd = r->read_idx;
    if (w >= rd)
        return w - rd;
    return DSD_SAMPLE_RING_SIZE - rd + w;
}

int16_t dsd_ring_read_one(dsd_sample_ring_t *r)
{
    while (dsd_ring_available(r) == 0) {
        extern void dsd_yield(void);
        dsd_yield();
    }
    int16_t val = r->buf[r->read_idx];
    r->read_idx = (r->read_idx + 1) % DSD_SAMPLE_RING_SIZE;
    return val;
}

int
getSymbol(dsd_opts *opts, dsd_state *state, int have_sync)
{
    short sample;
    int i, sum, symbol, count;
    /* Raw-sample buffer for diagnostic dump at end of function. */
    short saved_samples[10] = {0};
    int   saved_samples_valid = 0;
    int   saved_idx = 0;

    sum = 0;
    count = 0;
    for (i = 0; i < state->samplesPerSymbol; i++) {
        if ((i == 0) && (have_sync == 0)) {
            if (state->samplesPerSymbol == 20) {
                if ((state->jitter >= 7) && (state->jitter <= 10))
                    i--;
                else if ((state->jitter >= 11) && (state->jitter <= 14))
                    i++;
            } else if (state->rf_mod == 1) {
                if ((state->jitter >= 0) && (state->jitter < state->symbolCenter))
                    i++;
                else if ((state->jitter > state->symbolCenter) && (state->jitter < 10))
                    i--;
            } else if (state->rf_mod == 2) {
                if ((state->jitter >= state->symbolCenter - 1) && (state->jitter <= state->symbolCenter))
                    i--;
                else if ((state->jitter >= state->symbolCenter + 1) && (state->jitter <= state->symbolCenter + 2))
                    i++;
            } else if (state->rf_mod == 0) {
                if ((state->jitter > 0) && (state->jitter <= state->symbolCenter))
                    i--;
                else if ((state->jitter > state->symbolCenter) && (state->jitter < state->samplesPerSymbol))
                    i++;
            }
            state->jitter = -1;
        }

        sample = dsd_ring_read_one(opts->ring);

        /* Capture into saved_samples[] in the order we read them (up to 10). */
        if (saved_idx < 10) {
            saved_samples[saved_idx++] = sample;
            saved_samples_valid = 1;
        }

        if (opts->use_cosine_filter) {
            if (state->lastsynctype >= 10 && state->lastsynctype <= 13)
                sample = dmr_filter(sample);
            else if (state->lastsynctype == 8 || state->lastsynctype == 9 ||
                     state->lastsynctype == 16 || state->lastsynctype == 17) {
                if (state->samplesPerSymbol == 20)
                    sample = nxdn_filter(sample);
                else
                    sample = dmr_filter(sample);
            }
        }

        /* Removed: post-sync sample clamp to state->max/min.
         * state->max is itself a 15-frame rolling avg of lmax. Clamping
         * samples to it before they feed use_symbol creates a feedback
         * loop where state->max can never grow past its current value,
         * locking outer +3 symbols (~19000) down to the seeded ~6700. */

        if (sample > state->center) {
            if (state->lastsample < state->center)
                state->numflips += 1;
            if (sample > (state->maxref * 1.25)) {
                if (state->lastsample < (state->maxref * 1.25))
                    state->numflips += 1;
                if ((state->jitter < 0) && (state->rf_mod == 1))
                    state->jitter = i;
            } else {
                if ((state->jitter < 0) && (state->lastsample < state->center) && (state->rf_mod != 1))
                    state->jitter = i;
            }
        } else {
            if (state->lastsample > state->center)
                state->numflips += 1;
            if (sample < (state->minref * 1.25)) {
                if (state->lastsample > (state->minref * 1.25))
                    state->numflips += 1;
                if ((state->jitter < 0) && (state->rf_mod == 1))
                    state->jitter = i;
            } else {
                if ((state->jitter < 0) && (state->lastsample > state->center) && (state->rf_mod != 1))
                    state->jitter = i;
            }
        }

        /* Session 9: At 5 samples/symbol (our new default for P25 C4FM),
         * skip the sum accumulator entirely. Median-of-5 is computed after
         * the loop using saved_samples[0..4]. Keep sum accumulator path for
         * legacy 10-sps, 20-sps, and QPSK at 5-sps cases. */
        if (state->samplesPerSymbol == 20) {
            if ((i >= 9) && (i <= 11)) {
                sum += sample;
                count++;
            }
        } else if (state->samplesPerSymbol == 5) {
            /* C4FM: median taken after loop from saved_samples[]. No sum needed.
             * QPSK/GFSK (rf_mod != 0): fall back to mean of 2 adjacent samples
             * at symbol center (indices symbolCenter-1, symbolCenter+1 = 1, 3). */
            if (state->rf_mod != 0 &&
                ((i == state->symbolCenter - 1) || (i == state->symbolCenter + 1))) {
                sum += sample;
                count++;
            }
        } else {
            /* 10 sps (unused in session 9 but keep for parity with upstream DSD). */
            if (state->rf_mod == 0) {
                if ((i >= state->symbolCenter - 1) && (i <= state->symbolCenter + 2)) {
                    sum += sample;
                    count++;
                }
            } else {
                if ((i == state->symbolCenter - 1) || (i == state->symbolCenter + 1)) {
                    sum += sample;
                    count++;
                }
            }
        }

        state->lastsample = sample;
    }

    /* Session 9: C4FM symbol extraction via median-of-5 (spike-immune).
     * Other modes continue using sum/count mean as before. */
    if (state->samplesPerSymbol == 5 && state->rf_mod == 0 && saved_samples_valid) {
        symbol = median_of_5(saved_samples[0], saved_samples[1], saved_samples[2],
                             saved_samples[3], saved_samples[4]);
    } else if (count > 0) {
        symbol = sum / count;
    } else {
        symbol = 0;
    }
    state->symbolcnt++;

    /* Session 10: C4FM clock-assist TED.
     * Feeds the three middle samples from the 5-sample window to the
     * Mueller-Müller / Early-Late timing error detector. It may nudge
     * state->symbolCenter ±1 after observing 4 consistent directional
     * errors, with a 12-symbol cooldown between nudges. No effect when
     * state->c4fm_clk_mode == 0 (disabled) or rf_mod != 0 (not C4FM).
     *
     * Session 10b: gated on have_sync == 0. The TED may only adjust
     * symbolCenter while searching for sync. Once sync is acquired,
     * symbolCenter is frozen for the remainder of the frame so that
     * symbols 0-5 (NAC) and symbols 6-32 (DUID + BCH) are read through
     * the same sampling window. This avoids the failure mode seen in
     * first device runs: NAC decoded correctly (pre-nudge center),
     * rest of NID decoded wrong (post-nudge center), uncorrectable BCH. */
    if (have_sync == 0 &&
        state->samplesPerSymbol == 5 &&
        state->rf_mod == 0 &&
        saved_samples_valid &&
        state->symbolCenter >= 1 &&
        state->symbolCenter + 1 < state->samplesPerSymbol) {
        int early = saved_samples[state->symbolCenter - 1];
        int mid   = saved_samples[state->symbolCenter];
        int late  = saved_samples[state->symbolCenter + 1];
        maybe_c4fm_clock(state, state->c4fm_clk_mode, early, mid, late);
    }

    /* Flight-recorder hook: captures this symbol's zero-crossing position and
     * value for the diagnostic dump in dsd_frame.c. */
    timing_trace_record(state->jitter, symbol);

    /* Session 9 RAW flight recorder.
     *
     * Stashes the 5-sample window plus the median symbol into a parallel
     * ring keyed by the same index the timing trace uses. dsd_frame.c dumps
     * these on BCH failure. Only captures when the timing trace is armed
     * (during an NID read) and when we have 5 samples per symbol (C4FM mode). */
    if (timing_trace_armed &&
        raw_trace_idx < RAW_TRACE_LEN &&
        state->samplesPerSymbol == 5 &&
        saved_samples_valid) {
        int k = raw_trace_idx;
        for (int r = 0; r < RAW_TRACE_SAMPLES; r++)
            raw_trace_samples[k][r] = saved_samples[r];
        raw_trace_sum[k] = sum;          /* unused at 5 sps C4FM but kept for other modes */
        raw_trace_symbol[k] = symbol;    /* for C4FM this is the median */
        raw_trace_valid[k] = 1;
        raw_trace_idx++;
    }

    return symbol;
}
