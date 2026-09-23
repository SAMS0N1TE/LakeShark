/* Reading .sub files back - the ones this board writes, and a Flipper's. */

#include "ls_test.h"

#include "subghz_file.h"

#include <string.h>

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
