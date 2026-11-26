/* Fault injection for the data packs (main/ls_packs.c): a truncated, corrupt
   or hostile catalog and pack files, failing downloads, and the retry after
   them. The unit is included so its statics can be inspected.
   test_fault_packs_old builds this file over the parser as it was before LANG
   rows (LS_PACKS_OLD_SRC, from git at configure time): the cases that are not
   about LANG pass there too, and the LANG catalog case checks such a build
   skips those rows. */
#include "ls_test.h"
#include "ls_fault.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "mbedtls/sha256.h"
#include <math.h>
#ifdef _WIN32
#include <direct.h>
#else
#include <sys/stat.h>
#define _mkdir(p) mkdir(p, 0777)
#endif
#ifdef LS_PACKS_OLD_SRC
#include LS_PACKS_OLD_SRC
#else
#include "../../main/ls_packs.c"
#endif

/* ---- seams ---- */
static double s_lat = 28.5, s_lon = -81.4;
static bool s_have_pos = true;
ls_loc_src_t ls_location_here(double *lat, double *lon)
{
    if (!s_have_pos) return LS_LOC_SRC_NONE;
    *lat = s_lat; *lon = s_lon;
    return LS_LOC_SRC_LIVE;
}
float ls_location_km(double a, double b, double c, double d)
{
    const double dy = (c - a) * 111.0, dx = (d - b) * 111.0 * cos((a + c) * 0.00872665);
    return (float)sqrt(dx * dx + dy * dy);
}
void ls_location_want(void) {}
void ls_location_init(void) {}
void ls_location_get(ls_location_t *o) { memset(o, 0, sizeof(*o)); }
ls_loc_state_t ls_location_key(char *o, size_t c) { snprintf(o, c, "fix"); return LS_LOC_FIX; }
void ls_location_key_tap(void) {}
void ls_location_set_enabled(bool on) { (void)on; }
static bool s_auto;                                         /* Settings > Auto data */
bool settings_get_auto_data(void) { return s_auto; }
void settings_set_auto_data(bool on) { (void)on; }
bool settings_get_gps_on(void) { return true; }
esp_err_t ls_wifi_sta_ensure(void) { return ESP_OK; }
void ls_hub_console_register(void) {}
bool ls_value_publish(const char *p, const char *u, ls_val_fn f) { (void)p; (void)u; (void)f; return true; }
bool ls_action_register(const char *p, const char *s, ls_cap_t n, ls_act_fn f, const char *h) { (void)p; (void)s; (void)n; (void)f; (void)h; return true; }

/* ---- fixtures ---- */
#define CAT "/sdcard/lakeshark/packs/catalog.txt"
#define CATURL "https://data.terminalbay.com/v1/catalog.txt"

static void rm_tree(const char *p)
{
    DIR *d = opendir(p);
    struct dirent *e;
    if (!d) return;
    while ((e = readdir(d))) {
        char c[400];
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        snprintf(c, sizeof(c), "%s/%s", p, e->d_name);
        rm_tree(c);
        remove(c);
        rmdir(c);
    }
    closedir(d);
}
static void put(const char *p, const void *d, size_t n)
{
    char b[400];
    lf_real(p, b, sizeof(b));
    for (char *s = b + strlen(lf_root) + 1; *s; s++) if (*s == '/') { *s = 0; _mkdir(b); *s = '/'; }
    FILE *f = fopen(b, "wb");
    fwrite(d, 1, n, f);
    fclose(f);
}
static bool exists(const char *p) { struct stat st; char b[400]; return !stat(lf_real(p, b, sizeof(b)), &st); }

static char g_cat[16384];
static size_t g_cat_len;
/* body of a catalog: header + lines + a good signature */
static void sign_cat(const char *lines, bool final_nl)
{
    uint8_t h[32];
    int n = snprintf(g_cat, sizeof(g_cat), "board=LS-data\n%s", lines);
    if (g_cat[n - 1] != '\n') g_cat[n++] = '\n';
    mbedtls_sha256((const unsigned char *)g_cat, (size_t)n, h, 0);
    n += snprintf(g_cat + n, sizeof(g_cat) - (size_t)n, "sig=");
    for (int i = 0; i < 8; i++) n += snprintf(g_cat + n, sizeof(g_cat) - (size_t)n, "%02x", h[i]);
    n += snprintf(g_cat + n, sizeof(g_cat) - (size_t)n, "\n");
    g_cat_len = (size_t)n;
}
static char g_pack[4][4096];
static size_t g_pack_len[4];
static const char *g_pack_id[4];
static bool g_pack_bin[4];                                  /* served as .bin: LANG or DATA */
static int g_npack;
static void hex(const uint8_t *h, char *o) { for (int i = 0; i < 32; i++) sprintf(o + i * 2, "%02x", h[i]); }
/* a catalog line for a pack body; returns it in out */
static void cat_line(char *out, size_t cap, const char *id, const char *kinds, const char *box, const char *body, const char *date)
{
    uint8_t h[32]; char x[65];
    mbedtls_sha256((const unsigned char *)body, strlen(body), h, 0);
    hex(h, x);
    snprintf(out, cap, "pack %s %s %s %u %s packs/%s.txt %s\n", id, kinds, box, (unsigned)strlen(body), x, id, date);
}

/* a LANG row: its bytes in packs/<id>.bin, a box of the world */
static void lang_line(char *out, size_t cap, const char *id, const char *kinds, const char *body, const char *ext)
{
    uint8_t h[32]; char x[65];
    mbedtls_sha256((const unsigned char *)body, strlen(body), h, 0);
    hex(h, x);
    snprintf(out, cap, "pack %s %s -180,-90,180,90 %u %s packs/%s%s 20261119\n", id, kinds, (unsigned)strlen(body), x, id, ext);
}

