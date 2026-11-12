/* FREQS: the bank format and its quirks, the four importers, the filters,
   where TUNE sends each mode, and the store on a card in a directory. */
#include "ls_test.h"
#include "ls_freqs.h"
#include "scan_import.h"

#include <dirent.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#ifdef _WIN32
#include <direct.h>
#define make_dir(p) _mkdir(p)
#else
#define make_dir(p) mkdir((p), 0775)
#endif

static ls_freq_t s_got[64];
static int s_ngot;
static bool keep(const ls_freq_t *e, void *ctx)
{
    (void)ctx;
    if (s_ngot < 64) s_got[s_ngot] = *e;
    s_ngot++;
    return true;
}

/* Read `text` as a file in whatever format it is. */
static ls_freq_tally_t read_text(const char *text, ls_freq_kind_t *kind)
{
    ls_freq_tally_t t;
    FILE *f = tmpfile();
    fputs(text, f);
    rewind(f);
    s_ngot = 0;
    LS_CHECK(ls_freq_read(f, "TEST", keep, NULL, &t, kind));
    fclose(f);
    return t;
}

static int one_line(const char *text, ls_freq_t *e)
{
    char buf[512];
    ls_freq_csv_t c;
    ls_freq_csv_begin(&c);
    c.seen = true;
    snprintf(buf, sizeof(buf), "%s", text);
    return ls_freq_csv_line(&c, buf, e);
}

LS_CASE(csv_quirks_load_and_refuse_line_by_line)
{
    ls_freq_kind_t kind;
    const ls_freq_tally_t t = read_text(
        "\xef\xbb\xbf# a comment first\r\n"
        "name,freq_mhz,mode,notes,extra\r\n"            /* any order, extras ignored */
        "\r\n"
        "\"Tower, main\",118.1,AM,\"says \"\"hello\"\"\",x\r\n"
        "Polic\xc3\xad" "a Federal,  460.025 ,nfm,,\r\n"  /* accent folded, blanks trimmed */
        "no mode,121.9,,,\r\n"                           /* airband guesses AM */
        "bad freq,abc,NFM,,\r\n"
        "too high,9000,NFM,,\r\n"
        "odd mode,145.5,WHAT,,\r\n"
        "   # an indented comment\n"
        "last,433.92,OOK,,", &kind);
    LS_EQ_INT(kind, LS_FI_CSV);
    LS_EQ_INT(t.loaded, 4);
    LS_EQ_INT(t.skipped, 3);
    LS_EQ_INT(t.first_bad, 7);
    LS_EQ_STR(s_got[0].name, "Tower, main");
    LS_EQ_STR(s_got[0].notes, "says \"hello\"");
    LS_EQ_UINT(s_got[0].hz, 118100000u);
    LS_EQ_STR(s_got[1].name, "Policia Federal");
    LS_EQ_UINT(s_got[1].hz, 460025000u);
    LS_EQ_INT(s_got[1].mode, LS_FM_NFM);
    LS_EQ_INT(s_got[2].mode, LS_FM_AM);
    LS_EQ_INT(s_got[2].line, 6);
    LS_EQ_STR(s_got[3].name, "last");
}

LS_CASE(csv_place_tone_and_bandwidth)
{
    ls_freq_t e;
    LS_EQ_INT(one_line("162.55,NFM,12.5,WX,WEATHER,listen,-34.6,-58.4,,123.0,", &e), 1);
    LS_EQ_INT(e.bw, 125);
    LS_EQ_INT(e.lat_e5, -3460000);
    LS_EQ_INT(e.radius_km, 50);              /* placed without a radius */
    LS_EQ_STR(e.tone, "123.0");
    LS_EQ_STR(e.group, "WEATHER");
    LS_EQ_INT(one_line("162.55,NFM,,lat only,,,-34.6,,,,", &e), -1);
    LS_EQ_INT(one_line("162.55,NFM,,far,,,-95,10,,,", &e), -1);
    LS_EQ_INT(one_line("162.55,NFM,,,,,,,,,", &e), 1);
    LS_EQ_STR(e.name, "162.550");            /* unnamed: the frequency */
    LS_EQ_INT(e.lat_e5, LS_FREQ_NOWHERE);
    LS_EQ_INT(one_line("27.185,FM,,CB,,,,,,,", &e), 1);
    LS_EQ_INT(e.mode, LS_FM_NFM);            /* other tools' spellings */
    LS_EQ_INT(one_line("0.001,AM,,too low,,,,,,,", &e), -1);
}

