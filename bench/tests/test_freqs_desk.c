/* FREQS on the desktop: the table's orders, the star, duplicates, fields
   typed and checked, banks made and renamed, entries copied, moved, deleted
   many at a time and put back, CHIRP's CSV out and in again - and the desk
   widgets it is drawn with (the text field, the menu, the table's columns). */
#include "ls_test.h"
#include "ls_freqs.h"
#include "ls_freqs_desk.h"
#include "ls_deskui.h"

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

static void wipe(const char *dir)
{
    DIR *d = opendir(dir);
    if (!d) return;
    struct dirent *e;
    char path[320];
    while ((e = readdir(d))) {
        if (e->d_name[0] == '.') continue;
        snprintf(path, sizeof(path), "%s/%s", dir, e->d_name);
        remove(path);
    }
    closedir(d);
}

static const char *card(void)
{
    static const char *dir = "build/test_freqs_desk_card";
    char sub[96];
    snprintf(sub, sizeof(sub), "%s/export", dir); wipe(sub); rmdir(sub);
    snprintf(sub, sizeof(sub), "%s/trash", dir); wipe(sub); rmdir(sub);
    wipe(dir);
    make_dir("build");
    make_dir(dir);
    ls_freqs_set_root(dir);
    ls_freqs_undo_forget();
    return dir;
}

static void write_bank(const char *name, const char *text)
{
    char path[160];
    snprintf(path, sizeof(path), "%s/%s.csv", ls_freqs_root(), name);
    FILE *f = fopen(path, "w");
    LS_CHECK(f != NULL);
    if (f) { fputs(text, f); fclose(f); }
}

static char *slurp(const char *name)
{
    static char buf[8192];
    char path[160];
    snprintf(path, sizeof(path), "%s/%s", ls_freqs_root(), name);
    FILE *f = fopen(path, "r");
    buf[0] = 0;
    if (!f) return buf;
    const size_t n = fread(buf, 1, sizeof(buf) - 1, f);
    buf[n] = 0;
    fclose(f);
    return buf;
}

static void load(int which)
{
    ls_freqs_scan();
    ls_freqs_load(which);
    for (int i = 0; i < 1000 && ls_freqs_pump(7); i++) {}
    LS_CHECK(!ls_freqs_loading());
}

static int all(uint16_t *idx)
{
    for (int i = 0; i < ls_freqs_count(); i++) idx[i] = (uint16_t)i;
    return ls_freqs_count();
}

#define HEAD "freq_mhz,mode,bw_khz,name,group,tags,lat,lon,radius_km,tone,notes\n"
static const char CITY[] = HEAD
    "146.94,NFM,,LU1 REPEATER,AMATEUR,tx-ok,-31.41,-64.19,60,88.5,shift -0.6\n"       /* 0 */
    "118.3,AM,,TOWER,AVIATION,fav,-31.3236,-64.2080,40,,\n"                             /* 1 */
    "433.92,OOK,,GARAGE,MINE,,,,,,\n"                                                   /* 2 */
    "146.94,NFM,,LU1 AGAIN,AMATEUR,,-31.4102,-64.1903,,D023N,\n"                        /* 3: same channel as 0 */
    "7.1,LSB,,FORTY METRES,AMATEUR,net fav,,,,,\n"                                      /* 4 */
    "156.8,NFM,,CH16,MARINE,,-31.38,-64.47,15,123.0,\n";                                /* 5 */

static const char *name_at(const uint16_t *idx, int k) { return ls_freqs_at(idx[k])->name; }