static bool route(const char *url, const uint8_t **b, size_t *n)
{
    if (!strcmp(url, CATURL)) { *b = (const uint8_t *)g_cat; *n = g_cat_len; return true; }
    for (int i = 0; i < g_npack; i++) {
        char want[160];
        snprintf(want, sizeof(want), "https://data.terminalbay.com/v1/packs/%s%s", g_pack_id[i],
                 g_pack_bin[i] ? ".bin" : ".txt");
        if (!strcmp(url, want)) { *b = (const uint8_t *)g_pack[i]; *n = g_pack_len[i]; return true; }
    }
    return false;
}
static void add_pack(const char *id, const char *body)
{
    g_pack_id[g_npack] = id;
    g_pack_bin[g_npack] = !strncmp(id, "lang-", 5);
    g_pack_len[g_npack] = strlen(body);
    memcpy(g_pack[g_npack++], body, strlen(body) + 1);
}
/* a DATA pack on the server: packs/<id>.bin */
static void add_bin(const char *id, const char *body)
{
    add_pack(id, body);
    g_pack_bin[g_npack - 1] = true;
}

static const char GOOD_BODY[] =
    "S|ORL1|P25|Orange County|28.5|-81.4|40|svc=fire;lic=x\n"
    "F|ORL1|851012500|ctl|P25|Control\n"
    "F|ORL1|852000000|ch|P25|Voice 2\n"
    "S|AIR1|AIR|MCO tower|28.43|-81.31|10|\n"
    "F|AIR1|119100000|twr|AM|Tower\n";
#define BOX_ORL "-82.5,27.5,-80.5,29.5"
/* A translator file. It even holds a system line at the board's position:
   nothing may ever read a LANG pack as frequencies. */
static const char LANG_BODY[] =
    "LSXL\x01\x7f\xfe binary lexicon\n"
    "S|evil|P25|Not a site|28.5|-81.4|10|\n"
    "F|evil|851000000|ctl|P25|Not a channel\n";

static void fresh(void)
{
    ls_shim_heap_reset();
    lf_net_reset();
    lf_fs_reset();
    ls_shim_time_set(1000000);
    snprintf(lf_root, sizeof(lf_root), "lf_packs_root");
    rm_tree(lf_root);
    _mkdir(lf_root);
    g_npack = 0;
    s_auto = false;
    lf_srv.route = route;
    s_have_pos = true; s_lat = 28.5; s_lon = -81.4;
    memset(s_ncat, 0, sizeof(s_ncat));
    s_cl = 0; s_ninst = s_nupd = 0; s_nhere = 0;
}

static int load_with(const char *lines, bool nl)
{
    sign_cat(lines, nl);
    put(CAT, g_cat, g_cat_len);
    return cat_load();
}

LS_CASE(packs_catalog_bad_lines_are_skipped_good_ones_survive)
{
    char a[400], b[400], tail[400], all[8192] = "";
    fresh();
    cat_line(a, sizeof(a), "lm-us-fl", "P25,AIR", BOX_ORL, GOOD_BODY, "20260101");
    strcat(all, a);
    const char *bad[] = {
        "pack lm-bad1 P25 -82,27,-80,29 100 00 packs/lm-bad1.txt 20260101\n",                         /* 6 fields */
        "pack lm-bad2 P25 -82,27,-80 100 SHA packs/lm-bad2.txt 20260101\n",                           /* box of three */
        "pack\n", "pack \n", "pack x\n", "\n", "board=other\n",
        "pack Lm-Up P25 -82,27,-80,29 100 SHA packs/Lm-Up.txt 20260101\n",                            /* uppercase id */
        "pack ../x P25 -82,27,-80,29 100 SHA packs/../x.txt 20260101\n",                               /* path escape */
        "pack lm-nokind XX -82,27,-80,29 100 SHA packs/lm-nokind.txt 20260101\n",
        "pack lm-nodate P25 -82,27,-80,29 100 SHA packs/lm-nodate.txt 2026\n",
        "pack lm-zero P25 -82,27,-80,29 0 SHA packs/lm-zero.txt 20260101\n",
        "pack lm-big P25 -82,27,-80,29 99999999999999999999999 SHA packs/lm-big.txt 20260101\n",
        "pack lm-xtra P25 -82,27,-80,29 100 SHA packs/lm-xtra.txt 20260101 extra words here\n",
    };
    /* put a real 64-digit sha in each bad line so only the named defect applies */
    char sha[65];
    memset(sha, 'a', 64); sha[64] = 0;
    for (unsigned i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        char l[300];
        const char *p = strstr(bad[i], "SHA");
        if (p) snprintf(l, sizeof(l), "%.*s%s%s", (int)(p - bad[i]), bad[i], sha, p + 3);
        else snprintf(l, sizeof(l), "%s", bad[i]);
        strcat(all, l);
    }
    static const char *const boxes[] = { "nan,27,-80,29", "-82,inf,-80,29", "abc,def,ghi,jkl", "-80,27,-82,29",
                                         "-82,29,-80,27", "-200,27,-80,29", "-82,27,-80,95", "1e999,27,-80,29" };
    for (unsigned i = 0; i < sizeof(boxes) / sizeof(boxes[0]); i++) {
        char l[300];
        snprintf(l, sizeof(l), "pack lm-box%u P25 %s 100 %s packs/lm-box%u.txt 20260101\n", i, boxes[i], sha, i);
        strcat(all, l);
    }
    cat_line(b, sizeof(b), "air-all", "AIR", "-180,-90,180,90", GOOD_BODY, "20260102");
    strcat(all, b);
    cat_line(tail, sizeof(tail), "lm-us-ga", "FM", "-85,30,-81,35", GOOD_BODY, "20260103");
    strcat(all, tail);
    int n = load_with(all, false);              /* and no newline after the signature */
    int ok_ids = 0;
    for (int i = 0; i < n; i++) {
        const char *id = s_cat[s_cl][i].id;
        if (!strcmp(id, "lm-us-fl") || !strcmp(id, "air-all") || !strcmp(id, "lm-us-ga")) ok_ids++;
        else ls_note("accepted a bad pack line: %s", id);
    }
    LS_EQ_INT(ok_ids, 3);
    LS_EQ_INT(n, 3);
}

LS_CASE(packs_catalog_crlf_and_no_trailing_newline_load)
{
    char a[400], all[2000];
    fresh();
    cat_line(a, sizeof(a), "lm-us-fl", "P25", BOX_ORL, GOOD_BODY, "20260101");
    snprintf(all, sizeof(all), "%.*s\r\n", (int)strlen(a) - 1, a);
    sign_cat(all, true);
    put(CAT, g_cat, g_cat_len);
    LS_EQ_INT(cat_load(), 1);
    LS_CHECK(!strcmp(s_cat[s_cl][0].date, "20260101"));
}

