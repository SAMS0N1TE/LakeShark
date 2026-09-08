#include "p25_cqpsk_controls.h"

#include <errno.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

void p25_cqpsk_config_defaults(p25_cqpsk_config_t *config)
{
    if (!config) return;
    config->timing_gain = P25_CQPSK_TIMING_GAIN_DEFAULT;
    config->carrier_gain = P25_CQPSK_CARRIER_GAIN_DEFAULT;
}

static bool gain_valid(float value, float minimum, float maximum)
{
    return isfinite(value) && value >= minimum && value <= maximum;
}

bool p25_cqpsk_config_valid(const p25_cqpsk_config_t *config)
{
    return config &&
           gain_valid(config->timing_gain, P25_CQPSK_TIMING_GAIN_MIN,
                      P25_CQPSK_TIMING_GAIN_MAX) &&
           gain_valid(config->carrier_gain, P25_CQPSK_CARRIER_GAIN_MIN,
                      P25_CQPSK_CARRIER_GAIN_MAX);
}

bool p25_cqpsk_gain_parse(const char *text, float minimum, float maximum,
                          float *value)
{
    if (!text || !*text || !value || !gain_valid(minimum, 0.0f, maximum))
        return false;
    bool digit = false, exponent = false, exponent_digit = false;
    for (const char *p = text; *p; ++p) {
        if (*p >= '0' && *p <= '9') {
            digit = true;
            if (exponent) exponent_digit = true;
        } else if (*p == '.' && !exponent) {
            if (strchr(p + 1, '.')) return false;
        } else if ((*p == 'e' || *p == 'E') && digit && !exponent) {
            exponent = true;
            if (p[1] == '+' || p[1] == '-') ++p;
            if (p[1] == '\0') return false;
        } else {
            return false;
        }
    }
    if (!digit || (exponent && !exponent_digit)) return false;
    errno = 0;
    char *end = NULL;
    float parsed = strtof(text, &end);
    if (end == text || *end != '\0' || errno == ERANGE ||
        !gain_valid(parsed, minimum, maximum))
        return false;
    *value = parsed;
    return true;
}

bool p25_cqpsk_gain_encode(float value, float minimum, float maximum,
                           uint32_t *raw)
{
    if (!raw || !gain_valid(value, minimum, maximum)) return false;
    double scaled = (double)value * (double)P25_CQPSK_GAIN_SCALE;
    if (!isfinite(scaled) || scaled < 0.0 || scaled > (double)UINT32_MAX)
        return false;
    *raw = (uint32_t)(scaled + 0.5);
    return true;
}

bool p25_cqpsk_gain_decode(uint32_t raw, float minimum, float maximum,
                           float *value)
{
    if (!value) return false;
    float decoded = (float)((double)raw / (double)P25_CQPSK_GAIN_SCALE);
    if (!gain_valid(decoded, minimum, maximum)) return false;
    *value = decoded;
    return true;
}

bool p25_cqpsk_config_pack(const p25_cqpsk_config_t *config, uint64_t *packed)
{
    uint32_t timing = 0, carrier = 0;
    if (!packed || !config ||
        !p25_cqpsk_gain_encode(config->timing_gain,
                               P25_CQPSK_TIMING_GAIN_MIN,
                               P25_CQPSK_TIMING_GAIN_MAX, &timing) ||
        !p25_cqpsk_gain_encode(config->carrier_gain,
                               P25_CQPSK_CARRIER_GAIN_MIN,
                               P25_CQPSK_CARRIER_GAIN_MAX, &carrier))
        return false;
    *packed = ((uint64_t)timing << 32) | carrier;
    return true;
}

bool p25_cqpsk_config_unpack(uint64_t packed, p25_cqpsk_config_t *config)
{
    p25_cqpsk_config_t candidate;
    if (!config ||
        !p25_cqpsk_gain_decode((uint32_t)(packed >> 32),
                               P25_CQPSK_TIMING_GAIN_MIN,
                               P25_CQPSK_TIMING_GAIN_MAX,
                               &candidate.timing_gain) ||
        !p25_cqpsk_gain_decode((uint32_t)packed,
                               P25_CQPSK_CARRIER_GAIN_MIN,
                               P25_CQPSK_CARRIER_GAIN_MAX,
                               &candidate.carrier_gain))
        return false;
    *config = candidate;
    return true;
}

static void control_lock(p25_cqpsk_control_t *control)
{
    while (__atomic_test_and_set(&control->writer_lock, __ATOMIC_ACQUIRE)) {
    }
}

static void control_unlock(p25_cqpsk_control_t *control)
{
    __atomic_clear(&control->writer_lock, __ATOMIC_RELEASE);
}

void p25_cqpsk_control_init(p25_cqpsk_control_t *control,
                            const p25_cqpsk_config_t *initial)
{
    if (!control) return;
    p25_cqpsk_config_t safe;
    if (p25_cqpsk_config_valid(initial)) safe = *initial;
    else p25_cqpsk_config_defaults(&safe);
    memset(control, 0, sizeof(*control));
    control->requested = safe;
    control->effective = safe;
}

bool p25_cqpsk_control_request(p25_cqpsk_control_t *control,
                              const p25_cqpsk_config_t *config)
{
    if (!control || !p25_cqpsk_config_valid(config)) return false;
    control_lock(control);
    control->requested = *config;
    control->request_generation++;
    __atomic_store_n(&control->pending, true, __ATOMIC_RELEASE);
    control_unlock(control);
    return true;
}

bool p25_cqpsk_control_take(p25_cqpsk_control_t *control,
                           p25_cqpsk_config_t *config,
                           uint32_t *generation)
{
    if (!control || !config) return false;
    control_lock(control);
    bool have = __atomic_exchange_n(&control->pending, false, __ATOMIC_ACQUIRE);
    if (have) {
        *config = control->requested;
        if (generation) *generation = control->request_generation;
    }
    control_unlock(control);
    return have;
}

void p25_cqpsk_control_applied(p25_cqpsk_control_t *control,
                               const p25_cqpsk_config_t *config,
                               uint32_t generation)
{
    if (!control || !p25_cqpsk_config_valid(config)) return;
    control_lock(control);
    control->effective = *config;
    control->effective_generation = generation;
    control_unlock(control);
}

void p25_cqpsk_control_status(p25_cqpsk_control_t *control,
                              p25_cqpsk_config_t *requested,
                              p25_cqpsk_config_t *effective,
                              bool *pending)
{
    if (!control) return;
    control_lock(control);
    if (requested) *requested = control->requested;
    if (effective) *effective = control->effective;
    if (pending) *pending = control->pending;
    control_unlock(control);
}
