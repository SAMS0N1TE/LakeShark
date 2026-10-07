/* Reading .sub files back - the ones this board writes, and a Flipper's. */

#include "ls_test.h"

#include "subghz_file.h"

#include <stdio.h>
#include <string.h>

#ifndef CC1101_FSK_FIXTURE
#define CC1101_FSK_FIXTURE "fixtures/cc1101-fsk-ch2-t99.sub"
#endif

static void feed(subghz_file_t *f, const char *text, int32_t *e, int cap)
{
    char line[512];
    subghz_file_begin(f);
    while (*text) {
        size_t n = strcspn(text, "\n");
        memcpy(line, text, n);
        line[n] = 0;
        subghz_file_line(f, line, e, cap);
        text += n + (text[n] == '\n');
    }
}

LS_CASE(a_flipper_ook_file_reads_without_fsk_parameters)
{
    static const char *T =
        "Filetype: Flipper SubGhz RAW File\n"
        "Version: 1\n"
        "Frequency: 433920000\n"
        "Preset: FuriHalSubGhzPresetOok650Async\n"
        "Protocol: RAW\n"
        "RAW_Data: 350 -1050 1050 -350 0 350 -10500\n";
    int32_t e[16];
    subghz_file_t f;
    feed(&f, T, e, 16);
    LS_CHECK(f.filetype_ok);
    LS_CHECK(f.freq_hz == 433920000u);
    LS_CHECK(!strcmp(f.preset, "FuriHalSubGhzPresetOok650Async"));
    LS_CHECK(!strcmp(f.protocol, "RAW"));
    LS_CHECK_MSG(f.edges == 6, "the zero is not a legal edge and is skipped");
    LS_CHECK(e[0] == 350 && e[5] == -10500);
    LS_CHECK(f.span_us == 350 + 1050 + 1050 + 350 + 350 + 10500);
    LS_CHECK(!subghz_file_is_fsk(&f));
}

LS_CASE(the_watch_export_comment_gives_the_fsk_parameters)
{
    static const char *T =
        "Filetype: Flipper SubGhz RAW File\n"
        "Frequency: 915000000\n"
        "Preset: FuriHalSubGhzPresetCustom\n"
        "# 4800 baud, 25000 Hz deviation, sync 2DD42DD4, 32-bit preamble\n"
        "Protocol: RAW\n"
        "RAW_Data: 208 -208 208 -208 208 -416\n";
    int32_t e[16];
    subghz_file_t f;
    feed(&f, T, e, 16);
    LS_CHECK(f.bitrate == 4800);
    LS_CHECK(f.deviation_hz == 25000);
    LS_CHECK(f.sync_word == 0x2DD42DD4u);
    LS_CHECK(f.preamble_bits == 32);
    LS_CHECK(subghz_file_is_fsk(&f));
}

LS_CASE(the_console_export_comments_give_the_same_parameters)
{
    static const char *T =
        "Filetype: Flipper SubGhz RAW File\n"
        "Frequency: 433920000\n"
        "# 9600 baud, 19200 Hz deviation requested, 19043 Hz encodable\n"
        "# 16-bit preamble, sync 12345678, 8 byte payload\n"
        "RAW_Data: 104 -104 104 -104 104 -104\n";
    int32_t e[16];
    subghz_file_t f;
    feed(&f, T, e, 16);
    LS_CHECK_MSG(f.deviation_hz == 19200, "the requested deviation, not the encodable one");
    LS_CHECK(f.bitrate == 9600);
    LS_CHECK(f.preamble_bits == 16);
    LS_CHECK(f.sync_word == 0x12345678u);
}

LS_CASE(edges_past_the_buffer_are_counted_but_not_stored)
{
    static const char *T =
        "Filetype: Flipper SubGhz RAW File\n"
        "RAW_Data: 1 -2 3 -4 5 -6\n"
        "RAW_Data: 7 -8\n";
    int32_t e[4];
    subghz_file_t f;
    feed(&f, T, e, 4);
    LS_CHECK(f.edges == 4);
    LS_CHECK(f.edges_total == 8);
    LS_CHECK(f.span_us == 1 + 2 + 3 + 4);
}

