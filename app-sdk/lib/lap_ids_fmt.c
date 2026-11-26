/* Bluetooth and MAC names: the pure part of lap_ids (see lap_ids.h). No card,
   no lap calls, no allocation: the app engine (lap_ids.c) and the firmware's
   name service (components/apps/tui/ls_ble_names.c) both build on it. */
#include "lap_ids.h"
#include "lap_ids_fmt.h"
#include <stdio.h>
#include <string.h>

/* ---- the format ---------------------------------------------------------- */

static uint32_t le(const uint8_t *p, int n)
{
    uint32_t v = 0;
    for (int i = n - 1; i >= 0; i--) v = (v << 8) | p[i];
    return v;
}

int lap_ids_head(lap_ids_head_t *h, const uint8_t *b, uint32_t n)
{
    memset(h, 0, sizeof(*h));
    if (n < 32 || memcmp(b, "BHID", 4) || le(b + 4, 2) != 1) return 0;
    const uint32_t nt = le(b + 6, 2);
    if (!nt || nt > LAP_IDS_TABLES || n < 32 + 16 * nt) return 0;
    h->pool_off = le(b + 8, 4);
    h->pool_len = le(b + 12, 4);
    h->built = le(b + 16, 4);
    for (uint32_t i = 0; i < nt; i++) {
        const uint8_t *d = b + 32 + 16 * i;
        lap_ids_tab_t *t = &h->t[h->nt];
        t->kind = d[0];
        t->keyb = d[1];
        t->every = (uint16_t)le(d + 2, 2);
        t->count = le(d + 4, 4);
        t->rec_off = le(d + 8, 4);
        t->idx_off = le(d + 12, 4);
        if (t->keyb < 1 || t->keyb > 4 || !t->every || !t->count) continue;
        t->recb = (uint8_t)(t->keyb + 3);
        t->nidx = (uint32_t)(((uint64_t)t->count + t->every - 1) / t->every);
        /* the index is read into a buffer of nidx * keyb: a damaged file whose
           product wraps would leave a tiny one */
        if ((uint64_t)t->nidx * t->keyb > LAP_IDS_IDX_MAX) continue;
        t->idx = NULL;
        h->nt++;
    }
    return h->nt > 0;
}

lap_ids_tab_t *lap_ids_table(lap_ids_head_t *h, int kind)
{
    for (int i = 0; i < h->nt; i++) if (h->t[i].kind == kind) return &h->t[i];
    return NULL;
}

bool lap_ids_block(const lap_ids_tab_t *t, uint32_t key, uint32_t *first, uint32_t *n)
{
    if (!t->idx || !t->nidx) return false;
    uint32_t lo = 0, hi = t->nidx;      /* the last index key at or under key */
    while (hi - lo > 1) {
        const uint32_t mid = (lo + hi) / 2;
        if (le(t->idx + mid * t->keyb, t->keyb) <= key) lo = mid; else hi = mid;
    }
    if (le(t->idx + lo * t->keyb, t->keyb) > key) return false;
    *first = lo * t->every;
    *n = t->count - *first < t->every ? t->count - *first : t->every;
    return true;
}

long lap_ids_in_block(const lap_ids_tab_t *t, const uint8_t *recs, uint32_t n, uint32_t key)
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

void lap_ids_name_fix(char *s, size_t cap)
{
    if (!cap) return;
    s[cap - 1] = 0;
    for (size_t i = 0; s[i]; i++) if ((unsigned char)s[i] < 32 || (unsigned char)s[i] > 126) s[i] = '?';
}

/* ---- UUIDs --------------------------------------------------------------- */

static const uint8_t SIG_BASE[12] = {0x00, 0x00, 0x10, 0x00, 0x80, 0x00, 0x00, 0x80, 0x5f, 0x9b, 0x34, 0xfb};

static int hexv(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    c = (char)(c | 32);
    return c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1;
}

void lap_ids_uuid16_be(uint16_t u, uint8_t be[16])
{
    be[0] = be[1] = 0;
    be[2] = (uint8_t)(u >> 8);
    be[3] = (uint8_t)u;
    memcpy(be + 4, SIG_BASE, 12);
}

int lap_ids_uuid_short(const uint8_t be[16])
{
    if (be[0] || be[1] || memcmp(be + 4, SIG_BASE, 12)) return -1;
    return be[2] << 8 | be[3];
}