LS_CASE(table_sorts_by_any_column_either_way)
{
    card();
    write_bank("city", CITY);
    load(0);
    uint16_t idx[16];
    int n = all(idx);
    LS_EQ_INT(n, 6);
    ls_freqd_sort(idx, n, LS_FC_FREQ, false, false, 0, 0);
    LS_EQ_STR(name_at(idx, 0), "FORTY METRES");
    LS_EQ_STR(name_at(idx, 5), "GARAGE");
    /* The two on 146.94 stay in the file's order going up, and turn with the rest going down. */
    LS_EQ_STR(name_at(idx, 2), "LU1 REPEATER");
    LS_EQ_STR(name_at(idx, 3), "LU1 AGAIN");
    ls_freqd_sort(idx, n, LS_FC_FREQ, true, false, 0, 0);
    LS_EQ_STR(name_at(idx, 0), "GARAGE");
    LS_EQ_STR(name_at(idx, 5), "FORTY METRES");
    ls_freqd_sort(idx, n, LS_FC_NAME, false, false, 0, 0);
    LS_EQ_STR(name_at(idx, 0), "CH16");
    LS_EQ_STR(name_at(idx, 5), "TOWER");
    /* Group, and inside a group by frequency. */
    ls_freqd_sort(idx, n, LS_FC_GROUP, false, false, 0, 0);
    LS_EQ_STR(name_at(idx, 0), "FORTY METRES");
    LS_EQ_STR(name_at(idx, 1), "LU1 REPEATER");
    LS_EQ_STR(name_at(idx, 3), "TOWER");
    /* Tones as numbers, and an entry with none after every one that has. */
    ls_freqd_sort(idx, n, LS_FC_TONE, false, false, 0, 0);
    LS_EQ_STR(name_at(idx, 0), "LU1 AGAIN");       /* D023N reads as 0 */
    LS_EQ_STR(name_at(idx, 1), "LU1 REPEATER");
    LS_EQ_STR(name_at(idx, 2), "CH16");
    LS_CHECK(!ls_freqs_at(idx[3])->tone[0]);
    /* Starred first. */
    ls_freqd_sort(idx, n, LS_FC_STAR, false, false, 0, 0);
    LS_CHECK(ls_freqd_starred(ls_freqs_at(idx[0])) && ls_freqd_starred(ls_freqs_at(idx[1])));
    LS_CHECK(!ls_freqd_starred(ls_freqs_at(idx[2])));
    /* Distance from the tower: the tower, then the repeater, the lake; the unplaced last. */
    ls_freqd_sort(idx, n, LS_FC_DIST, false, true, -31.3236, -64.2080);
    LS_EQ_STR(name_at(idx, 0), "TOWER");
    LS_EQ_STR(name_at(idx, 1), "LU1 REPEATER");
    LS_EQ_STR(name_at(idx, 2), "LU1 AGAIN");
    LS_EQ_STR(name_at(idx, 3), "CH16");
    LS_CHECK(ls_freqs_at(idx[4])->lat_e5 == LS_FREQ_NOWHERE);
    /* As listed, and listed backwards. */
    ls_freqd_sort(idx, n, LS_FC_LISTED, false, false, 0, 0);
    for (int i = 0; i < n; i++) LS_EQ_INT(idx[i], i);
    ls_freqd_sort(idx, n, LS_FC_LISTED, true, false, 0, 0);
    for (int i = 0; i < n; i++) LS_EQ_INT(idx[i], n - 1 - i);
    LS_EQ_INT(ls_freqd_band(7100000), LS_FB_HF);
    LS_EQ_INT(ls_freqd_band(146940000), LS_FB_VHF);
    LS_EQ_INT(ls_freqd_band(433920000), LS_FB_UHF);
}

LS_CASE(star_is_a_tag_and_duplicates_are_marked)
{
    card();
    write_bank("city", CITY);
    load(0);
    ls_freq_t e = *ls_freqs_at(0);
    LS_CHECK(!ls_freqd_starred(&e));
    ls_freqd_set_star(&e, true);
    LS_EQ_STR(e.tags, "fav tx-ok");
    LS_CHECK(ls_freqd_starred(&e));
    ls_freqd_set_star(&e, true);
    LS_EQ_STR(e.tags, "fav tx-ok");
    ls_freqd_set_star(&e, false);
    LS_EQ_STR(e.tags, "tx-ok");
    e = *ls_freqs_at(4);                     /* "net fav" */
    LS_CHECK(ls_freqd_starred(&e));
    ls_freqd_set_star(&e, false);
    LS_EQ_STR(e.tags, "net");
    snprintf(e.tags, sizeof(e.tags), "favourite");      /* not the word */
    LS_CHECK(!ls_freqd_starred(&e));
    /* A tag list with no room left keeps the star and loses its tail. */
    memset(e.tags, 'x', sizeof(e.tags) - 1);
    e.tags[sizeof(e.tags) - 1] = 0;
    ls_freqd_set_star(&e, true);
    LS_CHECK(ls_freqd_starred(&e));
    LS_EQ_INT((int)strlen(e.tags), LS_FREQ_TAGS - 1);

    uint16_t idx[16];
    uint8_t bits[4];
    const int n = all(idx);
    LS_EQ_INT(ls_freqd_dups(idx, n, bits, sizeof(bits)), 2);
    LS_CHECK(ls_dui_bit(bits, 0) && ls_dui_bit(bits, 3));
    LS_CHECK(!ls_dui_bit(bits, 1) && !ls_dui_bit(bits, 5));
    /* Out of the view, out of the count. */
    LS_EQ_INT(ls_freqd_dups(idx, 3, bits, sizeof(bits)), 0);
    /* The same frequency at another airport is another channel; a line for
       the same place again is not. */
    write_bank("towers", HEAD "118.3,AM,,TOWER A,AVIATION,,-31.3236,-64.2080,40,,\n"
                              "118.3,AM,,TOWER B,AVIATION,,-34.5597,-58.4150,40,,\n"
                              "118.3,AM,,TOWER A AGAIN,AVIATION,,-31.3240,-64.2075,40,,\n"
                              "118.3,AM,,NO PLACE,AVIATION,,,,,,\n");
    ls_freqs_scan();
    load(ls_freqs_bank_find("towers"));
    LS_EQ_INT(ls_freqs_count(), 4);
    LS_EQ_INT(ls_freqd_dups(idx, all(idx), bits, sizeof(bits)), 2);
    LS_EQ_INT(bits[0], 5);
}