LS_CASE(csv_format_round_trips)
{
    ls_freq_t a, b;
    LS_EQ_INT(one_line("446.00625,NFM,12.5,\"PMR, 1\",PMR/LICENSE-FREE,listen tx-ok,-31.4167,-64.1833,25,D023N,\"a \"\"note\"\"\"", &a), 1);
    char line[256];
    LS_CHECK(ls_freq_csv_format(&a, line, sizeof(line)) > 0);
    LS_EQ_INT(one_line(line, &b), 1);
    LS_EQ_UINT(b.hz, a.hz);
    LS_EQ_STR(b.name, "PMR, 1");
    LS_EQ_STR(b.notes, "a \"note\"");
    LS_EQ_INT(b.lat_e5, a.lat_e5);
    LS_EQ_INT(b.lon_e5, a.lon_e5);
    LS_EQ_INT(b.radius_km, 25);
    LS_EQ_INT(b.bw, 125);
    LS_EQ_STR(b.tone, "D023N");
    char mhz[16];
    ls_freq_mhz_text(121500000u, mhz, sizeof(mhz));
    LS_EQ_STR(mhz, "121.500");
    ls_freq_mhz_text(446006250u, mhz, sizeof(mhz));
    LS_EQ_STR(mhz, "446.00625");
}

LS_CASE(chirp_csv_imports)
{
    ls_freq_kind_t kind;
    const ls_freq_tally_t t = read_text(
        "Location,Name,Frequency,Duplex,Offset,Tone,rToneFreq,cToneFreq,DtcsCode,DtcsPolarity,Mode,TStep,Skip,Comment\n"
        "0,LU1AA,146.940000,-,0.600000,Tone,88.5,88.5,023,NN,FM,5.00,,Repetidora\n"
        "1,SIMPLEX,146.520000,,0.000000,TSQL,88.5,100.0,023,NN,NFM,5.00,,\n"
        "2,DIGI,438.500000,,0.000000,DTCS,88.5,88.5,047,RN,DV,12.50,,\n"
        "3,BROKEN,,,,,,,,,FM,,,\n", &kind);
    LS_EQ_INT(kind, LS_FI_CHIRP);
    LS_EQ_INT(t.loaded, 3);
    LS_EQ_INT(t.skipped, 1);
    LS_EQ_STR(s_got[0].name, "LU1AA");
    LS_EQ_UINT(s_got[0].hz, 146940000u);
    LS_EQ_STR(s_got[0].tone, "88.5");
    LS_EQ_STR(s_got[0].notes, "shift -0.6 MHz Repetidora");
    LS_EQ_STR(s_got[1].tone, "100.0");
    LS_EQ_INT(s_got[2].mode, LS_FM_DSTAR);
    LS_EQ_STR(s_got[2].tone, "D047I");
    LS_EQ_STR(s_got[0].group, "TEST");
}