bool lap_ids_parse_uuid(const char *s, uint8_t be[16], int *bits)
{
    if (!s) return false;
    while (*s == ' ' || *s == '\t') s++;
    if (s[0] == '0' && (s[1] | 32) == 'x') s += 2;
    uint8_t nib[32];
    int n = 0;
    for (; *s && *s != ' ' && *s != '\t' && *s != '\r' && *s != '\n'; s++) {
        if (*s == '-') continue;
        const int v = hexv(*s);
        if (v < 0 || n == 32) return false;
        nib[n++] = (uint8_t)v;
    }
    if (n == 4 || n == 8) {
        uint32_t v = 0;
        for (int i = 0; i < n; i++) v = v << 4 | nib[i];
        be[0] = (uint8_t)(v >> 24); be[1] = (uint8_t)(v >> 16); be[2] = (uint8_t)(v >> 8); be[3] = (uint8_t)v;
        memcpy(be + 4, SIG_BASE, 12);
    } else if (n == 32) {
        for (int i = 0; i < 16; i++) be[i] = (uint8_t)(nib[2 * i] << 4 | nib[2 * i + 1]);
    } else return false;
    if (bits) *bits = lap_ids_uuid_short(be) >= 0 ? 16 : 128;
    return true;
}

uint32_t lap_ids_uuid_key(const uint8_t be[16])
{
    uint32_t h = 2166136261u;
    for (int i = 0; i < 16; i++) { h ^= be[i]; h *= 16777619u; }
    return h;
}

void lap_ids_uuid_text(const uint8_t be[16], char *out, size_t cap)
{
    const int s = lap_ids_uuid_short(be);
    if (s >= 0) { snprintf(out, cap, "%04x", s); return; }
    snprintf(out, cap, "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
             be[0], be[1], be[2], be[3], be[4], be[5], be[6], be[7], be[8], be[9], be[10], be[11], be[12], be[13], be[14],
             be[15]);
}

/* ---- user lines ---------------------------------------------------------- */

static const struct { const char *word; uint8_t kind; } WORDS[] = {
    {"company", LAP_IDS_COMPANY}, {"service", LAP_IDS_SERVICE}, {"uuid", LAP_IDS_SERVICE},
    {"char", LAP_IDS_CHAR}, {"characteristic", LAP_IDS_CHAR}, {"desc", LAP_IDS_DESC}, {"descriptor", LAP_IDS_DESC},
    {"maker", LAP_IDS_OUI}, {"oui", LAP_IDS_OUI}, {"appearance", LAP_IDS_APPEARANCE}, {"device", LAP_IDS_DEVICE},
};

const char *lap_ids_kind_word(int kind)
{
    if (kind == LAP_IDS_UUID128) return "uuid";
    for (size_t i = 0; i < sizeof(WORDS) / sizeof(WORDS[0]); i++) if (WORDS[i].kind == kind) return WORDS[i].word;
    return "?";
}

static bool word_is(const char *a, size_t n, const char *w)
{
    if (strlen(w) != n) return false;
    for (size_t i = 0; i < n; i++) if ((a[i] | 32) != w[i]) return false;
    return true;
}

/* Hex bytes with optional ':' '-' between: up to max bytes, exactly want when want > 0. */
static int mac_bytes(const char *s, size_t n, uint8_t *out, int max)
{
    int got = 0, half = -1;
    for (size_t i = 0; i < n; i++) {
        if (s[i] == ':' || s[i] == '-') { if (half >= 0) return -1; continue; }
        const int v = hexv(s[i]);
        if (v < 0) return -1;
        if (half < 0) half = v;
        else { if (got == max) return -1; out[got++] = (uint8_t)(half << 4 | v); half = -1; }
    }
    return half >= 0 ? -1 : got;
}

static bool number(const char *s, size_t n, uint32_t *out)
{
    if (!n || n > 10) return false;
    uint32_t v = 0;
    if (n > 2 && s[0] == '0' && (s[1] | 32) == 'x') {
        for (size_t i = 2; i < n; i++) { const int d = hexv(s[i]); if (d < 0) return false; v = v << 4 | (uint32_t)d; }
        *out = v;
        return true;
    }
    bool dec = true;
    for (size_t i = 0; i < n; i++) if (s[i] < '0' || s[i] > '9') dec = false;
    if (dec) { for (size_t i = 0; i < n; i++) v = v * 10 + (uint32_t)(s[i] - '0'); *out = v; return true; }
    for (size_t i = 0; i < n; i++) { const int d = hexv(s[i]); if (d < 0) return false; v = v << 4 | (uint32_t)d; }
    *out = v;
    return true;
}