LS_CASE(a_file_that_is_not_a_sub_is_refused)
{
    subghz_file_t f;
    feed(&f, "hello\nRAW_Data: 1 -1\n", NULL, 0);
    LS_CHECK(!f.filetype_ok);
    LS_CHECK(f.edges == 0 && f.edges_total == 2);
}

LS_CASE(ook_replay_requires_complete_known_raw_modulation)
{
    int32_t e[8];subghz_file_t f;
    const char *text="Filetype: Flipper SubGhz RAW File\nFrequency: 433920000\n"
        "Preset: FuriHalSubGhzPresetOok650Async\nProtocol: RAW\nRAW_Data: 350 -1050 1050 -350 350 -10500\n";
    feed(&f,text,e,8);LS_CHECK(subghz_file_is_ook(&f));
    feed(&f,text,e,4);LS_CHECK(!subghz_file_is_ook(&f));
    feed(&f,text,e,8);strcpy(f.preset,"FuriHalSubGhzPresetCustom");LS_CHECK(!subghz_file_is_ook(&f));
    feed(&f,text,e,8);strcpy(f.protocol,"Princeton");LS_CHECK(!subghz_file_is_ook(&f));
    feed(&f,text,e,8);f.freq_hz=500000000;LS_CHECK(!subghz_file_is_ook(&f));
    feed(&f,text,e,8);f.edges=f.edges_total=4097;LS_CHECK(!subghz_file_is_ook(&f));
    feed(&f,text,e,8);f.span_us=10000001;LS_CHECK(!subghz_file_is_ook(&f));
    feed(&f,text,e,8);subghz_file_line(&f,"RAW_Data: 2147483648 junk",e,8);
    LS_CHECK(f.invalid);LS_CHECK(!subghz_file_is_ook(&f));
}

LS_CASE(rmt_replay_preserves_levels_and_long_space_duration)
{
    int32_t e[]={350,-1050,1050,-350,350,-100000};
    uint32_t words[16];
    size_t n=subghz_ook_symbols(e,6,words,16);
    LS_CHECK(n==subghz_ook_symbols(e,6,NULL,0));LS_CHECK(n>0);
    unsigned i=0;uint64_t duration=0,expected=0;
    for(unsigned j=0;j<6;j++)expected+=e[j]<0?-e[j]:e[j];
    for(size_t h=0;h<n*2;h++) {
        uint32_t v=(words[h/2]>>((h%2)*16))&65535;
        unsigned us=v&32767;if(!us)break;
        duration+=us;
        if(i<6){LS_CHECK((v>>15)==(e[i]>0));e[i]+=e[i]>0?-(int)us:(int)us;if(!e[i])i++;}
        else LS_CHECK((v>>15)==0);
    }
    LS_CHECK(i==6);LS_CHECK(duration==expected+1);
}

LS_CASE(rmt_replay_rejects_bad_edges_and_unbounded_duration)
{
    int32_t e[]={350,-1050,1050,-350,350,-10500};uint32_t tiny[1];
    LS_CHECK(!subghz_ook_symbols(e,6,tiny,1));
    e[1]=1050;LS_CHECK(!subghz_ook_symbols(e,6,NULL,0));
    e[1]=0;LS_CHECK(!subghz_ook_symbols(e,6,NULL,0));
    e[1]=-10000000;LS_CHECK(!subghz_ook_symbols(e,6,NULL,0));
    e[1]=INT32_MIN;LS_CHECK(!subghz_ook_symbols(e,6,NULL,0));
}

static const char *fsk_raw = "Filetype: Flipper SubGhz RAW File\n"
    "Frequency: 433420000\nProtocol: RAW\nRAW_Data: 417 -417 834 -417 417 -1251\n";
static const char *fsk_custom = "Custom_preset_data: 02 0D 08 32 10 67 11 83 12 04 15 34 0C 02 13 02 00 00 C0 00 00 00 00 00 00 00";

