/*LS-960*/
/* The `.sub` file is what a Flipper reads and must not change; everything
   else worth knowing about a capture goes here.  Format and parse both live
   in this one source so the bench can round-trip a struct through a string
   and prove no field goes missing across a save/reload. */

#include "rec_sidecar.h"
#include "rec_file_open.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

/* Copy `src` into a `dst` field, replacing anything that would need JSON
   escaping ('"', '\\', or control characters) with '_'.  Sidecar strings
   come from us - board names, firmware versions, timestamps - so this is
   defensive rather than expressive; anything unprintable indicates the
   source string is corrupt and truncating is preferable to shipping a
   broken JSON object. */
static void copy_safe(char *dst, size_t cap, const char *src)
{
    if (!dst || cap == 0) return;
    if (!src) { dst[0] = '\0'; return; }
    size_t j = 0;
    for (size_t i = 0; src[i] && j + 1 < cap; i++) {
        unsigned char c = (unsigned char)src[i];
        if (c < 0x20 || c == '"' || c == '\\') dst[j++] = '_';
        else                                   dst[j++] = (char)c;
    }
    dst[j] = '\0';
}

int rec_sidecar_format(char *out, size_t len, const rec_sidecar_t *s)
{
    if (!out || len == 0) return 0;
    out[0] = '\0';
    if (!s) return 0;

    char time_s[REC_SIDECAR_TIME_MAX];
    char board_s[REC_SIDECAR_ID_MAX];
    char fw_s[REC_SIDECAR_ID_MAX];
    copy_safe(time_s,  sizeof(time_s),  s->time);
    copy_safe(board_s, sizeof(board_s), s->board);
    copy_safe(fw_s,    sizeof(fw_s),    s->firmware);

    int n = snprintf(out, len,
        "{\n"
        "  \"time\": \"%s\",\n"
        "  \"freq_hz\": %lu,\n"
        "  \"gain_tenths\": %d,\n"
        "  \"bw_hz\": %lu,\n"
        "  \"sample_rate\": %lu,\n"
        "  \"edges\": %d,\n"
        "  \"span_us\": %lu,\n"
        "  \"mag_peak\": %d,\n"
        "  \"mag_floor\": %d,\n"
        "  \"board\": \"%s\",\n"
        "  \"firmware\": \"%s\"\n"
        "}\n",
        time_s,
        (unsigned long)s->freq_hz,
        s->gain_tenths,
        (unsigned long)s->bw_hz,
        (unsigned long)s->sample_rate,
        s->edges,
        (unsigned long)s->span_us,
        s->mag_peak,
        s->mag_floor,
        board_s,
        fw_s);

    if (n < 0) { out[0] = '\0'; return 0; }
    if ((size_t)n >= len) {
        /* Truncated - the caller's buffer is smaller than the format
           needs.  Refuse to hand back a half-formed JSON object; a
           downstream parser cannot recover from missing braces. */
        out[0] = '\0';
        return 0;
    }
    return n;
}

/* --------------------------------------------------------------- parser */

/* The parser walks the input linearly, key by key, without building a
   token stream.  JSON here is our own output rather than arbitrary user
   input, so this is a compatible-subset reader: flat object, no nesting,
   no arrays, integer and string values only.  Unknown keys and extra
   whitespace are tolerated so a future field can be added without
   invalidating older sidecars. */

static const char *skip_ws(const char *p)
{
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++;
    return p;
}

/* Find the value position for `key`.  Walks the input looking for a
   `"key"` string followed by optional whitespace and a colon.  Returns
   the position after the colon (with leading whitespace skipped) or NULL
   if the key is not present. */
static const char *find_value(const char *in, const char *key)
{
    if (!in || !key) return NULL;
    size_t klen = strlen(key);
    for (const char *p = in; *p; p++) {
        if (*p != '"') continue;
        if (strncmp(p + 1, key, klen) != 0) continue;
        if (p[1 + klen] != '"') continue;
        const char *q = skip_ws(p + 2 + klen);
        if (*q != ':') continue;
        return skip_ws(q + 1);
    }
    return NULL;
}

