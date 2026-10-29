/* See ls_tmach_clip.h. The REC half of the sidecar is REC's own formatter
   and parser, so the two cannot drift; this file only appends to it. */
#include "ls_tmach_clip.h"
#include "ls_tmach_ring.h"
#include "rec_file_open.h"

#include <ctype.h>
#include <inttypes.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

void ls_tm_clip_base(char *out, size_t n, uint32_t number)
{
    snprintf(out, n, "tm%04lu", (unsigned long)number);
}

uint32_t ls_tm_clip_number(const char *name)
{
    if (!name) return 0;
    const char *slash = strrchr(name, '/');
    if (slash) name = slash + 1;
    if (name[0] != 't' || name[1] != 'm' || !isdigit((unsigned char)name[2])) return 0;
    return (uint32_t)strtoul(name + 2, NULL, 10);
}

/* Names from the board never carry quotes; keep it that way. */
static void safe(char *dst, size_t cap, const char *src)
{
    size_t j = 0;
    for (size_t i = 0; src && src[i] && j + 1 < cap; i++) {
        const unsigned char c = (unsigned char)src[i];
        dst[j++] = (c < 0x20 || c == '"' || c == '\\' || c > 0x7e) ? '_' : (char)c;
    }
    dst[j] = 0;
}

int ls_tm_clip_format(char *out, size_t len, const ls_tm_clip_meta_t *m)
{
    if (!out || !len || !m) return 0;
    int n = rec_sidecar_format(out, len, &m->rec);
    /* REC's object ends "\n}\n"; continue it instead. */
    if (n < 3 || strcmp(out + n - 3, "\n}\n")) { out[0] = 0; return 0; }
    n -= 3;
    char iq[sizeof(m->iq)], dec[sizeof(m->decoder)];
    safe(iq, sizeof(iq), m->iq);
    safe(dec, sizeof(dec), m->decoder);
    const int k = snprintf(out + n, len - (size_t)n,
        ",\n"
        "  \"format\": \"%s\",\n"
        "  \"version\": %d,\n"
        "  \"iq\": \"%s\",\n"
        "  \"iq_format\": \"%s\",\n"
        "  \"center_hz\": %" PRIu64 ",\n"
        "  \"pairs\": %" PRIu64 ",\n"
        "  \"source_rate\": %lu,\n"
        "  \"source_format\": \"%s\",\n"
        "  \"decim\": %u,\n"
        "  \"start_us\": %" PRId64 ",\n"
        "  \"end_us\": %" PRId64 ",\n"
        "  \"saved_us\": %" PRId64 ",\n"
        "  \"fix\": %s,\n"
        "  \"lat\": %.6f,\n"
        "  \"lon\": %.6f,\n"
        "  \"alt_m\": %.1f,\n"
        "  \"decoder\": \"%s\",\n"
        "  \"gaps\": %lu\n"
        "}\n",
        LS_TM_CLIP_FORMAT, LS_TM_CLIP_VERSION, iq, ls_tm_fmt_name(m->iq_fmt),
        m->center_hz, m->pairs, (unsigned long)m->src_rate, ls_tm_fmt_name(m->src_fmt),
        (unsigned)m->decim, m->start_us, m->end_us, m->saved_us,
        m->fix ? "true" : "false", m->fix ? m->lat : 0.0, m->fix ? m->lon : 0.0,
        m->fix ? (double)m->alt_m : 0.0, dec, (unsigned long)m->gaps);
    if (k < 0 || (size_t)k >= len - (size_t)n) { out[0] = 0; return 0; }
    return n + k;
}

/* --------------------------------------------------------------- parse -- */

static const char *value_of(const char *in, const char *key)
{
    const size_t kl = strlen(key);
    for (const char *p = strchr(in, '"'); p; p = strchr(p + 1, '"')) {
        if (strncmp(p + 1, key, kl) || p[1 + kl] != '"') continue;
        const char *q = p + 2 + kl;
        while (*q == ' ' || *q == '\t' || *q == '\r' || *q == '\n') q++;
        if (*q != ':') continue;
        q++;
        while (*q == ' ' || *q == '\t' || *q == '\r' || *q == '\n') q++;
        return q;
    }
    return NULL;
}

