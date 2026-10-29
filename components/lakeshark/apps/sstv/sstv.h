/* SSTV receive: VIS header, line sync, slant correction, pixels.

   In: 16 kHz audio from any demodulator (NFM, AM, USB). The tone is turned
   into an instantaneous frequency (a quadrature mix to 1700 Hz, a low-pass,
   and the phase step between samples), kept in a ring long enough for the
   slowest line. The VIS header (1900 Hz leader, 1200 Hz start, eight 30 ms
   bits at 1100/1300 Hz, 1200 Hz stop) names the mode. From then on each
   line's sync pulse is looked for near where the line timing says it should
   be, and a least-squares line through the found syncs gives the real line
   period, so a sound card or radio clock a few hundred ppm off still makes
   a straight picture (slant correction). Rows come out through the sink as
   8-bit RGB the moment their line is complete.

   Modes: Robot 36 and 72, Martin 1 and 2, Scottie 1, 2 and DX, PD 50, 90,
   120, 160, 180 and 240. Timings from the published mode specifications
   (JL Barber, "Proposal for SSTV Mode Specifications", Dayton 2000). Own
   implementation.

   Pure C, no allocation; the struct is about 60 KB, so PSRAM. Output is a
   push-samples / emit-rows interface, ready to be a decoder plugin. */
#ifndef LS_SSTV_H
#define LS_SSTV_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SSTV_RATE      16000
#define SSTV_MAX_W     640
#define SSTV_RING      24576      /* 1.5 s: the slowest line and its search */

typedef enum {
    SSTV_SEG_SYNC = 0, SSTV_SEG_GAP,       /* gap: porch or separator       */
    SSTV_SEG_R, SSTV_SEG_G, SSTV_SEG_B,
    SSTV_SEG_Y, SSTV_SEG_Y2, SSTV_SEG_RY, SSTV_SEG_BY,
    SSTV_SEG_SEP,                          /* Robot 36: 1500 R-Y / 2300 B-Y  */
    SSTV_SEG_C,                            /* Robot 36: the chroma it named  */
} sstv_seg_kind_t;

typedef struct { uint8_t kind; float ms; } sstv_seg_t;

typedef enum { SSTV_RGB = 0, SSTV_ROBOT36, SSTV_YUV, SSTV_PD } sstv_family_t;

typedef struct {
    const char *name;
    uint8_t  vis;
    uint8_t  family;
    uint16_t width, lines;        /* lines: sync periods (PD: row pairs)   */
    float    pre_ms;              /* after VIS, before line 0 (Scottie)    */
    uint8_t  nseg;
    sstv_seg_t seg[10];
} sstv_mode_t;

const sstv_mode_t *sstv_mode_by_vis(uint8_t vis);
int sstv_mode_count(void);
const sstv_mode_t *sstv_mode_at(int i);
/* One line's length in ms, and image height in rows. */
float sstv_line_ms(const sstv_mode_t *m);
int sstv_height(const sstv_mode_t *m);

typedef struct {
    void (*begin)(void *user, const sstv_mode_t *m);
    void (*row)(void *user, int y, const uint8_t *rgb, int width);
    void (*end)(void *user, bool complete, int rows);
    void *user;
} sstv_sink_t;

typedef enum { SSTV_HUNT = 0, SSTV_IMAGE } sstv_phase_t;

typedef struct {
    sstv_phase_t phase;
    const sstv_mode_t *mode;
    int   rows;                   /* rows delivered                        */
    int   height;
    float slant_ppm;              /* the clock difference being corrected */
    int   syncs_found, syncs_missed;
    float tone_hz;                /* the audio's frequency just now        */
    float level;                  /* audio RMS, full scale = 1             */
    uint32_t images;
} sstv_status_t;

typedef struct {
    sstv_sink_t sink;
    /* Front end. */
    float nco_c, nco_s, rot_c, rot_s;
    int   nco_n;
    float fir[25];
    float zi[50], zq[50];
    int   zpos;
    float pi, pq;
    float lvl;
    uint16_t ring[SSTV_RING];     /* frequency, 1/16 Hz                     */
    int64_t  n;                   /* samples seen                           */
    /* 1 kHz frequency track for the VIS hunt. */
    uint16_t ms[1024];
    int64_t  nms;
    int      ms_acc_n;
    float    ms_acc;
    int64_t  vis_block_until;
    /* The picture. */
    sstv_phase_t phase;
    const sstv_mode_t *mode;
    double   t0, period, nominal;  /* line n starts at t0 + n * period       */
    double   seg_at[10];          /* offsets in samples                     */
    double   sync_at;
    int      line, rows, misses;
    /* Slant fit over (line, start). */
    double   sx, sy, sxx, sxy;
    int      npts;
    int      found, missed;
    uint8_t  chan[4][SSTV_MAX_W];  /* scans of the current line             */
    uint8_t  ry[SSTV_MAX_W], by[SSTV_MAX_W];
    uint8_t  rgb[SSTV_MAX_W * 3];
    float    slant_ppm;           /* of the picture now or the last one     */
    uint32_t images;
} sstv_t;

void sstv_init(sstv_t *d, const sstv_sink_t *sink);
/* Abandon a picture and hunt again (a retune). */
void sstv_reset(sstv_t *d);
void sstv_push(sstv_t *d, const int16_t *pcm, int n);
void sstv_status(const sstv_t *d, sstv_status_t *out);

#ifdef __cplusplus
}
#endif

#endif
