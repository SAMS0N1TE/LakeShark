#ifndef REC_CATCH_H
#define REC_CATCH_H
#include "rec_catch_core.h"
#include "rec_catch_chan.h"
#include "radio_endpoint.h"
#include "../../../apps/tui/experiments/lr433_dec.h"
#ifdef __cplusplus
extern "C" {
#endif
#define CATCH_RESULTS 16
typedef struct {
    catch_burst_t burst;
    float lr_rssi;
    uint32_t latency_us, captures;
    int edges;
    bool decoded;
    lr433_msg_t message;
    char decode_status[64];
} catch_result_t;
typedef struct {
    bool running, stopping;
    uint32_t center, detections, captures, decoded, dropped, handoffs, max_latency_us;
    uint32_t blocks, block_drops, detector_drops, fft_frames;
    uint32_t ignored;               /* bursts outside every chosen window */
    uint64_t usb_drop_bytes, active_us, elapsed_us;
    uint64_t processed_bytes, lost_samples, deficit_samples;
    uint64_t last_block_us, block_period_us, block_gap_us, max_block_gap_us;
    uint64_t read_us, checks_us, fft_us, spectrum_us, scan_us;
    uint32_t preemptions;
    float floor_dbfs, threshold_dbfs, iq_rms;
    uint32_t sample_rate, actual_center, coarse_frames, full_allowance, over_budget;
    int gain_mode, gain_tenths_db;
    uint64_t clips;
    uint32_t reasons[CATCH_REASON_COUNT], peak_count;
    catch_peak_t peaks[CATCH_PEAKS], recent[CATCH_RECENT_SECONDS];
    uint32_t spur_count;
    catch_spur_info_t spurs[CATCH_SPUR_GROUPS];
    int count;
    char rtl_status[64], lr_status[64], save_status[112];
} catch_status_t;
ls_radio_err_t rec_catch_start(uint32_t center);
void rec_catch_stop(void);
void rec_catch_status(catch_status_t *);
bool rec_catch_result(int newest_index, catch_result_t *);
bool rec_catch_save(int newest_index);
int rec_catch_command(int argc, char **argv);
#ifdef __cplusplus
}
#endif
#endif
