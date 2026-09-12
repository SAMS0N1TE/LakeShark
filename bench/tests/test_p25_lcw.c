/* LS_TEST_SOURCES: ${APP}/p25/p25_lcw.c */

#include "ls_test.h"
#include "dsd.h"

#include <string.h>

static void make_group_voice(uint8_t lcinfo[7], uint8_t svc,
                             uint16_t tg, uint32_t src)
{
    lcinfo[0] = svc;
    lcinfo[1] = 0;
    lcinfo[2] = (uint8_t)(tg >> 8);
    lcinfo[3] = (uint8_t)(tg & 0xff);
    lcinfo[4] = (uint8_t)((src >> 16) & 0xff);
    lcinfo[5] = (uint8_t)((src >> 8) & 0xff);
    lcinfo[6] = (uint8_t)(src & 0xff);
}

static void make_unit_voice(uint8_t lcinfo[7], uint8_t svc,
                            uint32_t target, uint32_t src)
{
    lcinfo[0] = svc;
    lcinfo[1] = (uint8_t)((target >> 16) & 0xff);
    lcinfo[2] = (uint8_t)((target >> 8) & 0xff);
    lcinfo[3] = (uint8_t)(target & 0xff);
    lcinfo[4] = (uint8_t)((src >> 16) & 0xff);
    lcinfo[5] = (uint8_t)((src >> 8) & 0xff);
    lcinfo[6] = (uint8_t)(src & 0xff);
}

LS_CASE(group_voice_yields_tg_source_and_no_flags_when_svc_is_zero)
{
    dsd_state state;
    uint8_t lcinfo[7];
    memset(&state, 0, sizeof(state));
    make_group_voice(lcinfo, 0x00, 0x4567, 0x123456);

    LS_EQ_INT(p25_lcw_dispatch(&state, 0x00, 0x00, lcinfo, 1, 0), 1);
    LS_EQ_UINT(state.p25_lcw_valid, 1);
    LS_EQ_UINT(state.p25_lcw_talkgroup, 0x4567);
    LS_EQ_UINT(state.p25_lcw_source, 0x123456u);
    LS_EQ_UINT(state.p25_lcw_emergency, 0);
    LS_EQ_UINT(state.p25_lcw_encrypted, 0);
    LS_EQ_UINT(state.p25_lcw_priority, 0);
    LS_EQ_UINT(state.p25_lcw_is_unit_to_unit, 0);
    LS_EQ_UINT(state.p25_lcw_is_regroup, 0);
    /* The LCW is authoritative for who is talking here: mirror to
     * state->lasttg / lastsrc for the UI, but only these fields - the
     * TSBK-only fields that the grant follower reads stay zero. */
    LS_EQ_INT(state.lasttg, 0x4567);
    LS_EQ_INT(state.lastsrc, 0x123456);
    LS_EQ_UINT(state.p25_tsbk_talkgroup, 0);
    LS_EQ_UINT(state.p25_tsbk_source, 0);
    LS_EQ_UINT(state.p25_tsbk_last_opcode, 0);
    LS_EQ_UINT(state.p25_tsbk_frequency_hz, 0);
    LS_EQ_UINT(state.p25_lcw_ok_count, 1);
}

