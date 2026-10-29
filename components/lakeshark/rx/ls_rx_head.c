/* See ls_rx_head.h. */
#include "ls_rx_head.h"
#include "esp_timer.h"
#include <stdio.h>
#include <string.h>

/* One SHED line on the timeline per this long at most. */
#define SHED_LOG_US 5000000

static bool is_audio(const ls_rx_slot_t *s) { return s->p->needs.flags & LS_RX_F_AUDIO; }

void ls_rx_head_init(ls_rx_head_t *h, const ls_rx_stream_t *s)
{
    memset(h, 0, sizeof(*h));
    if (s) h->stream = *s;
    h->cpu_budget = LS_RX_CPU_BUDGET;
}

void ls_rx_head_set_audio(ls_rx_head_t *h, ls_rx_audio_fn fn, ls_rx_gate_fn gate, void *user)
{
    h->audio = fn; h->gate = gate; h->audio_user = user;
}

void ls_rx_head_set_tune(ls_rx_head_t *h, ls_rx_tune_fn fn, void *user)
{
    h->tune = fn; h->tune_user = user;
}

static int find(const ls_rx_head_t *h, const ls_rx_plugin_t *p)
{
    for (int i = 0; i < h->n; i++) if (h->slot[i].p == p) return i;
    return -1;
}

static int refuse(char *why, size_t n, const char *fmt, const char *a)
{
    if (why && n) snprintf(why, n, fmt, a);
    return -1;
}

int ls_rx_head_attach(ls_rx_head_t *h, const ls_rx_plugin_t *p, char *why, size_t n)
{
    if (!h || !p) return refuse(why, n, "%s", "no plugin");
    const int have = find(h, p);
    if (have >= 0) return have;
    if (p->abi != LS_RX_ABI || !p->process || !p->doc)
        return refuse(why, n, "%s: wrong ABI", p->id);
    uint32_t usable = p->needs.inputs & h->stream.inputs;
    if (p->needs.front != LS_RX_FRONT_NONE && p->needs.front != h->stream.front)
        usable &= ~(uint32_t)LS_RX_FRONT_INPUTS;
    /* An adapter that owns the radio reads it itself, not the stream. */
    if (!usable && !(p->needs.flags & LS_RX_F_OWNS_RADIO))
        return refuse(why, n, "%s: no input here", p->name);
    if (p->needs.freq_hi_hz && (h->stream.center_hz < p->needs.freq_lo_hz ||
                                h->stream.center_hz > p->needs.freq_hi_hz))
        return refuse(why, n, "%s: off band", p->name);
    if (p->needs.radios && !(p->needs.radios & (1u << h->stream.radio)))
        return refuse(why, n, "%s: wrong radio", p->name);
    if (h->n >= LS_RX_SLOTS) return refuse(why, n, "%s: head full", p->name);
    unsigned cpu = p->needs.cpu_permille;
    for (int i = 0; i < h->n; i++) {
        const uint16_t f = h->slot[i].p->needs.flags;
        if ((f | p->needs.flags) & LS_RX_F_EXCLUSIVE)
            return refuse(why, n, "%s: exclusive", p->name);
        if (f & p->needs.flags & LS_RX_F_AUDIO)
            return refuse(why, n, "%s: audio taken", p->name);
        if (f & p->needs.flags & LS_RX_F_TUNES)
            return refuse(why, n, "%s: tuner taken", p->name);
        cpu += h->slot[i].p->needs.cpu_permille;
    }
    if (cpu > h->cpu_budget) return refuse(why, n, "%s: no CPU left", p->name);

    void *st = NULL;
    if (p->attach) {
        char w[48] = "";
        h->attaching = p;
        st = p->attach(h, &h->stream, w, sizeof(w));
        h->attaching = NULL;
        if (!st) return refuse(why, n, "%s", w[0] ? w : "no memory");
    }
    /* Audio first: the speaker never waits behind a decoder. */
    int at = h->n;
    if (p->needs.flags & LS_RX_F_AUDIO) {
        memmove(&h->slot[1], &h->slot[0], sizeof(h->slot[0]) * (size_t)h->n);
        at = 0;
    }
    h->slot[at] = (ls_rx_slot_t){ .p = p, .st = st };
    h->n++;
    return at;
}

void ls_rx_head_detach(ls_rx_head_t *h, const ls_rx_plugin_t *p)
{
    const int i = find(h, p);
    if (i < 0) return;
    if (p->detach) p->detach(h->slot[i].st);
    memmove(&h->slot[i], &h->slot[i + 1], sizeof(h->slot[0]) * (size_t)(h->n - i - 1));
    h->n--;
    memset(&h->slot[h->n], 0, sizeof(h->slot[0]));
}

void ls_rx_head_detach_all(ls_rx_head_t *h)
{
    while (h->n) ls_rx_head_detach(h, h->slot[h->n - 1].p);
    h->shedding = false;
}

void *ls_rx_head_state(const ls_rx_head_t *h, const ls_rx_plugin_t *p)
{
    const int i = find(h, p);
    return i < 0 ? NULL : h->slot[i].st;
}

uint32_t ls_rx_head_wants(const ls_rx_head_t *h)
{
    uint32_t w = 0;
    for (int i = 0; i < h->n; i++) w |= h->slot[i].p->needs.inputs;
    return w & h->stream.inputs;
}