LS_CASE(sdrpp_json_imports)
{
    ls_freq_kind_t kind;
    const ls_freq_tally_t t = read_text(
        "{\n  \"bookmarkDisplayMode\": 1,\n  \"lists\": {\n"
        "    \"Aviacion\": { \"bookmarks\": {\n"
        "        \"Ezeiza TWR\": { \"bandwidth\": 8000.0, \"frequency\": 118850000.0, \"mode\": 2 },\n"
        "        \"Upper \\\"SSB\\\"\": { \"bandwidth\": 2800.0, \"frequency\": 8855000.0, \"mode\": 4 }\n"
        "      }, \"showOnWaterfall\": true },\n"
        "    \"FM\": { \"bookmarks\": { \"Radio 10\": { \"frequency\": 106100000, \"bandwidth\": 200000, \"mode\": 1 } } }\n"
        "  },\n  \"selectedList\": \"Aviacion\"\n}\n", &kind);
    LS_EQ_INT(kind, LS_FI_SDRPP);
    LS_EQ_INT(t.loaded, 3);
    LS_EQ_STR(s_got[0].name, "Ezeiza TWR");
    LS_EQ_STR(s_got[0].group, "AVIACION");
    LS_EQ_INT(s_got[0].mode, LS_FM_AM);
    LS_EQ_INT(s_got[0].bw, 80);
    LS_EQ_STR(s_got[1].name, "Upper \"SSB\"");
    LS_EQ_INT(s_got[1].mode, LS_FM_USB);
    LS_EQ_STR(s_got[2].group, "FM");
    LS_EQ_INT(s_got[2].mode, LS_FM_WFM);
}

/* A file cut off inside a number: strtod stops at the end of what was read,
   not somewhere past it in the heap. */
LS_CASE(sdrpp_json_cut_off_in_a_number_reads_nothing_past_the_end)
{
    ls_freq_kind_t kind;
    const ls_freq_tally_t t = read_text("{\"lists\":{\"G\":{\"bookmarks\":{\"A\":{\"frequency\":1625", &kind);
    LS_EQ_INT(kind, LS_FI_SDRPP);
    LS_EQ_INT(t.loaded, 0);
}

LS_CASE(gqrx_bookmarks_import)
{
    ls_freq_kind_t kind;
    const ls_freq_tally_t t = read_text(
        "# Tag name          ;  color\n"
        "Untagged            ; #c0c0c0\n"
        "Marine              ; #00ff00\n"
        "\n"
        "# Frequency ; Name                     ; Modulation          ;  Bandwidth; Tags\n"
        "  156800000 ; Canal 16                 ; Narrow FM           ;      10000; Marine\n"
        "   98300000 ; Radio Nacional           ; WFM (stereo)        ;     160000; Untagged\n"
        "    7074000 ; FT8 40m                  ; USB                 ;       2800; Ham,Digital\n"
        "  nonsense  ; x ; AM ; 1 ; y\n", &kind);
    LS_EQ_INT(kind, LS_FI_GQRX);
    LS_EQ_INT(t.loaded, 3);
    LS_EQ_INT(t.skipped, 1);
    LS_EQ_STR(s_got[0].name, "Canal 16");
    LS_EQ_STR(s_got[0].group, "MARINE");
    LS_EQ_INT(s_got[1].mode, LS_FM_WFM);
    LS_EQ_STR(s_got[1].group, "TEST");       /* Untagged is no group */
    LS_EQ_INT(s_got[2].mode, LS_FM_USB);
    LS_EQ_STR(s_got[2].group, "HAM");
    LS_EQ_INT(s_got[2].bw, 28);
}

LS_CASE(lscan_round_trips_through_the_scanners_parser)
{
    ls_freq_t e;
    LS_EQ_INT(one_line("851.0125,P25,,\"Sistema|Troncal muy largo\",PUBLIC SAFETY,priority,-34.6,-58.38,40,,", &e), 1);
    char line[192];
    LS_CHECK(ls_freq_lscan_format(&e, line, sizeof(line)));
    scan_channel_t ch;
    LS_CHECK(scan_import_line(line, &ch));
    LS_EQ_UINT(ch.freq_hz, 851012500u);
    LS_EQ_INT(ch.mode, SCAN_MODE_P25);
    LS_CHECK(ch.flags & SCAN_FLAG_PRIORITY);
    LS_EQ_INT(ch.radius_m, 40000);
    LS_EQ_INT((int)strlen(ch.name), SCAN_NAME_LEN - 1);
    LS_CHECK(strchr(ch.name, '|') == NULL);
    LS_EQ_INT(one_line("121.5,AM,,AIR,,,,,,,", &e), 1);
    LS_CHECK(!ls_freq_lscan_format(&e, line, sizeof(line)));   /* scanner takes NFM and P25 */
    ls_freq_kind_t kind;
    const ls_freq_tally_t t = read_text("LSCAN1\n# list\nFire|154130000|NFM|2|0|0|0|0\n"
                                        "Bad|nope|NFM|0|0|0|0|0\nPD|460125000|P25|1|-34.6|-58.4|25|1\n", &kind);
    LS_EQ_INT(kind, LS_FI_LSCAN);
    LS_EQ_INT(t.loaded, 2);
    LS_EQ_INT(t.skipped, 1);
    LS_EQ_STR(s_got[0].tags, "zone2");
    LS_EQ_INT(s_got[0].lat_e5, LS_FREQ_NOWHERE);
    LS_EQ_INT(s_got[1].mode, LS_FM_P25);
    LS_EQ_INT(s_got[1].radius_km, 25);
    LS_EQ_STR(s_got[1].tags, "zone1 priority");
}