LS_CASE(fields_are_checked_as_they_are_typed)
{
    ls_freq_t e;
    char why[96], text[LS_FF_TEXT];
    ls_freqd_blank(&e);
    LS_CHECK(e.lat_e5 == LS_FREQ_NOWHERE && e.mode == LS_FM_NFM);
    LS_CHECK(ls_freqd_field_set(&e, LS_FF_FREQ, " 446.00625 ", why, sizeof(why)));
    LS_EQ_UINT(e.hz, 446006250u);
    ls_freqd_field_get(&e, LS_FF_FREQ, text, sizeof(text));
    LS_EQ_STR(text, "446.00625");
    LS_CHECK(!ls_freqd_field_set(&e, LS_FF_FREQ, "abc", why, sizeof(why)));
    LS_CHECK(strstr(why, "0.009") != NULL);
    LS_CHECK(!ls_freqd_field_set(&e, LS_FF_FREQ, "146.5x", why, sizeof(why)));
    LS_CHECK(!ls_freqd_field_set(&e, LS_FF_FREQ, "7000", why, sizeof(why)));
    LS_CHECK(!ls_freqd_field_set(&e, LS_FF_FREQ, "", why, sizeof(why)));
    LS_EQ_UINT(e.hz, 446006250u);            /* a refusal leaves it alone */
    LS_CHECK(ls_freqd_field_set(&e, LS_FF_MODE, "c4fm", why, sizeof(why)));
    LS_EQ_INT(e.mode, LS_FM_YSF);
    LS_CHECK(!ls_freqd_field_set(&e, LS_FF_MODE, "WARBLE", why, sizeof(why)));
    LS_CHECK(ls_freqd_field_set(&e, LS_FF_BW, "12.5", why, sizeof(why)));
    LS_EQ_INT(e.bw, 125);
    ls_freqd_field_get(&e, LS_FF_BW, text, sizeof(text));
    LS_EQ_STR(text, "12.5");
    LS_CHECK(ls_freqd_field_set(&e, LS_FF_BW, "", why, sizeof(why)));
    LS_EQ_INT(e.bw, 0);
    LS_CHECK(ls_freqd_field_set(&e, LS_FF_NAME, "  PMR 1  ", why, sizeof(why)));
    LS_EQ_STR(e.name, "PMR 1");
    LS_CHECK(!ls_freqd_field_set(&e, LS_FF_NAME, "A NAME THAT IS LONGER THAN THE LIST HOLDS", why, sizeof(why)));
    LS_CHECK(!ls_freqd_field_set(&e, LS_FF_NAME, "SAY \"HI\"", why, sizeof(why)));
    LS_EQ_STR(e.name, "PMR 1");
    LS_CHECK(ls_freqd_field_set(&e, LS_FF_TONE, "88.5", why, sizeof(why)));
    LS_CHECK(ls_freqd_field_set(&e, LS_FF_TONE, "d023N", why, sizeof(why)));
    LS_EQ_STR(e.tone, "D023N");
    LS_CHECK(!ls_freqd_field_set(&e, LS_FF_TONE, "12", why, sizeof(why)));
    LS_CHECK(!ls_freqd_field_set(&e, LS_FF_TONE, "loud", why, sizeof(why)));
    LS_CHECK(ls_freqd_field_set(&e, LS_FF_LAT, "-31.4100", why, sizeof(why)));
    LS_EQ_INT(e.lat_e5, -3141000);
    ls_freqd_field_get(&e, LS_FF_LAT, text, sizeof(text));
    LS_EQ_STR(text, "-31.4100");
    LS_CHECK(!ls_freqd_field_set(&e, LS_FF_LAT, "91", why, sizeof(why)));
    LS_CHECK(!ls_freqd_field_set(&e, LS_FF_LON, "-181", why, sizeof(why)));
    LS_CHECK(ls_freqd_field_set(&e, LS_FF_LON, "-0.5", why, sizeof(why)));
    ls_freqd_field_get(&e, LS_FF_LON, text, sizeof(text));
    LS_EQ_STR(text, "-0.5000");
    LS_CHECK(ls_freqd_field_set(&e, LS_FF_LAT, " ", why, sizeof(why)));
    LS_CHECK(e.lat_e5 == LS_FREQ_NOWHERE);
    LS_CHECK(ls_freqd_field_set(&e, LS_FF_RADIUS, "60", why, sizeof(why)));
    LS_EQ_INT(e.radius_km, 60);
    LS_CHECK(!ls_freqd_field_set(&e, LS_FF_RADIUS, "far", why, sizeof(why)));
    /* Every field says what it is and holds what its entry holds. */
    for (int f = 0; f < LS_FF__COUNT; f++) {
        LS_CHECK(ls_freqd_field_label(f)[0] && ls_freqd_field_hint(f)[0]);
        LS_CHECK(ls_freqd_field_max(f) > 0 && ls_freqd_field_max(f) < LS_FF_TEXT && ls_freqd_field_max(f) < LS_DUI_TEXT);
    }
}

