#include "p25_iq_capture.h"
#include <stdio.h>
#include <string.h>

static bool number(const char *s, uint32_t *value)
{
    uint32_t n = 0;
    if (!s || !*s) return false;
    for (; *s; ++s) {
        if (*s < '0' || *s > '9' || n > (UINT32_MAX - (*s - '0')) / 10)
            return false;
        n = n * 10 + (*s - '0');
    }
    *value = n;
    return true;
}

/* argv starts at the action, following "p25 capture". All payload reads
 * happen on the existing console stack, never on the radio owner. */
int p25_iq_capture_command(int argc, char **argv, uint32_t now_ms)
{
    p25_iq_capture_result_t error = P25_IQ_CAPTURE_ARGUMENT;
    if (argc == 1 && !strcmp(argv[0], "status")) {
        p25_iq_capture_status_t s;
        if (!p25_iq_capture_status(&s, now_ms)) error = P25_IQ_CAPTURE_BUSY;
        else {
            uint32_t gain_bits;
            memcpy(&gain_bits, &s.receiver.demod_gain, sizeof(gain_bits));
            printf("P25IQ state=%s reason=%s session=%lu requested=%lu captured=%lu "
                   "armed_ms=%lu first_ms=%lu last_ms=%lu format=u8-iq "
                   "frequency=%lu rate=%lu bandwidth=%lu generation=%lu "
                   "gain_tenths=%ld mode=%ld demod_gain_bits=%08lx\n",
                p25_iq_capture_phase_name(s.phase), p25_iq_capture_result_name(s.reason),
                (unsigned long)s.session, (unsigned long)s.requested_bytes,
                (unsigned long)s.captured_bytes, (unsigned long)s.armed_ms,
                (unsigned long)s.first_ms, (unsigned long)s.last_ms,
                (unsigned long)s.receiver.frequency_hz, (unsigned long)s.receiver.sample_rate_hz,
                (unsigned long)s.receiver.bandwidth_hz, (unsigned long)s.receiver.tune_generation,
                (long)s.receiver.gain_tenths, (long)s.receiver.demod_mode,
                (unsigned long)gain_bits);
            return 0;
        }
    } else if ((argc == 1 || argc == 2) && !strcmp(argv[0], "start")) {
        uint32_t blocks = P25_IQ_CAPTURE_DEFAULT_BLOCKS;
        if (argc == 1 || number(argv[1], &blocks))
            error = p25_iq_capture_start(blocks, now_ms);
    } else if (argc == 1 && !strcmp(argv[0], "cancel")) {
        error = p25_iq_capture_cancel();
    } else if (argc == 1 && !strcmp(argv[0], "free")) {
        error = p25_iq_capture_free();
    } else if ((argc == 2 || argc == 3) && !strcmp(argv[0], "read")) {
        uint32_t offset, count = P25_IQ_CAPTURE_READ_MAX;
        if (number(argv[1], &offset) && (argc == 2 || number(argv[2], &count)) &&
            count && count <= P25_IQ_CAPTURE_READ_MAX) {
            char hex[P25_IQ_CAPTURE_READ_MAX * 2 + 1];
            size_t size = 0;
            error = p25_iq_capture_read(offset, (uint8_t *)hex, count, &size, now_ms);
            if (error == P25_IQ_CAPTURE_OK) {
                static const char digits[] = "0123456789abcdef";
                for (size_t i = size; i-- > 0;) {
                    uint8_t byte = (uint8_t)hex[i];
                    hex[2*i] = digits[byte >> 4];
                    hex[2*i+1] = digits[byte & 15];
                }
                hex[2*size] = 0;
                printf("P25IQ offset=%lu bytes=%u hex=%s\n", (unsigned long)offset,
                       (unsigned)size, hex);
                return 0;
            }
        }
    }
    printf("P25IQ result=%s\n", p25_iq_capture_result_name(error));
    return error == P25_IQ_CAPTURE_OK ? 0 : 1;
}
