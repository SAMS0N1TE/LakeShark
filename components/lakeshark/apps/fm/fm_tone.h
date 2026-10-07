#ifndef FM_TONE_H
#define FM_TONE_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

#define FM_CTCSS_COUNT 50
#define FM_DCS_COUNT 104
#define FM_TONE_CHOICES (1 + FM_CTCSS_COUNT + 2 * FM_DCS_COUNT)
/* Selection: 0 off, 1..50 CTCSS, then normal and inverted DCS. */
extern const uint16_t fm_ctcss_tenths[FM_CTCSS_COUNT];
extern const uint16_t fm_dcs_codes[FM_DCS_COUNT];
typedef struct {
    uint16_t selection;
    float confidence;
} fm_tone_result_t;
typedef struct {
    float low[4], dc, sum;
    unsigned decim, pos, filled, hop, ticks;
    float ring[300], coeff[FM_CTCSS_COUNT];
    struct {
        unsigned phase, bits, last_tick[2];
        uint32_t word;
        uint16_t candidate[2];
    } dcs[8];
    unsigned seen;
    bool carrier;
    fm_tone_result_t result;
} fm_tone_t;
void fm_tone_init(fm_tone_t *s);
/* Feed 32 kHz discriminator audio before voice filtering or de-emphasis. */
void fm_tone_process(fm_tone_t *s, const float *audio, int n);
bool fm_tone_receive(fm_tone_t *s, const float *audio, int n, bool carrier, uint16_t required);
bool fm_tone_matches(uint16_t required, fm_tone_result_t result);
void fm_tone_label(uint16_t selection, char *out, size_t n);
uint32_t fm_dcs_word(uint16_t code);
#ifdef __cplusplus
}
#endif
#endif
