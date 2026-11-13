/* See ls_journal_desk.h. */
#include "ls_journal_desk.h"

#include <stdio.h>
#include <string.h>
#include <strings.h>

static bool has(const ls_jrnd_row_t *r, int col)
{
    switch (col) {
    case LS_JC_TITLE: return r->title[0] != 0;
    case LS_JC_SOURCE: return r->source[0] != 0;
    case LS_JC_FREQ: return r->freq != 0;
    case LS_JC_RSSI: return r->signal;
    case LS_JC_PLACE: return r->gps;
    default: return true;
    }
}

static int cmp(const ls_jrnd_row_t *a, const ls_jrnd_row_t *b, int col)
{
#define NUM(x, y) ((x) < (y) ? -1 : (x) > (y))
    switch (col) {
    case LS_JC_TITLE: return strcasecmp(a->title, b->title);
    case LS_JC_WHEN:
        /* A stamped entry is newer than any from before the clock was set. */
        if (a->clock != b->clock) return a->clock ? -1 : 1;
        if (a->clock) { const int c = strcmp(b->when, a->when); if (c) return c; }
        else if (a->time_us != b->time_us) return NUM(b->time_us, a->time_us);
        return NUM(a->index, b->index);
    case LS_JC_SOURCE: return strcmp(a->source, b->source);
    case LS_JC_FREQ: return NUM(a->freq, b->freq);
    case LS_JC_RSSI: return NUM(b->rssi, a->rssi);          /* ascending is the strongest first */
    case LS_JC_PLACE: return a->lat != b->lat ? NUM(b->lat, a->lat) : NUM(a->lon, b->lon);    /* north to south */
    case LS_JC_SIZE: return NUM(b->chars, a->chars);
    case LS_JC_KEPT: return NUM(b->saved, a->saved);
    default: return 0;
    }
#undef NUM
}

void ls_jrnd_sort(uint8_t *order, const ls_jrnd_row_t *rows, int n, int col, bool desc)
{
    for (int i = 0; i < n; i++) order[i] = (uint8_t)i;
    for (int i = 1; i < n; i++) {
        const uint8_t v = order[i];
        const ls_jrnd_row_t *b = &rows[v];
        int k = i;
        while (k > 0) {
            const ls_jrnd_row_t *a = &rows[order[k - 1]];
            int c;
            if (has(a, col) != has(b, col)) c = has(a, col) ? -1 : 1;
            else { c = cmp(a, b, col); if (desc) c = -c; }
            if (c <= 0) break;
            order[k] = order[k - 1];
            k--;
        }
        order[k] = v;
    }
}

void ls_jrnd_cell(const ls_jrnd_row_t *r, int col, char *out, size_t n)
{
    if (!n) return;
    out[0] = 0;
    switch (col) {
    case LS_JC_TITLE: snprintf(out, n, "%s", r->title[0] ? r->title : "(no title)"); break;
    case LS_JC_WHEN: snprintf(out, n, "%s", r->when); break;
    case LS_JC_SOURCE: snprintf(out, n, "%s", r->source); break;
    case LS_JC_FREQ: if (r->freq) snprintf(out, n, "%.4f", r->freq / 1e6); break;
    case LS_JC_RSSI: if (r->signal) snprintf(out, n, "%.0f", (double)r->rssi); break;
    case LS_JC_PLACE: if (r->gps) snprintf(out, n, "%.4f %.4f", r->lat, r->lon); break;
    case LS_JC_SIZE: snprintf(out, n, "%u", r->chars); break;
    case LS_JC_KEPT: snprintf(out, n, "%s", r->saved ? "SD" : "RAM"); break;
    default: break;
    }
}

bool ls_jrnd_holds(const char *text, const char *needle)
{
    if (!needle || !*needle) return true;
    if (!text) return false;
    const size_t n = strlen(needle);
    for (; *text; text++)
        if (!strncasecmp(text, needle, n)) return true;
    return false;
}

int ls_jrnd_wrap(const char *text, int off, int width, char *line, size_t cap)
{
    const char *s = text + off;
    int n = 0;
    if (width < 1) width = 1;
    while (s[n] && s[n] != '\n' && n < width) n++;
    if (s[n] && s[n] != '\n') {
        int space = n;
        while (space > 0 && s[space] != ' ') space--;
        if (space > 0) n = space;
    }
    if (line && cap) snprintf(line, cap, "%.*s", n, s);
    off += n;
    if (text[off] == '\n') return off + 1;
    while (text[off] == ' ') off++;
    return off;
}

int ls_jrnd_lines(const char *text, int width)
{
    int off = 0, lines = 0;
    if (!text || !*text) return 0;
    do { off = ls_jrnd_wrap(text, off, width, NULL, 0); lines++; } while (text[off] && lines < 4096);
    return lines;
}
