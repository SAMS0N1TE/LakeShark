#ifndef P25_HEALTH_H
#define P25_HEALTH_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define P25_HEALTH_IDENTITY_STALE_MS 10000U
#define P25_HEALTH_NEIGHBOR_MAX 4U
#define P25_HEALTH_OPCODE_COUNT 64U

typedef enum {
    P25_HEALTH_ID_UNKNOWN = 0,
    P25_HEALTH_ID_CURRENT,
    P25_HEALTH_ID_STALE,
    P25_HEALTH_ID_INVALID,
} p25_health_identity_state_t;

typedef enum {
    P25_HEALTH_ACQ_MANUAL = 0,
    P25_HEALTH_ACQ_HUNT,
    P25_HEALTH_ACQ_LOCKED,
    P25_HEALTH_ACQ_NO_LOCK,
} p25_health_acquisition_t;

typedef struct {
    uint8_t  rfss;
    uint8_t  site;
    uint16_t sysid;
    uint16_t channel;
    uint64_t frequency_hz;
} p25_health_neighbor_t;

/* The RX owner assembles this bounded input once per second. Generations are
 * incremented only by a CRC-valid identity/control TSBK, so unrelated control
 * traffic cannot make retained identity look fresh after a retune. */
typedef struct {
    uint32_t net_generation;
    uint32_t rfss_generation;
    uint32_t sccb_generation;
    uint32_t neighbor_generation;
    bool     net_valid;
    bool     rfss_valid;
    bool     sccb_valid;
    uint32_t wacn;
    uint16_t sysid;
    uint16_t rfss_sysid;
    uint8_t  rfss;
    uint8_t  site;
    uint16_t control_channel;
    uint64_t control_hz;
    uint8_t  sccb_rfss;
    uint8_t  sccb_site;
    uint16_t sccb_channel_1;
    uint16_t sccb_channel_2;
    uint8_t  neighbor_count;
    p25_health_neighbor_t neighbors[P25_HEALTH_NEIGHBOR_MAX];

    uint32_t nid_valid;
    uint32_t nid_invalid;
    uint32_t tsbk_valid;
    uint32_t tsbk_invalid;
    uint32_t tsbk_vendor;
    uint16_t unhandled[P25_HEALTH_OPCODE_COUNT];

    char     effective_demod[16];
    p25_health_acquisition_t acquisition;
    uint32_t c4fm_nids;
    uint32_t c4fm_tsbks;
    uint32_t cqpsk_nids;
    uint32_t cqpsk_tsbks;
    uint32_t reacquires;
    bool     timing_error_valid;
    float    timing_error_samples;
    bool     carrier_error_valid;
    float    carrier_error_hz;

    uint16_t rf_level_permille;
    uint32_t usb_read_errors;
    uint64_t usb_dropped_bytes;
    uint16_t ring_fill;
    uint16_t ring_size;
    uint32_t audio_drops;
    uint32_t audio_underruns;
    bool     buffers_ok;
    bool     cpu_valid;
    uint8_t  cpu_core0_pct;
    uint8_t  cpu_core1_pct;
    uint32_t heap_internal_free;
    uint32_t heap_internal_largest;
    uint32_t heap_psram_free;

    uint32_t phase2_grants;
    uint16_t phase2_talkgroup;
    uint64_t phase2_frequency_hz;
    uint8_t  phase2_slot;
    uint8_t  phase2_slots_per_carrier;
} p25_health_raw_t;

typedef struct {
    uint32_t sequence;
    uint32_t captured_ms;
    uint32_t net_seen_ms;
    uint32_t rfss_seen_ms;
    uint32_t sccb_seen_ms;
    uint32_t neighbor_seen_ms;
    bool     net_valid;
    bool     rfss_valid;
    bool     sccb_valid;
    uint32_t wacn;
    uint16_t sysid;
    uint16_t rfss_sysid;
    uint8_t  rfss;
    uint8_t  site;
    uint16_t control_channel;
    uint64_t control_hz;
    uint8_t  sccb_rfss;
    uint8_t  sccb_site;
    uint16_t sccb_channel_1;
    uint16_t sccb_channel_2;
    uint8_t  neighbor_count;
    p25_health_neighbor_t neighbors[P25_HEALTH_NEIGHBOR_MAX];

    uint32_t nid_valid;
    uint32_t nid_invalid;
    uint32_t tsbk_valid;
    uint32_t tsbk_invalid;
    uint32_t tsbk_vendor;
    uint32_t unhandled_total;
    uint8_t  unhandled_distinct;
    uint16_t unhandled[P25_HEALTH_OPCODE_COUNT];

    char     effective_demod[16];
    p25_health_acquisition_t acquisition;
    uint32_t c4fm_nids;
    uint32_t c4fm_tsbks;
    uint32_t cqpsk_nids;
    uint32_t cqpsk_tsbks;
    uint32_t reacquires;
    bool     timing_error_valid;
    float    timing_error_samples;
    bool     carrier_error_valid;
    float    carrier_error_hz;

    uint16_t rf_level_permille;
    uint32_t usb_read_errors;
    uint64_t usb_dropped_bytes;
    uint16_t ring_fill;
    uint16_t ring_size;
    uint32_t audio_drops;
    uint32_t audio_underruns;
    bool     buffers_ok;
    bool     cpu_valid;
    uint8_t  cpu_core0_pct;
    uint8_t  cpu_core1_pct;
    uint32_t heap_internal_free;
    uint32_t heap_internal_largest;
    uint32_t heap_psram_free;

    uint32_t phase2_grants;
    uint16_t phase2_talkgroup;
    uint64_t phase2_frequency_hz;
    uint8_t  phase2_slot;
    uint8_t  phase2_slots_per_carrier;
} p25_health_snapshot_t;

void p25_health_init(void);
void p25_health_publish(const p25_health_raw_t *raw, uint32_t now_ms);
bool p25_health_read(p25_health_snapshot_t *out);
void p25_health_reset_counters(void);

p25_health_identity_state_t p25_health_identity_state(
    const p25_health_snapshot_t *snapshot, uint32_t now_ms);
size_t p25_health_format_identity(const p25_health_snapshot_t *snapshot,
                                  uint32_t now_ms, char *out,
                                  size_t out_size);
size_t p25_health_format_signal(const p25_health_snapshot_t *snapshot,
                                char *out, size_t out_size);

#ifdef __cplusplus
}
#endif

#endif
