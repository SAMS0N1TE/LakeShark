/*LS-990  Pull a file off the device over the console.

   The device writes captures and screenshots to the SD card and the only way
   to retrieve one is WiFi - which needs either the operator's network
   credentials or their machine moved onto the board's access point. Neither is
   always available, and neither is quick.

   A previous attempt at this streamed base64 over the console and produced
   corrupt files, for a reason worth remembering: the console is shared. P25TEL
   and ADSB log lines interleave into the stream between data lines, and at
   115200 the UART also drops bytes under load - about 16% of one image.

   So every data line carries a marker the receiver filters on, the payload is
   fixed-width, and the whole transfer is covered by a CRC-32 that is checked
   at the far end. A log line landing in the middle is now harmless: it does
   not start with the marker, so it is discarded, and if bytes are lost the CRC
   says so rather than handing back a plausible-looking broken file.

   Slow by nature - 1.1 MB of BMP is about four minutes at 115200 - so this is
   a fallback, not the normal route. `wifi on` remains the fast path. */

#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <sys/stat.h>

#include "esp_log.h"
#include "esp_crc.h"
#include "serial_get.h"
#include "rec_state.h"

#define GET_LINE_BYTES  57          /* 57 raw -> 76 base64 chars, one tidy line */

static const char B64[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static void b64_line(const uint8_t *in, size_t n, char *out)
{
    size_t o = 0;
    for (size_t i = 0; i < n; i += 3) {
        uint32_t v = (uint32_t)in[i] << 16;
        if (i + 1 < n) v |= (uint32_t)in[i + 1] << 8;
        if (i + 2 < n) v |= (uint32_t)in[i + 2];

        out[o++] = B64[(v >> 18) & 0x3F];
        out[o++] = B64[(v >> 12) & 0x3F];
        out[o++] = (i + 1 < n) ? B64[(v >> 6) & 0x3F] : '=';
        out[o++] = (i + 2 < n) ? B64[v & 0x3F]        : '=';
    }
    out[o] = '\0';
}

/* Resolve a bare name against the captures directory, so `get rec1.sub` works
   without the operator knowing where captures live. An absolute path is taken
   as given. */
static FILE *open_named(const char *name, char *shown, size_t shown_len, long *size)
{
    char path[192];
    if (name[0] == '/') snprintf(path, sizeof(path), "%s", name);
    else                snprintf(path, sizeof(path), "%s/%s", rec_dir(), name);

    struct stat st;
    if (stat(path, &st) != 0 || !S_ISREG(st.st_mode)) return NULL;

    FILE *f = fopen(path, "rb");
    if (!f) return NULL;

    snprintf(shown, shown_len, "%s", path);
    *size = (long)st.st_size;
    return f;
}

int serial_get_file(const char *name)
{
    char path[192];
    long size = 0;

    FILE *f = open_named(name, path, sizeof(path), &size);
    if (!f) {
        printf("#GET# error no-such-file %s\n", name);
        return -1;
    }

    /* The receiver needs the length before the data so it can tell a truncated
       transfer from a complete one without waiting for a timeout. */
    printf("#GET# begin %s %ld\n", path, size);

    uint8_t  buf[GET_LINE_BYTES];
    char     line[GET_LINE_BYTES * 4 / 3 + 8];
    uint32_t crc = 0;
    long     sent = 0;
    size_t   n;

    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) {
        crc = esp_crc32_le(crc, buf, n);
        b64_line(buf, n, line);
        printf("#B64#%s\n", line);
        sent += (long)n;
        /* No flush or delay here on purpose: the console already blocks on the
           UART, and adding a sleep per line turns four minutes into twenty. */
    }
    fclose(f);

    printf("#GET# end %ld %08lx\n", sent, (unsigned long)crc);
    return (sent == size) ? 0 : -1;
}

int serial_get_list(char *out, size_t len)
{
    return screenshot_list_dir(rec_dir(), out, len);
}