LS_CASE(near_me_band_find_and_sort)
{
    /* Buenos Aires, Cordoba and an unplaced entry; the fix is in Buenos Aires. */
    ls_freq_t ba, cba, any;
    LS_EQ_INT(one_line("119.1,AM,,AEROPARQUE TWR,AVIATION,SABE,-34.5592,-58.4156,30,,", &ba), 1);
    LS_EQ_INT(one_line("118.3,AM,,CORDOBA TWR,AVIATION,SACO,-31.3236,-64.2080,30,,", &cba), 1);
    LS_EQ_INT(one_line("156.8,NFM,,MARINE CH16,MARINE,,,,,,", &any), 1);
    const double lat = -34.6037, lon = -58.3816;
    ls_freq_filter_t f = { 0 };
    f.near = true;
    LS_CHECK(ls_freq_matches(&ba, &f, true, lat, lon));
    LS_CHECK(!ls_freq_matches(&cba, &f, true, lat, lon));
    LS_CHECK(!ls_freq_matches(&any, &f, true, lat, lon));    /* near means placed */
    LS_CHECK(ls_freq_matches(&cba, &f, false, lat, lon));    /* no fix: nothing hidden */
    LS_NEAR(ls_freq_km(lat, lon, -31.3236, -64.2080), 646, 10);
    LS_EQ_STR(ls_freq_bearing(lat, lon, -31.3236, -64.2080), "NW");
    LS_CHECK(ls_freq_distance(&any, lat, lon) < 0);
    memset(&f, 0, sizeof(f));
    f.band = LS_FB_VHF;
    LS_CHECK(ls_freq_matches(&ba, &f, false, 0, 0));
    f.band = LS_FB_UHF;
    LS_CHECK(!ls_freq_matches(&ba, &f, false, 0, 0));
    LS_CHECK(ls_freq_in_band(LS_FB_L, 2400000000u));
    LS_CHECK(ls_freq_in_band(LS_FB_900, 915000000u));
    LS_CHECK(ls_freq_in_band(LS_FB_HF, 27185000u));
    memset(&f, 0, sizeof(f));
    snprintf(f.find, sizeof(f.find), "saco");
    LS_CHECK(ls_freq_matches(&cba, &f, false, 0, 0));
    LS_CHECK(!ls_freq_matches(&ba, &f, false, 0, 0));
    snprintf(f.find, sizeof(f.find), "156.8");
    LS_CHECK(ls_freq_matches(&any, &f, false, 0, 0));
    snprintf(f.group, sizeof(f.group), "aviation");
    f.find[0] = 0;
    LS_CHECK(ls_freq_matches(&ba, &f, false, 0, 0));
    LS_CHECK(!ls_freq_matches(&any, &f, false, 0, 0));
    LS_EQ_STR(ls_freq_group_tag("PMR/LICENSE-FREE"), "PMR");
    LS_EQ_STR(ls_freq_group_tag("whatever"), "---");
}