LS_CASE(banks_are_made_renamed_and_put_in_the_trash)
{
    card();
    char why[96], name[24];
    LS_CHECK(ls_freqd_bank_name("My Repeaters!", name, sizeof(name), why, sizeof(why)));
    LS_EQ_STR(name, "my-repeaters");
    LS_CHECK(!ls_freqd_bank_name(" ../ ", name, sizeof(name), why, sizeof(why)));
    LS_CHECK(ls_freqd_bank_new("Field Day", why, sizeof(why)));
    LS_EQ_INT(ls_freqs_bank_find("field-day"), 0);
    LS_EQ_STR(slurp("field-day.csv"), HEAD);
    LS_CHECK(!ls_freqd_bank_new("field day", why, sizeof(why)));         /* there already */
    LS_CHECK(strstr(why, "already") != NULL);
    write_bank("city", CITY);
    ls_freqs_scan();
    LS_CHECK(!ls_freqd_bank_rename(ls_freqs_bank_find("city"), "field-day", why, sizeof(why)));
    LS_CHECK(ls_freqd_bank_rename(ls_freqs_bank_find("city"), "Cordoba", why, sizeof(why)));
    LS_EQ_INT(ls_freqs_bank_find("city"), -1);
    LS_CHECK(ls_freqs_bank_find("cordoba") >= 0);
    LS_CHECK(!ls_freqd_bank_rename(-1, "x", why, sizeof(why)));
    /* Trashed: out of the list, and the file is still on the card. */
    LS_CHECK(ls_freqd_bank_trash(ls_freqs_bank_find("cordoba"), why, sizeof(why)));
    LS_EQ_INT(ls_freqs_bank_find("cordoba"), -1);
    LS_EQ_INT(ls_freqs_banks(), 1);
    LS_CHECK(strstr(slurp("trash/cordoba.csv"), "LU1 REPEATER") != NULL);
}

