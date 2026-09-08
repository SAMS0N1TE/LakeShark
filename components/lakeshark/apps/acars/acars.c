
#include "acars.h"
#include "acars_msk.h"

#include "esp_timer.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "ls_time.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static const char *TAG = "acars";

/* Wire constants, mirrored from bench/fixtures/acars_gen.  Duplicated on
   purpose - the decoder is what runs on device, the fixture is host-only,
   and the contract between them is these constants. */
#define ACARS_SOH    0x01
#define ACARS_STX    0x02
#define ACARS_ETX    0x03
#define ACARS_SYN    0x16
#define ACARS_DEL    0x7F

/* Fixed header length between SOH and STX exclusive of both:
   mode(1) + reg(7) + tak(1) + label(2) + block_id(1) = 12. */
#define HDR_FIXED_LEN 12

/* Sync pattern for two SYN bytes with odd parity, transmitted LSB bit first.
   0x16 = 0b00010110 has 3 ones (odd), so parity = 0.  LSB-first byte:
   0,1,1,0,1,0,0,0 (parity = last bit).  Two of them shifted into a 16-bit
   register with the newest bit at bit 0 give 0x6868. */
#define SYNC_PATTERN  0x6868u
#define SYNC_MASK     0xFFFFu

typedef enum {
    ST_HUNT = 0,        /* shifting bits, correlating against SYNC_PATTERN */
    ST_HEADER,          /* have byte alignment, reading SOH + fixed header */
    ST_TEXT,            /* between STX and ETX, gathering payload          */
    ST_BCS,             /* reading 16 raw BCS bits after ETX               */
    ST_DEL,             /* reading trailing DEL (parity byte)              */
} fstate_t;

struct acars_ctx {
    acars_state_t   *out;
    acars_msk_t     *msk;

    /* Sync hunt.  A 16-bit shift register into which the newest bit lands
       at bit 0.  We compare against SYNC_PATTERN and its complement so a
       receiver with inverted audio polarity still finds sync. */
    uint32_t         sr;
    int              invert;

    fstate_t         st;

    /* Byte assembly - once byte-aligned, we accumulate 8 bits then
       consume the resulting byte. */
    uint8_t          byte_bits;
    int              bit_idx;

    /* Frame accumulator. */
    uint8_t          hdr[HDR_FIXED_LEN];
    int              hdr_idx;
    uint8_t          text_raw[ACARS_TEXT_MAX + 1];
    int              text_len;
    uint16_t         crc_running;
    uint16_t         bcs_observed;
    int              bcs_bits;
    int              parity_errors;

    /* Diagnostics. */
    bool             synced;
    uint32_t         n_synced;
    uint32_t         n_pages;
    uint32_t         n_bad_crc;
};

/* CRC-16-CCITT, poly 0x1021, initial 0xFFFF, no reflection.  Runs on the
   low 7 bits of each byte only - the parity bit is deliberately excluded
   so that a per-character parity hit does not force a whole-message drop.
   Contract mirrored in bench/fixtures/acars_gen.c::acars_crc16. */
static uint16_t crc16_update(uint16_t crc, uint8_t b)
{
    crc ^= (uint16_t)(b & 0x7Fu) << 8;
    for (int k = 0; k < 8; k++)
        crc = (crc & 0x8000u) ? (uint16_t)((crc << 1) ^ 0x1021u)
                              : (uint16_t)(crc << 1);
    return crc;
}

static int popcount8(uint8_t v)
{
    int n = 0; while (v) { v &= (uint8_t)(v - 1); n++; } return n;
}

static int byte_parity_ok(uint8_t byte)
{
    /* Odd parity: total 1-count over all 8 bits is odd. */
    return (popcount8(byte) & 1) == 1;
}

static void reset_hunt(acars_ctx_t *c)
{
    c->st = ST_HUNT;
    c->sr = 0;
    c->invert = 0;
    c->byte_bits = 0;
    c->bit_idx = 0;
    c->hdr_idx = 0;
    c->text_len = 0;
    c->crc_running = 0xFFFFu;
    c->bcs_observed = 0;
    c->bcs_bits = 0;
    c->parity_errors = 0;
    c->synced = false;
}

acars_ctx_t *acars_create(acars_state_t *out)
{
    /* LS-725: this task-only frame state used plain calloc immediately
       before FM allocated its internal 16 KiB receive stack.  Prefer PSRAM
       explicitly; boards without usable PSRAM retain an explicit 8-bit
       internal fallback.  Together with acars_msk this removes both decoder
       payloads plus allocator metadata from entry-time internal-heap demand. */
    acars_ctx_t *c = (acars_ctx_t *)heap_caps_calloc(
        1, sizeof(*c), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!c) c = (acars_ctx_t *)heap_caps_calloc(
        1, sizeof(*c), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!c) return NULL;
    c->out = out;
    c->msk = acars_msk_create();
    if (!c->msk) { heap_caps_free(c); return NULL; }
    acars_reset(c);
    return c;
}

