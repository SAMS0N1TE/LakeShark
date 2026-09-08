/* LS-650: P25 Phase 1 Link Control Word parser.
 *
 * Every LDU1 and TDULC carries a 72-bit LCW after the Reed-Solomon(24,12,13)
 * FEC upstream in processLDU1 / processTDULC. Before this file, the whole
 * dispatcher was one function that recognised LCO 0x00 (Group Voice Channel
 * User) and a mis-attributed LCO 0x04 branch, wrote raw talkgroup/source
 * into state->lasttg / lastsrc without reading any service-option flag, and
 * ran even when Reed-Solomon reported an irrecoverable error - so a corrupt
 * LCW could relabel the call and, if a stale-TG check downstream had
 * triggered, could evict the ESS from a legitimate encrypted call.
 *
 * The LCW is the late-entry path. On a followed grant the traffic channel is
 * decoded with no preceding HDU or grant - the follower simply retuned - so
 * until an LDU1 arrives the call has no identity. On a brief control-channel
 * loss the LCW is what keeps the call labelled.
 *
 * Layout of the 72-bit LCW as it lands here (bit strings are MSB-first ASCII
 * '0'/'1' out of the LDU1/TDULC hex-word extraction; the byte form is what
 * bench tests drive):
 *   lcformat  8 bits   LCO  (protection + standard-MFID flag in top 2 bits,
 *                            opcode in low 6 bits per TIA-102.AABF)
 *   mfid      8 bits   Manufacturer ID (0x00 or 0x01 for standard messages;
 *                      vendor blocks re-purpose the payload and are counted
 *                      rather than parsed)
 *   lcinfo   56 bits   Payload, layout is LCO-specific.
 *
 * LCO 0x00  Group Voice Channel User:
 *   svcopt  8   Service Options: E, P, D, M, R, priority(3)
 *   rsvd    8
 *   group  16   Talkgroup
 *   src    24   Source radio ID
 *
 * LCO 0x03  Unit-to-Unit Voice Channel User:
 *   svcopt  8   Service Options
 *   target 24   Target radio ID
 *   src    24   Source radio ID
 *
 * LCO 0x0A  Group Regroup Voice Channel User (patch / super-group):
 *   svcopt  8   Service Options
 *   rsvd    8
 *   sg     16   Super Group ID (regrouped talkgroup)
 *   src    24   Source radio ID
 *
 * Talker Alias (LCO 0x15/0x16/0x17/0x18):
 *   0x15  Header: format(8), length(8), reserved(40)
 *   0x16  Block 1: 7 chars (ASCII 8-bit)
 *   0x17  Block 2: 7 chars
 *   0x18  Block 3: 7 chars
 *   Max alias 21 characters; truncated sequence drops on
 *   P25_LCW_ALIAS_TIMEOUT_US (2 s) from the header.
 */

#include <string.h>

#include "dsd.h"

/* Service Options bit numbering per TIA-102.AABF, MSB-first (bit 7 = MSB). */
#define P25_SVC_EMERGENCY_MASK 0x80u
#define P25_SVC_ENCRYPTED_MASK 0x40u
#define P25_SVC_PRIORITY_MASK  0x07u

/* Standard MFIDs. Everything else is vendor and gets counted, not parsed -
 * a vendor 0x00 opcode is not GRP_V_CH_USER, and parsing it as one would
 * relabel the call from a fabricated field pick. */
#define P25_LCW_MFID_STD_A 0x00u
#define P25_LCW_MFID_STD_B 0x01u

/* LC opcodes, low 6 bits of the LCO byte. */
enum {
    P25_LCO_GROUP_VOICE_USER   = 0x00,
    P25_LCO_UNIT_UNIT_VOICE    = 0x03,
    P25_LCO_GROUP_REGROUP      = 0x0A,
    P25_LCO_TALKER_ALIAS_HDR   = 0x15,
    P25_LCO_TALKER_ALIAS_BLK1  = 0x16,
    P25_LCO_TALKER_ALIAS_BLK2  = 0x17,
    P25_LCO_TALKER_ALIAS_BLK3  = 0x18,
};

#define P25_LCW_ALIAS_BLOCK_CHARS 7
#define P25_LCW_ALIAS_MAX_CHARS   21 /* three blocks fit in p25_lcw_alias[32] */

