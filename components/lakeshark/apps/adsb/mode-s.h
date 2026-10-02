/* Derived from watson/libmodes (dump1090); see LICENSE.libmodes.
 * Local changes: PSRAM magnitude storage, preamble callback, buffer and retry
 * guards, and known-address gating for corrected messages.
 *
 * Mode1090, a Mode S messages decoder for RTLSDR devices.
 *
 * Copyright (C) 2012 by Salvatore Sanfilippo <antirez@gmail.com>
 *
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are
 * met:
 *
 *  *  Redistributions of source code must retain the above copyright
 *     notice, this list of conditions and the following disclaimer.
 *
 *  *  Redistributions in binary form must reproduce the above copyright
 *     notice, this list of conditions and the following disclaimer in the
 *     documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 * HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */
#include <string.h>
#include <stdlib.h>
#include <stdint.h>
#include <unistd.h>
#include <math.h>
#include <sys/time.h>

#define MODE_S_ICAO_CACHE_LEN 1024
#define MODE_S_LONG_MSG_BYTES (112 / 8)
#define MODE_S_UNIT_FEET 0
#define MODE_S_UNIT_METERS 1

typedef struct
{

    uint32_t icao_cache[sizeof(uint32_t) * MODE_S_ICAO_CACHE_LEN * 2];

    int fix_errors;
    int aggressive;
    int check_crc;

    void (*on_preamble)(void);
} mode_s_t;

struct mode_s_msg
{

    unsigned char msg[MODE_S_LONG_MSG_BYTES];
    int msgbits;
    int msgtype;
    int crcok;
    uint32_t crc;
    int errorbit;
    int aa1, aa2, aa3;
    int phase_corrected;

    int ca;

    /* DF18 only: the control field, which says what kind of message it is. */
    int cf;
    /* The ME field is laid out as an ADS-B one and was decoded below: always
       for DF17, for DF18 only where the control field and type code allow. */
    int me_ok;
    /* The address in aa1..aa3 is not an ICAO address (DF18 CF=1 and 5, or
       a TIS-B/ADS-R message whose IMF bit says so), so it must not be taken
       as one. */
    int addr_nonicao;
    /* The message is a ground station's relay (TIS-B, ADS-R), not the
       aircraft's own transmission. */
    int rebroadcast;

    int metype;
    int mesub;
    int heading_is_valid;
    int heading;
    int aircraft_type;
    int fflag;
    int tflag;
    int raw_latitude;
    int raw_longitude;
    char flight[9];
    int ew_dir;
    int ew_velocity;
    int ns_dir;
    int ns_velocity;
    int vert_rate_source;
    int vert_rate_sign;
    int vert_rate;
    int velocity;

    int fs;
    int dr;
    int um;
    int identity;

    int altitude, unit;
};

typedef void (*mode_s_callback_t)(mode_s_t *self, struct mode_s_msg *mm);

void mode_s_init(mode_s_t *self);
void mode_s_compute_magnitude_vector(unsigned char *data, uint16_t *mag, uint32_t size);
void mode_s_detect(mode_s_t *self, uint16_t *mag, uint32_t maglen, mode_s_callback_t);
void mode_s_decode(mode_s_t *self, struct mode_s_msg *mm, unsigned char *msg);
void runme();
int mode_s_msg_len_by_type(int type);

/* Mode S frames from the LR2021 OOK packet engine: the bytes it delivers
   after the 0x0285 preamble, 2 chips per bit (PPM: chips 10 = 1, 01 = 0). */
#define MODE_S_CHIP_BYTES_SHORT 14   /* 56 bits * 2 chips / 8 */
#define MODE_S_CHIP_BYTES_LONG  28   /* 112 bits * 2 chips / 8 */

typedef struct
{
    int bad_pairs;      /* 00 or 11 pairs among the first 56 bits: what mode_s_detect rejects on */
    int bad_pairs_tail; /* the same beyond bit 56 of a 112-bit frame; the CRC judges those */
    int first_bad_bit;  /* message bit index of the first bad pair, -1 if none */
    uint8_t bad[MODE_S_LONG_MSG_BYTES]; /* bit set where the pair was bad, msg layout */
} mode_s_chip_info_t;

/* head_bits/head_value: leading message bits the detector pattern already
   consumed (0 for the plain 0x0285 preamble; 4 and 0x8 = "1000" for a
   DF17-assisted pattern). Returns 56 or 112, the length the DF field names,
   with msg filled and zero beyond it; or -1 for bad arguments or fewer chips
   than that frame needs. Bad pairs do not fail the call: they are counted in
   info (may be NULL) and the bit takes the pair's first chip. */
int mode_s_msg_from_chips(const uint8_t *chips, int chip_bytes,
                          int head_bits, uint8_t head_value,
                          uint8_t msg[MODE_S_LONG_MSG_BYTES],
                          mode_s_chip_info_t *info);
