#include "subghz_file.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <limits.h>
#include <ctype.h>

void subghz_file_begin(subghz_file_t *f)
{
    if (f) memset(f, 0, sizeof(*f));
}

static void copy_value(char *out, size_t len, const char *v)
{
    while (*v == ' ') v++;
    size_t n = strcspn(v, "\r\n");
    if (n >= len) n = len - 1;
    memcpy(out, v, n);
    out[n] = 0;
}

/* The number just before `word`, e.g. "4800" in "4800 baud". */
static bool number_before(const char *line, const char *word, unsigned long *out)
{
    const char *at = strstr(line, word);
    if (!at || at == line) return false;
    const char *p = at;
    while (p > line && p[-1] == ' ') p--;
    const char *end = p;
    while (p > line && p[-1] >= '0' && p[-1] <= '9') p--;
    if (p == end) return false;
    *out = strtoul(p, NULL, 10);
    return true;
}

/* Both comment layouts this board has written:
     # 4800 baud, 25000 Hz deviation, sync 2DD42DD4, 32-bit preamble
     # 4800 baud, 25000 Hz deviation requested, 24963 Hz encodable
     # 32-bit preamble, sync 2DD42DD4, 8 byte payload */
static void comment(subghz_file_t *f, const char *line)
{
    unsigned long v;
    if (number_before(line, "baud", &v)) f->bitrate = (uint32_t)v;
    if (number_before(line, "Hz deviation", &v)) f->deviation_hz = (uint32_t)v;
    if (number_before(line, "-bit preamble", &v)) f->preamble_bits = (uint16_t)v;
    const char *s = strstr(line, "sync ");
    if (s) f->sync_word = (uint32_t)strtoul(s + 5, NULL, 16);
}

void subghz_file_line(subghz_file_t *f, const char *line,
                      int32_t *edges, int cap)
{
    if (!f || !line) return;
    if (!strncmp(line, "Filetype:", 9)) {
        f->filetype_ok = strstr(line, "SubGhz") != NULL;
    } else if (!strncmp(line, "Frequency:", 10)) {
        char *end;
        errno=0;
        unsigned long long hz=strtoull(line+10,&end,10);
        while(isspace((unsigned char)*end))end++;
        if(errno || hz>UINT32_MAX || !hz || *end)f->invalid=true;
        f->freq_hz=(uint32_t)hz;
    } else if (!strncmp(line, "Preset:", 7)) {
        copy_value(f->preset, sizeof(f->preset), line + 7);
    } else if (!strncmp(line, "Protocol:", 9)) {
        copy_value(f->protocol, sizeof(f->protocol), line + 9);
    } else if (line[0] == '#') {
        comment(f, line);
    } else if (!strncmp(line, "RAW_Data:", 9)) {
        const char *p = line + 9;
        char *end;
        for (;;) {
            errno = 0;
            const long long v = strtoll(p, &end, 10);
            if (end == p) {
                while (isspace((unsigned char)*p)) p++;
                if (*p) f->invalid = true;
                break;
            }
            p = end;
            if (errno || v > INT32_MAX || v < -INT32_MAX) {
                f->invalid = true;
                continue;
            }
            if (!v) { f->invalid=true; continue; }
            f->edges_total++;
            if (edges && f->edges < cap) {
                if (f->edges && (edges[f->edges-1]>0)==(v>0)) f->invalid=true;
                edges[f->edges++] = (int32_t)v;
                f->span_us += (uint64_t)(v < 0 ? -v : v);
            }
        }
    }
}

bool subghz_file_is_fsk(const subghz_file_t *f)
{
    return f && !f->invalid && f->filetype_ok && f->edges == f->edges_total &&
        f->bitrate && f->deviation_hz && f->edges >= 6 && f->edges <= 4096;
}

bool subghz_file_is_ook(const subghz_file_t *f)
{
    return f && !f->invalid && f->filetype_ok && !strcmp(f->protocol,"RAW") &&
        (!strcmp(f->preset,"FuriHalSubGhzPresetOok650Async") ||
         !strcmp(f->preset,"FuriHalSubGhzPresetOok270Async")) &&
        !f->bitrate && !f->deviation_hz && f->edges >= 6 && f->edges <= 4096 &&
        f->edges == f->edges_total && f->span_us <= 10000000 &&
        ((f->freq_hz>=300000000 && f->freq_hz<=348000000) ||
         (f->freq_hz>=387000000 && f->freq_hz<=464000000) ||
         (f->freq_hz>=779000000 && f->freq_hz<=928000000));
}

size_t subghz_ook_symbols(const int32_t *edges, size_t count,
                          uint32_t *out, size_t capacity)
{
    if (!edges || count < 6 || count > 4096) return 0;
    uint64_t span = 0;
    size_t halves = 0;
    for (size_t i=0;i<count;i++) {
        int64_t v=edges[i];
        if (!v || (i && (v>0)==(edges[i-1]>0))) return 0;
        uint64_t us=v<0?-v:v;
        span+=us;
        if (span>10000000) return 0;
        halves+=(us+32766)/32767;
    }
    /* A trailing low half ensures the final mark completes before EOT. */
    size_t words=(halves+2)/2;
    if (!out) return words;
    if (capacity<words) return 0;
    memset(out,0,words*sizeof(*out));
    size_t at=0;
    for (size_t i=0;i<count;i++) {
        uint32_t us=edges[i]<0?-(int64_t)edges[i]:edges[i];
        while(us) {
            uint32_t n=us>32767?32767:us;
            out[at/2]|=(n | (edges[i]>0?0x8000u:0)) << ((at%2)*16);
            at++;us-=n;
        }
    }
    out[at/2]|=1u<<((at%2)*16);
    return words;
}

bool subghz_file_load(const char *path, subghz_file_t *f, int32_t *edges,
                      int cap, char *line, size_t line_len)
{
    subghz_file_begin(f);
    if (!path || !f || !line || line_len < 16) return false;
    FILE *fp = fopen(path, "r");
    if (!fp) return false;
    while (fgets(line, (int)line_len, fp)) {
        if (!strchr(line,'\n') && !feof(fp)) f->invalid=true;
        subghz_file_line(f, line, edges, cap);
    }
    if (ferror(fp)) f->invalid=true;
    fclose(fp);
    return f->filetype_ok;
}
