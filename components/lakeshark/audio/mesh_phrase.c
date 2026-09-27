#include "mesh_phrase.h"

#include <string.h>

static bool speakable(char c)
{
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
           (c >= '0' && c <= '9') || c == '.' || c == ',' || c == '!' ||
           c == '?' || c == '\'';
}

static size_t put_char(char *out, size_t n, size_t pos, char c)
{
    if (pos + 1 < n) {
        out[pos++] = c;
        out[pos] = 0;
    }
    return pos;
}

/* Appends src with anything the voice cannot say turned into single
   spaces. A colon or emoji becomes a pause, not a silence or a stumble. */
static size_t put_clean(char *out, size_t n, size_t pos, const char *src, size_t len)
{
    bool space = pos == 0 || out[pos - 1] == ' ';
    for (size_t i = 0; i < len && pos + 1 < n; i++) {
        const char c = src[i];
        if (speakable(c)) {
            out[pos++] = c;
            space = false;
        } else if (!space) {
            out[pos++] = ' ';
            space = true;
        }
    }
    while (pos > 0 && out[pos - 1] == ' ') pos--;
    out[pos] = 0;
    return pos;
}

void mesh_phrase(char *out, size_t n, const char *text, bool direct, bool full)
{
    if (!out || n == 0) return;
    out[0] = 0;
    if (!text) return;

    /* On air a message reads "name: message". */
    const char *colon = strstr(text, ": ");
    const size_t name_len = colon ? (size_t)(colon - text) : 0;
    const char *body = colon ? colon + 2 : text;

    char name[40];
    put_clean(name, sizeof(name), 0, text, name_len);

    const char *lead = direct ? "DIRECT MESSAGE" : "MESSAGE";
    size_t pos = put_clean(out, n, 0, lead, strlen(lead));
    if (name[0]) {
        pos = put_clean(out, n, pos, " FROM ", 6);
        pos = put_char(out, n, pos, ' ');
        pos = put_clean(out, n, pos, name, strlen(name));
    }
    pos = put_char(out, n, pos, '.');

    /* One long unbroken token is machine data (cell reports travel as
       base64), and reading it out would be noise. */
    const bool data = strlen(body) >= 16 && !strchr(body, ' ');
    if (full && !data) {
        pos = put_char(out, n, pos, ' ');
        const size_t before = pos;
        pos = put_clean(out, n, pos, body, strlen(body));
        if (pos > before && out[pos - 1] != '.' && out[pos - 1] != '!' && out[pos - 1] != '?')
            pos = put_char(out, n, pos, '.');
    }
}