static uint16_t lcw_u16(const uint8_t *b)
{
    return (uint16_t)(((uint16_t)b[0] << 8) | b[1]);
}

static uint32_t lcw_u24(const uint8_t *b)
{
    return ((uint32_t)b[0] << 16) | ((uint32_t)b[1] << 8) | (uint32_t)b[2];
}

static void parse_service_options(dsd_state *state, uint8_t svc)
{
    state->p25_lcw_svc_options = svc;
    state->p25_lcw_emergency   = (svc & P25_SVC_EMERGENCY_MASK) ? 1u : 0u;
    state->p25_lcw_encrypted   = (svc & P25_SVC_ENCRYPTED_MASK) ? 1u : 0u;
    state->p25_lcw_priority    = (uint8_t)(svc & P25_SVC_PRIORITY_MASK);
}

static void alias_reset(dsd_state *state)
{
    state->p25_lcw_alias_expected = 0;
    state->p25_lcw_alias_blocks   = 0;
    state->p25_lcw_alias_len      = 0;
    state->p25_lcw_alias_ready    = 0;
    state->p25_lcw_alias_start_us = 0;
    state->p25_lcw_alias[0]       = 0;
}

static void alias_check_timeout(dsd_state *state, int64_t now_us)
{
    /* "Active but not yet ready" is what we time out. Anchoring on
     * alias_expected rather than alias_start_us lets now_us == 0 be a legal
     * bench timestamp; using a zero start time as the sentinel would leave
     * the state machine unable to time out any sequence that started at 0. */
    if (state->p25_lcw_alias_expected == 0) return;
    if (state->p25_lcw_alias_ready) return;
    if (now_us - state->p25_lcw_alias_start_us > P25_LCW_ALIAS_TIMEOUT_US)
        alias_reset(state);
}

/* Copy up to `count` printable characters from `src` into the alias buffer at
 * `offset`. Non-printable bytes stop the copy - a lot of vendors pad short
 * blocks with 0x00 or 0xFF and continuing past them would spray the alias
 * with control codes. */
static uint8_t alias_copy_block(dsd_state *state, uint8_t offset,
                                const uint8_t *src, uint8_t count)
{
    uint8_t written = 0;
    for (uint8_t i = 0; i < count; i++) {
        if (offset + written >= P25_LCW_ALIAS_MAX_CHARS) break;
        uint8_t c = src[i];
        if (c < 0x20 || c > 0x7E) break;
        state->p25_lcw_alias[offset + written] = (char)c;
        written++;
    }
    state->p25_lcw_alias[offset + written] = 0;
    return written;
}

static void handle_talker_alias_hdr(dsd_state *state, const uint8_t *lcinfo,
                                    int64_t now_us)
{
    /* A new header always restarts the sequence - the header carries the
     * length and everything before was either a stale run or a truncated
     * one. Bench: a truncated sequence times out before the next header. */
    alias_reset(state);
    uint8_t length = lcinfo[1];
    if (length == 0 || length > P25_LCW_ALIAS_MAX_CHARS)
        length = P25_LCW_ALIAS_MAX_CHARS;
    state->p25_lcw_alias_expected = length;
    state->p25_lcw_alias_blocks   = 0x01u; /* bit0 = header */
    state->p25_lcw_alias_start_us = now_us;
}

static void handle_talker_alias_block(dsd_state *state, uint8_t block,
                                      const uint8_t *lcinfo)
{
    /* A block before a header is meaningless - two consecutive TA_BLK1s
     * without a header cannot say "this is a fresh sequence" without one,
     * and combining them would produce a random-looking alias. */
    if (state->p25_lcw_alias_expected == 0) return;
    uint8_t bit = (uint8_t)(1u << block); /* block 1 => 0x02, 2 => 0x04, 3 => 0x08 */
    if (state->p25_lcw_alias_blocks & bit) return; /* duplicate */
    uint8_t offset = (uint8_t)((block - 1) * P25_LCW_ALIAS_BLOCK_CHARS);
    if (offset >= state->p25_lcw_alias_expected) return;
    uint8_t remaining = (uint8_t)(state->p25_lcw_alias_expected - offset);
    uint8_t take = remaining < P25_LCW_ALIAS_BLOCK_CHARS
                 ? remaining : P25_LCW_ALIAS_BLOCK_CHARS;
    uint8_t written = alias_copy_block(state, offset, lcinfo, take);
    state->p25_lcw_alias_blocks |= bit;
    if (offset + written > state->p25_lcw_alias_len)
        state->p25_lcw_alias_len = (uint8_t)(offset + written);
    if (state->p25_lcw_alias_len >= state->p25_lcw_alias_expected)
        state->p25_lcw_alias_ready = 1;
}