LS_CASE(many_are_deleted_at_once_and_put_back_whole)
{
    card();
    /* Its own column order, a comment, an extra column and a note longer than a screen line. */
    write_bank("alpha", "# keep me\nname,freq_mhz,mode,notes,owner\n"
                        "ONE,118.1,AM,short,ana\n"
                        "TWO,119.1,AM,a note that goes on a good deal longer than the fifty-five characters a line shows,ben\n"
                        "# and me\n"
                        "THREE,120.1,AM,,cat\n"
                        "FOUR,121.1,AM,,dan");                    /* no last newline */
    write_bank("beta", HEAD "433.92,OOK,,GARAGE,MINE,,,,,,\n145.5,NFM,,CALL,AMATEUR,,,,,,\n");
    load(LS_FREQS_ALL);
    LS_EQ_INT(ls_freqs_count(), 6);
    char why[128];
    /* TWO and FOUR from alpha, GARAGE from beta, in no order. */
    const uint16_t gone[3] = { 4, 1, 3 };
    LS_EQ_INT(ls_freqs_remove(gone, 3, why, sizeof(why)), 3);
    LS_CHECK(strstr(why, "Deleted 3") != NULL);
    LS_EQ_INT(ls_freqs_undo_count(), 3);
    LS_EQ_STR(slurp("alpha.csv"), "# keep me\nname,freq_mhz,mode,notes,owner\nONE,118.1,AM,short,ana\n# and me\nTHREE,120.1,AM,,cat\n");
    LS_EQ_STR(slurp("beta.csv"), HEAD "145.5,NFM,,CALL,AMATEUR,,,,,,\n");
    LS_CHECK(ls_freqs_is_dirty());
    /* Back at the end of their own files, every column as it was. */
    LS_EQ_INT(ls_freqs_undo(why, sizeof(why)), 3);
    LS_EQ_INT(ls_freqs_undo_count(), 0);
    LS_CHECK(strstr(slurp("alpha.csv"), "THREE,120.1,AM,,cat\n"
        "TWO,119.1,AM,a note that goes on a good deal longer than the fifty-five characters a line shows,ben\n"
        "FOUR,121.1,AM,,dan\n") != NULL);
    LS_CHECK(strstr(slurp("beta.csv"), "CALL,AMATEUR,,,,,,\n433.92,OOK,,GARAGE,MINE,,,,,,\n") != NULL);
    LS_EQ_INT(ls_freqs_undo(why, sizeof(why)), 0);
    LS_CHECK(strstr(why, "Nothing") != NULL);
    load(LS_FREQS_ALL);
    LS_EQ_INT(ls_freqs_count(), 6);

    /* A file that changed since it was loaded is not edited blind, and the other one still is. */
    write_bank("alpha", "name,freq_mhz,mode\nSOMEONE ELSE,1.8,LSB\nONE,118.1,AM\n");
    uint16_t idx[8];
    const int n = all(idx);
    const int went = ls_freqs_remove(idx, n, why, sizeof(why));
    LS_EQ_INT(went, 2);                                         /* beta's two */
    LS_CHECK(strstr(why, "changed") != NULL);
    LS_CHECK(strstr(slurp("alpha.csv"), "SOMEONE ELSE") != NULL);
    LS_EQ_STR(slurp("beta.csv"), HEAD);
    /* The built-in set stays. */
    load(LS_FREQS_BUILTIN);
    const uint16_t first = 0;
    LS_EQ_INT(ls_freqs_remove(&first, 1, why, sizeof(why)), 0);
    LS_CHECK(strstr(why, "built-in") != NULL);
}

LS_CASE(entries_copy_and_move_between_banks)
{
    card();
    write_bank("city", CITY);
    write_bank("mine", HEAD "433.92,OOK,,MY REMOTE,MINE,,,,,,\n");
    load(LS_FREQS_ALL);
    LS_EQ_INT(ls_freqs_count(), 7);                             /* city 0..5, mine 6 */
    char why[128];
    /* A copy leaves out what the bank has already: GARAGE is on 433.92 OOK, as MY REMOTE is. */
    const uint16_t some[3] = { 1, 2, 6 };
    LS_EQ_INT(ls_freqd_copy(some, 3, "mine", true, why, sizeof(why)), 1);
    LS_CHECK(strstr(why, "Copied 1 to mine") != NULL && strstr(why, "already there") != NULL);
    LS_CHECK(strstr(slurp("mine.csv"), "MY REMOTE,MINE,,,,,,\n118.300,AM,,TOWER,AVIATION,fav,-31.3236") != NULL);
    LS_CHECK(strstr(slurp("city.csv"), "TOWER") != NULL);
    /* Into a bank that is not there yet: it is made. */
    load(LS_FREQS_ALL);
    const uint16_t two[2] = { 0, 3 };
    LS_EQ_INT(ls_freqd_copy(two, 2, "repeaters", false, why, sizeof(why)), 2);
    LS_CHECK(ls_freqs_bank_find("repeaters") >= 0);
    LS_CHECK(strstr(slurp("repeaters.csv"), "LU1 AGAIN") != NULL);

    /* A move takes them out of their file and nothing else. */
    load(LS_FREQS_ALL);
    const int city = ls_freqs_bank_find("city");
    uint16_t pick[4];
    int n = 0;
    for (int i = 0; i < ls_freqs_count(); i++) {
        const ls_freq_t *e = ls_freqs_at(i);
        if (e->bank == city && (!strcmp(e->name, "CH16") || !strcmp(e->name, "FORTY METRES"))) pick[n++] = (uint16_t)i;
    }
    LS_EQ_INT(n, 2);
    LS_EQ_INT(ls_freqd_move(pick, n, "mine", why, sizeof(why)), 2);
    LS_CHECK(strstr(why, "Moved 2 to mine") != NULL);
    LS_CHECK(strstr(slurp("city.csv"), "CH16") == NULL && strstr(slurp("city.csv"), "FORTY") == NULL);
    LS_CHECK(strstr(slurp("city.csv"), "GARAGE") != NULL);
    LS_CHECK(strstr(slurp("mine.csv"), "7.100,LSB,,FORTY METRES") != NULL);
    LS_CHECK(strstr(slurp("mine.csv"), "CH16,MARINE,,-31.38") != NULL);
    LS_CHECK(strstr(slurp("mine.csv"), ",15,123.0,") != NULL);
    LS_EQ_INT(ls_freqs_undo_count(), 0);                        /* a move is not a delete to undo */
    /* Into the bank they are in: nothing happens. */
    load(LS_FREQS_ALL);
    n = 0;
    for (int i = 0; i < ls_freqs_count(); i++) if (ls_freqs_at(i)->bank == ls_freqs_bank_find("mine")) pick[n < 4 ? n++ : 3] = (uint16_t)i;
    const int before = (int)strlen(slurp("mine.csv"));
    LS_EQ_INT(ls_freqd_move(pick, n < 4 ? n : 4, "mine", why, sizeof(why)), 0);
    LS_EQ_INT((int)strlen(slurp("mine.csv")), before);
    /* Built-in entries are copied by a move, and stay. */
    load(LS_FREQS_BUILTIN);
    const uint16_t guard = 0;
    LS_EQ_INT(ls_freqd_move(&guard, 1, "mine", why, sizeof(why)), 1);
    LS_CHECK(strstr(why, "built-in") != NULL);
    LS_CHECK(strstr(slurp("mine.csv"), "AIR DISTRESS") != NULL);
    /* A list into one bank in one go. */
    ls_freq_t list[3];
    for (int i = 0; i < 3; i++) list[i] = *ls_freqs_at(i);
    LS_CHECK(ls_freqs_append_many("three", list, 3, why, sizeof(why)));
    LS_CHECK(strstr(why, "Saved 3 to three") != NULL);
    load(ls_freqs_bank_find("three"));
    LS_EQ_INT(ls_freqs_count(), 3);
}

