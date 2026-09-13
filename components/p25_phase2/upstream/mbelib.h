/*
 * Copyright (C) 2010 mbelib Author
 * GPG Key ID: 0xEA5EFE2C (9E7A 5527 9CDC EBF7 BF1B  D772 4F98 E863 EA5E FE2C)
 *
 * Permission to use, copy, modify, and/or distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND ISC DISCLAIMS ALL WARRANTIES WITH
 * REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF MERCHANTABILITY
 * AND FITNESS.  IN NO EVENT SHALL ISC BE LIABLE FOR ANY SPECIAL, DIRECT,
 * INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES WHATSOEVER RESULTING FROM
 * LOSS OF USE, DATA OR PROFITS, WHETHER IN AN ACTION OF CONTRACT, NEGLIGENCE
 * OR OTHER TORTIOUS ACTION, ARISING OUT OF OR IN CONNECTION WITH THE USE OR
 * PERFORMANCE OF THIS SOFTWARE.
 */

#ifndef _MBELIB_H
#define _MBELIB_H

#define MBELIB_VERSION "1.2.3"

/* #include "config.h" */
#include <stdio.h>
#include <stdint.h>
#include <math.h>

#define M_1_PI 0.31830988618379067154 /* 1/pi */
#define M_2_PI 0.63661977236758134308 /* 2/pi */
#define M_2_SQRTPI 1.12837916709551257390 /* 2/\fBsqrt\fP(pi) */
#define M_E 2.7182818284590452354
#define M_LN10 2.30258509299404568402 /* log_e 10 */
#define M_LN2 0.69314718055994530942 /* log_e 2 */
#define M_LOG10E 0.43429448190325182765 /* log_10 e */
#define M_LOG2E 1.4426950408889634074 /* log_2 e */
#define M_PI 3.14159265358979323846 /* pi */
#define M_PI_2 1.57079632679489661923 /* pi/2 */
#define M_PI_4 0.78539816339744830962 /* pi/4 */
#define M_SQRT1_2 0.70710678118654752440 /* 1/\fBsqrt\fP(2) */
#define M_SQRT2 1.41421356237309504880 /* \fBsqrt\fP(2) */

struct ls_p2_mbe_parameters
{
  uint32_t noise_state;
  float w0;
  int L;
  int K;
  int Vl[57];
  float Ml[57];
  float log2Ml[57];
  float PHIl[57];
  float PSIl[57];
  float gamma;
  int un;
  int repeat;
};

struct ls_p2_mbe_tones
{
  int ID;
  int AD;
  int n;
};

struct ls_p2_mbe_errors
{
  int E0;
  int E1;
  double ER;
};

typedef struct ls_p2_mbe_parameters ls_p2_mbe_parms;
typedef struct ls_p2_mbe_tones ls_p2_mbe_tone;
typedef struct ls_p2_mbe_errors ls_p2_mbe_errs;

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Prototypes from mbelib.c
 */
void ls_p2_mbe_printVersion (char *str);
void ls_p2_mbe_moveMbeParms (ls_p2_mbe_parms * cur_mp, ls_p2_mbe_parms * prev_mp);
void ls_p2_mbe_useLastMbeParms (ls_p2_mbe_parms * cur_mp, ls_p2_mbe_parms * prev_mp);
void ls_p2_mbe_initMbeParms (ls_p2_mbe_parms * cur_mp, ls_p2_mbe_parms * prev_mp, ls_p2_mbe_parms * prev_mp_enhanced);
void ls_p2_mbe_initToneParms (ls_p2_mbe_tone * tone_mp);
void ls_p2_mbe_initErrParms (ls_p2_mbe_errs * errs_mp);
void ls_p2_mbe_spectralAmpEnhance (ls_p2_mbe_parms * cur_mp);
void ls_p2_mbe_synthesizeSilencef (float *aout_buf);
void ls_p2_mbe_synthesizeSilence (short *aout_buf);
void ls_p2_mbe_synthesizeSpeechf (float *aout_buf, ls_p2_mbe_parms * cur_mp, ls_p2_mbe_parms * prev_mp, int uvquality);
void ls_p2_mbe_synthesizeSpeech (short *aout_buf, ls_p2_mbe_parms * cur_mp, ls_p2_mbe_parms * prev_mp, int uvquality);
void ls_p2_mbe_floattoshort (float *float_buf, short *aout_buf);

#ifdef __cplusplus
}
#endif
#endif