void p25_lcw_call_clear(dsd_state *state)
{
    if (!state) return;
    state->p25_lcw_valid            = 0;
    state->p25_lcw_lco              = 0;
    state->p25_lcw_mfid             = 0;
    state->p25_lcw_svc_options      = 0;
    state->p25_lcw_emergency        = 0;
    state->p25_lcw_encrypted        = 0;
    state->p25_lcw_priority         = 0;
    state->p25_lcw_is_unit_to_unit  = 0;
    state->p25_lcw_is_regroup       = 0;
    state->p25_lcw_talkgroup        = 0;
    state->p25_lcw_source           = 0;
    state->p25_lcw_target           = 0;
    state->p25_lcw_patch_sg         = 0;
    alias_reset(state);
}

int p25_lcw_dispatch(dsd_state *state, uint8_t lco, uint8_t mfid,
                     const uint8_t lcinfo[P25_LCW_LCINFO_BYTES],
                     int fec_ok, int64_t now_us)
{
    if (!state || !lcinfo) return 0;

    /* A failed LCW must change nothing. Acting on a corrupt group ID would
     * relabel the call wrongly and, downstream, could trip the "talkgroup
     * changed - drop the ESS" check and mute a legitimate encrypted call. */
    if (!fec_ok) {
        state->p25_lcw_fec_reject_count++;
        return 0;
    }

    /* Talker alias timeout: run every LCW so a truncated header sequence
     * eventually clears even if only the wrong LCO keeps arriving. */
    alias_check_timeout(state, now_us);

    /* MFID gate. Vendor blocks share the LCO space and repurpose the
     * payload - a Motorola LCO 0x00 is not GRP_V_CH_USER. Same reason
     * p25_tsbk_parse gates on MFID. */
    if (mfid != P25_LCW_MFID_STD_A && mfid != P25_LCW_MFID_STD_B) {
        return 0;
    }

    uint8_t opcode = (uint8_t)(lco & 0x3fu);

    /* LS-739: preserve the newly decoded identity while retiring an alias
     * belonging to the previous group/talker. The frame dispatcher used to
     * clear the entire new LCW after a TG change instead. */
    if (opcode == P25_LCO_GROUP_VOICE_USER || opcode == P25_LCO_GROUP_REGROUP) {
        uint16_t next_tg = lcw_u16(&lcinfo[2]);
        uint32_t next_src = lcw_u24(&lcinfo[4]);
        if ((state->lasttg && next_tg && state->lasttg != next_tg) ||
            (state->lastsrc && next_src && (uint32_t)state->lastsrc != next_src))
            alias_reset(state);
    }

    switch (opcode) {
    case P25_LCO_GROUP_VOICE_USER: {
        parse_service_options(state, lcinfo[0]);
        uint16_t tg  = lcw_u16(&lcinfo[2]);
        uint32_t src = lcw_u24(&lcinfo[4]);
        state->p25_lcw_talkgroup       = tg;
        state->p25_lcw_source          = src;
        state->p25_lcw_target          = 0;
        state->p25_lcw_is_unit_to_unit = 0;
        state->p25_lcw_is_regroup      = 0;
        state->p25_lcw_lco             = lco;
        state->p25_lcw_mfid            = mfid;
        state->p25_lcw_valid           = 1;
        state->p25_lcw_ok_count++;
        /* Mirror to the shared "who is talking" fields for the UI and the
         * ESS-on-TG-change hook in dsd_frame.c. The grant follower reads
         * state->p25_tsbk_talkgroup / p25_tsbk_source directly - never these
         * - so an LCW-derived TG cannot cause a retune. */
        if (tg != 0)  state->lasttg  = (int)tg;
        if (src != 0) state->lastsrc = (int)src;
        return 1;
    }
    case P25_LCO_UNIT_UNIT_VOICE: {
        parse_service_options(state, lcinfo[0]);
        uint32_t target = lcw_u24(&lcinfo[1]);
        uint32_t src    = lcw_u24(&lcinfo[4]);
        state->p25_lcw_target          = target;
        state->p25_lcw_source          = src;
        state->p25_lcw_talkgroup       = 0;
        state->p25_lcw_is_unit_to_unit = 1;
        state->p25_lcw_is_regroup      = 0;
        state->p25_lcw_lco             = lco;
        state->p25_lcw_mfid            = mfid;
        state->p25_lcw_valid           = 1;
        state->p25_lcw_ok_count++;
        /* Private call: no talkgroup exists. Publish source only. */
        if (src != 0) state->lastsrc = (int)src;
        return 1;
    }
    case P25_LCO_GROUP_REGROUP: {
        parse_service_options(state, lcinfo[0]);
        uint16_t sg  = lcw_u16(&lcinfo[2]);
        uint32_t src = lcw_u24(&lcinfo[4]);
        state->p25_lcw_talkgroup       = sg;
        state->p25_lcw_patch_sg        = sg;
        state->p25_lcw_source          = src;
        state->p25_lcw_target          = 0;
        state->p25_lcw_is_unit_to_unit = 0;
        state->p25_lcw_is_regroup      = 1;
        state->p25_lcw_lco             = lco;
        state->p25_lcw_mfid            = mfid;
        state->p25_lcw_valid           = 1;
        state->p25_lcw_ok_count++;
        /* Follow the super-group as the current talkgroup - a talkgroup
         * that got patched into a super-group is heard on the SG until the
         * regroup is torn down, and showing the constituent TG label
         * while sitting on the SG is what makes a patched system look
         * dead. */
        if (sg != 0)  state->lasttg  = (int)sg;
        if (src != 0) state->lastsrc = (int)src;
        return 1;
    }
    case P25_LCO_TALKER_ALIAS_HDR:
        handle_talker_alias_hdr(state, lcinfo, now_us);
        state->p25_lcw_ok_count++;
        return 1;
    case P25_LCO_TALKER_ALIAS_BLK1:
        handle_talker_alias_block(state, 1, lcinfo);
        state->p25_lcw_ok_count++;
        return 1;
    case P25_LCO_TALKER_ALIAS_BLK2:
        handle_talker_alias_block(state, 2, lcinfo);
        state->p25_lcw_ok_count++;
        return 1;
    case P25_LCO_TALKER_ALIAS_BLK3:
        handle_talker_alias_block(state, 3, lcinfo);
        state->p25_lcw_ok_count++;
        return 1;
    default:
        return 0;
    }
}