LS_CASE(cc1101_fsk_stock_presets_need_no_private_comments)
{
    int32_t e[8]; subghz_file_t f;
    feed(&f,fsk_raw,e,8);
    subghz_file_line(&f,"Preset: FuriHalSubGhzPreset2FSKDev238Async",e,8);
    LS_CHECK(subghz_file_is_cc_fsk(&f));
    LS_EQ_INT(f.cc_fsk.mdmcfg4,0x67); LS_EQ_INT(f.cc_fsk.mdmcfg3,0x83);
    LS_EQ_INT(f.cc_fsk.deviatn,0x04);
    subghz_file_line(&f,"Preset: FuriHalSubGhzPreset2FSKDev476Async",e,8);
    LS_EQ_INT(f.cc_fsk.deviatn,0x47); LS_CHECK(subghz_file_is_cc_fsk(&f));
    LS_CHECK(!subghz_file_is_ook(&f));
}

LS_CASE(cc1101_fsk_custom_preserves_sampler_deviation_offset_and_raw)
{
    int32_t e[8]; subghz_file_t f;
    feed(&f,fsk_raw,e,8);
    subghz_file_line(&f,"Preset: FuriHalSubGhzPresetCustom",e,8);
    subghz_file_line(&f,"Custom_preset_module: CC1101",e,8);
    subghz_file_line(&f,fsk_custom,e,8);
    LS_CHECK(subghz_file_is_cc_fsk(&f));
    LS_EQ_INT(f.cc_fsk.mdmcfg4,0x67); LS_EQ_INT(f.cc_fsk.mdmcfg3,0x83);
    LS_EQ_INT(f.cc_fsk.deviatn,0x34); LS_EQ_INT(f.cc_fsk.freqoff,2);
    LS_EQ_INT(e[0],417); LS_EQ_INT(e[5],-1251);
    f.freq_hz=500000000; LS_CHECK(!subghz_file_is_cc_fsk(&f));
    f.freq_hz=433420000; f.span_us=10000001; LS_CHECK(!subghz_file_is_cc_fsk(&f));
    f.span_us=3753; f.edges_total++; LS_CHECK(!subghz_file_is_cc_fsk(&f));
}

LS_CASE(is_fsk_rejects_a_declared_module_other_than_cc1101)
{
    static const char *unknown_module =
        "Filetype: Flipper SubGhz RAW File\n"
        "Frequency: 433394300\n"
        "Preset: FuriHalSubGhzPresetCustom\n"
        "Custom_preset_module: UnknownRadio\n"
        "Custom_preset_data: 08 32 10 67 11 85 12 04 15 33 00 00 C0 00 00 00 00 00 00 00\n"
        "Protocol: RAW\n"
        "# 2400 baud, 18500 Hz deviation\n"
        "RAW_Data: 417 -417 834 -417 417 -1251\n";
    int32_t e[8]; subghz_file_t f;

    /* Valid CC1101-shaped register bytes, but a declared module that is not
       CC1101: is_cc_fsk correctly refuses it, and is_fsk must not silently
       fall back to the SX1262 generic FSK path on its behalf. */
    feed(&f,unknown_module,e,8);
    LS_CHECK(f.bitrate==2400); LS_CHECK(f.deviation_hz==18500);
    LS_CHECK(f.cc_fsk_valid);
    LS_CHECK(!subghz_file_is_cc_fsk(&f));
    LS_CHECK_MSG(!subghz_file_is_fsk(&f),
        "a declared module other than CC1101 must not fall back to the SX1262 FSK path");

    /* Backward compatibility: a declared module of exactly CC1101 with valid
       register bytes still satisfies both the CC1101 and generic FSK checks. */
    subghz_file_line(&f,"Custom_preset_module: CC1101",e,8);
    LS_CHECK(subghz_file_is_cc_fsk(&f));
    LS_CHECK(subghz_file_is_fsk(&f));

    /* Backward compatibility: no declared module at all (this board's own
       private-comment-only FSK captures) is untouched by the module check. */
    static const char *no_module =
        "Filetype: Flipper SubGhz RAW File\n"
        "Frequency: 433394300\n"
        "Preset: FuriHalSubGhzPresetCustom\n"
        "Protocol: RAW\n"
        "# 2400 baud, 18500 Hz deviation\n"
        "RAW_Data: 417 -417 834 -417 417 -1251\n";
    feed(&f,no_module,e,8);
    LS_CHECK(!f.custom_module[0]);
    LS_CHECK(subghz_file_is_fsk(&f));
}

