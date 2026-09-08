
#ifndef ACARS_H
#define ACARS_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Audio-band sample rate the decoder consumes.  Also mirrored in
   bench/fixtures/acars_gen.h - the contract between fixture and decoder is
   these constants.  A device-side consumer resamples FM audio to this rate
   before handing samples to acars_process(). */
#define ACARS_SAMP_RATE     19200
#define ACARS_BAUD           2400
#define ACARS_TONE_MARK      1200      /* bit '1' */
#define ACARS_TONE_SPACE     2400      /* bit '0' */

/* Text field cap; matches acars_gen.h. */
#define ACARS_TEXT_MAX        220

#define ACARS_MSG_LOG_MAX       8

typedef struct {
    int64_t   ts_us;              /* uptime, always present within one boot */
    int64_t   ts_epoch;           /* wall clock at decode, 0 if not synced  */
    char      mode;
    char      reg[8];             /* aircraft registration, 7 chars + NUL   */
    char      tak;                /* technical acknowledgement byte         */
    char      label[3];           /* 2-char message label + NUL             */
    char      block_id;
    char      text[ACARS_TEXT_MAX + 1];
    int       text_len;
    uint16_t  bcs;                /* observed CRC, for diagnostics          */
    int       parity_errors;      /* count of characters with bad parity    */
    bool      crc_ok;             /* CRC matched; false frames never leak   */
} acars_msg_out_t;

typedef struct {
    int              msg_head;
    int              msg_count;
    acars_msg_out_t  msgs[ACARS_MSG_LOG_MAX];

    /* Diagnostics.  msg_count is the head of the ring; these are cumulative
       across the ring rollover so a caller can tell "we saw N frames, of
       which K passed CRC" without needing to inspect the ring. */
    uint32_t         n_synced;
    uint32_t         n_delivered;
    uint32_t         n_bad_crc;
    uint32_t         n_parity_err;
} acars_state_t;

typedef struct acars_ctx acars_ctx_t;

acars_ctx_t *acars_create (acars_state_t *out);
void         acars_destroy(acars_ctx_t *c);
void         acars_reset  (acars_ctx_t *c);

/* Feed audio samples at ACARS_SAMP_RATE.  Any complete message is written
   to the ring buffer in the acars_state_t handed at create-time. */
void         acars_process(acars_ctx_t *c, const float *audio, int n);

bool         acars_synced   (const acars_ctx_t *c);
uint32_t     acars_n_synced (const acars_ctx_t *c);
uint32_t     acars_n_pages  (const acars_ctx_t *c);
uint32_t     acars_n_bad_crc(const acars_ctx_t *c);

#ifdef __cplusplus
}
#endif

#endif