LS_CASE(tune_routes_each_mode_to_an_app)
{
    static const struct { int mode; uint8_t target; const char *app, *arg; } WANT[] = {
        { LS_FM_NFM, LS_FR_FM, "fm", "listen" }, { LS_FM_WFM, LS_FR_FM, "fm", "wfm" },
        { LS_FM_AM, LS_FR_HF, "fm", "am" }, { LS_FM_USB, LS_FR_HF, "fm", "usb" },
        { LS_FM_LSB, LS_FR_HF, "fm", "lsb" }, { LS_FM_CW, LS_FR_HF, "fm", "cw" },
        { LS_FM_APRS, LS_FR_FM, "fm", "aprs" }, { LS_FM_POCSAG, LS_FR_FM, "fm", "pocsag" },
        { LS_FM_ACARS, LS_FR_FM, "fm", "acars" }, { LS_FM_AIS, LS_FR_FM, "fm", "ais" },
        { LS_FM_P25, LS_FR_P25, "p25", NULL }, { LS_FM_DMR, LS_FR_P25, "p25", NULL },
        { LS_FM_OOK, LS_FR_SUBGHZ, "subghz", NULL }, { LS_FM_FSK, LS_FR_SUBGHZ, "subghz", NULL },
        { LS_FM_LORA, LS_FR_LABS, "labs", NULL }, { LS_FM_ADSB, LS_FR_OPEN, "adsb", NULL },
        { LS_FM_GPS, LS_FR_OPEN, "gps", NULL }, { LS_FM_LTE, LS_FR_OPEN, "cell", NULL },
        { LS_FM_APT, LS_FR_FM, "fm", "wfm" }, { LS_FM_NXDN, LS_FR_FM, "fm", "listen" },
        { LS_FM_LRPT, LS_FR_NONE, NULL, NULL }, { LS_FM_IRIDIUM, LS_FR_NONE, NULL, NULL },
    };
    for (unsigned i = 0; i < sizeof(WANT) / sizeof(WANT[0]); i++) {
        const ls_freq_route_t r = ls_freq_route(WANT[i].mode, 100000000u);
        LS_EQ_INT(r.target, WANT[i].target);
        LS_EQ_STR(r.app, WANT[i].app);
        LS_EQ_STR(r.arg, WANT[i].arg);
        /* Anything not handed over as it is says why. */
        if (r.target == LS_FR_NONE || WANT[i].mode == LS_FM_NXDN || WANT[i].mode == LS_FM_APT) LS_CHECK(r.note != NULL);
    }
    for (int m = 0; m < LS_FM__COUNT; m++) LS_EQ_INT(ls_freq_mode_parse(ls_freq_mode_name(m)), m);
}

LS_CASE(builtin_set_reads_clean)
{
    ls_freq_kind_t kind;
    const ls_freq_tally_t t = read_text(ls_freqs_builtin_text(), &kind);
    LS_EQ_INT(t.skipped, 0);
    LS_CHECK(t.loaded >= 30 && t.loaded <= 48);
    bool guard = false;
    for (int i = 0; i < s_ngot && i < 64; i++) guard |= s_got[i].hz == 121500000u && s_got[i].mode == LS_FM_AM;
    LS_CHECK(guard);
}

/* ------------------------------------------------------- on the card -- */

static const char *card(void)
{
    static const char *dir = "build/test_freqs_card";
    DIR *d = opendir(dir);
    if (d) {
        struct dirent *e;
        char path[320];
        while ((e = readdir(d))) {
            if (e->d_name[0] == '.') continue;
            snprintf(path, sizeof(path), "%s/%s", dir, e->d_name);
            remove(path);
        }
        closedir(d);
        snprintf(path, sizeof(path), "%s/export", dir);
        rmdir(path);
    }
    make_dir("build");
    make_dir(dir);
    ls_freqs_set_root(dir);
    return dir;
}

static void write_file(const char *path, const char *text)
{
    FILE *f = fopen(path, "w");
    LS_CHECK(f != NULL);
    if (f) { fputs(text, f); fclose(f); }
}

static void load(int which)
{
    ls_freqs_load(which);
    for (int i = 0; i < 1000 && ls_freqs_pump(7); i++) {}
    LS_CHECK(!ls_freqs_loading());
}

