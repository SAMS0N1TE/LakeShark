/* See ls_ota_core.h. */
#include "ls_ota_core.h"

#include <ctype.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_LINE 400

static int hexval(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/* n hex digits to n/2 bytes; false on an odd count, a stray character or no room. */
static bool unhex(const char *s, size_t n, uint8_t *out, size_t cap, size_t *got)
{
    if (n % 2 || n / 2 > cap) return false;
    for (size_t i = 0; i < n; i += 2) {
        const int hi = hexval(s[i]), lo = hexval(s[i + 1]);
        if (hi < 0 || lo < 0) return false;
        out[i / 2] = (uint8_t)(hi << 4 | lo);
    }
    if (got) *got = n / 2;
    return true;
}

static bool put(char *dst, size_t cap, const char *v, size_t n)
{
    if (n >= cap) return false;
    memcpy(dst, v, n);
    dst[n] = 0;
    return true;
}

/* X.Y.Z and an optional -rcN. Whatever follows (-g<hash>, -dirty) is the
   build, not the release. */
static bool release_of(const char *s, long part[4])
{
    char *end;
    for (int i = 0; i < 3; i++) {
        if (!isdigit((unsigned char)*s)) return false;
        part[i] = strtol(s, &end, 10);
        s = end;
        if (i < 2) {
            if (*s != '.') return false;
            s++;
        }
    }
    part[3] = LONG_MAX;                     /* a release sorts above its rcs */
    if (!strncmp(s, "-rc", 3) && isdigit((unsigned char)s[3]))
        part[3] = strtol(s + 3, NULL, 10);
    return true;
}

int ls_ota_version_cmp(const char *a, const char *b)
{
    long x[4], y[4];
    const bool ok_a = a && release_of(a, x), ok_b = b && release_of(b, y);
    if (!ok_a || !ok_b) return (int)ok_a - (int)ok_b;
    for (int i = 0; i < 4; i++)
        if (x[i] != y[i]) return x[i] < y[i] ? -1 : 1;
    return 0;
}

ls_ota_verdict_t ls_ota_judge(const char *running_build, const ls_ota_manifest_t *m)
{
    const int c = ls_ota_version_cmp(m->version, running_build);
    if (c > 0) return LS_OTA_NEWER;
    if (c < 0) return LS_OTA_OLDER;
    return strcmp(m->build, running_build ? running_build : "") ? LS_OTA_OTHER_BUILD : LS_OTA_CURRENT;
}

bool ls_ota_resolve(const char *base, const char *ref, char *out, size_t cap)
{
    if (!ref || !*ref || !out || !cap) return false;
    const char *colon = strstr(ref, "://");
    const char *slash = strchr(ref, '/');
    int n;
    if (colon && (!slash || colon < slash)) {
        n = snprintf(out, cap, "%s", ref);
    } else {
        const char *scheme = base ? strstr(base, "://") : NULL;
        if (!scheme) return false;
        const char *path = strchr(scheme + 3, '/');
        size_t keep;
        const char *sep = "";
        if (ref[0] == '/') {
            keep = path ? (size_t)(path - base) : strlen(base);
        } else if (!path) {
            keep = strlen(base);
            sep = "/";
        } else {
            keep = (size_t)(strrchr(path, '/') - base) + 1;
        }
        n = snprintf(out, cap, "%.*s%s%s", (int)keep, base, sep, ref);
    }
    return n > 0 && (size_t)n < cap;
}

bool ls_ota_other_host(const char *url, char *out, size_t cap)
{
    static const char HTTPS[] = "https://";
    if (!url || !out || !cap || strncmp(url, HTTPS, sizeof(HTTPS) - 1)) return false;
    const char *host = url + sizeof(HTTPS) - 1;
    const size_t hl = strcspn(host, "/?#");
    const char *to;
    if (hl == strlen(LS_OTA_HOST_PRIMARY) && !memcmp(host, LS_OTA_HOST_PRIMARY, hl)) to = LS_OTA_HOST_FALLBACK;
    else if (hl == strlen(LS_OTA_HOST_FALLBACK) && !memcmp(host, LS_OTA_HOST_FALLBACK, hl)) to = LS_OTA_HOST_PRIMARY;
    else return false;
    const int n = snprintf(out, cap, "%s%s%s", HTTPS, to, host + hl);
    return n > 0 && (size_t)n < cap;
}

bool ls_ota_verify_any(size_t nkeys, ls_ota_key_fn verify, void *ctx)
{
    for (size_t i = 0; verify && i < nkeys; i++)
        if (verify(i, ctx)) return true;
    return false;
}

bool ls_ota_channel_of(const char *manifest_url, char *out, size_t cap)
{
    if (!manifest_url || !out || !cap) return false;
    const char *scheme = strstr(manifest_url, "://");
    const char *path = scheme ? strchr(scheme + 3, '/') : NULL;
    if (!path) return false;
    size_t end = strcspn(path, "?#");
    const char *name = path;
    for (const char *p = path; p < path + end; p++)
        if (*p == '/') name = p + 1;
    size_t n = (size_t)(path + end - name);
    if (n >= 4 && !memcmp(name + n - 4, ".txt", 4)) n -= 4;
    if (!n || n >= cap) return false;
    memcpy(out, name, n);
    out[n] = 0;
    return true;
}

bool ls_ota_channel_ok(const ls_ota_manifest_t *m, const char *manifest_url)
{
    char want[sizeof(m->channel)];
    return m->channel[0] && ls_ota_channel_of(manifest_url, want, sizeof(want))
           && !strcmp(m->channel, want);
}

const char *ls_ota_err_text(ls_ota_err_t e)
{
    switch (e) {
    case LS_OTA_OK:          return "ok";
    case LS_OTA_ERR_MAGIC:   return "not an update manifest";
    case LS_OTA_ERR_LINE:    return "bad line";
    case LS_OTA_ERR_MISSING: return "missing field";
    case LS_OTA_ERR_VALUE:   return "bad value";
    case LS_OTA_ERR_URL:     return "image not on https";
    case LS_OTA_ERR_SIG:     return "no signature";
    }
    return "?";
}

ls_ota_err_t ls_ota_parse(const char *text, size_t len, const char *manifest_url,
                          ls_ota_manifest_t *out)
{
    memset(out, 0, sizeof(*out));
    const size_t ml = strlen(LS_OTA_MAGIC);
    if (!text || len > LS_OTA_MAX_TEXT || len < ml + 1 || memcmp(text, LS_OTA_MAGIC, ml))
        return LS_OTA_ERR_MAGIC;
    size_t pos = ml;
    if (text[pos] == '\r') pos++;
    if (pos >= len || text[pos] != '\n') return LS_OTA_ERR_MAGIC;
    pos++;

    enum { K_BOARD, K_VERSION, K_BUILD, K_SIZE, K_SHA, K_URL, K_NOTES, K_CHANNEL, K_N };
    static const char *const KEYS[K_N] = {
        "board", "version", "build", "size", "sha256", "url", "notes", "channel" };
    bool seen[K_N] = { false };
    char url[sizeof(out->url)] = "";

    while (pos < len) {
        const char *line = text + pos;
        const char *nl = memchr(line, '\n', len - pos);
        size_t n = nl ? (size_t)(nl - line) : len - pos;
        const size_t next = pos + n + (nl ? 1 : 0);
        if (n && line[n - 1] == '\r') n--;
        if (n == 0) { pos = next; continue; }
        if (n > MAX_LINE) return LS_OTA_ERR_LINE;

        /* The signature covers every byte before its own line, and nothing
           may follow it: text after it would be read but not signed. */
        if (n >= 4 && !memcmp(line, "sig=", 4)) {
            out->signed_len = pos;
            if (!unhex(line + 4, n - 4, out->sig, sizeof(out->sig), &out->sig_len) || out->sig_len < 8)
                return LS_OTA_ERR_SIG;
            for (size_t i = next; i < len; i++)
                if (!isspace((unsigned char)text[i])) return LS_OTA_ERR_SIG;
            break;
        }
        const char *eq = memchr(line, '=', n);
        if (!eq || eq == line) return LS_OTA_ERR_LINE;
        const size_t kl = (size_t)(eq - line), vl = n - kl - 1;
        const char *v = eq + 1;
        int k = -1;
        for (int i = 0; i < K_N; i++)
            if (strlen(KEYS[i]) == kl && !memcmp(line, KEYS[i], kl)) k = i;
        if (k >= 0) {
            if (seen[k]) return LS_OTA_ERR_VALUE;
            seen[k] = true;
            bool ok = true;
            long rel[4];
            switch (k) {
            case K_BOARD:   ok = vl && put(out->board, sizeof(out->board), v, vl); break;
            case K_VERSION: ok = put(out->version, sizeof(out->version), v, vl)
                                 && release_of(out->version, rel); break;
            case K_BUILD:   ok = vl && put(out->build, sizeof(out->build), v, vl); break;
            case K_SIZE: {
                char num[12];
                ok = vl && put(num, sizeof(num), v, vl);
                for (size_t i = 0; ok && i < vl; i++) ok = isdigit((unsigned char)num[i]);
                const unsigned long s = ok ? strtoul(num, NULL, 10) : 0;
                ok = ok && s > 0 && s <= LS_OTA_MAX_SIZE;
                out->size = (uint32_t)s;
                break;
            }
            case K_SHA:     ok = vl == 64 && unhex(v, 64, out->sha256, sizeof(out->sha256), NULL); break;
            case K_URL:     ok = vl && put(url, sizeof(url), v, vl); break;
            case K_NOTES:   ok = put(out->notes, sizeof(out->notes), v, vl); break;
            case K_CHANNEL:
                ok = vl && put(out->channel, sizeof(out->channel), v, vl);
                for (size_t i = 0; ok && i < vl; i++)
                    ok = isalnum((unsigned char)v[i]) || v[i] == '-' || v[i] == '_';
                break;
            }
            if (!ok) return LS_OTA_ERR_VALUE;
        }
        pos = next;
    }
    if (!out->sig_len) return LS_OTA_ERR_SIG;
    for (int i = 0; i < K_NOTES; i++)               /* notes and channel may be left out */
        if (!seen[i]) return LS_OTA_ERR_MISSING;
    if (!ls_ota_resolve(manifest_url, url, out->url, sizeof(out->url))
        || strncmp(out->url, "https://", 8))
        return LS_OTA_ERR_URL;
    return LS_OTA_OK;
}

/* ------------------------------------------------- the background check */

int64_t ls_ota_bg_jitter_s(uint32_t seed)
{
    return LS_OTA_BG_FIRST_MIN_S + (int64_t)(seed % (LS_OTA_BG_FIRST_SPAN_S + 1));
}

int64_t ls_ota_bg_next_s(const ls_ota_bg_t *s)
{
    if (!s || !s->enabled || s->wifi_up_s < 0) return -1;
    const int64_t due = s->last_run_s < 0 ? s->wifi_up_s + ls_ota_bg_jitter_s(s->seed)
                                          : s->last_run_s + LS_OTA_BG_EVERY_S;
    /* A deferral at or after the due time moves the next try ten minutes on
       from it. One from before it (an earlier cycle) says nothing. */
    if (s->last_defer_s >= due) return s->last_defer_s + LS_OTA_BG_RETRY_S;
    return due;
}

unsigned ls_ota_bg_blockers(bool job_busy, bool rf_busy, bool wifi_up,
                            size_t internal_free, size_t dma_free)
{
    unsigned w = 0;
    if (job_busy) w |= LS_OTA_BG_BUSY_JOB;
    if (rf_busy)  w |= LS_OTA_BG_BUSY_RF;
    if (internal_free < LS_OTA_BG_MIN_INTERNAL || dma_free < LS_OTA_BG_MIN_DMA) w |= LS_OTA_BG_LOW_HEAP;
    if (!wifi_up) w |= LS_OTA_BG_NO_WIFI;
    return w;
}

ls_ota_bg_act_t ls_ota_bg_act(const ls_ota_bg_t *s, int64_t now_s, unsigned blockers)
{
    const int64_t next = ls_ota_bg_next_s(s);
    if (next < 0 || now_s < next) return LS_OTA_BG_WAIT;
    return blockers ? LS_OTA_BG_DEFER : LS_OTA_BG_RUN;
}

/* ------------------------------------------------------------ the notice */

bool ls_ota_notice_new(const char *newer, const char *announced)
{
    return newer && newer[0] && (!announced || strcmp(newer, announced) != 0);
}

bool ls_ota_toast_text(const char *version, char *out, size_t cap)
{
    if (!version || !version[0] || !out || !cap) return false;
    const int n = snprintf(out, cap, "%s available: SYSTEM > UPDATE", version);
    return n > 0 && (size_t)n < cap;
}