bool ls_rx_head_has_audio(const ls_rx_head_t *h)
{
    return h->n && is_audio(&h->slot[0]);
}

static void shed_data(ls_rx_head_t *h)
{
    const char *first = NULL;
    for (int i = 0; i < h->n; i++) {
        ls_rx_slot_t *s = &h->slot[i];
        if (is_audio(s)) continue;
        s->shed++;
        if (s->shedding) continue;
        s->shedding = true;
        if (s->p->reset) s->p->reset(s->st, LS_RX_RESET_GAP);
        if (!first) first = s->p->name;
    }
    if (h->shedding || !first) { h->shedding = true; return; }
    h->shedding = true;
    h->shed_episodes++;
    const int64_t now = esp_timer_get_time();
    if (h->shed_logged_us && now - h->shed_logged_us < SHED_LOG_US) return;
    h->shed_logged_us = now;
    ls_decoded_t ev = { .kind = LS_DECODED_TEXT, .at_us = now };
    snprintf(ev.from, sizeof(ev.from), "RX HEAD");
    snprintf(ev.text, sizeof(ev.text), "SHED %s", first);
    ls_rx_emit(h, &ev);
}

bool ls_rx_head_admit(ls_rx_head_t *h, bool spare)
{
    if (spare) {
        if (h->shedding) {
            h->shedding = false;
            for (int i = 0; i < h->n; i++) h->slot[i].shedding = false;
        }
        return true;
    }
    const bool voice = ls_rx_head_has_audio(h);
    const bool first = !h->shedding;
    if (h->n > (voice ? 1 : 0)) shed_data(h);
    if (voice && first && h->n > 1) return true;
    h->dropped++;
    return false;
}

void ls_rx_head_feed(ls_rx_head_t *h, const ls_rx_block_t *b)
{
    for (int i = 0; i < h->n; i++) {
        ls_rx_slot_t *s = &h->slot[i];
        if (!(s->p->needs.inputs & b->input) || s->shedding) continue;
        const int64_t t0 = esp_timer_get_time();
        s->p->process(s->st, b);
        const int64_t dt = esp_timer_get_time() - t0;
        const uint32_t us = dt > 0 ? (uint32_t)dt : 0;
        s->ema_us = s->blocks ? s->ema_us + ((int32_t)(us - s->ema_us) >> 4) : us;
        s->blocks++;
    }
}

void ls_rx_head_reset(ls_rx_head_t *h, ls_rx_reset_t why)
{
    for (int i = 0; i < h->n; i++)
        if (h->slot[i].p->reset) h->slot[i].p->reset(h->slot[i].st, why);
}

void ls_rx_head_retune(ls_rx_head_t *h, uint32_t hz)
{
    h->stream.center_hz = hz;
    for (int i = 0; i < h->n; i++) {
        const ls_rx_slot_t *s = &h->slot[i];
        if (s->p->retune) s->p->retune(s->st, hz);
        else if (s->p->reset) s->p->reset(s->st, LS_RX_RESET_RETUNE);
    }
}

void ls_rx_head_tick(ls_rx_head_t *h)
{
    for (int i = 0; i < h->n; i++)
        if (h->slot[i].p->tick) h->slot[i].p->tick(h->slot[i].st);
}

bool ls_rx_head_status(const ls_rx_head_t *h, int slot, ls_rx_status_t *out)
{
    memset(out, 0, sizeof(*out));
    if (slot < 0 || slot >= h->n) return false;
    const ls_rx_slot_t *s = &h->slot[slot];
    if (s->p->status) s->p->status(s->st, out);
    if (!out->chip[0]) snprintf(out->chip, sizeof(out->chip), "%s", s->p->name);
    out->shed = s->shedding;
    return true;
}

int ls_rx_head_chips(const ls_rx_head_t *h, char *out, size_t n)
{
    int k = 0;
    if (n) out[0] = 0;
    for (int i = 0; i < h->n && (size_t)k < n; i++)
        k += snprintf(out + k, n - (size_t)k, "%s%s%s", k ? " " : "",
                      h->slot[i].p->name, h->slot[i].shedding ? "(SHED)" : "");
    return k;
}

bool ls_rx_audio(ls_rx_head_t *h, const int16_t *pcm, int n, uint32_t rate_hz)
{
    if (!h->audio || n <= 0) return false;
    h->audio_blocks++;
    const bool dropped = h->audio(h->audio_user, pcm, n, rate_hz);
    if (dropped) h->audio_drops++;
    return dropped;
}

void ls_rx_audio_gate(ls_rx_head_t *h, bool open)
{
    if (h->gate) h->gate(h->audio_user, open);
}

bool ls_rx_request_tune(ls_rx_head_t *h, const ls_rx_plugin_t *p, uint32_t hz)
{
    if (!p || !(p->needs.flags & LS_RX_F_TUNES) || find(h, p) < 0 || !h->tune) return false;
    return h->tune(h->tune_user, hz);
}

void ls_rx_emit(ls_rx_head_t *h, const ls_decoded_t *d)
{
    if (h) h->emitted++;
    ls_decoded_publish(d);
}