static char *slurp(const char *path)
{
    static char buf[4096];
    FILE *f = fopen(path, "r");
    buf[0] = 0;
    if (!f) return buf;
    const size_t n = fread(buf, 1, sizeof(buf) - 1, f);
    buf[n] = 0;
    fclose(f);
    return buf;
}

LS_CASE(store_loads_banks_in_order_and_falls_back_to_builtin)
{
    const char *dir = card();
    LS_CHECK(ls_freqs_scan());
    LS_EQ_INT(ls_freqs_banks(), 0);
    load(LS_FREQS_ALL);
    LS_CHECK(ls_freqs_count() >= 30);                 /* an empty card: the built-in set */
    LS_EQ_INT(ls_freqs_at(0)->bank, LS_FREQ_BANK_BUILTIN);
    char path[128];
    snprintf(path, sizeof(path), "%s/zulu.csv", dir);
    write_file(path, "freq_mhz,mode,name\n1,AM,Z ONE\n");
    snprintf(path, sizeof(path), "%s/argentina.csv", dir);
    write_file(path, "# Argentina\nfreq_mhz,mode,bw_khz,name\n118.1,AM,,A ONE\nbad,AM,,X\n119.1,AM,,A TWO\n");
    snprintf(path, sizeof(path), "%s/notes.txt", dir);
    write_file(path, "not a bank");
    LS_CHECK(ls_freqs_scan());
    LS_EQ_INT(ls_freqs_banks(), 2);
    LS_EQ_STR(ls_freqs_bank(0), "argentina");
    load(LS_FREQS_ALL);
    LS_EQ_INT(ls_freqs_count(), 3);
    LS_EQ_STR(ls_freqs_at(0)->name, "A ONE");
    LS_EQ_INT(ls_freqs_at(0)->line, 3);
    LS_EQ_STR(ls_freqs_at(2)->name, "Z ONE");
    LS_EQ_INT(ls_freqs_at(2)->bank, 1);
    LS_EQ_INT(ls_freqs_tally()->skipped, 1);
    load(1);
    LS_EQ_INT(ls_freqs_count(), 1);
    load(LS_FREQS_BUILTIN);
    LS_CHECK(ls_freqs_count() >= 30);
}

LS_CASE(store_saves_edits_deletes_and_keeps_comments)
{
    const char *dir = card();
    char path[128], why[128];
    snprintf(path, sizeof(path), "%s/argentina.csv", dir);
    write_file(path, "# keep me\nfreq_mhz,mode,name\n118.1,AM,A ONE\n# and me\n119.1,AM,A TWO");  /* no last newline */
    ls_freqs_scan();
    ls_freq_t e;
    LS_EQ_INT(one_line("433.92,OOK,,GARAGE,MINE,subghz,,,,,", &e), 1);
    LS_CHECK(ls_freqs_append("mine", &e, why, sizeof(why)));
    LS_CHECK(strstr(why, "433.920") != NULL);
    LS_CHECK(ls_freqs_is_dirty());
    snprintf(path, sizeof(path), "%s/mine.csv", dir);
    LS_CHECK(!strncmp(slurp(path), LS_FREQ_CSV_HEADER, strlen(LS_FREQ_CSV_HEADER)));
    LS_EQ_INT(ls_freqs_bank_find("mine"), 1);
    /* Appending to a file that ends mid-line starts a new line. */
    LS_CHECK(ls_freqs_append("argentina", &e, why, sizeof(why)));
    snprintf(path, sizeof(path), "%s/argentina.csv", dir);
    LS_CHECK(strstr(slurp(path), "A TWO\n433.920,OOK,GARAGE\n") != NULL);   /* its own three columns */
    load(0);
    LS_EQ_INT(ls_freqs_count(), 3);
    /* Edit the second, delete the first: comments stay where they were. */
    ls_freq_t changed = *ls_freqs_at(1);
    snprintf(changed.name, sizeof(changed.name), "A TWO NEW");
    LS_CHECK(ls_freqs_rewrite(1, &changed, why, sizeof(why)));
    load(0);
    LS_CHECK(ls_freqs_rewrite(0, NULL, why, sizeof(why)));
    load(0);
    const char *text = slurp(path);
    LS_CHECK(strstr(text, "# keep me\n") == text);
    LS_CHECK(strstr(text, "# and me\n") != NULL);
    LS_CHECK(strstr(text, "A ONE") == NULL);
    LS_CHECK(strstr(text, "A TWO NEW") != NULL);
    LS_EQ_INT(ls_freqs_count(), 2);
    /* A note longer than memory keeps, and so does a column FREQS does not
       know; only the field that changed moves. */
    write_file(path, "freq_mhz,mode,name,notes,owner\n"
               "27.185,AM,CB 19,\"road channel by convention; Banda Ciudadana needs ENACOM authorisation, standard 40 channel plan\",lu1xyz\n");
    load(0);
    LS_EQ_INT(ls_freqs_count(), 1);
    LS_EQ_INT((int)strlen(ls_freqs_at(0)->notes), LS_FREQ_NOTES - 1);
    changed = *ls_freqs_at(0);
    snprintf(changed.name, sizeof(changed.name), "CB CH19");
    LS_CHECK(ls_freqs_rewrite(0, &changed, why, sizeof(why)));
    LS_CHECK(strstr(slurp(path), "27.185,AM,CB CH19,\"road channel by convention; Banda Ciudadana needs ENACOM "
                                 "authorisation, standard 40 channel plan\",lu1xyz\n") != NULL);
    /* A file changed behind its back is not edited blind. */
    write_file(path, "freq_mhz,mode,name\n999,NFM,SOMETHING ELSE\n888,NFM,X\n");
    LS_CHECK(!ls_freqs_rewrite(0, NULL, why, sizeof(why)));
    LS_CHECK(strstr(slurp(path), "SOMETHING ELSE") != NULL);
    /* The built-in set is not a file. */
    load(LS_FREQS_BUILTIN);
    LS_CHECK(!ls_freqs_rewrite(0, NULL, why, sizeof(why)));
}

