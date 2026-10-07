#include "dmr_listen.h"
#include <string.h>

void dmr_listen_init(dmr_listen_t *p)
{
    memset(p, 0, sizeof(*p));
    p->colour_code = -1;
}

bool dmr_listen_allow(dmr_listen_t *p, const char *text)
{
    if (!p || !text || strlen(text) >= sizeof(p->text)) return false;
    uint32_t parsed[DMR_ALLOW_MAX];
    unsigned count = 0;
    const char *s = text;
    while (*s) {
        while (*s == ' ' || *s == ',') ++s;
        if (!*s) break;
        uint32_t value = 0;
        if (*s < '0' || *s > '9' || count == DMR_ALLOW_MAX) return false;
        while (*s >= '0' && *s <= '9') {
            value = value * 10 + (unsigned)(*s++ - '0');
            if (value > 0xFFFFFFu) return false;
        }
        if (!value || (*s && *s != ' ' && *s != ',')) return false;
        parsed[count++] = value;
    }
    memcpy(p->allow, parsed, count * sizeof(*parsed));
    p->count = count;
    strcpy(p->text, text);
    return true;
}

bool dmr_listen_matches(const dmr_listen_t *p, unsigned slot, const dmr_call_t *c)
{
    if (!p || !c || slot < 1 || slot > 2 || !c->have_lc ||
        (p->slot && p->slot != slot) ||
        (p->colour_code >= 0 && p->colour_code != c->colour_code) ||
        c->lc.flco != 0 || (p->hold && p->hold != c->lc.destination)) return false;
    if (!p->count) return true;
    for (unsigned i = 0; i < p->count; ++i)
        if (p->allow[i] == c->lc.destination) return true;
    return false;
}

unsigned dmr_listen_select(const dmr_listen_t *p, const dmr_watch_t *w, int64_t now)
{
    if (!p || !w) return 0;
    /* A single candidate keeps the eventual vocoder bounded to one slot.
       Encrypted calls remain visible, but can never become an audio candidate. */
    for (unsigned i = 0; i < 2; ++i) {
        const dmr_call_t *c = &w->call[i];
        if (c->active && now >= c->last_us && now - c->last_us <= 1500000 &&
            !c->encrypted && dmr_listen_matches(p, i + 1, c)) return i + 1;
    }
    return 0;
}

uint32_t dmr_call_duration_ms(const dmr_call_t *c, int64_t now)
{
    if (!c || !c->have_lc) return 0;
    int64_t end = c->active ? now : c->end_us;
    if (c->active && end - c->last_us > 1500000) end = c->last_us;
    return end > c->start_us ? (uint32_t)((end - c->start_us) / 1000) : 0;
}
