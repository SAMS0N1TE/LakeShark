#ifndef P25_IQ_CAPTURE_H
#define P25_IQ_CAPTURE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define P25_IQ_CAPTURE_BLOCK_BYTES 16384u
#define P25_IQ_CAPTURE_DEFAULT_BLOCKS 32u
#define P25_IQ_CAPTURE_MAX_BLOCKS 256u
#define P25_IQ_CAPTURE_READ_MAX 256u
#define P25_IQ_CAPTURE_TIMEOUT_MS 20000u

typedef enum {
    P25_IQ_CAPTURE_EMPTY, P25_IQ_CAPTURE_ARMED, P25_IQ_CAPTURE_ACTIVE,
    P25_IQ_CAPTURE_DONE, P25_IQ_CAPTURE_ABORTED, P25_IQ_CAPTURE_FAILED,
} p25_iq_capture_phase_t;

typedef enum {
    P25_IQ_CAPTURE_OK, P25_IQ_CAPTURE_BUSY, P25_IQ_CAPTURE_ARGUMENT,
    P25_IQ_CAPTURE_NO_MEMORY, P25_IQ_CAPTURE_NOT_RUNNING,
    P25_IQ_CAPTURE_CANCELLED, P25_IQ_CAPTURE_SESSION_CHANGED,
    P25_IQ_CAPTURE_TUNE_CHANGED, P25_IQ_CAPTURE_CONFIG_CHANGED,
    P25_IQ_CAPTURE_TIMEOUT, P25_IQ_CAPTURE_GAP,
} p25_iq_capture_result_t;

typedef struct {
    uint32_t frequency_hz;
    uint32_t sample_rate_hz;
    uint32_t bandwidth_hz;
    uint32_t tune_generation;
    int32_t gain_tenths;
    int32_t demod_mode;
    float demod_gain;
    uint32_t now_ms;
} p25_iq_capture_meta_t;

typedef struct {
    p25_iq_capture_phase_t phase;
    p25_iq_capture_result_t reason;
    uint32_t session;
    uint32_t requested_bytes;
    uint32_t captured_bytes;
    uint32_t armed_ms;
    uint32_t first_ms;
    uint32_t last_ms;
    p25_iq_capture_meta_t receiver;
} p25_iq_capture_status_t;

/* Only RX announces session boundaries and feeds owned input blocks. */
void p25_iq_capture_receiver(bool running);
bool p25_iq_capture_collecting(void);
int p25_iq_capture_command(int argc, char **argv, uint32_t now_ms);
void p25_iq_capture_interrupt(p25_iq_capture_result_t reason);
void p25_iq_capture_feed(const uint8_t *iq, size_t bytes,
                         const p25_iq_capture_meta_t *meta);

/* Console-side APIs never wait for RX. BUSY means retry; no pending free
 * touches a writer's buffer. Completed/aborted bytes remain until free. */
p25_iq_capture_result_t p25_iq_capture_start(unsigned blocks, uint32_t now_ms);
p25_iq_capture_result_t p25_iq_capture_cancel(void);
p25_iq_capture_result_t p25_iq_capture_free(void);
bool p25_iq_capture_status(p25_iq_capture_status_t *out, uint32_t now_ms);
p25_iq_capture_result_t p25_iq_capture_read(uint32_t offset, uint8_t *out,
    size_t capacity, size_t *read_bytes, uint32_t now_ms);
const char *p25_iq_capture_phase_name(p25_iq_capture_phase_t phase);
const char *p25_iq_capture_result_name(p25_iq_capture_result_t result);

#ifdef __cplusplus
}
#endif
#endif