LS_CASE(store_exports_and_imports)
{
    const char *dir = card();
    char path[128], out[128], why[128];
    snprintf(path, sizeof(path), "%s/argentina.csv", dir);
    write_file(path, "freq_mhz,mode,name\n154.13,NFM,FIRE\n154.13,NFM,FIRE AGAIN\n851.0125,P25,TRUNK\n121.5,AM,AIR\n");
    ls_freqs_scan();
    load(0);
    uint16_t idx[8];
    ls_freq_filter_t f = { 0 };
    const int n = ls_freqs_view(&f, false, 0, 0, idx, 8);
    LS_EQ_INT(n, 4);
    snprintf(out, sizeof(out), "%s/export/view.lscan", dir);
    LS_CHECK(ls_freqs_export(idx, n, out, true, why, sizeof(why)));
    LS_CHECK(strstr(why, "Wrote 2") != NULL);          /* AM left out, the duplicate too */
    LS_CHECK(!strncmp(slurp(out), "LSCAN1\n", 7));
    /* What the scanner itself will read. */
    char msg[96];
    FILE *lf = fopen(out, "r");
    int good = 0;
    char line[192];
    while (lf && fgets(line, sizeof(line), lf)) {
        scan_channel_t ch;
        if (line[0] != '#' && strncmp(line, "LSCAN1", 6)) good += scan_import_line(line, &ch);
    }
    if (lf) fclose(lf);
    LS_EQ_INT(good, 2);
    (void)msg;
    snprintf(out, sizeof(out), "%s/export/view.csv", dir);
    LS_CHECK(ls_freqs_export(idx, n, out, false, why, sizeof(why)));
    /* Sorted by name, then imported back as a bank of its own. */
    f.sort = LS_FS_NAME;
    LS_EQ_INT(ls_freqs_view(&f, false, 0, 0, idx, 8), 4);
    LS_EQ_STR(ls_freqs_at(idx[0])->name, "AIR");
    snprintf(path, sizeof(path), "%s/My Radio.json", dir);
    write_file(path, "{\"lists\":{\"General\":{\"bookmarks\":{\"Ch 1\":{\"frequency\":162550000.0,\"bandwidth\":12500.0,\"mode\":0}}}}}");
    LS_CHECK(ls_freqs_import(path, why, sizeof(why)));
    LS_CHECK(strstr(why, "SDR++: 1 into myradio") != NULL);
    LS_CHECK(ls_freqs_bank_find("myradio") >= 0);
    LS_CHECK(ls_freqs_import(out, why, sizeof(why)));   /* a CSV from export */
    LS_CHECK(ls_freqs_bank_find("view") >= 0);
    snprintf(path, sizeof(path), "%s/garbage.txt", dir);
    write_file(path, "nothing,here\nat,all\n");
    LS_CHECK(!ls_freqs_import(path, why, sizeof(why)));
    LS_CHECK(ls_freqs_bank_find("garbage") < 0);
}