LS_CASE(group_voice_reads_emergency_encrypted_and_priority_bits_independently)
{
    dsd_state state;
    uint8_t lcinfo[7];

    /* Emergency alone: bit 7 = 0x80. */
    memset(&state, 0, sizeof(state));
    make_group_voice(lcinfo, 0x80, 100, 200);
    LS_EQ_INT(p25_lcw_dispatch(&state, 0x00, 0x00, lcinfo, 1, 0), 1);
    LS_EQ_UINT(state.p25_lcw_emergency, 1);
    LS_EQ_UINT(state.p25_lcw_encrypted, 0);
    LS_EQ_UINT(state.p25_lcw_priority, 0);

    /* Encrypted alone: bit 6 = 0x40. */
    memset(&state, 0, sizeof(state));
    make_group_voice(lcinfo, 0x40, 100, 200);
    LS_EQ_INT(p25_lcw_dispatch(&state, 0x00, 0x00, lcinfo, 1, 0), 1);
    LS_EQ_UINT(state.p25_lcw_emergency, 0);
    LS_EQ_UINT(state.p25_lcw_encrypted, 1);
    LS_EQ_UINT(state.p25_lcw_priority, 0);

    /* Priority alone in bits 2..0. */
    memset(&state, 0, sizeof(state));
    make_group_voice(lcinfo, 0x05, 100, 200);
    LS_EQ_INT(p25_lcw_dispatch(&state, 0x00, 0x00, lcinfo, 1, 0), 1);
    LS_EQ_UINT(state.p25_lcw_emergency, 0);
    LS_EQ_UINT(state.p25_lcw_encrypted, 0);
    LS_EQ_UINT(state.p25_lcw_priority, 5);

    /* Emergency + encrypted + priority 3 together: 0x80 | 0x40 | 0x03. */
    memset(&state, 0, sizeof(state));
    make_group_voice(lcinfo, 0xC3, 100, 200);
    LS_EQ_INT(p25_lcw_dispatch(&state, 0x00, 0x00, lcinfo, 1, 0), 1);
    LS_EQ_UINT(state.p25_lcw_emergency, 1);
    LS_EQ_UINT(state.p25_lcw_encrypted, 1);
    LS_EQ_UINT(state.p25_lcw_priority, 3);
}

LS_CASE(unit_to_unit_voice_yields_both_addresses)
{
    /* LCO 0x03 UU_V_CH_USER. Private calls are a pure src/target pair -
     * there is no talkgroup, and the group field is not populated. */
    dsd_state state;
    uint8_t lcinfo[7];
    memset(&state, 0, sizeof(state));
    make_unit_voice(lcinfo, 0x00, 0xABCDEF, 0x123456);

    LS_EQ_INT(p25_lcw_dispatch(&state, 0x03, 0x00, lcinfo, 1, 0), 1);
    LS_EQ_UINT(state.p25_lcw_is_unit_to_unit, 1);
    LS_EQ_UINT(state.p25_lcw_target, 0xABCDEFu);
    LS_EQ_UINT(state.p25_lcw_source, 0x123456u);
    LS_EQ_UINT(state.p25_lcw_talkgroup, 0);
    LS_EQ_INT(state.lasttg, 0);      /* no TG on a private call */
    LS_EQ_INT(state.lastsrc, 0x123456);
}

LS_CASE(group_regroup_records_super_group_and_follows_it_as_current_tg)
{

    dsd_state state;
    uint8_t lcinfo[7];
    memset(&state, 0, sizeof(state));
    make_group_voice(lcinfo, 0x00, 0x9001, 0x123456);

    LS_EQ_INT(p25_lcw_dispatch(&state, 0x0A, 0x00, lcinfo, 1, 0), 1);
    LS_EQ_UINT(state.p25_lcw_is_regroup, 1);
    LS_EQ_UINT(state.p25_lcw_patch_sg, 0x9001);
    LS_EQ_UINT(state.p25_lcw_talkgroup, 0x9001);
    LS_EQ_INT(state.lasttg, 0x9001);
    LS_EQ_INT(state.lastsrc, 0x123456);
}

LS_CASE(fec_failure_provably_changes_nothing)
{
    /* The important guarantee: a corrupt LCW that is acted on relabels the
     * call and can trip stale-TG-clears-ESS in dsd_frame.c, muting a
     * legitimate encrypted call. Bench: the whole state is byte-identical
     * before and after when fec_ok == 0. */
    dsd_state before;
    dsd_state after;
    uint8_t lcinfo[7];
    memset(&before, 0, sizeof(before));
    before.p25_lcw_talkgroup = 0x1234;
    before.p25_lcw_source    = 0x5678;
    before.p25_lcw_emergency = 0;
    before.lasttg  = 0x1234;
    before.lastsrc = 0x5678;
    memcpy(&after, &before, sizeof(after));
    make_group_voice(lcinfo, 0xff, 0x9999, 0xaaaaaa); /* deliberately noisy */

    LS_EQ_INT(p25_lcw_dispatch(&after, 0x00, 0x00, lcinfo, /* fec_ok=*/0, 0), 0);
    /* Compare byte-wise via memcmp, but the reject counter is expected to
     * bump - roll it back before the compare so a real change would show. */
    LS_EQ_UINT(after.p25_lcw_fec_reject_count, 1);
    after.p25_lcw_fec_reject_count = 0;
    LS_CHECK(memcmp(&before, &after, sizeof(before)) == 0);
}