LS_CASE(packs_unsigned_or_tampered_catalog_changes_nothing)
{
    char a[400];
    fresh();
    cat_line(a, sizeof(a), "lm-us-fl", "P25", BOX_ORL, GOOD_BODY, "20260101");
    LS_EQ_INT(load_with(a, true), 1);
    sign_cat(a, true);
    g_cat[40] ^= 1;                                        /* a bit flipped under the signature */
    put(CAT, g_cat, g_cat_len);
    LS_EQ_INT(cat_load(), -1);
    LS_EQ_INT(s_ncat[s_cl], 1);                            /* the list in use is still the good one */
    put(CAT, g_cat, g_cat_len - 5);                        /* torn off inside the signature */
    LS_EQ_INT(cat_load(), -1);
    put(CAT, "", 0);
    LS_EQ_INT(cat_load(), -1);
    remove(CAT);
    LS_EQ_INT(cat_load(), -1);
    LS_EQ_INT(s_ncat[s_cl], 1);
}

LS_CASE(packs_catalog_over_capacity_is_cut_not_overrun)
{
    static char big[CAT_MAX * 200 + 20000];
    fresh();
    char l[300], sha[65];
    memset(sha, 'b', 64); sha[64] = 0;
    big[0] = 0;
    for (int i = 0; i < CAT_MAX + 80; i++) {
        snprintf(l, sizeof(l), "pack lm-r%d P25 -82,27,-80,29 100 %s packs/lm-r%d.txt 20260101\n", i, sha, i);
        strcat(big, l);
    }
    static char file[sizeof(big) + 200];
    uint8_t h[32];
    int n = snprintf(file, sizeof(file), "board=LS-data\n%s", big);
    mbedtls_sha256((const unsigned char *)file, (size_t)n, h, 0);
    n += snprintf(file + n, 40, "sig=");
    for (int i = 0; i < 8; i++) n += sprintf(file + n, "%02x", h[i]);
    file[n++] = '\n';
    put(CAT, file, (size_t)n);
    LS_EQ_INT(cat_load(), CAT_MAX);
}

LS_CASE(packs_catalog_reads_a_verified_index_that_is_garbage)
{
    char a[400];
    fresh();
    cat_line(a, sizeof(a), "lm-us-fl", "P25", BOX_ORL, GOOD_BODY, "20260101");
    put("/sdcard/lakeshark/packs/lm-us-fl.txt", GOOD_BODY, strlen(GOOD_BODY));
    uint8_t junk[3000];
    for (unsigned i = 0; i < sizeof(junk); i++) junk[i] = (uint8_t)(i * 7);
    put(VERIFIED, junk, sizeof(junk));                      /* binary junk, no newlines, NULs */
    LS_EQ_INT(load_with(a, true), 1);
    LS_CHECK(s_cat[s_cl][0].flags & F_INST);                /* the file's sha matches the catalog */
    put(VERIFIED, "lm-us-fl 5 20260101", 19);               /* wrong size: not trusted */
    remove("/sdcard/lakeshark/packs/lm-us-fl.txt");
    put("/sdcard/lakeshark/packs/lm-us-fl.txt", GOOD_BODY, strlen(GOOD_BODY) - 1);   /* one byte short */
    LS_EQ_INT(cat_load(), 1);
    LS_CHECK(!(s_cat[s_cl][0].flags & F_INST));             /* a short pack is not installed */
    LS_EQ_INT(s_ninst, 0);
}

static int s_seen;
static bool count_cb(const ls_pack_sys_t *s, void *ctx) { (void)ctx; LS_CHECK(isfinite(s->dist_km)); s_seen++; return true; }

LS_CASE(packs_scan_survives_corrupt_pack_lines)
{
    char a[400];
    fresh();
    cat_line(a, sizeof(a), "lm-us-fl", "P25,AIR", BOX_ORL, GOOD_BODY, "20260101");
    static char body[8000];
    char longline[400];
    memset(longline, 'x', sizeof(longline)); longline[399] = 0;
    snprintf(body, sizeof(body),
        "S|short|P25\r\n"
        "S||P25||||\r\n"
        "S|nanlat|P25|Nan Lat|nan|-81.4|10|\r\n"
        "S|inflat|P25|Inf Lat|inf|-81.4|10|\r\n"
        "S|nolat|P25|No Lat|||10|\r\n"
        "S|badkind|ZZZ|Bad|28.5|-81.4|10|\r\n"
        "X|garbage line\r\n"
        "S|long|P25|%s|28.5|-81.4|10|a;b\r\n"
        "S|ok1|P25|Good One|28.51|-81.41|10|svc=fire\r\n"
        "F|ok1|99999999999999999999999|ch|P25|Huge\r\n"
        "F|ok1|0|ch|P25|Zero\r\n"
        "F|ok1|851012500|ctl|P25\r\n"
        "F|ok1|852000000|ch|P25|Voice\r\n"
        "S|ok2|P25|Last no newline|28.52|-81.42|10|", longline);
    /* the pack on the card must match the catalog's sha/size, so catalog it as is */
    cat_line(a, sizeof(a), "lm-us-fl", "P25,AIR", BOX_ORL, body, "20260101");
    put("/sdcard/lakeshark/packs/lm-us-fl.txt", body, strlen(body));
    LS_EQ_INT(load_with(a, true), 1);
    LS_CHECK(s_cat[s_cl][0].flags & F_INST);
    s_nhere = choose(s_lat, s_lon);
    LS_EQ_INT(s_nhere, 1);
    s_seen = 0;
    ls_packs_scan(LS_PK_ALL, 1e9f, count_cb, NULL);
    LS_CHECK(s_seen >= 2);                                  /* ok1 and the last line without newline */
    ls_pack_sys_t sys[16];
    s_live = 0;
    near_build();
    int n = ls_packs_nearest(LS_PK_ALL, 1e9f, sys, 16);
    for (int i = 0; i < n; i++) {
        LS_CHECK(isfinite(sys[i].dist_km) && isfinite(sys[i].lat) && isfinite(sys[i].lon));
        LS_CHECK(fabsf(sys[i].lat) <= 90 && fabsf(sys[i].lon) <= 180);
    }
    LS_EQ_INT(n, 2);                                        /* ok1 and the last line; the overlong one is cut and dropped */
    ls_pack_freq_t f[8];
    for (int i = 0; i < n; i++)
        if (!strcmp(sys[i].id, "ok1")) {
            int k = ls_packs_freqs(&sys[i], f, 8);
            LS_EQ_INT(k, 2);                                /* the zero and the huge one are not frequencies */
            LS_CHECK(f[0].hz == 851012500ull && f[1].hz == 852000000ull);
        }
}