/* Convert an ASCII '0'/'1' string to a byte, MSB-first. Used only by the
 * LDU1/TDULC wrappers that are stuck on the char-string interface DSD passed
 * around. Non-'0'/'1' characters read as 0. */
static uint8_t bits8_to_byte(const char *bits)
{
    uint8_t v = 0;
    for (int i = 0; i < 8; i++)
        v = (uint8_t)((v << 1) | (bits[i] == '1' ? 1 : 0));
    return v;
}

void processP25lcw(dsd_opts *opts, dsd_state *state, char *lcformat,
                   char *mfid, char *lcinfo, int fec_ok)
{
    (void)opts;
    uint8_t lco_byte  = bits8_to_byte(lcformat);
    uint8_t mfid_byte = bits8_to_byte(mfid);
    uint8_t lcinfo_bytes[P25_LCW_LCINFO_BYTES];
    for (int i = 0; i < P25_LCW_LCINFO_BYTES; i++)
        lcinfo_bytes[i] = bits8_to_byte(lcinfo + i * 8);

    /* Firmware calls in real time; the bench passes an injected now_us. This
     * pulls the real timer only in the firmware build (the bench-visible
     * function is p25_lcw_dispatch itself). */
    int64_t now_us;
#ifdef ESP_PLATFORM
    extern int64_t esp_timer_get_time(void);
    now_us = esp_timer_get_time();
#else
    now_us = 0;
#endif
    (void)p25_lcw_dispatch(state, lco_byte, mfid_byte, lcinfo_bytes,
                           fec_ok, now_us);
}