LS_CASE(vendor_mfid_is_not_parsed_as_a_standard_lcw)
{
    /* Same reasoning as p25_tsbk_parse: Motorola MFID 0x90 shares the LCO
     * space and re-purposes the payload. Parsing a Motorola LCO 0x00 as
     * GRP_V_CH_USER would relabel the call from a fabricated pick. */
    dsd_state state;
    uint8_t lcinfo[7];
    memset(&state, 0, sizeof(state));
    make_group_voice(lcinfo, 0x00, 0x4567, 0x123456);

    LS_EQ_INT(p25_lcw_dispatch(&state, 0x00, 0x90, lcinfo, 1, 0), 0);
    LS_EQ_UINT(state.p25_lcw_valid, 0);
    LS_EQ_UINT(state.p25_lcw_talkgroup, 0);
    LS_EQ_INT(state.lasttg, 0);

    /* MFID 0x01 is standard: the same layout must parse. */
    LS_EQ_INT(p25_lcw_dispatch(&state, 0x00, 0x01, lcinfo, 1, 0), 1);
    LS_EQ_UINT(state.p25_lcw_talkgroup, 0x4567);
}

LS_CASE(lcw_derived_talkgroup_never_causes_a_retune)
{
    /* The grant follower reads state->p25_tsbk_talkgroup, state->p25_tsbk_source
     * and state->p25_tsbk_frequency_hz - and it gates the whole call on
     * state->p25_tsbk_last_opcode being 0x00 or 0x02. Feeding any number of
     * LCWs must leave every one of those fields alone. */
    dsd_state state;
    uint8_t lcinfo[7];
    memset(&state, 0, sizeof(state));

    make_group_voice(lcinfo, 0x00, 0x1111, 0xdeadbe);
    LS_EQ_INT(p25_lcw_dispatch(&state, 0x00, 0x00, lcinfo, 1, 0), 1);
    make_group_voice(lcinfo, 0x00, 0x2222, 0xdeadbf);
    LS_EQ_INT(p25_lcw_dispatch(&state, 0x0A, 0x00, lcinfo, 1, 0), 1);
    make_unit_voice(lcinfo, 0x00, 0x000001, 0x000002);
    LS_EQ_INT(p25_lcw_dispatch(&state, 0x03, 0x00, lcinfo, 1, 0), 1);

    LS_EQ_UINT(state.p25_tsbk_talkgroup, 0);
    LS_EQ_UINT(state.p25_tsbk_source, 0);
    LS_EQ_UINT(state.p25_tsbk_frequency_hz, 0);
    LS_EQ_UINT(state.p25_tsbk_last_opcode, 0);
    /* And no false grant landed in the TSBK-error/valid counters either. */
    LS_EQ_UINT(state.p25_tsbk_valid_count, 0);
    LS_EQ_UINT(state.p25_tsbk_crc_errors, 0);
}