LS_CASE(cc1101_fsk_refuses_malformed_or_other_modulation_presets)
{
    const char *bad[]={
        "Custom_preset_data: 08 32 10 67 11 83 12 04 15 34 00 00 C0",
        "Custom_preset_data: 08 32 10 67 11 83 12 04 15 34 00 00 C0 00 00 00 00 00 00 ZZ",
        "Custom_preset_data: 08 32 10 67 11 83 12 30 15 34 00 00 C0 00 00 00 00 00 00 00",
        "Custom_preset_data: 08 05 10 67 11 83 12 04 15 34 00 00 C0 00 00 00 00 00 00 00",
        "Custom_preset_data: 08 32 10 67 11 83 12 0C 15 34 00 00 C0 00 00 00 00 00 00 00",
        "Custom_preset_data: 08 32 10 67 11 83 12 04 15 34 13 82 00 00 C0 00 00 00 00 00 00 00",
        "Custom_preset_data: 08 32 10 67 11 83 12 04 15 34 15 47 00 00 C0 00 00 00 00 00 00 00",
    };
    for (unsigned i=0;i<sizeof(bad)/sizeof(bad[0]);i++) {
        int32_t e[8]; subghz_file_t f;
        feed(&f,fsk_raw,e,8);
        subghz_file_line(&f,"Preset: FuriHalSubGhzPresetCustom",e,8);
        subghz_file_line(&f,"Custom_preset_module: CC1101",e,8);
        subghz_file_line(&f,bad[i],e,8);
        LS_CHECK(!subghz_file_is_cc_fsk(&f));
    }
    int32_t e[8]; subghz_file_t f;
    feed(&f,fsk_raw,e,8);
    subghz_file_line(&f,"Preset: FuriHalSubGhzPresetCustom",e,8);
    subghz_file_line(&f,fsk_custom,e,8);
    LS_CHECK(!subghz_file_is_cc_fsk(&f));
    subghz_file_line(&f,"Custom_preset_module: CC1101",e,8);
    strcpy(f.protocol,"Princeton"); LS_CHECK(!subghz_file_is_cc_fsk(&f));
}

/* fixtures/cc1101-fsk-ch2-t99.sub is a byte-verbatim copy of a real over-air
   RTL-SDR capture of this board's CC1101 custom 2-FSK RAW transmit, not a
   synthetic vector. These cases pin the exact classification and replay
   shape of that capture so a parser or RMT-packing regression trips here
   instead of only showing up on the bench with real hardware. */
LS_CASE(the_real_cc1101_fsk_capture_classifies_with_its_exact_parameters)
{
    int32_t e[128];
    char line[600];
    subghz_file_t f;
    LS_CHECK(subghz_file_load(CC1101_FSK_FIXTURE, &f, e, 128, line, sizeof(line)));
    LS_CHECK(f.filetype_ok);
    LS_CHECK(!f.invalid);
    LS_EQ_INT(f.edges, 97);
    LS_EQ_INT(f.edges_total, 97);
    LS_CHECK_MSG(f.span_us == 61524ull, "got %llu", (unsigned long long)f.span_us);
    LS_EQ_UINT(f.freq_hz, 433394300u);
    LS_CHECK(subghz_file_is_cc_fsk(&f));
    LS_EQ_INT(f.cc_fsk.mdmcfg4, 0x67);
    LS_EQ_INT(f.cc_fsk.mdmcfg3, 0x85);
    LS_EQ_INT(f.cc_fsk.deviatn, 0x33);
    LS_EQ_INT(f.cc_fsk.freqoff, 0);
    LS_EQ_INT(e[0], 46);
    LS_EQ_INT(e[96], 1691);
}