static ls_freq_t s_got[16];
static int s_ngot;
static bool keep(const ls_freq_t *e, void *ctx)
{
    (void)ctx;
    if (s_ngot < 16) s_got[s_ngot] = *e;
    s_ngot++;
    return true;
}

LS_CASE(chirp_csv_goes_out_and_reads_back)
{
    card();
    write_bank("city", CITY);
    load(0);
    uint16_t idx[16];
    const int n = all(idx);
    char path[160], why[128];
    snprintf(path, sizeof(path), "%s/export/city-chirp.csv", ls_freqs_root());
    LS_CHECK(ls_freqd_export_chirp(idx, n, path, why, sizeof(why)));
    LS_CHECK(strstr(why, "Wrote 4") != NULL && strstr(why, "2 not FM or AM") != NULL);      /* OOK and LSB stay */
    const char *text = slurp("export/city-chirp.csv");
    LS_CHECK(!strncmp(text, "Location,Name,Frequency,Duplex,Offset,Tone,", 43));
    LS_CHECK(strstr(text, "0,LU1 REPEATER,146.940000,-,0.600000,TSQL,88.5,88.5,023,NN,FM,5.00,,shift -0.6,") != NULL);
    LS_CHECK(strstr(text, "2,LU1 AGAIN,146.940000,,0.000000,DTCS,88.5,88.5,023,NN,FM,") != NULL);
    LS_CHECK(strstr(text, "3,CH16,156.800000,,0.000000,TSQL,123.0,123.0,023,NN,FM,") != NULL);
    /* And FREQS' own importer takes it for what it is. */
    FILE *f = fopen(path, "r");
    LS_CHECK(f != NULL);
    ls_freq_tally_t t;
    ls_freq_kind_t kind;
    s_ngot = 0;
    LS_CHECK(f && ls_freq_read(f, "BACK", keep, NULL, &t, &kind));
    if (f) fclose(f);
    LS_EQ_INT(kind, LS_FI_CHIRP);
    LS_EQ_INT(s_ngot, 4);
    LS_EQ_UINT(s_got[0].hz, 146940000u);
    LS_EQ_STR(s_got[0].tone, "88.5");
    LS_EQ_STR(s_got[2].tone, "D023N");
    LS_EQ_INT(s_got[1].mode, LS_FM_AM);
    LS_EQ_STR(s_got[3].name, "CH16");
}

/* ------------------------------------------------------- the widgets -- */

