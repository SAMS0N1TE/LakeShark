#ifndef REC_CATCH_CORE_H
#define REC_CATCH_CORE_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
#define CATCH_CONTINUOUS_IQ 1
#define CATCH_BINS 512
#define CATCH_TRACKS 16
#define CATCH_PEAKS 16
#define CATCH_PEAK_WINDOW_US 10000000u
#define CATCH_RECENT_SECONDS 11
#define CATCH_COARSE 32
#define CATCH_DC_BINS 10
#define CATCH_HANDOFF_WARM_US 3000000u
#define CATCH_FULL_PER_SECOND 100u
#define CATCH_STRONG_DBFS (-20.0f)
#define CATCH_RATE 2400000u
#define CATCH_MARGIN_DB 12.0f
#define CATCH_MIN_US 2000u
#define CATCH_MAX_US 250000u
#define CATCH_MERGE_US 10000u
/* Bound silence/blind gaps since the last positive coarse observation. */
#define CATCH_OBSERVATION_US 50000u
#define CATCH_QUIET_BLOCKS 2u
#define CATCH_WARM_US 250000u
#define CATCH_SPUR_GROUPS (CATCH_BINS/4)
#define CATCH_SPUR_TOP 8
#define CATCH_HOLD_US 5000000u
#define CATCH_SILENT_US CATCH_HOLD_US
typedef struct {
    uint64_t first_us, last_us, edge_us;
    float level, hits, looks;
    uint16_t repeats;
    uint32_t period_us;
    bool on, learned, comb;
    uint16_t bin, low_bin, high_bin;
} catch_spur_t;
typedef struct { uint32_t frequency; float dbfs; bool comb; } catch_spur_info_t;
typedef enum { CATCH_OOK, CATCH_FSK } catch_mod_t;
typedef struct {
    uint32_t id, frequency, bandwidth, deviation;
    uint64_t start_us, detected_us, stop_us;
    float rtl_dbfs;
    catch_mod_t modulation;
    bool spur;
    uint16_t repeats;
} catch_burst_t;
typedef void (*catch_event_fn)(const catch_burst_t *, bool start, void *);
typedef enum { CATCH_ACCEPTED, CATCH_TOO_SHORT, CATCH_TOO_LONG, CATCH_TOO_WIDE,
    CATCH_UNDER_MARGIN, CATCH_MASKED, CATCH_MERGED, CATCH_SPUR, CATCH_CANDIDATE,
    CATCH_REASON_COUNT } catch_reason_t;
typedef struct {
    uint64_t first_us, last_us, peak_us;
    uint32_t frequency;
    float snr_db, dbfs;
    catch_reason_t reason;
} catch_peak_t;
typedef struct {
    uint64_t block_us;
    uint32_t id, frequency;
    float coarse_db;
    unsigned bin;
    const char *fft, *state, *reason;
} catch_trace_t;
typedef void (*catch_trace_fn)(const catch_trace_t *, void *);
const char *catch_reason_name(catch_reason_t);
typedef struct {
    catch_spur_t spur[CATCH_SPUR_GROUPS];
    int32_t coarse_re[CATCH_COARSE], coarse_im[CATCH_COARSE];
    int16_t coarse_tw_re[CATCH_COARSE/2], coarse_tw_im[CATCH_COARSE/2], coarse_window[CATCH_COARSE];
    float coarse_floor[CATCH_COARSE], coarse_spur[CATCH_COARSE];
    float coarse_level[CATCH_COARSE];
    /* Candidate intervals start before frequency refinement is available. */
    uint64_t coarse_first[CATCH_COARSE], coarse_last[CATCH_COARSE];
    uint64_t block_us;
    bool iq_mode;
    catch_trace_fn trace; void *trace_context;
    const char *fft_state;
    uint64_t (*clock_us)(void *); void *clock_context;
    uint64_t checks_us, fft_us, spectrum_us, budget_second;
    uint64_t expensive_ready_us;
    unsigned second_frames;
    catch_peak_t peaks[CATCH_PEAKS], recent[CATCH_RECENT_SECONDS];
    uint32_t reasons[CATCH_REASON_COUNT], peak_count, coarse_frames;
    uint64_t candidate_us[CATCH_BINS];
    uint64_t clips, last_background_us;
    float iq_rms;
    bool probe, clip_active;
    float floor[CATCH_BINS], re[CATCH_BINS], im[CATCH_BINS];
    float window[CATCH_BINS], tw_re[CATCH_BINS/2], tw_im[CATCH_BINS/2];
    float db[CATCH_BINS];
    struct { catch_burst_t burst; uint64_t last_us, on_us; uint32_t frame, low_hz, high_hz; unsigned quiet; bool used, sent, rejected; } track[CATCH_TRACKS];
    uint32_t center, next_id, frames, dropped;
    unsigned fill, skip, power_samples, phase, allowance, candidate_limit;
    unsigned chunk_clips, fft_clips; uint32_t clip_floor;
    uint32_t power_sum, power_floor;
    int32_t power_re, power_im;
    uint64_t first_us, previous_us;
    float reported_floor;
    bool half; uint8_t first;
} catch_detector_t;
void catch_detector_init(catch_detector_t *, uint32_t center);
/* FFT-shifted dBFS bins. Events publish only after duration qualification. */
void catch_detector_spectrum(catch_detector_t *, const float *, uint64_t,
                             catch_event_fn, void *);
/* Fragment boundaries (including an odd byte) are preserved. No allocation. */
void catch_detector_iq(catch_detector_t *, const uint8_t *, size_t,
                       uint64_t end_us, catch_event_fn, void *);
/* Clear incomplete windows/tracks after a transport gap. Keep learned floors. */
void catch_detector_gap(catch_detector_t *);
/* Full FFT allowance per transport block. Cheap checks are never budgeted out. */
void catch_detector_allow(catch_detector_t *, unsigned full_frames);
bool catch_detector_is_spur(const catch_detector_t *, uint32_t hz, float dbfs);
unsigned catch_detector_spurs(const catch_detector_t *, catch_spur_info_t *, unsigned capacity);

typedef struct { uint32_t frequency; uint64_t until_us, activity_us; float dbfs; bool listening, repeated; } catch_hold_t;
typedef struct {
    struct { uint32_t frequency, duration; uint64_t last_us; uint16_t repeats; } source[32];
} catch_sources_t;
/* Observe completed bursts once, after overlap merging. No radio calls. */
bool catch_source_observe(catch_sources_t *, catch_burst_t *, uint32_t center,
                          uint64_t elapsed_us);
bool catch_handoff_expired(const catch_hold_t *, uint64_t now);
bool catch_handoff_prefer(const catch_burst_t *, const catch_burst_t *);
typedef struct {
    uint64_t (*now)(void *);
    bool (*begin)(void *, const catch_burst_t *);
    void (*end)(void *);
    void *context;
} catch_handoff_ops_t;
/* Measures actual begin completion, never an assumed retune time. */
bool catch_handoff(catch_hold_t *, const catch_burst_t *,
                   const catch_handoff_ops_t *, uint32_t *latency_us);
void catch_handoff_stop(catch_hold_t *, const catch_handoff_ops_t *);
#ifdef __cplusplus
}
#endif
#endif