LS_CASE(the_real_cc1101_fsk_capture_replays_through_rmt_with_every_source_edge)
{
    int32_t e[128];
    char line[600];
    subghz_file_t f;
    LS_CHECK(subghz_file_load(CC1101_FSK_FIXTURE, &f, e, 128, line, sizeof(line)));
    LS_CHECK(subghz_file_is_cc_fsk(&f));

    uint32_t words[64];
    size_t n = subghz_ook_symbols(e, (size_t)f.edges, words, 64);
    LS_CHECK(n > 0);
    LS_CHECK(n == subghz_ook_symbols(e, (size_t)f.edges, NULL, 0));

    unsigned i = 0;
    uint64_t duration = 0;
    for (size_t h = 0; h < n * 2; h++) {
        uint32_t v = (words[h/2] >> ((h%2)*16)) & 65535;
        unsigned us = v & 32767;
        if (!us) break;
        duration += us;
        if (i < (unsigned)f.edges) {
            LS_CHECK((v>>15) == (e[i] > 0));
            e[i] += e[i] > 0 ? -(int)us : (int)us;
            if (!e[i]) i++;
        } else {
            LS_CHECK_MSG((v>>15) == 0, "only idle low padding may follow the source edges");
        }
    }
    LS_CHECK_MSG(i == (unsigned)f.edges, "every source edge must be consumed");
    LS_CHECK_MSG(duration == 61524ull + 1, "source span plus the mandatory trailing low padding");
}

/* Every replay, on the CC1101 or the LoRa chip, passes subghz_tx_refusal
   before a radio is keyed. These are what it must stop and what it must
   still let through. */
static const char *FSK_433 =
    "Filetype: Flipper SubGhz RAW File\n"
    "Frequency: 433920000\n"
    "Preset: FuriHalSubGhzPresetCustom\n"
    "# 4800 baud, 25000 Hz deviation, sync 2DD42DD4, 32-bit preamble\n"
    "Protocol: RAW\n"
    "RAW_Data: 208 -208 208 -208 208 -416\n";

LS_CASE(an_fsk_file_on_marine_channel_16_is_refused)
{
    static const char *T =
        "Filetype: Flipper SubGhz RAW File\n"
        "Frequency: 156800000\n"
        "Preset: FuriHalSubGhzPresetCustom\n"
        "# 4800 baud, 25000 Hz deviation, sync 2DD42DD4, 32-bit preamble\n"
        "Protocol: RAW\n"
        "RAW_Data: 208 -208 208 -208 208 -416\n";
    int32_t e[16]; subghz_file_t f;
    feed(&f, T, e, 16);
    LS_CHECK(f.bitrate == 4800 && f.deviation_hz == 25000);
    LS_CHECK_MSG(!subghz_file_is_fsk(&f),
                 "156.8 MHz is the marine distress channel, not a replay band");
    LS_CHECK(!subghz_file_is_ook(&f) && !subghz_file_is_cc_fsk(&f));
    LS_CHECK(subghz_tx_refusal(f.freq_hz, -9, (size_t)f.edges_total,
                               f.span_us) != NULL);
}

LS_CASE(fsk_files_outside_every_replay_band_are_refused)
{
    static const uint32_t out[] = {
        150000000u, 299999999u, 348000001u, 386999999u,
        406050000u, 464000001u, 778999999u, 928000001u, 960000000u };
    int32_t e[16]; subghz_file_t f;
    for (size_t i = 0; i < sizeof(out) / sizeof(out[0]); i++) {
        feed(&f, FSK_433, e, 16);
        f.freq_hz = out[i];
        LS_CHECK_MSG(!subghz_file_is_fsk(&f), "an FSK file out of band");
        LS_CHECK(subghz_tx_refusal(out[i], 0, 6, 1456) != NULL);
    }
}

LS_CASE(the_406_mhz_distress_beacon_band_is_refused_on_the_cc1101_too)
{
    static const char *T =
        "Filetype: Flipper SubGhz RAW File\n"
        "Frequency: 406025000\n"
        "Preset: FuriHalSubGhzPresetOok650Async\n"
        "Protocol: RAW\n"
        "RAW_Data: 350 -1050 1050 -350 350 -10500\n";
    int32_t e[16]; subghz_file_t f;
    feed(&f, T, e, 16);
    LS_CHECK(!subghz_file_is_ook(&f));
    f.freq_hz = 405999999u; LS_CHECK(subghz_file_is_ook(&f));
    f.freq_hz = 406100001u; LS_CHECK(subghz_file_is_ook(&f));
}