void acars_destroy(acars_ctx_t *c)
{
    if (!c) return;
    if (c->msk) acars_msk_destroy(c->msk);
    heap_caps_free(c);
}

void acars_reset(acars_ctx_t *c)
{
    if (!c) return;
    if (c->msk) acars_msk_reset(c->msk);
    reset_hunt(c);
    c->n_synced = c->n_pages = c->n_bad_crc = 0;
}

bool     acars_synced   (const acars_ctx_t *c) { return c && c->synced; }
uint32_t acars_n_synced (const acars_ctx_t *c) { return c ? c->n_synced  : 0; }
uint32_t acars_n_pages  (const acars_ctx_t *c) { return c ? c->n_pages   : 0; }
uint32_t acars_n_bad_crc(const acars_ctx_t *c) { return c ? c->n_bad_crc : 0; }

/* Emit the assembled message to the ring buffer.  crc_ok == false frames
   are never emitted (they would put fiction on screen); this function is
   only called after the CRC has passed. */
static void deliver(acars_ctx_t *c)
{
    acars_state_t *o = c->out;
    if (!o) return;

    acars_msg_out_t msg;
    memset(&msg, 0, sizeof(msg));
    msg.ts_us    = esp_timer_get_time();
    msg.ts_epoch = ls_time_is_synced() ? (int64_t)time(NULL) : 0;
    msg.mode     = (char)(c->hdr[0] & 0x7Fu);
    for (int i = 0; i < 7; i++)
        msg.reg[i] = (char)(c->hdr[1 + i] & 0x7Fu);
    msg.reg[7]  = '\0';
    msg.tak     = (char)(c->hdr[8] & 0x7Fu);
    msg.label[0] = (char)(c->hdr[9]  & 0x7Fu);
    msg.label[1] = (char)(c->hdr[10] & 0x7Fu);
    msg.label[2] = '\0';
    msg.block_id = (char)(c->hdr[11] & 0x7Fu);

    int tn = c->text_len;
    if (tn > ACARS_TEXT_MAX) tn = ACARS_TEXT_MAX;
    for (int i = 0; i < tn; i++)
        msg.text[i] = (char)(c->text_raw[i] & 0x7Fu);
    msg.text[tn] = '\0';
    msg.text_len = tn;

    msg.bcs           = c->bcs_observed;
    msg.parity_errors = c->parity_errors;
    msg.crc_ok        = true;

    o->msgs[o->msg_head] = msg;
    o->msg_head = (o->msg_head + 1) % ACARS_MSG_LOG_MAX;
    if (o->msg_count < ACARS_MSG_LOG_MAX) o->msg_count++;
    o->n_delivered++;
    if (c->parity_errors > 0) o->n_parity_err += (uint32_t)c->parity_errors;

    c->n_pages++;
    ESP_LOGI(TAG, "acars msg reg=%s label=%s text=[%s]",
             msg.reg, msg.label, msg.text);
}

/* Consume a byte through the framer.  Returns 1 to keep processing, 0 to
   stop (used when we bail back to hunt). */
static void framer_byte(acars_ctx_t *c, uint8_t b)
{
    if (c->st == ST_HEADER) {
        /* hdr_idx counts bytes consumed in ST_HEADER, from SOH onwards.
             0        expect SOH
             1..12    expect header bytes (mode, reg[7], tak, label[2], blk)
             13       expect STX, transition to ST_TEXT
           A parity failure on SOH or STX drops us back to hunt - those
           carry no ambiguity because they're single fixed bytes.  A parity
           failure inside the addressing header is counted but preserved,
           so a message with a single-bit hit on the label still reaches
           display with the error count visible. */
        if (c->hdr_idx == 0) {
            if (!byte_parity_ok(b) || (b & 0x7Fu) != ACARS_SOH) {
                reset_hunt(c);
                return;
            }
            c->crc_running = crc16_update(c->crc_running, b);
            c->hdr_idx = 1;
            return;
        }
        if (c->hdr_idx <= HDR_FIXED_LEN) {
            if (!byte_parity_ok(b)) c->parity_errors++;
            c->hdr[c->hdr_idx - 1] = b;
            c->crc_running = crc16_update(c->crc_running, b);
            c->hdr_idx++;
            return;
        }
        /* hdr_idx == HDR_FIXED_LEN + 1: expect STX. */
        if (!byte_parity_ok(b) || (b & 0x7Fu) != ACARS_STX) {
            reset_hunt(c);
            return;
        }
        c->crc_running = crc16_update(c->crc_running, b);
        c->st = ST_TEXT;
        c->text_len = 0;
        return;
    }

    if (c->st == ST_TEXT) {
        /* Text bytes until ETX.  ETX itself is included in the CRC.  A
           byte with bad parity in text is counted but preserved - callers
           can see the parity error count and decide what to display.  A
           byte with the ETX ASCII code but bad parity is treated as a
           text byte, not as a terminator, because we would not know
           whether the bit error hit the parity or the payload. */
        if (byte_parity_ok(b) && (b & 0x7Fu) == ACARS_ETX) {
            c->crc_running = crc16_update(c->crc_running, b);
            c->st = ST_BCS;
            c->bcs_observed = 0;
            c->bcs_bits = 0;
            return;
        }
        if (!byte_parity_ok(b)) c->parity_errors++;
        if (c->text_len < ACARS_TEXT_MAX)
            c->text_raw[c->text_len++] = b;
        c->crc_running = crc16_update(c->crc_running, b);
        /* Runaway text without an ETX would sit here forever.  Cap at
           ACARS_TEXT_MAX and bail if we go past. */
        if (c->text_len >= ACARS_TEXT_MAX) {
            reset_hunt(c);
        }
        return;
    }

    /* Unreachable - ST_HUNT / ST_BCS / ST_DEL are handled at bit level. */
}