/* Copy a JSON string value (starting at the opening quote) into `dst`,
   honouring backslash escapes for '\\' and '"' only - the writer does not
   emit any others, so a well-formed sidecar cannot contain them. */
static void read_string(const char *p, char *dst, size_t cap)
{
    if (!dst || cap == 0) return;
    dst[0] = '\0';
    if (!p || *p != '"') return;
    p++;
    size_t j = 0;
    while (*p && *p != '"' && j + 1 < cap) {
        if (*p == '\\' && p[1]) {
            dst[j++] = p[1];
            p += 2;
        } else {
            dst[j++] = *p++;
        }
    }
    dst[j] = '\0';
}

static long read_long(const char *p)
{
    if (!p) return 0;
    return strtol(p, NULL, 10);
}

static unsigned long read_ulong(const char *p)
{
    if (!p) return 0;
    return strtoul(p, NULL, 10);
}

bool rec_sidecar_parse(const char *in, rec_sidecar_t *out)
{
    if (!out) return false;
    memset(out, 0, sizeof(*out));
    if (!in) return false;

    const char *brace = strchr(in, '{');
    if (!brace) return false;

    read_string(find_value(in, "time"),     out->time,     sizeof(out->time));
    read_string(find_value(in, "board"),    out->board,    sizeof(out->board));
    read_string(find_value(in, "firmware"), out->firmware, sizeof(out->firmware));

    out->freq_hz     = (uint32_t)read_ulong(find_value(in, "freq_hz"));
    out->gain_tenths = (int)     read_long (find_value(in, "gain_tenths"));
    out->bw_hz       = (uint32_t)read_ulong(find_value(in, "bw_hz"));
    out->sample_rate = (uint32_t)read_ulong(find_value(in, "sample_rate"));
    out->edges       = (int)     read_long (find_value(in, "edges"));
    out->span_us     = (uint32_t)read_ulong(find_value(in, "span_us"));
    out->mag_peak    = (int)     read_long (find_value(in, "mag_peak"));
    out->mag_floor   = (int)     read_long (find_value(in, "mag_floor"));

    return true;
}

/* --------------------------------------------------------- file wrappers */

static int compose_path(char *out, size_t len,
                        const char *dir, const char *base)
{
    if (!out || len == 0 || !dir || !base) return -1;
    int w = snprintf(out, len, "%s/%s%s", dir, base, REC_SIDECAR_EXT);
    if (w < 0 || (size_t)w >= len) return -1;
    return 0;
}

int rec_sidecar_write(const char *dir, const char *base, const rec_sidecar_t *s)
{
    if (!dir || !base || !s) return -1;

    char path[192];
    if (compose_path(path, sizeof(path), dir, base) != 0) return -1;

    char body[REC_SIDECAR_JSON_MAX];
    int n = rec_sidecar_format(body, sizeof(body), s);
    if (n <= 0) return -1;

    /* "wx" refuses to open an existing file - a stale sidecar from a
       previous capture must not be silently paved over, same rule
       rec_save enforces on the .sub. */
    FILE *f = rec_file_open_new(path);
    if (!f) return -1;

    size_t wrote = fwrite(body, 1, (size_t)n, f);
    int rc = (wrote == (size_t)n) ? 0 : -1;
    if (fclose(f) != 0) rc = -1;
    return rc;
}

bool rec_sidecar_read(const char *dir, const char *base, rec_sidecar_t *out)
{
    if (!out) return false;
    memset(out, 0, sizeof(*out));
    if (!dir || !base) return false;

    char path[192];
    if (compose_path(path, sizeof(path), dir, base) != 0) return false;

    FILE *f = fopen(path, "rb");
    if (!f) return false;

    char body[REC_SIDECAR_JSON_MAX];
    size_t n = fread(body, 1, sizeof(body) - 1, f);
    fclose(f);
    body[n] = '\0';

    return rec_sidecar_parse(body, out);
}