LS_CASE(packs_scan_with_the_heap_gone_returns_quietly)
{
    char a[400];
    fresh();
    cat_line(a, sizeof(a), "lm-us-fl", "P25,AIR", BOX_ORL, GOOD_BODY, "20260101");
    put("/sdcard/lakeshark/packs/lm-us-fl.txt", GOOD_BODY, strlen(GOOD_BODY));
    LS_EQ_INT(load_with(a, true), 1);
    s_nhere = choose(s_lat, s_lon);
    ls_shim_heap_fail_from(1);
    s_seen = 0;
    ls_packs_scan(LS_PK_ALL, 1e9f, count_cb, NULL);
    ls_shim_heap_fail_from(0);
    LS_EQ_INT(s_seen, 0);
    /* a catalog load with no heap */
    ls_shim_heap_fail_from(1);
    (void)cat_load();                                        /* cannot even verify: refused, not a crash */
    ls_shim_heap_fail_from(0);
    LS_EQ_INT(s_ncat[s_cl], 1);
}

/* ---- the task, end to end over the faulty network ---- */

static ls_packs_state_t round_(void)
{
    lf_task_pump_n("ls_packs", 1);
    ls_packs_status_t st;
    ls_packs_status(&st);
    return st.state;
}

static void world(void)
{
    char a[400], b[400], all[1000];
    fresh();
    cat_line(a, sizeof(a), "lm-us-fl", "P25,AIR", BOX_ORL, GOOD_BODY, "20260101");
    cat_line(b, sizeof(b), "lm-us-ga", "P25", "-85,30,-81,35", GOOD_BODY, "20260101");
    snprintf(all, sizeof(all), "%s%s", a, b);
    sign_cat(all, true);
    add_pack("lm-us-fl", GOOD_BODY);
    add_pack("lm-us-ga", GOOD_BODY);
    ls_services_start();
}

LS_CASE(packs_install_survives_http_errors_and_next_try_succeeds)
{
    world();
    const char *ids[] = { "lm-us-fl" };
    lf_srv.open_status = 500;                               /* the catalog itself fails */
    LS_EQ_INT(ls_packs_install(ids, 1), 1);
    LS_EQ_INT(round_(), LS_PACKS_FAILED);
    LS_CHECK(!exists(CAT));
    lf_srv.open_status = 0;
    LS_EQ_INT(ls_packs_install(ids, 1), 1);
    LS_EQ_INT(round_(), LS_PACKS_READY);
    LS_CHECK(exists("/sdcard/lakeshark/packs/lm-us-fl.txt"));
    ls_packs_status_t st;
    ls_packs_status(&st);
    LS_EQ_INT(st.installed, 1);
    LS_EQ_INT(st.here, 1);
}

LS_CASE(packs_install_with_a_corrupt_pack_download_never_installs_it)
{
    world();
    const char *ids[] = { "lm-us-fl" };
    g_pack[0][5] ^= 0x20;                                   /* a flipped byte in the served pack */
    LS_EQ_INT(ls_packs_install(ids, 1), 1);
    LS_EQ_INT(round_(), LS_PACKS_FAILED);
    LS_CHECK(!exists("/sdcard/lakeshark/packs/lm-us-fl.txt"));
    LS_CHECK(!exists("/sdcard/lakeshark/packs/lm-us-fl.txt.part"));
    g_pack[0][5] ^= 0x20;
    LS_EQ_INT(ls_packs_install(ids, 1), 1);
    LS_EQ_INT(round_(), LS_PACKS_READY);
    LS_CHECK(exists("/sdcard/lakeshark/packs/lm-us-fl.txt"));
}

LS_CASE(packs_install_drop_mid_pack_resumes_and_card_full_fails_cleanly)
{
    world();
    const char *ids[] = { "lm-us-fl" };
    lf_srv.drop_after = 50;
    LS_EQ_INT(ls_packs_install(ids, 1), 1);
    LS_EQ_INT(round_(), LS_PACKS_FAILED);
    LS_CHECK(!exists("/sdcard/lakeshark/packs/lm-us-fl.txt"));
    lf_srv.drop_after = -1;
    lf_fs.write_budget = 60;                                /* then the card fills up */
    LS_EQ_INT(ls_packs_install(ids, 1), 1);
    LS_EQ_INT(round_(), LS_PACKS_FAILED);
    LS_CHECK(!exists("/sdcard/lakeshark/packs/lm-us-fl.txt"));
    lf_fs.write_budget = -1;
    LS_EQ_INT(ls_packs_install(ids, 1), 1);
    LS_EQ_INT(round_(), LS_PACKS_READY);
}

LS_CASE(packs_remove_unknown_and_hostile_ids_are_ignored)
{
    world();
    const char *ids[] = { "../../etc/x", "", "LM-UP", "lm-us-fl-nope-" };
    LS_EQ_INT(ls_packs_remove(ids, 4), 4);
    LS_EQ_INT(ls_packs_install(ids, 4), 4);
    (void)round_();                                          /* no crash, nothing created */
    LS_CHECK(!exists("/sdcard/lakeshark/etc"));
    char many[100][32];
    const char *p[100];
    for (int i = 0; i < 100; i++) { snprintf(many[i], 32, "id-%d", i); p[i] = many[i]; }
    LS_CHECK(ls_packs_install(p, 100) <= INST_MAX);          /* the queue is bounded */
    (void)round_();
}

