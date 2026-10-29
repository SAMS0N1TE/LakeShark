/* RDS / RBDS data layer: block sync on the 1187.5 bit/s stream, the
   (26,16) shortened cyclic check with burst correction, and group decode
   into a station card (PI, PTY, PS, RadioText, clock, AF list).

   Written from IEC 62106 (RDS) and NRSC-4-B (RBDS): annex B for the
   check word and offset words, section 3.1.5 for the group types used here.

   Pure C, no RTOS and no allocation: the caller owns the struct (PSRAM).
   Bits go in one at a time with rds_push_bit(); the optional group hook sees
   every group as it completes, so a later decoder-plugin head can forward
   groups without knowing this file. */
#ifndef LS_RDS_H
#define LS_RDS_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define RDS_BITRATE   1187.5f
#define RDS_PS_LEN    8
#define RDS_RT_LEN    64
#define RDS_AF_MAX    25

/* Block offsets, in the order a group carries them. C' replaces C in
   version B groups and is reported as RDS_BLK_C2. */
enum { RDS_BLK_A = 0, RDS_BLK_B, RDS_BLK_C, RDS_BLK_D, RDS_BLK_C2 };

/* The station card. Strings are NUL-terminated, printable ASCII only. */
typedef struct {
    uint32_t seq;            /* moves on every change below                 */
    bool     synced;
    bool     has_pi;
    uint16_t pi;
    uint8_t  pty;            /* 0..31; name via rds_pty_name()              */
    bool     tp, ta, music;
    char     ps[RDS_PS_LEN + 1];   /* last complete programme service name  */
    char     rt[RDS_RT_LEN + 1];   /* RadioText, trailing blanks trimmed    */
    bool     rt_complete;          /* every segment up to the end arrived   */
    uint8_t  af_n;
    uint8_t  af[RDS_AF_MAX];       /* AF codes: 87.5 + 0.1 * code MHz       */
    /* Clock-time group 4A: UTC as sent plus the station's local offset.  */
    bool     ct_valid;
    uint32_t ct_seq;               /* moves on every CT group               */
    int64_t  ct_unix;              /* UTC seconds, minute resolution        */
    int8_t   ct_offset_half;       /* local offset, half hours, signed      */
    uint32_t groups;
    uint32_t blocks_ok, blocks_fixed, blocks_bad;
    uint16_t type_seen;            /* bit per group type 0..15 heard        */
} rds_info_t;

typedef void (*rds_group_fn)(void *user, const uint16_t blk[4], uint8_t ok_mask);

typedef struct {
    rds_info_t info;
    /* Sync search and tracking. */
    uint32_t reg;            /* last 26 bits                                */
    uint32_t nbits;          /* bits seen                                   */
    uint32_t cand_at;        /* bit count of the last valid block in search */
    int8_t   cand_blk;
    bool     synced;
    uint8_t  next_blk;       /* A..D expected next                          */
    uint8_t  bitpos;         /* bits into the current block                 */
    uint8_t  bad_run;        /* consecutive uncorrectable blocks            */
    uint8_t  bad_hist;       /* of the last 16 blocks, how many were bad    */
    uint16_t hist_bits;
    /* The group being assembled. */
    uint16_t blk[4];
    uint8_t  ok;
    /* PS and RT under construction. */
    char     ps_work[RDS_PS_LEN];
    uint8_t  ps_mask;
    char     rt_work[RDS_RT_LEN];
    uint64_t rt_mask;        /* character cells received                    */
    int8_t   rt_ab;
    int8_t   rt_end;         /* index of the 0x0D terminator, -1 none       */
    int8_t   rt_len_b;       /* 2B text is 32 long                          */
    rds_group_fn on_group;
    void    *user;
} rds_t;

void rds_init(rds_t *r, rds_group_fn on_group, void *user);
/* Drop sync and the card (a retune). */
void rds_reset(rds_t *r);
/* One differentially decoded data bit, oldest first. */
void rds_push_bit(rds_t *r, int bit);
/* A whole group, already blocked and checked (tests, other front ends). */
void rds_decode_group(rds_t *r, const uint16_t blk[4], uint8_t ok_mask);

/* The check word for 16 data bits, without any offset (annex B). */
uint16_t rds_check(uint16_t data);
/* The offset word of a block: A, B, C, D, C' (RDS_BLK_*). */
uint16_t rds_offset(int blk);
/* 26-bit block for `data` at `blk` (tests and the synthesiser). */
uint32_t rds_block(uint16_t data, int blk);

/* PTY names, at most 10 characters. North American RBDS and European RDS
   number the programme types differently. */
const char *rds_pty_name(uint8_t pty, bool rbds);
/* RBDS call letters from a PI (K/W four-letter calls), "" when the PI is not
   a call-sign code. out holds at least 5. */
void rds_callsign(uint16_t pi, char *out);
/* AF code to kHz, 0 for a code that is not a frequency. */
uint32_t rds_af_khz(uint8_t code);

#ifdef __cplusplus
}
#endif

#endif