LS_CASE(text_field_edits_at_the_caret)
{
    ls_dui_edit_t e;
    ls_dui_edit_set(&e, "145.5", 8);
    LS_EQ_INT(e.caret, 5);
    LS_CHECK(ls_dui_edit_key(&e, LS_TK_CHAR, '0'));
    LS_EQ_STR(e.text, "145.50");
    LS_CHECK(ls_dui_edit_key(&e, LS_TK_HOME, 0));
    LS_CHECK(ls_dui_edit_key(&e, LS_TK_DELETE, 0));
    LS_EQ_STR(e.text, "45.50");
    LS_CHECK(ls_dui_edit_key(&e, LS_TK_CHAR, '4'));
    LS_CHECK(ls_dui_edit_key(&e, LS_TK_RIGHT, 0));
    LS_CHECK(ls_dui_edit_key(&e, LS_TK_BACKSPACE, 0));
    LS_EQ_STR(e.text, "45.50");
    LS_EQ_INT(e.caret, 1);
    LS_CHECK(ls_dui_edit_key(&e, LS_TK_END, 0));
    for (int i = 0; i < 6; i++) LS_CHECK(ls_dui_edit_key(&e, LS_TK_CHAR, '9'));     /* full: taken, not added */
    LS_EQ_STR(e.text, "45.50999");
    LS_EQ_INT(e.len, 8);
    LS_CHECK(!ls_dui_edit_key(&e, LS_TK_ENTER, 0));
    LS_CHECK(!ls_dui_edit_key(&e, LS_TK_ESC, 0));
    LS_CHECK(!ls_dui_edit_key(&e, LS_TK_TAB, 0));
    /* A click puts the caret under it; past the text, at its end. */
    ls_dui_edit_click(&e, 10, 12, 13);
    LS_EQ_INT(e.caret, 2);
    ls_dui_edit_click(&e, 10, 12, 21);
    LS_EQ_INT(e.caret, 8);
    ls_dui_edit_set(&e, "a name far too long for it", 4);
    LS_EQ_STR(e.text, "a na");
}

LS_CASE(menu_walks_past_rules_and_greyed_rows)
{
    static const ls_dui_item_t M[] = {
        { "HEAD", NULL, -1, LS_DUI_HEAD, 0 }, { "New", "Ins", 10, 0, 0 }, { "Edit", "F2", 11, LS_DUI_OFF, 0 },
        { NULL, NULL, -1, LS_DUI_SEP, 0 }, { "Delete", "Del", 12, 0, 0 }, { "Near", NULL, 13, LS_DUI_CHECK, 0 },
    };
    ls_dui_popup_t p;
    ls_dui_popup_open(&p, M, 6, 5, 5, -1, TUI_YELLOW);
    LS_CHECK(p.open);
    LS_EQ_INT(p.sel, 5);                                         /* the ticked one */
    LS_EQ_INT(ls_dui_popup_key(&p, LS_TK_DOWN, 0), LS_DUI_NONE);
    LS_EQ_INT(p.sel, 1);                                         /* round, past the caption */
    LS_EQ_INT(ls_dui_popup_key(&p, LS_TK_DOWN, 0), LS_DUI_NONE);
    LS_EQ_INT(p.sel, 4);                                         /* past the greyed row and the rule */
    LS_EQ_INT(ls_dui_popup_key(&p, LS_TK_CHAR, 'n'), LS_DUI_NONE);
    LS_EQ_INT(p.sel, 5);
    LS_EQ_INT(ls_dui_popup_key(&p, LS_TK_LEFT, 0), LS_DUI_NONE);  /* not on a bar */
    LS_EQ_INT(ls_dui_popup_key(&p, LS_TK_UP, 0), LS_DUI_NONE);
    LS_EQ_INT(ls_dui_popup_key(&p, LS_TK_ENTER, 0), 12);
    LS_CHECK(!p.open);
    ls_dui_popup_open(&p, M, 6, 5, 5, 2, TUI_YELLOW);
    LS_EQ_INT(ls_dui_popup_key(&p, LS_TK_RIGHT, 0), LS_DUI_NEXT);
    LS_EQ_INT(ls_dui_popup_key(&p, LS_TK_ESC, 0), LS_DUI_CLOSED);
    /* Clicks: drawn, then a row, a greyed row, outside. */
    static tui_cell back[40 * 20], front[40 * 20];
    tui_surface sf;
    tui_surface_setup(&sf, back, front, 40, 20);
    ls_dui_popup_open(&p, M, 6, 5, 5, -1, TUI_YELLOW);
    ls_dui_popup_draw(&sf, tui_rect_make(0, 0, 40, 20), &p);
    LS_EQ_INT(p.box.x, 5);
    LS_EQ_INT(p.box.h, 8);
    LS_EQ_INT(ls_dui_popup_tap(&p, 8, p.box.y + 3), LS_DUI_NONE);     /* Edit, greyed */
    LS_CHECK(p.open);
    LS_EQ_INT(ls_dui_popup_tap(&p, 8, p.box.y + 5), 12);
    ls_dui_popup_open(&p, M, 6, 38, 18, -1, TUI_YELLOW);              /* asked for off the corner */
    ls_dui_popup_draw(&sf, tui_rect_make(0, 0, 40, 20), &p);
    LS_CHECK(p.box.x + p.box.w <= 40 && p.box.y + p.box.h <= 20 && p.box.y >= 0);
    LS_EQ_INT(ls_dui_popup_tap(&p, 0, 0), LS_DUI_CLOSED);
    /* A list longer than the room scrolls to keep its row in view. */
    ls_dui_popup_open(&p, M, 6, 0, 0, -1, TUI_YELLOW);
    ls_dui_popup_draw(&sf, tui_rect_make(0, 0, 40, 5), &p);
    LS_EQ_INT(p.box.h, 5);
    LS_CHECK(p.sel >= p.first && p.sel < p.first + 3);
}