LS_CASE(packs_no_position_and_no_network_report_a_state)
{
    world();
    const char *ids[] = { "lm-us-fl" };
    lf_online = false;
    LS_EQ_INT(ls_packs_install(ids, 1), 1);
    LS_EQ_INT(round_(), LS_PACKS_NO_WIFI);
    lf_online = true;
    s_have_pos = false;
    LS_EQ_INT(ls_packs_install(ids, 1), 1);
    LS_EQ_INT(round_(), LS_PACKS_NO_FIX);
    s_have_pos = true;
    LS_EQ_INT(ls_packs_install(ids, 1), 1);
    LS_EQ_INT(round_(), LS_PACKS_READY);
}

/* ---- LANG packs: translator files, installed or not, never by position ---- */

LS_CASE(packs_catalog_lang_rows_skipped_by_old_parsers_read_by_new)
{
    char a[400], m[400], l[400], all[4000] = "";
    fresh();
    cat_line(a, sizeof(a), "lm-us-fl", "P25,AIR", BOX_ORL, GOOD_BODY, "20260101");
    cat_line(m, sizeof(m), "mar-all", "MAR", "-180,-90,180,90", GOOD_BODY, "20260101");
    lang_line(l, sizeof(l), "lang-pt", "LANG", LANG_BODY, ".bin");
    strcat(all, l);
    strcat(all, a);
    lang_line(l, sizeof(l), "lang-de", "LANG", LANG_BODY, ".bin");
    strcat(all, l);
    strcat(all, m);
    /* rows the new parser refuses as well */
    static const char *const bad[][3] = {
        { "lang-fr", "LANG", ".txt" },          /* a language as a text pack */
        { "lang-it", "P25", ".bin" },           /* a lang- id that is not LANG */
        { "lang-nl", "LANG,AIR", ".bin" },      /* LANG mixed with a kind of system */
        { "lm-us-zz", "LANG", ".bin" },         /* LANG under an id that is not lang- */
        { "lm-us-yy", "LANG", ".txt" },
        { "lang-", "LANG", ".bin" },            /* no language */
    };
    for (unsigned i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        lang_line(l, sizeof(l), bad[i][0], bad[i][1], LANG_BODY, bad[i][2]);
        strcat(all, l);
    }
    const int n = load_with(all, true);
    for (int i = 0; i < n; i++) ls_note("row %s kinds 0x%x", s_cat[s_cl][i].id, (unsigned)s_cat[s_cl][i].kinds);
#ifdef LS_PACKS_OLD_SRC
    LS_EQ_INT(n, 2);                                        /* a build from before LANG keeps its packs, drops the rest */
    for (int i = 0; i < n; i++) LS_CHECK(strncmp(s_cat[s_cl][i].id, "lang-", 5));
#else
    LS_EQ_INT(n, 4);
    ls_pack_region_t r[8];
    LS_EQ_INT(ls_packs_catalog_regions(r, 0, 8), 4);
    /* the global row, LANGUAGES (by code), then the countries */
    LS_EQ_STR(r[0].id, "mar-all");
    LS_EQ_STR(r[1].id, "lang-de");
    LS_EQ_STR(r[2].id, "lang-pt");
    LS_EQ_STR(r[3].id, "lm-us-fl");
    LS_EQ_STR(r[1].cc, LS_PACK_LANG_CC);
    LS_EQ_STR(r[2].cc, LS_PACK_LANG_CC);
    LS_EQ_STR(r[1].sub, "DE");
    LS_EQ_STR(r[2].sub, "PT");
    LS_EQ_INT(r[2].kinds, LS_PK_LANG);
    LS_EQ_INT(r[2].bytes, (int)strlen(LANG_BODY));
    LS_CHECK(!(r[2].kinds & LS_PK_ALL));
    LS_EQ_STR(r[3].cc, "US");
#endif
}

/* ---- DATA packs: any app's raw file, <id>.bin, installed or not, never by position ---- */

/* An app's bytes, with a system line at the board's position too. */
static const char DATA_BODY[] =
    "LAPI\x01\x02 ble names\n"
    "S|evil|P25|Not a site|28.5|-81.4|10|\n"
    "F|evil|851000000|ctl|P25|Not a channel\n";
static const char DATA_BODY2[] = "LAPI\x01\x03 ble names, a newer list\n";

LS_CASE(packs_catalog_data_rows_skipped_by_old_parsers_read_by_new)
{
    char a[400], m[400], l[400], all[4000] = "";
    fresh();
    cat_line(a, sizeof(a), "lm-us-fl", "P25,AIR", BOX_ORL, GOOD_BODY, "20260101");
    cat_line(m, sizeof(m), "mar-all", "MAR", "-180,-90,180,90", GOOD_BODY, "20260101");
    strcat(all, a);
    lang_line(l, sizeof(l), "ble-ids", "DATA", DATA_BODY, ".bin");
    strcat(all, l);
    lang_line(l, sizeof(l), "lang-pt", "LANG", LANG_BODY, ".bin");
    strcat(all, l);
    lang_line(l, sizeof(l), "ble-flows", "DATA", DATA_BODY2, ".bin");
    strcat(all, l);
    lang_line(l, sizeof(l), "bughunt-ids", "DATA", DATA_BODY, ".bin");
    strcat(all, l);
    strcat(all, m);
    static const char *const bad[][3] = {
        { "ble-x", "DATA", ".txt" },            /* app data as a text pack */
        { "ble-y", "DATA,P25", ".bin" },        /* DATA mixed with a kind of system */
        { "ble-z", "P25,DATA", ".txt" },
        { "ble-v", "DATA,LANG", ".bin" },       /* two raw kinds */
        { "lang-es", "DATA", ".bin" },          /* a lang- id is LANG only */
        { "ble-w", "P25", ".bin" },             /* a frequency pack in a .bin */
    };
    for (unsigned i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        lang_line(l, sizeof(l), bad[i][0], bad[i][1], DATA_BODY, bad[i][2]);
        strcat(all, l);
    }
    const int n = load_with(all, true);
    for (int i = 0; i < n; i++) ls_note("row %s kinds 0x%x", s_cat[s_cl][i].id, (unsigned)s_cat[s_cl][i].kinds);
#ifdef LS_PACKS_OLD_SRC
    /* a build from before LANG and DATA keeps its packs and never takes a DATA
       row (ble-z, P25 in a .txt, it reads as a P25 pack: a row no generator writes) */
    int good = 0;
    for (int i = 0; i < n; i++) {
        const char *id = s_cat[s_cl][i].id;
        good += !strcmp(id, "lm-us-fl") || !strcmp(id, "mar-all");
        LS_CHECK(strcmp(id, "ble-ids") && strcmp(id, "ble-flows") && strcmp(id, "bughunt-ids") && strncmp(id, "lang-", 5));
    }
    LS_EQ_INT(good, 2);
#else
    LS_EQ_INT(n, 6);
    ls_pack_region_t r[8];
    LS_EQ_INT(ls_packs_catalog_regions(r, 0, 8), 6);
    /* the global row, LANGUAGES, APP DATA (by id), then the countries */
    LS_EQ_STR(r[0].id, "mar-all");
    LS_EQ_STR(r[1].id, "lang-pt");
    LS_EQ_STR(r[2].id, "ble-flows");
    LS_EQ_STR(r[3].id, "ble-ids");
    LS_EQ_STR(r[4].id, "bughunt-ids");
    LS_EQ_STR(r[5].id, "lm-us-fl");
    LS_EQ_STR(r[1].cc, LS_PACK_LANG_CC);
    LS_EQ_STR(r[1].sub, "PT");
    LS_EQ_INT(r[1].kinds, LS_PK_LANG);
    for (int i = 2; i <= 4; i++) {
        LS_EQ_STR(r[i].cc, LS_PACK_DATA_CC);
        LS_EQ_STR(r[i].sub, "");
        LS_EQ_INT(r[i].kinds, LS_PK_DATA);
        LS_CHECK(!(r[i].kinds & LS_PK_ALL));
    }
    LS_EQ_INT(r[3].bytes, (int)strlen(DATA_BODY));
    LS_EQ_STR(r[5].cc, "US");
#endif
}

