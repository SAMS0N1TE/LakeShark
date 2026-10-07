#ifndef LS_DMR_LISTEN_H
#define LS_DMR_LISTEN_H
#include "dmr_watch.h"
#define DMR_ALLOW_MAX 16
typedef struct {
    unsigned slot; /* 0 both, 1 or 2 */
    int colour_code; /* -1 any */
    uint32_t hold;
    unsigned count;
    uint32_t allow[DMR_ALLOW_MAX];
    char text[128];
} dmr_listen_t;
void dmr_listen_init(dmr_listen_t *p);
bool dmr_listen_allow(dmr_listen_t *p, const char *text);
bool dmr_listen_matches(const dmr_listen_t *p, unsigned slot, const dmr_call_t *c);
unsigned dmr_listen_select(const dmr_listen_t *p, const dmr_watch_t *w, int64_t now);
uint32_t dmr_call_duration_ms(const dmr_call_t *c, int64_t now);
#endif