LS_CASE(talker_alias_reassembles_across_three_blocks)
{
    /* Header declares the length; the three continuation blocks each carry
     * seven characters. Reassembly is a small state machine, not a field
     * read: the alias is only "ready" once all declared characters have
     * arrived. */
    dsd_state state;
    uint8_t hdr[7]  = { 0 };
    uint8_t blk1[7] = { 'M','E','D','I','C',' ','4' };
    uint8_t blk2[7] = { ' ','O','P','S',' ','H','Q' };
    uint8_t blk3[7] = { ' ','U','N','I','T',' ',' ' };
    memset(&state, 0, sizeof(state));

    hdr[0] = 0x00;   /* format */
    hdr[1] = 21;     /* length in characters */
    LS_EQ_INT(p25_lcw_dispatch(&state, 0x15, 0x00, hdr,  1, 100000), 1);
    LS_EQ_UINT(state.p25_lcw_alias_expected, 21);
    LS_EQ_UINT(state.p25_lcw_alias_ready, 0);

    /* Blocks may arrive out of order in the wild, but a straightforward
     * ordered sequence must produce the header-declared string. */
    LS_EQ_INT(p25_lcw_dispatch(&state, 0x16, 0x00, blk1, 1, 200000), 1);
    LS_EQ_UINT(state.p25_lcw_alias_ready, 0);
    LS_EQ_INT(p25_lcw_dispatch(&state, 0x17, 0x00, blk2, 1, 300000), 1);
    LS_EQ_UINT(state.p25_lcw_alias_ready, 0);
    LS_EQ_INT(p25_lcw_dispatch(&state, 0x18, 0x00, blk3, 1, 400000), 1);
    LS_EQ_UINT(state.p25_lcw_alias_ready, 1);
    LS_EQ_STR(state.p25_lcw_alias, "MEDIC 4 OPS HQ UNIT  ");
}

LS_CASE(talker_alias_truncated_sequence_times_out_rather_than_showing_partial)
{
    /* Header arrives, block 1 arrives, then the sequence stops - a partial
     * alias emitted here would be worse than silence, as it would leave
     * "MEDIC 4        " on the display long after the call had moved on.
     * The state machine drops after P25_LCW_ALIAS_TIMEOUT_US. */
    dsd_state state;
    uint8_t hdr[7]  = { 0 };
    uint8_t blk1[7] = { 'M','E','D','I','C',' ','4' };
    uint8_t nul[7]  = { 0 };
    memset(&state, 0, sizeof(state));

    hdr[0] = 0x00;
    hdr[1] = 21;
    LS_EQ_INT(p25_lcw_dispatch(&state, 0x15, 0x00, hdr,  1, 0), 1);
    LS_EQ_INT(p25_lcw_dispatch(&state, 0x16, 0x00, blk1, 1, 100000), 1);
    LS_EQ_UINT(state.p25_lcw_alias_ready, 0);
    /* An unrelated LCW arrives after the timeout: the alias state clears. */
    LS_EQ_INT(p25_lcw_dispatch(&state, 0x00, 0x00, nul, 1,
                               P25_LCW_ALIAS_TIMEOUT_US + 100000), 1);
    LS_EQ_UINT(state.p25_lcw_alias_ready, 0);
    LS_EQ_UINT(state.p25_lcw_alias_expected, 0);
    LS_EQ_UINT(state.p25_lcw_alias_blocks, 0);
}

LS_CASE(talker_alias_block_before_header_is_ignored)
{
    /* A block that arrives without a preceding header cannot say "this is
     * a fresh sequence"; using it would spray the alias with garbage. */
    dsd_state state;
    uint8_t blk1[7] = { 'M','E','D','I','C',' ','4' };
    memset(&state, 0, sizeof(state));

    LS_EQ_INT(p25_lcw_dispatch(&state, 0x16, 0x00, blk1, 1, 0), 1);
    LS_EQ_UINT(state.p25_lcw_alias_ready, 0);
    LS_EQ_UINT(state.p25_lcw_alias_len, 0);
}

LS_CASE(unknown_lco_is_ignored_and_leaves_state_untouched)
{
    dsd_state before;
    dsd_state after;
    uint8_t lcinfo[7] = { 0xaa, 0xbb, 0xcc, 0xdd, 0xee, 0xff, 0x11 };
    memset(&before, 0, sizeof(before));
    /* Populate a plausible existing LCW state so we can prove it is not
     * disturbed by an unknown opcode. */
    before.p25_lcw_valid     = 1;
    before.p25_lcw_talkgroup = 0x4567;
    before.p25_lcw_source    = 0x123456;
    memcpy(&after, &before, sizeof(after));

    /* LCO 0x1F is not one of the standard voice / regroup / alias opcodes
     * this dispatcher recognises. */
    LS_EQ_INT(p25_lcw_dispatch(&after, 0x1F, 0x00, lcinfo, 1, 0), 0);
    LS_CHECK(memcmp(&before, &after, sizeof(before)) == 0);
}

