/* Fault injection for the downloader (main/ls_fetch.c): every stage of an
   HTTPS transfer can fail, and after any failure the next attempt must work,
   nothing may sit under the final name half written, and the UI must get a
   clear result. The server, the card and the heap are the fault shims. */
#include "ls_test.h"
#include "ls_fault.h"
#include "ls_fetch.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "mbedtls/sha256.h"
#include <stdio.h>
#ifdef _WIN32
#include <direct.h>
#endif

void lf_task_pump(void);

#define D   "/sdcard/lakeshark/t/x.bin"
#define DP  D ".part"
#define URL "https://data.terminalbay.com/v1/x.bin"

static uint8_t g_body[60000];
static uint8_t g_sha[32];

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

static void fresh(size_t len)
{
    ls_shim_heap_reset();
    lf_net_reset();
    lf_fs_reset();
    ls_shim_time_set(1000000);
    snprintf(lf_root, sizeof(lf_root), "lf_fetch_root");
    rm_tree(lf_root);
    _mkdir(lf_root);
    uint32_t x = 12345;
    for (size_t i = 0; i < sizeof(g_body); i++) { x = x * 1664525u + 1013904223u; g_body[i] = (uint8_t)('a' + ((x >> 24) % 26)); }
    lf_srv.body = g_body;
    lf_srv.body_len = len;
    mbedtls_sha256(g_body, len, g_sha, 0);
}