/* Framer at bit granularity: assembles bytes and runs the state machine. */
static void framer_bit(acars_ctx_t *c, int bit)
{
    bit &= 1;

    if (c->st == ST_HUNT) {
        /* Shift into a 16-bit register, newest bit at bit 0. */
        c->sr = ((c->sr << 1) & 0xFFFFu) | (uint32_t)bit;
        if ((c->sr & SYNC_MASK) == SYNC_PATTERN) {
            c->invert = 0;
            goto locked;
        }
        if (((~c->sr) & SYNC_MASK) == SYNC_PATTERN) {
            c->invert = 1;
            goto locked;
        }
        return;
locked:
        c->synced = true;
        c->n_synced++;
        if (c->out) c->out->n_synced++;
        c->st = ST_HEADER;
        c->byte_bits = 0;
        c->bit_idx = 0;
        c->hdr_idx = 0;
        c->text_len = 0;
        c->crc_running = 0xFFFFu;
        c->bcs_observed = 0;
        c->bcs_bits = 0;
        c->parity_errors = 0;
        return;
    }

    int b = c->invert ? (bit ^ 1) : bit;

    if (c->st == ST_BCS) {
        /* 16 raw bits after ETX carrying the CRC.  MSB byte first (crc_hi
           then crc_lo), each byte LSB bit first - same bit order as every
           other byte in the stream, so the timing loop does not have to
           switch modes.  Assemble one byte at a time. */
        c->byte_bits |= (uint8_t)((b & 1) << c->bit_idx);
        c->bit_idx++;
        if (c->bit_idx >= 8) {
            if (c->bcs_bits == 0) {
                c->bcs_observed = (uint16_t)((uint16_t)c->byte_bits << 8);
            } else {
                c->bcs_observed = (uint16_t)(c->bcs_observed | c->byte_bits);
            }
            c->bcs_bits += 8;
            c->byte_bits = 0;
            c->bit_idx = 0;

            if (c->bcs_bits >= 16) {
                if (c->bcs_observed == c->crc_running) {
                    c->st = ST_DEL;
                } else {
                    c->n_bad_crc++;
                    if (c->out) c->out->n_bad_crc++;
                    reset_hunt(c);
                }
            }
        }
        return;
    }

    if (c->st == ST_DEL) {
        /* 8 more LSB-first bits carrying DEL (0x7F with odd parity).  We
           do not fail the message if DEL is corrupted - by this point the
           CRC has already vouched for the payload.  A missing DEL just
           means the trailing marker was lost. */
        c->byte_bits |= (uint8_t)((b & 1) << c->bit_idx);
        c->bit_idx++;
        if (c->bit_idx >= 8) {
            deliver(c);
            reset_hunt(c);
        }
        return;
    }

    /* ST_HEADER / ST_TEXT: assemble bytes LSB bit first. */
    c->byte_bits |= (uint8_t)((b & 1) << c->bit_idx);
    c->bit_idx++;
    if (c->bit_idx >= 8) {
        uint8_t byte = c->byte_bits;
        c->byte_bits = 0;
        c->bit_idx = 0;
        framer_byte(c, byte);
    }
}

void acars_process(acars_ctx_t *c, const float *audio, int n)
{
    if (!c || !audio || n <= 0) return;

    acars_msk_process(c->msk, audio, n);

    /* Drain the MSK slicer's bit ring in chunks. */
    uint8_t bits[128];
    for (;;) {
        int got = acars_msk_read_bits(c->msk, bits, (int)sizeof(bits));
        if (got <= 0) break;
        for (int i = 0; i < got; i++) framer_bit(c, bits[i]);
    }
}

/* Bookkeeping updates for acars_state_t.n_synced live inside framer_bit's
   sync-locked branch above rather than here; the ctx's counters mirror
   the state's. */