LS_CASE(replay_power_above_ten_dbm_is_refused)
{
    LS_CHECK(subghz_tx_refusal(433920000u, 10, 6, 1456) == NULL);
    LS_CHECK(subghz_tx_refusal(433920000u, -10, 6, 1456) == NULL);
    LS_CHECK(subghz_tx_refusal(433920000u, 11, 6, 1456) != NULL);
    LS_CHECK(subghz_tx_refusal(915000000u, 14, 6, 1456) != NULL);
    LS_CHECK(subghz_tx_refusal(915000000u, 22, 6, 1456) != NULL);
    LS_CHECK(subghz_tx_refusal(915000000u, -11, 6, 1456) != NULL);
}

LS_CASE(an_fsk_file_longer_than_ten_seconds_is_refused)
{
    static const char *T =
        "Filetype: Flipper SubGhz RAW File\n"
        "Frequency: 433920000\n"
        "Preset: FuriHalSubGhzPresetCustom\n"
        "# 4800 baud, 25000 Hz deviation, sync 2DD42DD4, 32-bit preamble\n"
        "Protocol: RAW\n"
        "RAW_Data: 2000000 -2000000 2000000 -2000000 2000000 -2000000\n";
    int32_t e[16]; subghz_file_t f;
    feed(&f, T, e, 16);
    LS_CHECK(f.span_us == 12000000u);
    LS_CHECK(!subghz_file_is_fsk(&f));
    LS_CHECK(subghz_tx_refusal(f.freq_hz, 0, 6, f.span_us) != NULL);
    LS_CHECK(subghz_tx_refusal(433920000u, 0, 4097, 1000000u) != NULL);
    LS_CHECK(subghz_tx_refusal(433920000u, 0, 5, 1000u) != NULL);
    LS_CHECK(subghz_tx_refusal(433920000u, 0, 4096, 10000000u) == NULL);
    const int32_t edges[] = { 2000000, -2000000, 2000000, -2000000 };
    LS_CHECK(subghz_span_us(edges, 4) == 8000000u);
}

LS_CASE(fsk_replay_needs_a_raw_file_with_an_fsk_preset)
{
    int32_t e[16]; subghz_file_t f;
    feed(&f, FSK_433, e, 16);
    LS_CHECK(subghz_file_is_fsk(&f));
    snprintf(f.preset, sizeof(f.preset), "FuriHalSubGhzPresetOok650Async");
    LS_CHECK_MSG(!subghz_file_is_fsk(&f), "an OOK preset is not sent as FSK");
    feed(&f, FSK_433, e, 16);
    snprintf(f.protocol, sizeof(f.protocol), "Princeton");
    LS_CHECK(!subghz_file_is_fsk(&f));
}

LS_CASE(in_band_ism_files_are_accepted_within_policy)
{
    int32_t e[16]; subghz_file_t f;
    feed(&f, FSK_433, e, 16);
    LS_CHECK(subghz_file_is_fsk(&f));
    LS_CHECK(subghz_tx_refusal(f.freq_hz, 10, (size_t)f.edges_total,
                               f.span_us) == NULL);
    f.freq_hz = 915000000u;
    LS_CHECK(subghz_file_is_fsk(&f));
    LS_CHECK(subghz_tx_refusal(f.freq_hz, -9, 6, f.span_us) == NULL);
    static const char *OOK =
        "Filetype: Flipper SubGhz RAW File\n"
        "Frequency: 433920000\n"
        "Preset: FuriHalSubGhzPresetOok650Async\n"
        "Protocol: RAW\n"
        "RAW_Data: 350 -1050 1050 -350 350 -10500\n";
    feed(&f, OOK, e, 16);
    LS_CHECK(subghz_file_is_ook(&f));
    LS_CHECK(subghz_tx_refusal(f.freq_hz, 10, 6, f.span_us) == NULL);
}

LS_CASE(an_undersized_capture_buffer_is_rejected_for_cc_fsk_not_silently_replayed)
{
    int32_t e[40];
    char line[600];
    subghz_file_t f;
    LS_CHECK(subghz_file_load(CC1101_FSK_FIXTURE, &f, e, 40, line, sizeof(line)));
    LS_CHECK(f.filetype_ok);
    LS_EQ_INT(f.edges, 40);
    LS_EQ_INT(f.edges_total, 97);
    LS_CHECK_MSG(!subghz_file_is_cc_fsk(&f),
                 "a truncated edge buffer must not pass as a complete capture");
}
