/* BUGHUNT's device names: the ids.bin reader (see bh_ids.h). */
#include "bh_ids.h"
#include <string.h>

static uint32_t le(const uint8_t *p, int n)
{
    uint32_t v = 0;
    for (int i = n - 1; i >= 0; i--) v = (v << 8) | p[i];
    return v;
}
int bhid_head(bhid_head_t *h, const uint8_t *b, uint32_t n)
{
    memset(h, 0, sizeof(*h));
    if (n < 32 || memcmp(b, "BHID", 4) || le(b + 4, 2) != 1) return 0;
    const uint32_t nt = le(b + 6, 2);
    if (!nt || nt > BHID_TABLES || n < 32 + 16 * nt) return 0;
    h->pool_off = le(b + 8, 4);
    h->pool_len = le(b + 12, 4);
    h->built = le(b + 16, 4);
    for (uint32_t i = 0; i < nt; i++) {
        const uint8_t *d = b + 32 + 16 * i;
        bhid_tab_t *t = &h->t[h->nt];
        t->kind = d[0];
        t->keyb = d[1];
        t->every = (uint16_t)le(d + 2, 2);
        t->count = le(d + 4, 4);
        t->rec_off = le(d + 8, 4);
        t->idx_off = le(d + 12, 4);
        if (t->keyb < 1 || t->keyb > 4 || !t->every || !t->count) continue;
        t->recb = (uint8_t)(t->keyb + 3);
        t->nidx = (uint32_t)(((uint64_t)t->count + t->every - 1) / t->every);
        /* the app allocates nidx * keyb and the search reads that far: a
           damaged file whose product wraps would leave a tiny buffer */
        if ((uint64_t)t->nidx * t->keyb > BHID_IDX_MAX) continue;
        h->nt++;
    }
    return h->nt > 0;
}
bhid_tab_t *bhid_table(bhid_head_t *h, int kind)
{
    for (int i = 0; i < h->nt; i++) if (h->t[i].kind == kind) return &h->t[i];
    return NULL;
}
bool bhid_block(const bhid_tab_t *t, uint32_t key, uint32_t *first, uint32_t *n)
{
    if (!t->idx || !t->nidx) return false;
    /* the last index key at or under key */
    uint32_t lo = 0, hi = t->nidx;
    while (hi - lo > 1) {
        const uint32_t mid = (lo + hi) / 2;
        if (le(t->idx + mid * t->keyb, t->keyb) <= key) lo = mid; else hi = mid;
    }
    if (le(t->idx + lo * t->keyb, t->keyb) > key) return false;     /* under the first key */
    *first = lo * t->every;
    *n = t->count - *first < t->every ? t->count - *first : t->every;
    return true;
}
long bhid_in_block(const bhid_tab_t *t, const uint8_t *recs, uint32_t n, uint32_t key)
{
    uint32_t lo = 0, hi = n;
    while (lo < hi) {
        const uint32_t mid = (lo + hi) / 2;
        const uint32_t k = le(recs + mid * t->recb, t->keyb);
        if (k == key) return (long)le(recs + mid * t->recb + t->keyb, 3);
        if (k < key) lo = mid + 1; else hi = mid;
    }
    return -1;
}
void bhid_name_fix(char *s, size_t cap)
{
    if (!cap) return;
    s[cap - 1] = 0;
    for (size_t i = 0; s[i]; i++) if ((unsigned char)s[i] < 32 || (unsigned char)s[i] > 126) s[i] = '?';
}
static bool word_in(const char *hay, const char *w)
{
    const size_t n = strlen(w);
    for (const char *p = hay; *p; p++) {
        size_t i = 0;
        while (i < n && p[i] && (p[i] | 32) == (w[i] | 32)) i++;
        if (i == n) {
            const bool before = p == hay || !((p[-1] | 32) >= 'a' && (p[-1] | 32) <= 'z');
            const bool after = !((p[n] | 32) >= 'a' && (p[n] | 32) <= 'z');
            if (before && after) return true;
        }
    }
    return false;
}
bool bhid_camera_maker(const char *name)
{
    static const char *const CAM[] = {
        "Hikvision", "Dahua", "Wyze", "Reolink", "Amcrest", "Arlo", "Ring", "Blink", "Lorex", "Swann", "Uniview",
        "Hanwha Vision", "Vivotek", "Ezviz", "Zmodo", "Mobotix", "Avigilon", "Verkada", "GeoVision", "Tiandy", "Sunell",
        "Raysharp", "Milesight", "Axis Communications",
    };
    if (!name || !name[0]) return false;
    for (size_t i = 0; i < sizeof(CAM) / sizeof(CAM[0]); i++) if (word_in(name, CAM[i])) return true;
    return false;
}
