#ifndef LS_TRANSITION_H
#define LS_TRANSITION_H
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#ifdef __cplusplus
extern "C" {
#endif
typedef enum { LS_TRANS_IDLE, LS_TRANS_STOP, LS_TRANS_WAIT, LS_TRANS_UNLOAD,
               LS_TRANS_BUILD, LS_TRANS_FAILED } ls_transition_phase_t;
typedef struct {
    ls_transition_phase_t phase;
    int current, target;
    uint32_t started, token;
} ls_transition_t;
typedef struct {
    uint32_t (*stop)(void *);
    int (*stopped)(void *, uint32_t);
    bool (*unload)(void *, int);
    bool (*build)(void *, int);
    void *ctx;
} ls_transition_hooks_t;
void ls_transition_init(ls_transition_t *s);
int ls_transition_dependencies_stopped(int radio_status, bool app_stopped);
/* Minimal construction floor, not a prediction of an entire app's heap.
 * Reject a fragmented DMA pool before drawing/USB control paths assert. */
bool ls_transition_memory_admit(size_t free_8bit, size_t largest_8bit,
                                size_t largest_dma);
void ls_transition_request(ls_transition_t *s, int target);
void ls_transition_tick(ls_transition_t *s, const ls_transition_hooks_t *h,
                        uint32_t now, uint32_t timeout_ms);
#ifdef __cplusplus
}
#endif
#endif