static void str_of(const char *in, const char *key, char *dst, size_t cap)
{
    dst[0] = 0;
    const char *p = value_of(in, key);
    if (!p || *p != '"') return;
    size_t j = 0;
    for (p++; *p && *p != '"' && j + 1 < cap; p++) dst[j++] = *p;
    dst[j] = 0;
}

static int64_t i64_of(const char *in, const char *key)
{
    const char *p = value_of(in, key);
    return p ? strtoll(p, NULL, 10) : 0;
}

static double f_of(const char *in, const char *key)
{
    const char *p = value_of(in, key);
    return p ? strtod(p, NULL) : 0;
}

bool ls_tm_clip_parse(const char *in, ls_tm_clip_meta_t *out)
{
    if (!out) return false;
    memset(out, 0, sizeof(*out));
    if (!in || !rec_sidecar_parse(in, &out->rec)) return false;
    char fmt[24];
    str_of(in, "format", fmt, sizeof(fmt));
    if (strcmp(fmt, LS_TM_CLIP_FORMAT)) return false;
    out->version = (int)i64_of(in, "version");
    if (out->version < 1 || out->version > LS_TM_CLIP_VERSION) return false;
    char f[8];
    str_of(in, "iq", out->iq, sizeof(out->iq));
    str_of(in, "iq_format", f, sizeof(f));
    out->iq_fmt = (uint8_t)ls_tm_fmt_parse(f);
    str_of(in, "source_format", f, sizeof(f));
    out->src_fmt = (uint8_t)ls_tm_fmt_parse(f);
    out->center_hz = (uint64_t)i64_of(in, "center_hz");
    if (!out->center_hz) out->center_hz = out->rec.freq_hz;
    out->pairs = (uint64_t)i64_of(in, "pairs");
    out->src_rate = (uint32_t)i64_of(in, "source_rate");
    out->decim = (uint16_t)i64_of(in, "decim");
    out->start_us = i64_of(in, "start_us");
    out->end_us = i64_of(in, "end_us");
    out->saved_us = i64_of(in, "saved_us");
    const char *fx = value_of(in, "fix");
    out->fix = fx && !strncmp(fx, "true", 4);
    out->lat = f_of(in, "lat");
    out->lon = f_of(in, "lon");
    out->alt_m = (float)f_of(in, "alt_m");
    str_of(in, "decoder", out->decoder, sizeof(out->decoder));
    out->gaps = (uint32_t)i64_of(in, "gaps");
    /* A clip the upsampler cannot play is not a clip. */
    if ((out->iq_fmt != LS_TM_FMT_CS8 && out->iq_fmt != LS_TM_FMT_CS16) ||
        (out->src_fmt != LS_TM_FMT_CU8 && out->src_fmt != LS_TM_FMT_CS16) ||
        !out->decim || !out->rec.sample_rate || !out->iq[0]) return false;
    return true;
}

bool ls_tm_clip_write_meta(const char *dir, const char *base, const ls_tm_clip_meta_t *m)
{
    char path[160], body[LS_TM_CLIP_JSON_MAX];
    if (snprintf(path, sizeof(path), "%s/%s.json", dir, base) >= (int)sizeof(path)) return false;
    const int n = ls_tm_clip_format(body, sizeof(body), m);
    if (n <= 0) return false;
    FILE *f = rec_file_open_new(path);
    if (!f) return false;
    bool ok = fwrite(body, 1, (size_t)n, f) == (size_t)n;
    if (fclose(f)) ok = false;
    return ok;
}

bool ls_tm_clip_read_meta(const char *dir, const char *base, ls_tm_clip_meta_t *out)
{
    char path[160], body[LS_TM_CLIP_JSON_MAX];
    memset(out, 0, sizeof(*out));
    if (snprintf(path, sizeof(path), "%s/%s.json", dir, base) >= (int)sizeof(path)) return false;
    FILE *f = fopen(path, "rb");
    if (!f) return false;
    const size_t n = fread(body, 1, sizeof(body) - 1, f);
    fclose(f);
    body[n] = 0;
    return ls_tm_clip_parse(body, out);
}

