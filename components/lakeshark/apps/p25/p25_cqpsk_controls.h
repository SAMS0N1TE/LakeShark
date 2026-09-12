/* bounded CQPSK loop controls in the units used by recovered679.
 * timing_gain multiplies the Gardner error before changing the estimated
 * samples/symbol; carrier_gain multiplies residual differential phase before
 * changing the phase-step estimate.  These are not Costas-loop bandwidths. */
#ifndef P25_CQPSK_CONTROLS_H
#define P25_CQPSK_CONTROLS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define P25_CQPSK_TIMING_GAIN_DEFAULT  0.00015625f
#define P25_CQPSK_TIMING_GAIN_MIN      0.0000390625f
#define P25_CQPSK_TIMING_GAIN_MAX      0.000625f
#define P25_CQPSK_CARRIER_GAIN_DEFAULT 0.01f
#define P25_CQPSK_CARRIER_GAIN_MIN     0.0025f
#define P25_CQPSK_CARRIER_GAIN_MAX     0.04f

#define P25_CQPSK_GAIN_SCALE 2560000000U

typedef struct {
    float timing_gain;
    float carrier_gain;
} p25_cqpsk_config_t;

void p25_cqpsk_config_defaults(p25_cqpsk_config_t *config);
bool p25_cqpsk_config_valid(const p25_cqpsk_config_t *config);
bool p25_cqpsk_gain_parse(const char *text, float minimum, float maximum,
                          float *value);
bool p25_cqpsk_gain_encode(float value, float minimum, float maximum,
                           uint32_t *raw);
bool p25_cqpsk_gain_decode(uint32_t raw, float minimum, float maximum,
                           float *value);
bool p25_cqpsk_config_pack(const p25_cqpsk_config_t *config, uint64_t *packed);
bool p25_cqpsk_config_unpack(uint64_t packed, p25_cqpsk_config_t *config);

/* A latest-value command latch between UI/profile workers and the RX task.
 * Only take() removes a request; firmware calls it between IQ blocks. */
typedef struct {
    volatile bool writer_lock;
    volatile bool pending;
    p25_cqpsk_config_t requested;
    p25_cqpsk_config_t effective;
    volatile uint32_t request_generation;
    volatile uint32_t effective_generation;
} p25_cqpsk_control_t;

void p25_cqpsk_control_init(p25_cqpsk_control_t *control,
                            const p25_cqpsk_config_t *initial);
bool p25_cqpsk_control_request(p25_cqpsk_control_t *control,
                              const p25_cqpsk_config_t *config);
bool p25_cqpsk_control_take(p25_cqpsk_control_t *control,
                           p25_cqpsk_config_t *config,
                           uint32_t *generation);
void p25_cqpsk_control_applied(p25_cqpsk_control_t *control,
                               const p25_cqpsk_config_t *config,
                               uint32_t generation);
void p25_cqpsk_control_status(p25_cqpsk_control_t *control,
                              p25_cqpsk_config_t *requested,
                              p25_cqpsk_config_t *effective,
                              bool *pending);

#ifdef __cplusplus
}
#endif

#endif
