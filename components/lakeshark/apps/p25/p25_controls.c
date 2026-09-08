#include "p25_controls.h"

#include <stdio.h>

bool p25_controls_parse_mhz(const char *text, uint32_t *out_hz)
{
    if (!text || !out_hz || !*text) return false;

    const char *p = text;
    uint32_t whole = 0;
    unsigned whole_digits = 0;
    while (*p >= '0' && *p <= '9') {
        whole_digits++;
        whole = whole * 10u + (uint32_t)(*p - '0');
        if (whole > P25_CONTROL_TUNER_MAX_HZ / 1000000UL) return false;
        p++;
    }
    if (whole_digits == 0) return false;

    uint32_t fraction = 0;
    unsigned fraction_digits = 0;
    if (*p == '.') {
        p++;
        while (*p >= '0' && *p <= '9') {
            if (fraction_digits >= 6) return false;
            fraction = fraction * 10u + (uint32_t)(*p - '0');
            fraction_digits++;
            p++;
        }
        if (fraction_digits == 0) return false;
    }
    if (*p != '\0') return false;

    while (fraction_digits < 6) {
        fraction *= 10u;
        fraction_digits++;
    }

    uint64_t hz = (uint64_t)whole * 1000000ULL + fraction;
    if (hz < P25_CONTROL_TUNER_MIN_HZ || hz > P25_CONTROL_TUNER_MAX_HZ)
        return false;
    *out_hz = (uint32_t)hz;
    return true;
}

void p25_controls_format_mhz(char *out, size_t out_size, uint32_t hz)
{
    if (!out || out_size == 0) return;
    snprintf(out, out_size, "%lu.%06lu MHz",
             (unsigned long)(hz / 1000000UL),
             (unsigned long)(hz % 1000000UL));
}

uint32_t p25_controls_clamp_encrypted_skip_ms(uint32_t ms)
{
    if (ms < P25_CONTROL_ENCRYPTED_SKIP_MIN_MS)
        return P25_CONTROL_ENCRYPTED_SKIP_MIN_MS;
    if (ms > P25_CONTROL_ENCRYPTED_SKIP_MAX_MS)
        return P25_CONTROL_ENCRYPTED_SKIP_MAX_MS;
    return ms;
}