/* ------------------------------------------------------------ playback -- */

void ls_tm_up_init(ls_tm_up_t *u, unsigned stored_fmt, unsigned src_fmt, unsigned decim)
{
    memset(u, 0, sizeof(*u));
    u->in_fmt = (uint8_t)stored_fmt;
    u->out_fmt = (uint8_t)src_fmt;
    u->decim = decim ? decim : 1;
}

size_t ls_tm_up_bytes(const ls_tm_up_t *u, size_t pairs)
{
    return pairs * u->decim * ls_tm_pair_bytes(u->out_fmt);
}

static inline int iround(double v) { return (int)lround(v); }

size_t ls_tm_up_run(ls_tm_up_t *u, const uint8_t *in, size_t in_bytes,
                    uint8_t *out, size_t out_cap, size_t *used)
{
    const unsigned ipb = ls_tm_pair_bytes(u->in_fmt), opb = ls_tm_pair_bytes(u->out_fmt);
    size_t done = 0, wrote = 0;
    if (used) *used = 0;
    if (!ipb || !opb) return 0;
    const bool cs8 = u->in_fmt == LS_TM_FMT_CS8, cu8 = u->out_fmt == LS_TM_FMT_CU8;
    /* Stored CS8 is CU8 less 128, so a clip kept undecimated plays back
       byte for byte; CS16 is as is. */
    while (done + ipb <= in_bytes && wrote + (size_t)u->decim * opb <= out_cap) {
        int32_t ci, cq;
        if (cs8) { ci = (int8_t)in[done]; cq = (int8_t)in[done + 1]; }
        else {
            ci = (int16_t)(in[done] | in[done + 1] << 8);
            cq = (int16_t)(in[done + 2] | in[done + 3] << 8);
        }
        for (unsigned j = 1; j <= u->decim; j++) {
            const double t = (double)j / u->decim;
            const double yi = u->prev_i + (ci - u->prev_i) * t, yq = u->prev_q + (cq - u->prev_q) * t;
            uint8_t *o = out + wrote;
            if (cu8) {
                int bi = iround(yi) + 128, bq = iround(yq) + 128;
                o[0] = (uint8_t)(bi < 0 ? 0 : bi > 255 ? 255 : bi);
                o[1] = (uint8_t)(bq < 0 ? 0 : bq > 255 ? 255 : bq);
            } else {
                int vi = iround(yi), vq = iround(yq);
                vi = vi < -32768 ? -32768 : vi > 32767 ? 32767 : vi;
                vq = vq < -32768 ? -32768 : vq > 32767 ? 32767 : vq;
                o[0] = (uint8_t)vi; o[1] = (uint8_t)((uint16_t)vi >> 8);
                o[2] = (uint8_t)vq; o[3] = (uint8_t)((uint16_t)vq >> 8);
            }
            wrote += opb;
        }
        u->prev_i = ci; u->prev_q = cq;
        done += ipb;
    }
    if (used) *used = done;
    return wrote;
}

size_t ls_tm_clip_play(FILE *iq, const ls_tm_clip_meta_t *m, size_t chunk,
                       ls_tm_sink_fn sink, void *ctx)
{
    if (!iq || !m || !sink) return 0;
    ls_tm_up_t up;
    ls_tm_up_init(&up, m->iq_fmt, m->src_fmt, m->decim);
    const unsigned ipb = ls_tm_pair_bytes(m->iq_fmt);
    if (!ipb) return 0;
    if (!chunk) chunk = 1024;
    uint8_t *in = malloc(chunk * ipb);
    const size_t ocap = ls_tm_up_bytes(&up, chunk);
    uint8_t *out = malloc(ocap);
    size_t total = 0;
    if (in && out) {
        size_t n;
        while ((n = fread(in, 1, chunk * ipb, iq)) >= ipb) {
            size_t used = 0;
            const size_t k = ls_tm_up_run(&up, in, n - n % ipb, out, ocap, &used);
            total += k;
            if (!sink(ctx, out, k)) break;
        }
    }
    free(in);
    free(out);
    return total;
}
