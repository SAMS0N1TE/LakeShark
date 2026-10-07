#ifndef LS_SAME_H
#define LS_SAME_H
#include <stdbool.h>
#include <stdint.h>
#define SAME_HEADER_MAX 268
#define SAME_LOCATIONS 31
#define SAME_HISTORY 16
#define SAME_SAMPLE_RATE 16000
typedef struct {
    char header[SAME_HEADER_MAX], originator[4], event[4], sender[9];
    uint32_t locations[SAME_LOCATIONS]; /* PSSCCC, including county subdivision. */
    uint8_t location_count;
    uint16_t valid_minutes, day;
    uint8_t hour, minute;
    bool test, ended;
} same_alert_t;
typedef struct {
    uint32_t fips[6]; /* SSCCC; zero clears a slot. */
    bool only_mine, include_tests, log_sd;
} same_options_t;
typedef struct same_ctx same_ctx_t;
typedef void (*same_callback_t)(const same_alert_t *, void *);
bool same_parse(const char *header, same_alert_t *out);
bool same_matches(const same_alert_t *a, const same_options_t *o);
const char *same_event_name(const char *code);
same_ctx_t *same_create(same_callback_t callback, void *user);
void same_destroy(same_ctx_t *s);
void same_reset(same_ctx_t *s);
/* Feed mono audio at SAME_SAMPLE_RATE before volume or squelch gating. */
void same_process(same_ctx_t *s, const int16_t *pcm, int n);
/* Complete bursts also enter here so framing and voting can be tested alone. */
void same_burst(same_ctx_t *s, const char *header);
#endif
