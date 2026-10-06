/* TERMINAL's scrollback and history. No SDK calls; see ls_term.h. */

#include "ls_term.h"

#include <stdio.h>
#include <string.h>

void ls_term_scroll_reset(ls_term_scroll_t *s)
{
    if (!s) return;
    s->head = 0;
    s->used = 0;
    s->gen++;
}

static void put(ls_term_scroll_t *s, char c)
{
    s->buf[s->head] = c;
    s->head = (s->head + 1) % LS_TERM_SCROLL_BYTES;
    if (s->used < LS_TERM_SCROLL_BYTES) s->used++;
}

void ls_term_scroll_put(ls_term_scroll_t *s, const char *data, size_t n)
{
    if (!s || !data || !n) return;
    for (size_t i = 0; i < n; i++) {
        const unsigned char c = (unsigned char)data[i];
        if (c == '\n')                put(s, '\n');
        else if (c == '\t')           put(s, ' ');
        else if (c < 0x20 || c == 0x7F) continue;
        else if (c > 0x7E)            put(s, '.');
        else                          put(s, (char)c);
    }
    s->gen++;
}

static char at(const ls_term_scroll_t *s, uint32_t i)
{
    return s->buf[(s->head + LS_TERM_SCROLL_BYTES - s->used + i) % LS_TERM_SCROLL_BYTES];
}

size_t ls_term_scroll_copy(const ls_term_scroll_t *s, char *out, size_t n)
{
    if (!out || !n) return 0;
    out[0] = '\0';
    if (!s || !s->used) return 0;
    uint32_t start = 0;
    if (s->used == LS_TERM_SCROLL_BYTES) {          /* the oldest line lost its start */
        while (start < s->used && at(s, start) != '\n') start++;
        if (start < s->used) start++;
    }
    if (s->used - start > n - 1) {                  /* the newest whole lines that fit */
        start = s->used - (uint32_t)(n - 1);
        while (start < s->used && at(s, start - 1) != '\n') start++;
    }
    size_t k = 0;
    for (uint32_t i = start; i < s->used && k + 1 < n; i++) out[k++] = at(s, i);
    out[k] = '\0';
    return k;
}

void ls_term_history_add(ls_term_history_t *h, const char *line)
{
    if (!h || !line) return;
    while (*line == ' ') line++;
    if (!*line) return;
    const char *newest = ls_term_history_get(h, 1);
    if (newest && !strcmp(newest, line)) return;
    snprintf(h->line[h->next], sizeof(h->line[0]), "%s", line);
    h->next = (h->next + 1) % LS_TERM_HISTORY;
    if (h->count < LS_TERM_HISTORY) h->count++;
}

const char *ls_term_history_get(const ls_term_history_t *h, int back)
{
    if (!h || back < 1 || back > h->count) return NULL;
    return h->line[(h->next - back + LS_TERM_HISTORY) % LS_TERM_HISTORY];
}
