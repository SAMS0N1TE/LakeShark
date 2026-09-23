/* Reading a Flipper .sub file back: its header, and its RAW edges.

   What this board writes and what a Flipper writes are the same format, so
   one reader serves both. The FSK parameters the SX1262 needs to send a
   capture again are not part of the format; this board records them in a
   comment line, and a file without one is only good for amplitude keying. */
#ifndef SUBGHZ_FILE_H
#define SUBGHZ_FILE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint32_t freq_hz;
    char     preset[48];     /* e.g. FuriHalSubGhzPresetOok650Async */
    char     protocol[24];   /* RAW, Princeton, ... */
    bool     filetype_ok;    /* the first line said it is a SubGhz file */
    bool     invalid;        /* malformed/overflowed input must not transmit */

    /* From this board's comment line; zero when the file has none. */
    uint32_t bitrate;
    uint32_t deviation_hz;
    uint32_t sync_word;
    uint16_t preamble_bits;

    int      edges;          /* stored in the caller's buffer */
    int      edges_total;    /* in the file, which may be more */
    uint64_t span_us;        /* of the stored edges */
} subghz_file_t;

void subghz_file_begin(subghz_file_t *f);

/* One line of the file, without or with its newline. RAW_Data values go into
   `edges` up to `cap`; zeros, which a .sub may not contain, are skipped. */
void subghz_file_line(subghz_file_t *f, const char *line,
                      int32_t *edges, int cap);

/* True when the file carries what the SX1262 needs to send it again. */
bool subghz_file_is_fsk(const subghz_file_t *f);
bool subghz_file_is_ook(const subghz_file_t *f);

/* Validate and pack microsecond edges into pairs of 15-bit RMT durations.
   A NULL output returns the required word count. At most 4096 edges/10 s;
   long spaces are split without changing their level or duration. */
size_t subghz_ook_symbols(const int32_t *edges, size_t count,
                          uint32_t *out, size_t capacity);

/* Read a whole file. `line` is the caller's scratch, and must hold the
   longest RAW_Data line - 512 values, about 4 KB. */
bool subghz_file_load(const char *path, subghz_file_t *f, int32_t *edges,
                      int cap, char *line, size_t line_len);

#ifdef __cplusplus
}
#endif

#endif
