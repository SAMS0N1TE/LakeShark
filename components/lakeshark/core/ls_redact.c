#include "ls_redact.h"
#include <ctype.h>
#include <stdint.h>
#include <string.h>
#include <strings.h>

static const char *const s_words[] = { "password", "passwd", "pass", "psk", "pwd", "pin", "token", "secret",
                                       "apikey", "api_key", "key", "ssid", "auth", "bearer", "authorization" };

static bool wordc(char c) { return isalnum((unsigned char)c) || c == '_'; }
static bool hexc(char c) { return isxdigit((unsigned char)c) != 0; }
static bool valend(char c) { return !c || isspace((unsigned char)c) || c == '"' || c == '\'' || c == ',' || c == ';' || c == '&' || c == '}' || c == ')'; }

/* A keyword at s[i] (boundary before it already checked): length of the keyword part including a
   `_suffix`, or 0. */
static size_t keyword_at(const char *s, size_t i)
{
    for (size_t k = 0; k < sizeof(s_words) / sizeof(s_words[0]); k++) {
        const size_t n = strlen(s_words[k]);
        if (strncasecmp(s + i, s_words[k], n)) continue;
        size_t e = i + n;
        if (s[e] == '_') while (wordc(s[e])) e++;
        return e - i;
    }
    return 0;
}

/* After a keyword: separator, then the value's start; returns 0 when this is not a pair. */
static size_t value_start(const char *s, size_t p, bool bearer)
{
    size_t q = p;
    if (s[q] == '"' || s[q] == '\'') q++;                  /* "key" */
    while (s[q] == ' ') q++;
    if (bearer && q > p) return q;                          /* Bearer <value> */
    if (s[q] != ':' && s[q] != '=') return 0;
    q++;
    while (s[q] == ' ') q++;
    if (s[q] == '"' || s[q] == '\'') q++;
    return q;
}

/* A decimal with 4+ places and |v| <= 180 at s[i]; returns its length or 0. */
static size_t coord_at(const char *s, size_t i)
{
    size_t j = i;
    if (s[j] == '-') j++;
    const size_t d0 = j;
    while (isdigit((unsigned char)s[j])) j++;
    if (j == d0 || j - d0 > 3 || s[j] != '.') return 0;
    int ip = 0;
    for (size_t k = d0; k < j; k++) ip = ip * 10 + (s[k] - '0');
    const size_t f0 = ++j;
    while (isdigit((unsigned char)s[j])) j++;
    if (j - f0 < 4 || ip > 180) return 0;
    return j - i;
}

/* Wi-Fi network names in log lines: `joined "NAME"`, `joining "NAME"`, `Connected: NAME`. They
   carry no key word the pair rule catches, and a home network's name says where its owner lives. */
static void mask_ssids(char *s)
{
    static const char *const lead[] = { "joined \"", "joining \"", "network \"", "Connected: " };
    for (size_t k = 0; k < sizeof(lead) / sizeof(lead[0]); k++) {
        const size_t n = strlen(lead[k]);
        const bool quoted = lead[k][n - 1] == '"';
        for (char *p = strstr(s, lead[k]); p; p = strstr(p, lead[k])) {
            p += n;
            while (*p && *p != 10 && *p != 13 && (quoted ? *p != '"' : true)) *p++ = '*';
        }
    }
}