LS_CASE(call_clear_zeros_lcw_state_but_leaves_tsbk_untouched)
{
    /* On TDU / TDULC / talkgroup change, p25_lcw_call_clear runs. It must
     * drop every LCW-derived field so the next call starts unlabelled,
     * without perturbing the TSBK-derived world (WACN, SYSID, IDEN table,
     * counters). */
    dsd_state state;
    memset(&state, 0, sizeof(state));
    state.p25_lcw_valid       = 1;
    state.p25_lcw_talkgroup   = 0x4567;
    state.p25_lcw_source      = 0x123456;
    state.p25_lcw_emergency   = 1;
    state.p25_lcw_alias_ready = 1;
    memcpy(state.p25_lcw_alias, "MEDIC 4", 8);
    state.p25_lcw_alias_len    = 7;
    state.p25_lcw_alias_expected = 7;
    state.p25_tsbk_wacn       = 0xabcde;
    state.p25_tsbk_sysid      = 0x789;
    state.p25_tsbk_valid_count = 42;

    p25_lcw_call_clear(&state);

    LS_EQ_UINT(state.p25_lcw_valid, 0);
    LS_EQ_UINT(state.p25_lcw_talkgroup, 0);
    LS_EQ_UINT(state.p25_lcw_source, 0);
    LS_EQ_UINT(state.p25_lcw_emergency, 0);
    LS_EQ_UINT(state.p25_lcw_alias_ready, 0);
    LS_EQ_UINT(state.p25_lcw_alias_len, 0);
    LS_EQ_UINT(state.p25_lcw_alias_expected, 0);
    /* TSBK-derived state is not the LCW's business. */
    LS_EQ_UINT(state.p25_tsbk_wacn, 0xabcde);
    LS_EQ_UINT(state.p25_tsbk_sysid, 0x789);
    LS_EQ_UINT(state.p25_tsbk_valid_count, 42);
}

LS_CASE(late_entry_group_voice_labels_the_call_within_one_ldu1)
{
    /* The done-when this whole task hinges on: a call joined mid-stream
     * (missed grant, no HDU) has no identity in dsd_state at all. The very
     * first LDU1 with a valid LCW must fill in the talkgroup, source and
     * emergency flag so the panel does not stay 0/0/OFF for the duration
     * of the call. */
    dsd_state state;
    uint8_t lcinfo[7];
    memset(&state, 0, sizeof(state));
    /* Pre-call: nothing known. */
    LS_EQ_INT(state.lasttg, 0);
    LS_EQ_INT(state.lastsrc, 0);
    LS_EQ_UINT(state.p25_lcw_valid, 0);
    LS_EQ_UINT(state.p25_lcw_emergency, 0);

    /* First LDU1 lands. Emergency + priority 2 + a real TG/SRC. */
    make_group_voice(lcinfo, 0x82, 0x2A05, 0x00CAFE);
    LS_EQ_INT(p25_lcw_dispatch(&state, 0x00, 0x00, lcinfo, 1, 200000), 1);

    LS_EQ_INT(state.lasttg, 0x2A05);
    LS_EQ_INT(state.lastsrc, 0x00CAFE);
    LS_EQ_UINT(state.p25_lcw_valid, 1);
    LS_EQ_UINT(state.p25_lcw_talkgroup, 0x2A05);
    LS_EQ_UINT(state.p25_lcw_source, 0x00CAFEu);
    LS_EQ_UINT(state.p25_lcw_emergency, 1);
    LS_EQ_UINT(state.p25_lcw_priority, 2);
}