#ifndef LS_PACKS_OLD_SRC
static int s_evil;
static bool evil_cb(const ls_pack_sys_t *s, void *ctx)
{
    (void)ctx;
    s_seen++;
    s_evil += !strcmp(s->id, "evil") || !strcmp(s->pack, "lang-pt");
    return true;
}

LS_CASE(packs_lang_pack_is_never_picked_by_position)
{
    char a[400], l[400], all[1000];
    fresh();
    cat_line(a, sizeof(a), "lm-us-fl", "P25,AIR", BOX_ORL, GOOD_BODY, "20260101");
    lang_line(l, sizeof(l), "lang-pt", "LANG", LANG_BODY, ".bin");
    snprintf(all, sizeof(all), "%s%s", l, a);
    put("/sdcard/lakeshark/packs/lm-us-fl.txt", GOOD_BODY, strlen(GOOD_BODY));
    put("/sdcard/lakeshark/packs/lang-pt.bin", LANG_BODY, strlen(LANG_BODY));
    LS_EQ_INT(load_with(all, true), 2);
    LS_EQ_INT(s_ninst, 2);                                  /* both copied in by hand, both match the catalog */
    LS_CHECK(s_cat[s_cl][0].flags & F_INST);
    static const double at[][2] = { { 28.5, -81.4 }, { 0, 0 }, { 48.85, 2.35 }, { -33.9, 151.2 }, { 89.9, 179.9 }, { -89.9, -180 } };
    for (unsigned i = 0; i < sizeof(at) / sizeof(at[0]); i++) {
        const int n = choose(at[i][0], at[i][1]);
        for (int j = 0; j < n; j++) LS_CHECK(strcmp(s_here[j].id, "lang-pt"));
        LS_EQ_INT(n, i == 0 ? 1 : 0);                       /* Florida's pack in Florida; nothing anywhere else */
    }
    s_nhere = choose(s_lat, s_lon);
    s_seen = s_evil = 0;
    ls_packs_scan(0xffff, 1e9f, evil_cb, NULL);             /* every kind there is, LANG's bit too */
    LS_EQ_INT(s_evil, 0);
    LS_EQ_INT(s_seen, 2);                                   /* ORL1 and AIR1 */
    s_live = 0;
    near_build();
    ls_pack_sys_t sys[16];
    const int n = ls_packs_nearest(0xffff, 1e9f, sys, 16);
    LS_EQ_INT(n, 2);
    for (int i = 0; i < n; i++) LS_CHECK(strcmp(sys[i].pack, "lang-pt"));
}

LS_CASE(packs_lang_installs_and_removes_by_id_never_by_area)
{
    char a[400], l[400], all[1000];
    fresh();
    cat_line(a, sizeof(a), "lm-us-fl", "P25,AIR", BOX_ORL, GOOD_BODY, "20260101");
    lang_line(l, sizeof(l), "lang-pt", "LANG", LANG_BODY, ".bin");
    snprintf(all, sizeof(all), "%s%s", a, l);
    sign_cat(all, true);
    add_pack("lm-us-fl", GOOD_BODY);
    add_pack("lang-pt", LANG_BODY);
    ls_services_start();
    /* the whole world around here, every kind: Florida comes, the language does not */
    LS_CHECK(ls_packs_install_around(s_lat, s_lon, 20000.0f, 0));
    LS_EQ_INT(round_(), LS_PACKS_READY);
    LS_CHECK(exists("/sdcard/lakeshark/packs/lm-us-fl.txt"));
    LS_CHECK(!exists("/sdcard/lakeshark/packs/lang-pt.bin"));
    LS_CHECK(ls_packs_install_around(s_lat, s_lon, 20000.0f, LS_PK_ALL | LS_PK_LANG));
    (void)round_();
    LS_CHECK(!exists("/sdcard/lakeshark/packs/lang-pt.bin"));
    LS_CHECK(ls_packs_get_here(0));
    (void)round_();
    LS_CHECK(!exists("/sdcard/lakeshark/packs/lang-pt.bin"));
    /* by name: it comes, as a .bin with the catalog's bytes */
    const char *ids[] = { "lang-pt" };
    LS_EQ_INT(ls_packs_install(ids, 1), 1);
    LS_EQ_INT(round_(), LS_PACKS_READY);
    LS_CHECK(exists("/sdcard/lakeshark/packs/lang-pt.bin"));
    LS_CHECK(!exists("/sdcard/lakeshark/packs/lang-pt.txt"));
    ls_packs_status_t st;
    ls_packs_status(&st);
    LS_EQ_INT(st.installed, 2);
    LS_EQ_INT(st.here, 1);                                  /* installed, still not "here" */
    ls_pack_region_t r[4];
    const int nr = ls_packs_catalog_regions(r, 0, 4);
    int seen = 0;
    for (int i = 0; i < nr; i++) if (!strcmp(r[i].id, "lang-pt")) { seen++; LS_CHECK(r[i].installed); }
    LS_EQ_INT(seen, 1);
    LS_EQ_INT(ls_packs_remove(ids, 1), 1);
    (void)round_();
    LS_CHECK(!exists("/sdcard/lakeshark/packs/lang-pt.bin"));
    LS_CHECK(exists("/sdcard/lakeshark/packs/lm-us-fl.txt"));
    ls_packs_status(&st);
    LS_EQ_INT(st.installed, 1);
}