/* Importing the same file again takes the next free name, and when every
   name is taken it refuses: it never writes over a bank already there. */
LS_CASE(store_import_never_writes_over_a_bank)
{
    const char *dir = card();
    char path[128], bank[128], why[128];
    snprintf(path, sizeof(path), "%s/dup.txt", dir);
    write_file(path, "freq_mhz,mode,name\n146.52,NFM,CALL\n");
    for (int i = 1; i <= 99; i++) {
        LS_CHECK(ls_freqs_import(path, why, sizeof(why)));
        if (i == 1) LS_CHECK(strstr(why, "into dup") != NULL);
        if (i == 10) LS_CHECK(strstr(why, "into dup-10") != NULL);
    }
    /* Mark one so an overwrite would show. */
    snprintf(bank, sizeof(bank), "%s/dup-9.csv", dir);
    write_file(bank, "freq_mhz,mode,name\n162.55,NFM,KEEP ME\n");
    LS_CHECK(!ls_freqs_import(path, why, sizeof(why)));
    LS_CHECK(strstr(why, "taken") != NULL);
    LS_CHECK(strstr(slurp(bank), "KEEP ME") != NULL);
    snprintf(bank, sizeof(bank), "%s/dup-100.csv", dir);
    struct stat st;
    LS_CHECK(stat(bank, &st) != 0);
}

/* Groups the list has no colour for, and fields that reached their limit
   (which may have been cut). */
static int s_odd_group, s_full_field;
static bool audit(const ls_freq_t *e, void *ctx)
{
    (void)ctx;
    s_odd_group += !strcmp(ls_freq_group_tag(e->group), "---");
    s_full_field += strlen(e->group) == LS_FREQ_GROUP - 1 || strlen(e->tags) == LS_FREQ_TAGS - 1;
    return keep(e, ctx);
}

/* The country bank the cards carry, when it is in the tree: every line reads. */
LS_CASE(shipped_banks_read_clean)
{
    static const char *const PATHS[] = { FREQS_TOOLS "/freqs/argentina.csv", FREQS_TOOLS "/argentina.csv" };
    for (unsigned i = 0; i < sizeof(PATHS) / sizeof(PATHS[0]); i++) {
        FILE *f = fopen(PATHS[i], "r");
        if (!f) continue;
        ls_freq_tally_t t;
        s_ngot = s_odd_group = s_full_field = 0;
        LS_CHECK(ls_freq_read(f, "", audit, NULL, &t, NULL));
        fclose(f);
        ls_note("%s: %d loaded, %d skipped (first %d)", PATHS[i], t.loaded, t.skipped, t.first_bad);
        LS_EQ_INT(t.skipped, 0);
        LS_CHECK(t.loaded > 0 && t.loaded <= LS_FREQ_MAX);
        /* Every data line, counted the plain way, is an entry. */
        f = fopen(PATHS[i], "r");
        char line[512];
        int lines = -1;     /* the header */
        while (f && fgets(line, sizeof(line), f)) lines += line[0] != '#' && line[0] != '\n' && line[0] != '\r';
        if (f) fclose(f);
        LS_EQ_INT(t.loaded, lines);
        /* Nothing the data carries is cut, and every group has its colour. */
        LS_EQ_INT(s_odd_group, 0);
        LS_EQ_INT(s_full_field, 0);
    }
}
