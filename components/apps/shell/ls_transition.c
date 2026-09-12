#include "ls_transition.h"

int ls_transition_dependencies_stopped(int radio_status, bool app_stopped)
{
    if (radio_status <= 0) return radio_status;
    return app_stopped ? radio_status : 0;
}

bool ls_transition_memory_admit(size_t free_8bit, size_t largest_8bit,
                                size_t largest_dma)
{
    /* LCD failure evidence had 151 DMA bytes and an 84-byte largest
     * block. Keep a modest 256-byte contiguous control/render floor; this is
     * not a speculative whole-app reservation and does not hide later OOM. */
    return free_8bit >= 1024 && largest_8bit >= 256 && largest_dma >= 256;
}

void ls_transition_init(ls_transition_t *s)
{
    *s = (ls_transition_t){.phase = LS_TRANS_IDLE, .current = -1, .target = -1};
}

void ls_transition_request(ls_transition_t *s, int target)
{
    if (s->phase == LS_TRANS_IDLE && s->current == target) return;
    s->target = target;
    if (s->phase == LS_TRANS_IDLE || s->phase == LS_TRANS_FAILED)
        s->phase = LS_TRANS_STOP;
}

/* every destructive step occurs on a later GUI tick, never inside
 * the outgoing object's click callback. A stop acknowledgement is required
 * before releasing its object tree; failure retains it for diagnosis/retry. */
void ls_transition_tick(ls_transition_t *s, const ls_transition_hooks_t *h,
                        uint32_t now, uint32_t timeout_ms)
{
    switch (s->phase) {
    case LS_TRANS_STOP:
        s->started = now;
        s->token = h->stop(h->ctx);
        s->phase = s->token ? LS_TRANS_WAIT : LS_TRANS_FAILED;
        break;
    case LS_TRANS_WAIT: {
        int status = h->stopped(h->ctx, s->token);
        if (status > 0) s->phase = LS_TRANS_UNLOAD;
        else if (status < 0 || (uint32_t)(now - s->started) >= timeout_ms)
            s->phase = LS_TRANS_FAILED;
        break;
    }
    case LS_TRANS_UNLOAD:
        if (!h->unload(h->ctx, s->current)) s->phase = LS_TRANS_FAILED;
        else { s->current = -1; s->phase = LS_TRANS_BUILD; }
        break;
    case LS_TRANS_BUILD: {
        int target = s->target;
        if (!h->build(h->ctx, target)) { s->phase = LS_TRANS_FAILED; break; }
        s->current = target;
        s->phase = s->target == target ? LS_TRANS_IDLE : LS_TRANS_STOP;
        break;
    }
    default: break;
    }
}