size_t ls_redact(char *s, bool keep_gps)
{
    mask_ssids(s);
    size_t r = 0, w = 0;
    const size_t n = strlen(s);
    while (r < n) {
        const char c = s[r];
        const bool boundary = r == 0 || !isalnum((unsigned char)s[r - 1]);
        /* keyword=value */
        if (boundary && isalpha((unsigned char)c)) {
            const size_t kl = keyword_at(s, r);
            if (kl) {
                const bool bearer = !strncasecmp(s + r, "bearer", 6);
                const size_t vs = value_start(s, r + kl, bearer);
                if (vs) {
                    size_t ve = vs;
                    if (!strncasecmp(s + r, "auth", 4)) while (s[ve] && s[ve] != 10) ve++;   /* Authorization: Bearer x */
                    else while (!valend(s[ve])) ve++;
                    if (ve > vs) {
                        memmove(s + w, s + r, vs - r);
                        w += vs - r;
                        const size_t m = ve - vs < 3 ? ve - vs : 3;
                        memset(s + w, '*', m);
                        w += m;
                        r = ve;
                        continue;
                    }
                }
            }
        }
        /* long hex run */
        if (hexc(c) && (r == 0 || !hexc(s[r - 1]))) {
            size_t e = r;
            while (hexc(s[e])) e++;
            if (e - r >= 32 && !wordc(s[e])) {
                memcpy(s + w, "[hex]", 5);
                w += 5;
                r = e;
                continue;
            }
        }
        /* MAC aa:bb:cc:dd:ee:ff */
        if (hexc(c) && (r == 0 || !hexc(s[r - 1])) && r + 17 <= n) {
            bool mac = true;
            for (int k = 0; k < 17 && mac; k++)
                mac = (k % 3 == 2) ? s[r + k] == ':' : hexc(s[r + k]);
            if (mac && !hexc(s[r + 17])) {
                memcpy(s + w, s + r, 9);
                memcpy(s + w + 9, "**:**:**", 8);
                w += 17;
                r += 17;
                continue;
            }
        }
        /* IPv4 a.b.c.d */
        if (isdigit((unsigned char)c) && (r == 0 || !(isdigit((unsigned char)s[r - 1]) || s[r - 1] == '.'))) {
            size_t e = r;
            int parts = 0, dots = 0;
            size_t second_end = 0;
            for (;;) {
                size_t d = e;
                while (isdigit((unsigned char)s[e]) && e - d < 3) e++;
                if (e == d) { parts = 0; break; }
                parts++;
                if (parts == 2) second_end = e;
                if (s[e] == '.' && parts < 4) { e++; dots++; continue; }
                break;
            }
            if (parts == 4 && dots == 3 && !isdigit((unsigned char)s[e]) && s[e] != '.') {
                memcpy(s + w, s + r, second_end - r);
                w += second_end - r;
                memcpy(s + w, ".*.*", 4);
                w += 4;
                r = e;
                continue;
            }
        }
        /* lat, lon */
        if (!keep_gps && (c == '-' || isdigit((unsigned char)c)) && (r == 0 || !(isdigit((unsigned char)s[r - 1]) || s[r - 1] == '.'))) {
            const size_t a = coord_at(s, r);
            if (a) {
                size_t q = r + a;
                while (s[q] == ',' || s[q] == ' ' || s[q] == '/' || s[q] == ';') q++;
                const size_t b = q > r + a ? coord_at(s, q) : 0;
                if (b) {
                    memcpy(s + w, "[gps] [gps]", 11);
                    w += 11;
                    r = q + b;
                    continue;
                }
            }
        }
        s[w++] = c;
        r++;
    }
    s[w] = 0;
    return w;
}

size_t ls_json_esc(char *dst, size_t len, size_t cap, const char *src, size_t n)
{
    static const char hx[] = "0123456789abcdef";
    for (size_t i = 0; i < n; i++) {
        const unsigned char c = (unsigned char)src[i];
        char t[6];
        size_t k;
        if (c == '"' || c == '\\') { t[0] = '\\'; t[1] = (char)c; k = 2; }
        else if (c == '\n') { t[0] = '\\'; t[1] = 'n'; k = 2; }
        else if (c == '\t') { t[0] = '\\'; t[1] = 't'; k = 2; }
        else if (c < 0x20 || c == 0x7f) { memcpy(t, "\\u00", 4); t[4] = hx[c >> 4]; t[5] = hx[c & 15]; k = 6; }
        else { t[0] = (char)c; k = 1; }
        if (len + k + 1 > cap) break;
        memcpy(dst + len, t, k);
        len += k;
    }
    if (cap) dst[len] = 0;
    return len;
}