int lap_ids_parse_line(const char *line, size_t len, lap_ids_user_t *out)
{
    memset(out, 0, sizeof(*out));
    char b[192];
    if (len >= sizeof(b)) len = sizeof(b) - 1;
    memcpy(b, line, len);
    b[len] = 0;
    for (char *h = b; *h; h++) if (*h == '#' && (h == b || h[-1] == ' ' || h[-1] == '\t')) { *h = 0; break; }
    char *p = b;
    while (*p == ' ' || *p == '\t') p++;
    size_t end = strlen(p);
    while (end && (p[end - 1] == ' ' || p[end - 1] == '\t' || p[end - 1] == '\r' || p[end - 1] == '\n')) p[--end] = 0;
    if (!*p) return 0;
    /* kind word */
    const char *kw = p;
    while (*p && *p != ' ' && *p != '\t') p++;
    const size_t kn = (size_t)(p - kw);
    int kind = 0;
    for (size_t i = 0; i < sizeof(WORDS) / sizeof(WORDS[0]); i++) if (word_is(kw, kn, WORDS[i].word)) kind = WORDS[i].kind;
    if (!kind) return -1;
    while (*p == ' ' || *p == '\t') p++;
    /* key */
    const char *key = p;
    while (*p && *p != ' ' && *p != '\t') p++;
    const size_t keyn = (size_t)(p - key);
    if (!keyn) return -1;
    while (*p == ' ' || *p == '\t') p++;
    if (!*p) return -1;                 /* a key with no name */
    char kt[64];
    if (keyn >= sizeof(kt)) return -1;
    memcpy(kt, key, keyn);
    kt[keyn] = 0;
    switch (kind) {
    case LAP_IDS_COMPANY: case LAP_IDS_APPEARANCE: {
        uint32_t v;
        if (!number(kt, keyn, &v) || v > 0xFFFF) return -1;
        out->key = v;
        break;
    }
    case LAP_IDS_SERVICE: case LAP_IDS_CHAR: case LAP_IDS_DESC: {
        uint8_t be[16];
        int bits;
        if (!lap_ids_parse_uuid(kt, be, &bits)) return -1;
        if (bits == 16) out->key = (uint32_t)lap_ids_uuid_short(be);
        else { kind = LAP_IDS_UUID128; out->key = lap_ids_uuid_key(be); }
        break;
    }
    case LAP_IDS_OUI: {
        uint8_t m[3];
        if (mac_bytes(kt, keyn, m, 3) != 3) return -1;
        out->key = (uint32_t)m[0] << 16 | (uint32_t)m[1] << 8 | m[2];
        break;
    }
    case LAP_IDS_DEVICE:
        if (mac_bytes(kt, keyn, out->mac, 6) != 6) return -1;
        break;
    }
    out->kind = (uint8_t)kind;
    snprintf(out->name, sizeof(out->name), "%s", p);
    lap_ids_name_fix(out->name, sizeof(out->name));
    return 1;
}

/* ---- remembered device lines --------------------------------------------- */

int lap_ids_mac_bytes(const char *s, size_t n, uint8_t *out, int max) { return mac_bytes(s, n, out, max); }

void lap_ids_mac_text(const uint8_t m[6], char *out, size_t cap)
{
    snprintf(out, cap, "%02x:%02x:%02x:%02x:%02x:%02x", m[0], m[1], m[2], m[3], m[4], m[5]);
}

bool lap_ids_device_line(const char *l, size_t len, uint8_t mac[6], char *name, size_t cap)
{
    /* "aa:bb:cc:dd:ee:ff name" */
    if (!cap || len <= 18 || l[17] != ' ' || mac_bytes(l, 17, mac, 6) != 6) return false;
    size_t k = len - 18 < cap - 1 ? len - 18 : cap - 1;
    memcpy(name, l + 18, k);
    name[k] = 0;
    while (k && (name[k - 1] == '\r' || name[k - 1] == ' ')) name[--k] = 0;
    lap_ids_name_fix(name, cap);
    return name[0] != 0;
}

bool lap_ids_keepable(const uint8_t be[6], uint8_t type)
{
    return type == 0 || (be[0] >> 6) == 3;
}

bool lap_ids_heard_name(const char *in, char *out, size_t cap)
{
    if (!cap) return false;
    out[0] = 0;
    if (!in) return false;
    while (*in == ' ') in++;
    snprintf(out, cap, "%s", in);
    size_t k = strlen(out);
    while (k && out[k - 1] == ' ') out[--k] = 0;
    lap_ids_name_fix(out, cap);
    return out[0] != 0;
}
