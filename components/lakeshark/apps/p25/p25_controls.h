#ifndef P25_CONTROLS_H
#define P25_CONTROLS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* LS-687: one set of limits is shared by the P25 entry parser, backend and
 * radio request. The previous dialog accepted 1..2000 MHz even though the
 * P25 RTL session advertises 24..1766 MHz, and atof() accepted partial input
 * such as "154.7.8". Keep the parser integer-only so every accepted value is
 * an exact Hz value, including 6.25 kHz channel centres. */
#define P25_CONTROL_TUNER_MIN_HZ  24000000UL
#define P25_CONTROL_TUNER_MAX_HZ 1766000000UL

#define P25_CONTROL_ENCRYPTED_SKIP_DEFAULT_MS   30000UL
#define P25_CONTROL_ENCRYPTED_SKIP_MIN_MS        1000UL
#define P25_CONTROL_ENCRYPTED_SKIP_MAX_MS     3600000UL

bool p25_controls_parse_mhz(const char *text, uint32_t *out_hz);
void p25_controls_format_mhz(char *out, size_t out_size, uint32_t hz);
uint32_t p25_controls_clamp_encrypted_skip_ms(uint32_t ms);

#ifdef __cplusplus
}
#endif

#endif