/* ---- the verified index stays readable, a finished job keeps its result ---- */

LS_CASE(packs_verified_index_past_its_read_size_is_compacted)
{
    char a[400];
    static char fill[VB_MAX + 4096];
    fresh();
    size_t n = 0;
    for (int i = 0; n < sizeof(fill) - 64; i++)             /* a long life of re-hashes, over what is read */
        n += (size_t)snprintf(fill + n, sizeof(fill) - n, "lm-old-%d 1234 20250101\n", i);
    put(VERIFIED, fill, n);
    cat_line(a, sizeof(a), "lm-us-fl", "P25", BOX_ORL, GOOD_BODY, "20260101");
    put("/sdcard/lakeshark/packs/lm-us-fl.txt", GOOD_BODY, strlen(GOOD_BODY));
    LS_EQ_INT(load_with(a, true), 1);
    LS_CHECK(s_cat[s_cl][0].flags & F_INST);                /* hashed: its line was past the read */
    struct stat st;
    char b[400];
    LS_CHECK(!stat(lf_real(VERIFIED, b, sizeof(b)), &st) && st.st_size < 200);
    /* now the line is read: a same-size file is trusted without a re-hash */
    char other[sizeof(GOOD_BODY)];
    memcpy(other, GOOD_BODY, sizeof(GOOD_BODY));
    other[0] = 'X';
    remove(lf_real("/sdcard/lakeshark/packs/lm-us-fl.txt", b, sizeof(b)));
    put("/sdcard/lakeshark/packs/lm-us-fl.txt", other, strlen(other));
    LS_EQ_INT(cat_load(), 1);
    LS_CHECK(s_cat[s_cl][0].flags & F_INST);
}

/* The pack's job ends and another download takes its slot before the packs
   task looks again: the install still counts. */
static char s_other[20000];
static int s_reuse;
static char s_opened[160];
static bool route_seen(const char *url, const uint8_t **b, size_t *n)
{
    snprintf(s_opened, sizeof(s_opened), "%s", url);
    return route(url, b, n);
}
static void reuse_slot(size_t served)
{
    (void)served;
    const ls_fetch_req_t d = { .url = CATURL, .buf = s_other, .cap = sizeof(s_other), .timeout_s = 20 };
    if (s_reuse == 0 && strstr(s_opened, "/packs/lm-us-fl")) { s_reuse = 1; LS_CHECK(ls_fetch_start(&d) > 0); }
    else if (s_reuse == 1 && !strcmp(s_opened, CATURL)) { s_reuse = 2; LS_CHECK(ls_fetch_start(&d) > 0); }
}

LS_CASE(packs_install_counts_when_its_fetch_slot_is_reused)
{
    world();
    const char *ids[] = { "lm-us-fl" };
    s_reuse = 0;
    s_opened[0] = 0;
    lf_srv.route = route_seen;
    lf_srv.on_read = reuse_slot;
    LS_EQ_INT(ls_packs_install(ids, 1), 1);
    LS_EQ_INT(round_(), LS_PACKS_READY);
    lf_srv.on_read = NULL;
    LS_EQ_INT(s_reuse, 2);                                  /* the pack's slot was taken by then */
    LS_CHECK(exists("/sdcard/lakeshark/packs/lm-us-fl.txt"));
    ls_packs_status_t st;
    ls_packs_status(&st);
    LS_EQ_INT(st.installed, 1);
}

/* ---- DATA packs end to end ---- */

/* The file on the card is exactly body. */
static bool same_file(const char *p, const char *body)
{
    char b[400], got[4096];
    FILE *f = fopen(lf_real(p, b, sizeof(b)), "rb");
    if (!f) return false;
    const size_t n = fread(got, 1, sizeof(got), f);
    fclose(f);
    return n == strlen(body) && !memcmp(got, body, n);
}

static bool data_evil_cb(const ls_pack_sys_t *s, void *ctx)
{
    (void)ctx;
    s_seen++;
    s_evil += !strcmp(s->id, "evil") || !strcmp(s->pack, "ble-ids");
    return true;
}