LS_CASE(table_drops_columns_that_do_not_fit)
{
    static const ls_dui_col_t C[5] = {
        { "MHz", 10, 10, LS_DUI_RIGHT, 0 }, { "NAME", 24, 10, LS_DUI_FLEX, 0 }, { "MODE", 7, 7, 0, 0 },
        { "GROUP", 16, 16, 0, 2 }, { "NOTES", 40, 8, LS_DUI_FLEX, 5 },
    };
    ls_dui_table_t t = { 0 };
    ls_dui_table_layout(&t, tui_rect_make(2, 3, 120, 20), C, 5, 0x1F);
    LS_EQ_INT(t.head.y, 3);
    LS_EQ_INT(t.body.y, 4);
    LS_EQ_INT(t.rows, 19);
    for (int i = 0; i < 5; i++) LS_CHECK(t.x[i] > 0);
    LS_EQ_INT(t.w[1], 24);                                       /* all it wants */
    LS_CHECK(t.w[4] >= 40);                                      /* and the rest to the last flexing one */
    LS_CHECK(t.x[4] + t.w[4] <= t.body.x + t.body.w);
    /* Narrow: notes go first, then the group; the name gives way to its least. */
    ls_dui_table_layout(&t, tui_rect_make(0, 0, 66, 20), C, 5, 0x1F);
    LS_EQ_INT(t.x[4], -1);
    LS_CHECK(t.x[3] > 0);
    LS_EQ_INT(t.w[1], 24 + 2);                                   /* its full width before notes comes back, and the slack */
    ls_dui_table_layout(&t, tui_rect_make(0, 0, 36, 20), C, 5, 0x1F);
    LS_EQ_INT(t.x[3], -1);
    LS_CHECK(t.x[0] > 0 && t.x[1] > 0 && t.x[2] > 0);
    LS_CHECK(t.w[1] >= 10 && t.w[1] < 24 && t.x[2] + t.w[2] <= t.body.x + t.body.w);
    /* A column the user hid stays hidden. */
    ls_dui_table_layout(&t, tui_rect_make(0, 0, 120, 20), C, 5, 0x1F & ~4u);
    LS_EQ_INT(t.x[2], -1);
    /* Clicks. */
    int what = -9;
    LS_EQ_INT(ls_dui_table_hit(&t, 5, t.x[1] + 2, t.head.y, &what), LS_DUI_T_HEAD);
    LS_EQ_INT(what, 1);
    t.first = 7;
    LS_EQ_INT(ls_dui_table_hit(&t, 5, 5, t.body.y + 3, &what), LS_DUI_T_ROW);
    LS_EQ_INT(what, 10);
    LS_EQ_INT(ls_dui_table_hit(&t, 5, t.strip.x + 1, t.strip.y, &what), LS_DUI_T_STRIP);
    LS_EQ_INT(what, -1);
    LS_EQ_INT(ls_dui_table_hit(&t, 5, t.strip.x + 1, t.strip.y + t.strip.h - 1, &what), LS_DUI_T_STRIP);
    LS_EQ_INT(what, 1);
    /* The window follows the cursor and stays inside the list. */
    ls_dui_table_follow(&t, 40, 100);
    LS_EQ_INT(t.first, 40 - t.rows + 1);
    ls_dui_table_follow(&t, 3, 100);
    LS_EQ_INT(t.first, 3);
    t.first = 95;
    ls_dui_table_clamp(&t, 100);
    LS_EQ_INT(t.first, 100 - t.rows);
    ls_dui_table_clamp(&t, 5);
    LS_EQ_INT(t.first, 0);
}

/* The widgets ask which screen is drawing; here there is one. */
int ls_tui_screen_current(void) { return 0; }