static bool exists(const char *p) { struct stat st; char b[400]; return !stat(lf_real(p, b, sizeof(b)), &st); }
static long fsize(const char *p) { struct stat st; char b[400]; return !stat(lf_real(p, b, sizeof(b)), &st) ? (long)st.st_size : -1; }
static bool same(const char *p, const void *d, size_t n)
{
    char b[400];
    FILE *f = fopen(lf_real(p, b, sizeof(b)), "rb");
    if (!f) return false;
    uint8_t *t = malloc(n + 1);
    bool ok = fread(t, 1, n + 1, f) == n && !memcmp(t, d, n);
    free(t);
    fclose(f);
    return ok;
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
static ls_fetch_req_t req(size_t len, bool sha)
{
    ls_fetch_req_t q = { .url = URL, .dest = D, .bytes = (uint32_t)len, .sha256 = sha ? g_sha : NULL, .timeout_s = 20 };
    return q;
}
/* nothing half written under, or beside, the final name */
static void clean_dir(void) { LS_CHECK(!exists(DP)); LS_CHECK(!exists(D ".bak")); }

LS_CASE(fetch_good_download_lands_whole_and_hashed)
{
    fresh(50000);
    ls_fetch_req_t q = req(50000, true);
    uint32_t got = 0; int http = 0;
    LS_EQ_INT(ls_fetch_run(&q, &got, &http), LS_FETCH_OK);
    LS_EQ_INT(got, 50000);
    LS_CHECK(same(D, g_body, 50000));
    clean_dir();
    LS_EQ_INT(lf_srv.opens, lf_srv.cleanups);
}

LS_CASE(fetch_memory_destination_is_terminated_and_capped)
{
    fresh(100);
    char buf[101], small[100];
    ls_fetch_req_t q = { .url = URL, .buf = buf, .cap = sizeof(buf), .timeout_s = 20 };
    LS_EQ_INT(ls_fetch_run(&q, NULL, NULL), LS_FETCH_OK);
    LS_CHECK(!memcmp(buf, g_body, 100) && buf[100] == 0);
    q.buf = small; q.cap = sizeof(small);     /* body 100 does not fit with its NUL */
    LS_EQ_INT(ls_fetch_run(&q, NULL, NULL), LS_FETCH_SIZE);
    lf_srv.claim_len = -1;                    /* chunked: length unknown up front */
    q.buf = buf; q.cap = 50;
    LS_EQ_INT(ls_fetch_run(&q, NULL, NULL), LS_FETCH_SIZE);
}

LS_CASE(fetch_no_network_unreachable_and_deadline_leave_nothing)
{
    fresh(2000);
    ls_fetch_req_t q = req(2000, true);
    lf_online = false;
    LS_EQ_INT(ls_fetch_run(&q, NULL, NULL), LS_FETCH_NO_NET);
    lf_online = true;
    lf_srv.open_status = -1;
    LS_EQ_INT(ls_fetch_run(&q, NULL, NULL), LS_FETCH_UNREACHABLE);   /* DNS, connect or TLS */
    lf_srv.open_status = -3;
    LS_EQ_INT(ls_fetch_run(&q, NULL, NULL), LS_FETCH_TIMEOUT);
    LS_CHECK(!exists(D)); clean_dir();
    lf_srv.open_status = 0;
    LS_EQ_INT(ls_fetch_run(&q, NULL, NULL), LS_FETCH_OK);            /* and the next try works */
    LS_CHECK(same(D, g_body, 2000));
}

LS_CASE(fetch_http_errors_report_status_and_clean_up)
{
    static const int codes[] = { 404, 500, 403, 503, 416 };
    for (unsigned i = 0; i < sizeof(codes) / sizeof(codes[0]); i++) {
        fresh(2000);
        ls_fetch_req_t q = req(2000, true);
        int http = 0;
        lf_srv.open_status = codes[i];
        LS_EQ_INT(ls_fetch_run(&q, NULL, &http), LS_FETCH_HTTP);
        LS_EQ_INT(http, codes[i]);
        LS_CHECK(!exists(D)); clean_dir();
        lf_srv.open_status = 0;
        LS_EQ_INT(ls_fetch_run(&q, NULL, &http), LS_FETCH_OK);
    }
}

LS_CASE(fetch_http_error_never_replaces_the_good_file)
{
    fresh(2000);
    put(D, "OLD", 3);
    ls_fetch_req_t q = req(2000, true);
    lf_srv.open_status = 500;
    LS_EQ_INT(ls_fetch_run(&q, NULL, NULL), LS_FETCH_HTTP);
    LS_CHECK(same(D, "OLD", 3));
    lf_srv.open_status = 0;
    lf_srv.drop_after = 700;
    LS_EQ_INT(ls_fetch_run(&q, NULL, NULL), LS_FETCH_UNREACHABLE);
    LS_CHECK(same(D, "OLD", 3)); clean_dir();
}

LS_CASE(fetch_fallback_host_is_tried_once_and_used)
{
    fresh(2000);
    ls_fetch_req_t q = req(2000, true);
    q.fallback = "https://mirror.example/x.bin";
    lf_srv.open_status = -1; lf_srv.fail_url = "terminalbay";
    LS_EQ_INT(ls_fetch_run(&q, NULL, NULL), LS_FETCH_OK);
    LS_EQ_INT(lf_srv.nurls, 2);
    LS_CHECK(strstr(lf_srv.urls[1], "mirror"));
    /* a deadline hit on the first host must not burn a second try */
    fresh(2000);
    q.fallback = "https://mirror.example/x.bin";
    lf_srv.open_status = -3;
    LS_EQ_INT(ls_fetch_run(&q, NULL, NULL), LS_FETCH_TIMEOUT);
    LS_EQ_INT(lf_srv.nurls, 1);
}

LS_CASE(fetch_connection_drop_mid_body_keeps_no_partial_final)
{
    fresh(50000);
    ls_fetch_req_t q = req(50000, true);
    lf_srv.drop_after = 9000;
    uint32_t got = 0;
    LS_EQ_INT(ls_fetch_run(&q, &got, NULL), LS_FETCH_UNREACHABLE);
    LS_CHECK(got > 0 && got < 50000);
    LS_CHECK(!exists(D)); clean_dir();
    lf_srv.drop_after = -1;
    LS_EQ_INT(ls_fetch_run(&q, NULL, NULL), LS_FETCH_OK);
    LS_CHECK(same(D, g_body, 50000)); clean_dir();
    LS_EQ_INT(lf_srv.opens, lf_srv.cleanups);
}

LS_CASE(fetch_drop_resumes_with_range_and_verifies_the_join)
{
    fresh(50000);
    ls_fetch_req_t q = req(50000, true);
    q.flags = LS_FETCH_RESUME;
    lf_srv.drop_after = 20000;
    LS_EQ_INT(ls_fetch_run(&q, NULL, NULL), LS_FETCH_UNREACHABLE);
    LS_CHECK(exists(DP) && fsize(DP) > 0 && fsize(DP) < 50000);
    LS_CHECK(!exists(D));
    lf_srv.drop_after = -1;
    LS_EQ_INT(ls_fetch_run(&q, NULL, NULL), LS_FETCH_OK);
    LS_CHECK(lf_srv.last_from > 0);
    LS_CHECK(same(D, g_body, 50000)); clean_dir();
}

LS_CASE(fetch_resume_against_server_without_range_restarts_clean)
{
    fresh(50000);
    ls_fetch_req_t q = req(50000, true);
    q.flags = LS_FETCH_RESUME;
    lf_srv.drop_after = 20000;
    LS_EQ_INT(ls_fetch_run(&q, NULL, NULL), LS_FETCH_UNREACHABLE);
    lf_srv.drop_after = -1; lf_srv.range_ok = false;   /* answers 200 with everything */
    LS_EQ_INT(ls_fetch_run(&q, NULL, NULL), LS_FETCH_OK);
    LS_CHECK(same(D, g_body, 50000)); clean_dir();
}

LS_CASE(fetch_resume_over_a_corrupt_part_fails_hash_then_recovers)
{
    fresh(50000);
    ls_fetch_req_t q = req(50000, true);
    q.flags = LS_FETCH_RESUME;
    uint8_t junk[8000];
    memset(junk, 0xEE, sizeof(junk));
    put(DP, junk, sizeof(junk));                       /* leftover from a power cut, bad bytes */
    LS_EQ_INT(ls_fetch_run(&q, NULL, NULL), LS_FETCH_HASH);
    LS_CHECK(!exists(D));
    LS_CHECK(!exists(DP));                             /* so the retry cannot loop on the same junk */
    LS_EQ_INT(ls_fetch_run(&q, NULL, NULL), LS_FETCH_OK);
    LS_CHECK(same(D, g_body, 50000)); clean_dir();
}

LS_CASE(fetch_stale_part_at_boot_never_leaks_into_a_plain_download)
{
    fresh(50000);
    ls_fetch_req_t q = req(50000, true);
    uint8_t junk[3000];
    memset(junk, 0x55, sizeof(junk));
    put(DP, junk, sizeof(junk));
    LS_EQ_INT(ls_fetch_run(&q, NULL, NULL), LS_FETCH_OK);   /* no RESUME: starts over */
    LS_CHECK(same(D, g_body, 50000)); clean_dir();
    /* a part as long as, or longer than, the file is never a resume point */
    uint8_t big[60000];
    memset(big, 0x66, sizeof(big));
    put(DP, big, 50000);
    q.flags = LS_FETCH_RESUME;
    LS_EQ_INT(ls_fetch_run(&q, NULL, NULL), LS_FETCH_OK);
    LS_CHECK(same(D, g_body, 50000)); clean_dir();
    put(DP, big, 60000);
    LS_EQ_INT(ls_fetch_run(&q, NULL, NULL), LS_FETCH_OK);
    LS_CHECK(same(D, g_body, 50000)); clean_dir();
}

LS_CASE(fetch_stale_part_longer_than_unknown_size_file_recovers)
{
    fresh(5000);
    ls_fetch_req_t q = req(0, true);                   /* size not stated */
    q.flags = LS_FETCH_RESUME;
    uint8_t big[9000];
    memset(big, 0x66, sizeof(big));
    put(DP, big, sizeof(big));                         /* longer than the whole file: server says 416 */
    ls_fetch_result_t r = ls_fetch_run(&q, NULL, NULL);
    LS_EQ_INT(r, LS_FETCH_HTTP);
    LS_CHECK(!exists(DP));                             /* not kept, or this fails forever */
    LS_EQ_INT(ls_fetch_run(&q, NULL, NULL), LS_FETCH_OK);
    LS_CHECK(same(D, g_body, 5000));
}

LS_CASE(fetch_short_body_and_wrong_content_length)
{
    fresh(50000);
    ls_fetch_req_t q = req(50000, true);
    lf_srv.end_early = 12000;                          /* server closes cleanly, early */
    LS_EQ_INT(ls_fetch_run(&q, NULL, NULL), LS_FETCH_UNREACHABLE);
    LS_CHECK(!exists(D)); clean_dir();
    lf_srv.end_early = -1;
    lf_srv.claim_len = 49000;                          /* announces less than promised */
    LS_EQ_INT(ls_fetch_run(&q, NULL, NULL), LS_FETCH_SIZE);
    LS_CHECK(!exists(D)); clean_dir();
    lf_srv.claim_len = 51000;                          /* announces more than there is */
    LS_EQ_INT(ls_fetch_run(&q, NULL, NULL), LS_FETCH_SIZE);
    LS_CHECK(!exists(D)); clean_dir();
    lf_srv.claim_len = -2;
    LS_EQ_INT(ls_fetch_run(&q, NULL, NULL), LS_FETCH_OK);
    LS_CHECK(same(D, g_body, 50000));
}

LS_CASE(fetch_chunked_body_is_held_to_the_stated_size)
{
    fresh(50000);
    ls_fetch_req_t q = req(40000, true);               /* the catalog says 40000, the wire carries 50000 */
    lf_srv.claim_len = -1;
    LS_EQ_INT(ls_fetch_run(&q, NULL, NULL), LS_FETCH_SIZE);
    LS_CHECK(!exists(D)); clean_dir();
    q = req(60000, true);                              /* and one that is too short */
    LS_EQ_INT(ls_fetch_run(&q, NULL, NULL), LS_FETCH_SIZE);
    LS_CHECK(!exists(D)); clean_dir();
}

LS_CASE(fetch_over_the_cap_is_refused_before_a_byte_is_stored)
{
    fresh(50000);
    ls_fetch_req_t q = req(0, false);
    q.max_bytes = 10000;
    LS_EQ_INT(ls_fetch_run(&q, NULL, NULL), LS_FETCH_SIZE);
    LS_CHECK(!exists(D)); clean_dir();
    lf_srv.claim_len = -1;
    LS_EQ_INT(ls_fetch_run(&q, NULL, NULL), LS_FETCH_SIZE);   /* unannounced, cut at the cap */
    LS_CHECK(!exists(D)); clean_dir();
}

LS_CASE(fetch_slow_server_times_out_and_resumes_later)
{
    fresh(50000);
    ls_fetch_req_t q = req(50000, true);
    q.flags = LS_FETCH_RESUME;
    q.timeout_s = 5;
    lf_srv.read_us = 1000000;                          /* 1 s per read */
    LS_EQ_INT(ls_fetch_run(&q, NULL, NULL), LS_FETCH_TIMEOUT);
    LS_CHECK(!exists(D));
    lf_srv.read_us = 0;
    q.timeout_s = 30;
    LS_EQ_INT(ls_fetch_run(&q, NULL, NULL), LS_FETCH_OK);
    LS_CHECK(same(D, g_body, 50000)); clean_dir();
}

LS_CASE(fetch_stalled_connection_gives_up_after_the_stall_window)
{
    fresh(50000);
    ls_fetch_req_t q = req(50000, true);
    q.timeout_s = 600;
    lf_srv.stall_after = 8192;
    int64_t t0 = esp_timer_get_time();
    LS_EQ_INT(ls_fetch_run(&q, NULL, NULL), LS_FETCH_TIMEOUT);
    LS_CHECK(esp_timer_get_time() - t0 < 60000000);    /* the 30 s stall rule, not the 600 s deadline */
    LS_CHECK(!exists(D)); clean_dir();
    lf_srv.stall_after = -1;
    LS_EQ_INT(ls_fetch_run(&q, NULL, NULL), LS_FETCH_OK);
}

LS_CASE(fetch_wrong_sha_or_size_never_publishes)
{
    fresh(50000);
    ls_fetch_req_t q = req(50000, true);
    uint8_t bad[32];
    memcpy(bad, g_sha, 32); bad[31] ^= 1;
    q.sha256 = bad;
    put(D, "OLD", 3);
    LS_EQ_INT(ls_fetch_run(&q, NULL, NULL), LS_FETCH_HASH);
    LS_CHECK(same(D, "OLD", 3)); clean_dir();
    q.sha256 = g_sha; q.bytes = 49999;
    LS_EQ_INT(ls_fetch_run(&q, NULL, NULL), LS_FETCH_SIZE);
    LS_CHECK(same(D, "OLD", 3)); clean_dir();
    q.bytes = 50000;
    LS_EQ_INT(ls_fetch_run(&q, NULL, NULL), LS_FETCH_OK);
    LS_CHECK(same(D, g_body, 50000)); clean_dir();
}

static size_t signed_body(uint8_t *out, size_t payload, bool good, bool trailing)
{
    uint8_t h[32];
    memcpy(out, g_body, payload);
    out[payload - 1] = '\n';
    mbedtls_sha256(out, payload, h, 0);
    if (!good) h[0] ^= 0xFF;
    size_t n = payload + (size_t)sprintf((char *)out + payload, "sig=");
    for (int i = 0; i < 8; i++) n += (size_t)sprintf((char *)out + n, "%02x", h[i]);
    n += (size_t)sprintf((char *)out + n, trailing ? "\njunk\n" : "\n");
    return n;
}

LS_CASE(fetch_signature_good_bad_missing_and_trailing_text)
{
    static uint8_t sb[2000];
    fresh(0);
    ls_fetch_req_t q = req(0, false);
    q.flags = LS_FETCH_SIGNED;
    size_t n;
    n = signed_body(sb, 1000, true, false);
    lf_srv.body = sb; lf_srv.body_len = n;
    LS_EQ_INT(ls_fetch_run(&q, NULL, NULL), LS_FETCH_OK);
    LS_CHECK(same(D, sb, n)); clean_dir();
    uint32_t sl = 0;
    LS_CHECK(ls_fetch_file_signed(D, &sl) && sl == 1000);
    char mem[2100];
    ls_fetch_req_t m = { .url = URL, .buf = mem, .cap = sizeof(mem), .flags = LS_FETCH_SIGNED, .timeout_s = 20 };
    LS_EQ_INT(ls_fetch_run(&m, NULL, NULL), LS_FETCH_OK);
    n = signed_body(sb, 1000, false, false);                 /* signed by someone else */
    lf_srv.body_len = n;
    put(D, "OLD", 3);
    LS_EQ_INT(ls_fetch_run(&q, NULL, NULL), LS_FETCH_SIG);
    LS_EQ_INT(ls_fetch_run(&m, NULL, NULL), LS_FETCH_SIG);
    LS_CHECK(same(D, "OLD", 3)); clean_dir();
    lf_srv.body = g_body; lf_srv.body_len = 1000;            /* no sig line at all */
    LS_EQ_INT(ls_fetch_run(&q, NULL, NULL), LS_FETCH_SIG);
    lf_srv.body_len = 1;                                     /* shorter than any signature */
    LS_EQ_INT(ls_fetch_run(&q, NULL, NULL), LS_FETCH_SIG);
    lf_srv.body_len = 0;                                     /* empty body */
    LS_EQ_INT(ls_fetch_run(&q, NULL, NULL), LS_FETCH_SIG);
    lf_srv.body = sb;
    n = signed_body(sb, 1000, true, true);                   /* text after the signature */
    lf_srv.body_len = n;
    LS_EQ_INT(ls_fetch_run(&q, NULL, NULL), LS_FETCH_SIG);
    n = signed_body(sb, 1000, true, false);
    sb[n - 3] = 'z';                                         /* a non-hex digit inside the signature */
    lf_srv.body_len = n;
    LS_EQ_INT(ls_fetch_run(&q, NULL, NULL), LS_FETCH_SIG);
    n = signed_body(sb, 1000, true, false);
    lf_srv.body_len = n - 2;                                 /* truncated inside the hex */
    LS_EQ_INT(ls_fetch_run(&q, NULL, NULL), LS_FETCH_SIG);
    LS_CHECK(same(D, "OLD", 3)); clean_dir();
    n = signed_body(sb, 1000, true, false);                  /* CRLF after the signature is fine */
    memcpy(sb + n - 1, "\r\n", 2);
    lf_srv.body_len = n + 1;
    LS_EQ_INT(ls_fetch_run(&q, NULL, NULL), LS_FETCH_OK);
}

/* A power cut inside publish(), after the good file stepped aside as .bak and
   before the new one took its name: the next read or fetch puts it back. */
LS_CASE(fetch_power_cut_inside_publish_restores_the_bak)
{
    static uint8_t sb[2000];
    char b[400];
    fresh(2000);
    const size_t n = signed_body(sb, 1000, true, false);
    put(D ".bak", sb, n);
    LS_CHECK(!exists(D));
    LS_CHECK(ls_fetch_file_signed(D, NULL));                 /* the signed read finds it again */
    LS_CHECK(same(D, sb, n)); clean_dir();
    remove(lf_real(D, b, sizeof(b)));
    put(D ".bak", "OLD", 3);
    ls_fetch_req_t q = req(2000, true);
    lf_srv.open_status = 500;                                /* and a fetch that fails keeps it */
    LS_EQ_INT(ls_fetch_run(&q, NULL, NULL), LS_FETCH_HTTP);
    LS_CHECK(same(D, "OLD", 3)); clean_dir();
    lf_srv.open_status = 0;                                  /* one that works replaces it */
    LS_EQ_INT(ls_fetch_run(&q, NULL, NULL), LS_FETCH_OK);
    LS_CHECK(same(D, g_body, 2000)); clean_dir();
    remove(lf_real(D, b, sizeof(b)));
    LS_CHECK(!ls_fetch_recover(D));                          /* nothing to bring back */
}

LS_CASE(fetch_card_full_mid_write_reports_card_error_and_frees_space)
{
    fresh(50000);
    ls_fetch_req_t q = req(50000, true);
    put(D, "OLD", 3);
    lf_fs.write_budget = 11000;
    LS_EQ_INT(ls_fetch_run(&q, NULL, NULL), LS_FETCH_SD);
    LS_CHECK(same(D, "OLD", 3));
    LS_CHECK(!exists(DP));                              /* a full card must not keep a dead partial */
    lf_fs.write_budget = -1;
    LS_EQ_INT(ls_fetch_run(&q, NULL, NULL), LS_FETCH_OK);
    LS_CHECK(same(D, g_body, 50000)); clean_dir();
}

LS_CASE(fetch_sd_open_flush_close_and_rename_errors)
{
    fresh(50000);
    ls_fetch_req_t q = req(50000, true);
    put(D, "OLD", 3);
    lf_fs.open_fail_substr = ".part";
    LS_EQ_INT(ls_fetch_run(&q, NULL, NULL), LS_FETCH_SD);
    lf_fs.open_fail_substr = NULL;
    lf_fs.fflush_fail = 1;
    LS_EQ_INT(ls_fetch_run(&q, NULL, NULL), LS_FETCH_SD);
    lf_fs.fflush_fail = 0; lf_fs.fclose_fail = 1;
    LS_EQ_INT(ls_fetch_run(&q, NULL, NULL), LS_FETCH_SD);
    lf_fs.fclose_fail = 0;
    LS_CHECK(same(D, "OLD", 3)); LS_CHECK(!exists(DP));
    lf_fs.rename_fail_nth = 2;                          /* dest -> .bak works, .part -> dest fails */
    LS_EQ_INT(ls_fetch_run(&q, NULL, NULL), LS_FETCH_SD);
    LS_CHECK(same(D, "OLD", 3));                        /* the good file came back */
    LS_CHECK(!exists(DP)); LS_CHECK(!exists(D ".bak"));
    lf_fs.rename_fail_nth = 1;                          /* cannot even move the old one aside */
    LS_EQ_INT(ls_fetch_run(&q, NULL, NULL), LS_FETCH_SD);
    LS_CHECK(same(D, "OLD", 3)); LS_CHECK(!exists(DP));
    lf_fs.rename_fail_nth = 0;
    LS_EQ_INT(ls_fetch_run(&q, NULL, NULL), LS_FETCH_OK);
    LS_CHECK(same(D, g_body, 50000)); clean_dir();
}

LS_CASE(fetch_destination_folders_that_cannot_be_made_do_not_crash)
{
    fresh(2000);
    ls_fetch_req_t q = req(2000, true);
    lf_fs.mkdir_fail = 1;
    LS_EQ_INT(ls_fetch_run(&q, NULL, NULL), LS_FETCH_SD);
    lf_fs.mkdir_fail = 0;
    LS_EQ_INT(ls_fetch_run(&q, NULL, NULL), LS_FETCH_OK);
    ls_fetch_req_t bad = req(2000, true);
    bad.dest = "/etc/passwd";                            /* outside the card */
    LS_EQ_INT(ls_fetch_run(&bad, NULL, NULL), LS_FETCH_SD);
    bad.dest = "/sdcard/aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
    LS_EQ_INT(ls_fetch_run(&bad, NULL, NULL), LS_FETCH_SD);
    bad.dest = NULL;                                     /* neither file nor buffer */
    LS_EQ_INT(ls_fetch_run(&bad, NULL, NULL), LS_FETCH_SD);
    LS_EQ_INT(ls_fetch_run(NULL, NULL, NULL), LS_FETCH_SD);
}

LS_CASE(fetch_out_of_memory_is_an_error_not_a_crash)
{
    for (unsigned from = 1; from < 6; from++) {
        fresh(50000);
        ls_fetch_req_t q = req(50000, true);
        ls_shim_heap_fail_from(from);
        ls_fetch_result_t r = ls_fetch_run(&q, NULL, NULL);
        ls_shim_heap_fail_from(0);
        LS_CHECK(r == LS_FETCH_SD || r == LS_FETCH_OK || r == LS_FETCH_HASH || r == LS_FETCH_SIG);
        LS_CHECK(r != LS_FETCH_OK || same(D, g_body, 50000));
        LS_CHECK(r == LS_FETCH_OK || !exists(D));
        clean_dir();
        LS_EQ_INT(lf_srv.opens, lf_srv.cleanups);
        LS_EQ_INT(ls_fetch_run(&q, NULL, NULL), LS_FETCH_OK);
        LS_CHECK(same(D, g_body, 50000));
    }
    /* the signature and resume hash paths allocate too */
    fresh(50000);
    ls_fetch_req_t q = req(50000, true);
    q.flags = LS_FETCH_RESUME;
    lf_srv.drop_after = 9000;
    (void)ls_fetch_run(&q, NULL, NULL);
    lf_srv.drop_after = -1;
    ls_shim_heap_fail_from(2);                           /* chunk ok, hash buffer fails */
    ls_fetch_result_t r = ls_fetch_run(&q, NULL, NULL);
    ls_shim_heap_fail_from(0);
    LS_CHECK(r != LS_FETCH_OK && !exists(D));
    LS_EQ_INT(ls_fetch_run(&q, NULL, NULL), LS_FETCH_OK);
    LS_CHECK(same(D, g_body, 50000));
}

/* ---- the queue ---- */

static int s_done_n, s_done_ids[16];
static ls_fetch_result_t s_done_res[16];
static void on_done(int id, ls_fetch_result_t r, void *ctx)
{
    (void)ctx;
    if (s_done_n < 16) { s_done_ids[s_done_n] = id; s_done_res[s_done_n] = r; }
    s_done_n++;
}

static void drain(void)
{
    for (int i = 0; i < 40; i++) {
        lf_task_pump();
        ls_fetch_info_t a; unsigned q;
        if (!ls_fetch_active(&a, &q) && !q) break;
    }
}

LS_CASE(fetch_queue_two_jobs_for_the_same_file_both_end_cleanly)
{
    fresh(50000);
    s_done_n = 0;
    ls_fetch_req_t q = req(50000, true);
    q.done = on_done;
    q.flags = LS_FETCH_RESUME;
    int a = ls_fetch_start(&q), b = ls_fetch_start(&q);
    LS_CHECK(a > 0 && b > a);
    drain();
    LS_EQ_INT(s_done_n, 2);
    LS_EQ_INT(s_done_res[0], LS_FETCH_OK);
    LS_EQ_INT(s_done_res[1], LS_FETCH_OK);
    LS_CHECK(same(D, g_body, 50000)); clean_dir();
    ls_fetch_info_t i;
    LS_CHECK(ls_fetch_info(b, &i) && i.state == LS_FETCH_OK && i.got == 50000);
}

static int s_cancel_id;
static void cancel_hook(size_t served) { if (served >= 8192) ls_fetch_cancel(s_cancel_id); }

LS_CASE(fetch_cancel_running_job_stops_and_next_job_succeeds)
{
    fresh(50000);
    s_done_n = 0;
    ls_fetch_req_t q = req(50000, true);
    q.done = on_done;
    int a = ls_fetch_start(&q);
    int b = ls_fetch_start(&q);
    s_cancel_id = a;
    lf_srv.on_read = cancel_hook;
    ls_fetch_cancel(b);                                   /* cancelled while still queued */
    lf_task_pump();
    lf_srv.on_read = NULL;
    drain();
    LS_EQ_INT(s_done_n, 2);
    LS_EQ_INT(s_done_res[0], LS_FETCH_CANCELLED);
    LS_EQ_INT(s_done_res[1], LS_FETCH_CANCELLED);
    LS_CHECK(!exists(D)); clean_dir();
    int c = ls_fetch_start(&q);                           /* the queue is usable again */
    drain();
    ls_fetch_info_t i;
    LS_CHECK(ls_fetch_info(c, &i) && i.state == LS_FETCH_OK);
    LS_CHECK(same(D, g_body, 50000));
}

LS_CASE(fetch_cancel_of_resumable_job_then_retry_succeeds)
{
    fresh(50000);
    s_done_n = 0;
    ls_fetch_req_t q = req(50000, true);
    q.done = on_done;
    q.flags = LS_FETCH_RESUME;
    s_cancel_id = ls_fetch_start(&q);
    lf_srv.on_read = cancel_hook;
    lf_task_pump();
    lf_srv.on_read = NULL;
    LS_EQ_INT(s_done_res[0], LS_FETCH_CANCELLED);
    LS_CHECK(!exists(D));
    int c = ls_fetch_start(&q);
    drain();
    ls_fetch_info_t i;
    LS_CHECK(ls_fetch_info(c, &i) && i.state == LS_FETCH_OK);
    LS_CHECK(same(D, g_body, 50000)); clean_dir();
}

LS_CASE(fetch_queue_full_returns_zero_and_slots_come_back)
{
    fresh(2000);
    ls_fetch_req_t q = req(2000, true);
    int ids[LS_FETCH_JOBS + 2];
    int n = 0;
    for (int i = 0; i < LS_FETCH_JOBS + 2; i++) { ids[i] = ls_fetch_start(&q); if (ids[i]) n++; }
    LS_EQ_INT(n, LS_FETCH_JOBS);
    LS_EQ_INT(ids[LS_FETCH_JOBS], 0);
    drain();
    LS_CHECK(ls_fetch_start(&q) > 0);
    drain();
    ls_fetch_req_t bad = req(2000, true);
    char longurl[400];
    memset(longurl, 'a', sizeof(longurl)); memcpy(longurl, "https://", 8); longurl[399] = 0;
    bad.url = longurl;
    LS_EQ_INT(ls_fetch_start(&bad), 0);                    /* too long to copy: refused, not truncated */
    LS_EQ_INT(ls_fetch_start(NULL), 0);
}

LS_CASE(fetch_queued_job_waits_for_network_then_gives_up_clearly)
{
    fresh(2000);
    s_done_n = 0;
    ls_fetch_req_t q = req(2000, true);
    q.done = on_done;
    lf_online = false;
    int a = ls_fetch_start(&q);                             /* no WAIT_NET: fail at once */
    lf_task_pump();
    LS_EQ_INT(s_done_res[0], LS_FETCH_NO_NET);
    q.flags = LS_FETCH_WAIT_NET; q.timeout_s = 10;
    int b = ls_fetch_start(&q);                             /* waits, then runs out of time */
    lf_task_pump();
    LS_EQ_INT(s_done_res[1], LS_FETCH_NO_NET);
    q.flags = LS_FETCH_BACKGROUND; lf_online = true; q.timeout_s = 10;
    lf_heap_free = 1000;                                    /* RAM too low for background traffic */
    int c = ls_fetch_start(&q);
    lf_task_pump();
    LS_EQ_INT(s_done_res[2], LS_FETCH_BUSY);
    lf_heap_free = 1u << 20; lf_rf_busy = true;             /* an RF app is live: BUSY, not a hang */
    int d = ls_fetch_start(&q);
    lf_task_pump();
    LS_EQ_INT(s_done_res[3], LS_FETCH_BUSY);
    lf_rf_busy = false;
    int e = ls_fetch_start(&q);
    lf_task_pump();
    LS_EQ_INT(s_done_res[4], LS_FETCH_OK);
    (void)e;
    (void)a; (void)b; (void)c; (void)d;
}

static void nested_run(size_t served)
{
    static bool once;
    if (served < 4096 || once) return;
    once = true;
    ls_fetch_req_t q = req(50000, true);
    lf_srv.on_read = NULL;
    LS_EQ_INT(ls_fetch_run(&q, NULL, NULL), LS_FETCH_BUSY);   /* same file, already being fetched */
    once = false;
}

LS_CASE(fetch_second_download_of_a_file_already_running_is_refused)
{
    fresh(50000);
    ls_fetch_req_t q = req(50000, true);
    lf_srv.on_read = nested_run;
    LS_EQ_INT(ls_fetch_run(&q, NULL, NULL), LS_FETCH_OK);
    lf_srv.on_read = NULL;
    LS_CHECK(same(D, g_body, 50000)); clean_dir();
    LS_EQ_INT(ls_fetch_run(&q, NULL, NULL), LS_FETCH_OK);     /* the claim is released afterwards */
}
