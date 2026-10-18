#include "uat_settings.h"
#include "uat_hard.h"
#include <ctype.h>
#include <string.h>
typedef struct { const char *p, *end; } json_t;
static void ws(json_t *j) { while (j->p < j->end && isspace((unsigned char)*j->p)) ++j->p; }
static bool take(json_t *j, char c)
{
    ws(j); if (j->p == j->end || *j->p != c) return false;
    ++j->p; return true;
}
static bool word(json_t *j, char *out, size_t cap)
{
    if (!take(j, '"')) return false;
    size_t n = 0;
    while (j->p < j->end && *j->p != '"') {
        unsigned char c = *j->p++;
        if (c < 32 || c == '\\' || n + 1 >= cap) return false;
        out[n++] = c;
    }
    out[n] = 0; return take(j, '"');
}
static bool number(json_t *j, uint32_t *value)
{
    ws(j); uint32_t v = 0; const char *first = j->p;
    while (j->p < j->end && *j->p >= '0' && *j->p <= '9') {
        if (v > (UINT32_MAX - 9) / 10) return false;
        v = v * 10 + (*j->p++ - '0');
    }
    if (j->p == first || (j->p-first > 1 && *first == '0')) return false;
    *value = v; return true;
}
bool uat_settings_parse(const char *text, size_t n, uat_settings_t *out)
{
    if (!text || !out || n > 512) return false;
    json_t j = {text, text+n}; uat_settings_t result = {0}; unsigned seen = 0;
    if (!take(&j, '{')) return false;
    for (int i = 0; i < 3; ++i) {
        char key[24], mode[12]; uint32_t v;
        if ((i && !take(&j, ',')) || !word(&j, key, sizeof(key)) || !take(&j, ':')) return false;
        unsigned flag = !strcmp(key, "version") ? 1 : !strcmp(key, "mode") ? 2 : !strcmp(key, "sample_rate") ? 4 : 0;
        if (!flag || (seen & flag)) return false;
        seen |= flag;
        if (flag == 2) {
            if (!word(&j, mode, sizeof(mode)) || (strcmp(mode,"PACKET") && strcmp(mode,"STREAM"))) return false;
            result.stream = !strcmp(mode,"STREAM");
        } else {
            if (!number(&j, &v)) return false;
            if (flag == 1 && v != 1) return false;
            if (flag == 4) {
                if (!uat_hard_rate_valid(v)) return false;
                result.rate = v;
            }
        }
    }
    if (seen != 7 || !take(&j, '}')) return false;
    ws(&j); if (j.p != j.end) return false;
    *out = result; return true;
}