LS_CASE(packs_data_installs_by_id_never_by_area_never_as_frequencies)
{
    char a[400], l[400], all[1000];
    fresh();
    cat_line(a, sizeof(a), "lm-us-fl", "P25,AIR", BOX_ORL, GOOD_BODY, "20260101");
    lang_line(l, sizeof(l), "ble-ids", "DATA", DATA_BODY, ".bin");
    snprintf(all, sizeof(all), "%s%s", l, a);
    sign_cat(all, true);
    add_pack("lm-us-fl", GOOD_BODY);
    add_bin("ble-ids", DATA_BODY);
    ls_services_start();
    /* the whole world around here, every kind: Florida comes, the app data does not */
    LS_CHECK(ls_packs_install_around(s_lat, s_lon, 20000.0f, 0));
    LS_EQ_INT(round_(), LS_PACKS_READY);
    LS_CHECK(exists("/sdcard/lakeshark/packs/lm-us-fl.txt"));
    LS_CHECK(!exists("/sdcard/lakeshark/packs/ble-ids.bin"));
    LS_CHECK(ls_packs_install_around(s_lat, s_lon, 20000.0f, 0xffff));
    (void)round_();
    LS_CHECK(ls_packs_get_here(0));
    (void)round_();
    LS_CHECK(!exists("/sdcard/lakeshark/packs/ble-ids.bin"));
    /* by name: it comes as <id>.bin, the catalog's bytes */
    const char *ids[] = { "ble-ids" };
    LS_EQ_INT(ls_packs_install(ids, 1), 1);
    LS_EQ_INT(round_(), LS_PACKS_READY);
    LS_CHECK(same_file("/sdcard/lakeshark/packs/ble-ids.bin", DATA_BODY));
    LS_CHECK(!exists("/sdcard/lakeshark/packs/ble-ids.txt"));
    ls_packs_status_t st;
    ls_packs_status(&st);
    LS_EQ_INT(st.installed, 2);
    LS_EQ_INT(st.here, 1);                                  /* installed, never "here" */
    /* the frequency side never reads it, anywhere on the globe */
    static const double at[][2] = { { 28.5, -81.4 }, { 0, 0 }, { 48.85, 2.35 }, { -89.9, -180 } };
    for (unsigned i = 0; i < sizeof(at) / sizeof(at[0]); i++) {
        const int n = choose(at[i][0], at[i][1]);
        for (int j = 0; j < n; j++) LS_CHECK(strcmp(s_here[j].id, "ble-ids"));
    }
    s_nhere = choose(s_lat, s_lon);
    s_seen = s_evil = 0;
    ls_packs_scan(0xffff, 1e9f, data_evil_cb, NULL);
    LS_EQ_INT(s_evil, 0);
    LS_EQ_INT(s_seen, 2);                                   /* ORL1 and AIR1 */
    s_live = 0;
    near_build();
    ls_pack_sys_t sys[16];
    const int n = ls_packs_nearest(0xffff, 1e9f, sys, 16);
    LS_EQ_INT(n, 2);
    for (int i = 0; i < n; i++) LS_CHECK(strcmp(sys[i].pack, "ble-ids"));
    LS_CHECK(same_file("/sdcard/lakeshark/packs/ble-ids.bin", DATA_BODY));    /* untouched */
    /* a reload trusts it again from the verified index */
    LS_EQ_INT(cat_load(), 2);
    LS_EQ_INT(s_ninst, 2);
    /* removed by name: the .bin goes, the frequency pack stays */
    LS_EQ_INT(ls_packs_remove(ids, 1), 1);
    (void)round_();
    LS_CHECK(!exists("/sdcard/lakeshark/packs/ble-ids.bin"));
    LS_CHECK(exists("/sdcard/lakeshark/packs/lm-us-fl.txt"));
    ls_packs_status(&st);
    LS_EQ_INT(st.installed, 1);
}

LS_CASE(packs_data_copied_in_by_hand_is_checked_against_the_catalog)
{
    char l[400];
    fresh();
    lang_line(l, sizeof(l), "ble-flows", "DATA", DATA_BODY2, ".bin");
    put("/sdcard/lakeshark/packs/ble-flows.bin", DATA_BODY2, strlen(DATA_BODY2));
    LS_EQ_INT(load_with(l, true), 1);
    LS_CHECK(s_cat[s_cl][0].flags & F_INST);
    /* the same file under the wrong name, or changed, is not installed */
    fresh();
    put("/sdcard/lakeshark/packs/ble-flows.txt", DATA_BODY2, strlen(DATA_BODY2));
    char other[sizeof(DATA_BODY2)];
    memcpy(other, DATA_BODY2, sizeof(DATA_BODY2));
    other[0] = 'X';
    put("/sdcard/lakeshark/packs/ble-flows.bin", other, strlen(other));
    LS_EQ_INT(load_with(l, true), 1);
    LS_CHECK(!(s_cat[s_cl][0].flags & F_INST));
    LS_EQ_INT(s_ninst, 0);
}

LS_CASE(packs_data_and_lang_refresh_on_wifi_when_the_catalog_is_newer)
{
    char d[400], l[400], all[1000];
    fresh();
    lang_line(d, sizeof(d), "ble-ids", "DATA", DATA_BODY, ".bin");
    lang_line(l, sizeof(l), "lang-pt", "LANG", LANG_BODY, ".bin");
    snprintf(all, sizeof(all), "%s%s", d, l);
    sign_cat(all, true);
    add_bin("ble-ids", DATA_BODY);
    add_pack("lang-pt", LANG_BODY);
    ls_services_start();
    const char *ids[] = { "ble-ids", "lang-pt" };
    LS_EQ_INT(ls_packs_install(ids, 2), 2);
    (void)round_();
    LS_CHECK(same_file("/sdcard/lakeshark/packs/ble-ids.bin", DATA_BODY));
    LS_CHECK(same_file("/sdcard/lakeshark/packs/lang-pt.bin", LANG_BODY));
    /* a newer ble-ids (new bytes, later date); the language is unchanged */
    char d2[400];
    lang_line(d2, sizeof(d2), "ble-ids", "DATA", DATA_BODY2, ".bin");
    memcpy(strstr(d2, " 20261119"), " 20261201", 9);
    snprintf(all, sizeof(all), "%s%s", d2, l);
    sign_cat(all, true);
    g_pack_len[0] = strlen(DATA_BODY2);
    memcpy(g_pack[0], DATA_BODY2, sizeof(DATA_BODY2));
    /* Auto data off: nothing moves, however long */
    ls_shim_time_set(esp_timer_get_time() + 2 * CATALOG_US);
    (void)round_();
    LS_CHECK(same_file("/sdcard/lakeshark/packs/ble-ids.bin", DATA_BODY));
    /* on: the next look fetches the list and refreshes only what changed */
    s_auto = true;
    ls_shim_time_set(esp_timer_get_time() + 2 * CATALOG_US);
    (void)round_();
    s_auto = false;
    LS_CHECK(same_file("/sdcard/lakeshark/packs/ble-ids.bin", DATA_BODY2));
    LS_CHECK(same_file("/sdcard/lakeshark/packs/lang-pt.bin", LANG_BODY));
    ls_packs_status_t st;
    ls_packs_status(&st);
    LS_EQ_INT(st.installed, 2);
    LS_EQ_INT(st.updatable, 0);
    const int k = cat_find("ble-ids");
    LS_CHECK(k >= 0 && !strcmp(s_cat[s_cl][k].idate, "20261201"));
}
#endif
