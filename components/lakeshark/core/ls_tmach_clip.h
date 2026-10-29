/* TIME MACHINE clips: what "WHAT WAS THAT?" leaves on the card, and the
   upsampler that plays one back at the rate and format its receiver ran at.

   A clip is two files with one base name, in /sdcard/lakeshark/tmach:

     tm0007.cs8   (or .cs16)  the I/Q, raw interleaved I,Q pairs, no header:
                              cs8 signed 8-bit, cs16 signed 16-bit little
                              endian. Any SDR tool opens it as it is.
     tm0007.json  the sidecar. Its first keys are REC's sidecar, written by
                  rec_sidecar_format and readable by rec_sidecar_parse /
                  rec_sidecar_read unchanged (sample_rate is the STORED rate,
                  freq_hz the centre). The keys after them extend it:

       "format": "lakeshark-tmclip"   identifies the extension
       "version": 1                   bumped on any incompatible change
       "iq": "tm0007.cs8"             the data file, beside this one
       "iq_format": "cs8"|"cs16"      stored sample format
       "center_hz": 152600000         full 64-bit centre (freq_hz is 32-bit)
       "pairs": 1920000               stored I/Q pairs in the data file
       "source_rate": 256000          the receiver's rate before decimation
       "source_format": "cu8"|"cs16"  the receiver's sample format
       "decim": 4                     source_rate / sample_rate
       "start_us","end_us"            the board's clock (esp_timer) at the
                                      first sample and just after the last
       "saved_us"                     when the clip was asked for
       "lat","lon","alt_m","fix"      GPS at that moment; fix false = none
       "decoder": "POCSAG"            what the receiver was decoding, or ""
       "gaps": 0                      samples lost inside the window (0: none)

   Version 1 readers refuse a different "format" or a larger "version". */
#ifndef LS_TMACH_CLIP_H
#define LS_TMACH_CLIP_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#include "rec_sidecar.h"

#ifdef __cplusplus
extern "C" {
#endif

#define LS_TM_CLIP_FORMAT   "lakeshark-tmclip"
#define LS_TM_CLIP_VERSION  1
#define LS_TM_CLIP_DIR      "/sdcard/lakeshark/tmach"
#define LS_TM_CLIP_JSON_MAX 1280

typedef struct {
    rec_sidecar_t rec;        /* REC's own fields, first in the file      */
    int      version;
    char     iq[24];          /* data file name, no directory             */
    uint8_t  iq_fmt;          /* LS_TM_FMT_CS8 / LS_TM_FMT_CS16           */
    uint8_t  src_fmt;         /* LS_TM_FMT_CU8 / LS_TM_FMT_CS16           */
    uint16_t decim;
    uint32_t src_rate;
    uint64_t center_hz;
    uint64_t pairs;
    int64_t  start_us, end_us, saved_us;
    bool     fix;
    double   lat, lon;
    float    alt_m;
    char     decoder[16];
    uint32_t gaps;
} ls_tm_clip_meta_t;

/* "tm0007" for clip 7. */
void ls_tm_clip_base(char *out, size_t n, uint32_t number);
/* Clip number from a base or file name ("tm0007.json" -> 7), 0 if not one. */
uint32_t ls_tm_clip_number(const char *name);

/* JSON body, REC keys first. Returns bytes written (no NUL) or 0. */
int  ls_tm_clip_format(char *out, size_t len, const ls_tm_clip_meta_t *m);
/* False when it is not a clip sidecar this version can read. */
bool ls_tm_clip_parse(const char *in, ls_tm_clip_meta_t *out);

/* Write / read the sidecar at <dir>/<base>.json. */
bool ls_tm_clip_write_meta(const char *dir, const char *base, const ls_tm_clip_meta_t *m);
bool ls_tm_clip_read_meta(const char *dir, const char *base, ls_tm_clip_meta_t *out);

/* ---------------------------------------------------------- playback -- */

/* Stored pairs back to the receiver's rate and format: each stored pair
   becomes `decim` output pairs by straight-line interpolation from the one
   before. The decoders' own channel filters remove the images. */
typedef struct {
    uint8_t  in_fmt, out_fmt;
    unsigned decim;
    int32_t  prev_i, prev_q;
} ls_tm_up_t;

void ls_tm_up_init(ls_tm_up_t *u, unsigned stored_fmt, unsigned src_fmt, unsigned decim);
/* Output bytes needed for `pairs` stored pairs. */
size_t ls_tm_up_bytes(const ls_tm_up_t *u, size_t pairs);
/* Converts whole stored pairs while they fit in out_cap. *used gets the
   input bytes consumed. Returns output bytes. */
size_t ls_tm_up_run(ls_tm_up_t *u, const uint8_t *in, size_t in_bytes,
                    uint8_t *out, size_t out_cap, size_t *used);

/* Read the clip's data file and hand it to `sink` at the source rate and
   format, `chunk` stored pairs at a time. Stops early if sink returns false.
   Returns the source-rate bytes delivered. For the bench and the console;
   the board's worker steps the same pieces itself. */
typedef bool (*ls_tm_sink_fn)(void *ctx, const uint8_t *iq, size_t bytes);
size_t ls_tm_clip_play(FILE *iq, const ls_tm_clip_meta_t *m, size_t chunk,
                       ls_tm_sink_fn sink, void *ctx);

#ifdef __cplusplus
}
#endif

#endif
