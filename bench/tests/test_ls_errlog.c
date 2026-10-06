/* LS_TEST_SOURCES: ${FW}/components/lakeshark/core/ls_errlog.c */
/* LS_TEST_INCLUDE: ${FW}/components/lakeshark/core */

/* The error log a crash leaves for the next boot: the last-words ring that
   survives the reset, and the record slots in flash (a fake store here). */

#include "ls_test.h"
#include "ls_errlog.h"

#include <stdio.h>
#include <string.h>

static ls_errlog_tail_t s_tail;

LS_CASE(last_words_come_back_oldest_first_and_whole)
{
    ls_errlog_tail_reset(&s_tail);
    ls_errlog_tail_add(&s_tail, "E (100) usb: transfer failed");
    ls_errlog_tail_add(&s_tail, "W (200) p25: stream quiet\n");
    char out[256];
    ls_errlog_tail_copy(&s_tail, out, sizeof(out));
    LS_EQ_STR(out, "E (100) usb: transfer failed\nW (200) p25: stream quiet\n");
}

LS_CASE(a_full_ring_drops_the_oldest_lines_not_half_a_line)
{
    ls_errlog_tail_reset(&s_tail);
    char line[64];
    for (int i = 0; i < 200; i++) {
        snprintf(line, sizeof(line), "E (%d) tag: line number %d", i, i);
        ls_errlog_tail_add(&s_tail, line);
    }
    char out[LS_ERRLOG_TAIL_BYTES + 1];
    const size_t n = ls_errlog_tail_copy(&s_tail, out, sizeof(out));
    LS_CHECK(n > 0);
    LS_CHECK(!strncmp(out, "E (", 3));                 /* starts on a line */
    LS_CHECK(strstr(out, "line number 199\n") != NULL); /* ends on the newest */
    LS_CHECK(strstr(out, "line number 0\n") == NULL);
    /* a smaller window keeps the newest whole lines */
    char small[64];
    ls_errlog_tail_copy(&s_tail, small, sizeof(small));
    LS_CHECK(!strncmp(small, "E (", 3));
    LS_CHECK(strstr(small, "line number 199\n") != NULL);
    LS_CHECK(strlen(small) < sizeof(small));
}

LS_CASE(power_on_noise_is_not_last_words)
{
    memset(&s_tail, 0x5A, sizeof(s_tail));
    LS_CHECK(!ls_errlog_tail_valid(&s_tail));
    char out[32] = "x";
    LS_EQ_INT((int)ls_errlog_tail_copy(&s_tail, out, sizeof(out)), 0);
    LS_EQ_STR(out, "");
}

/* --------------------------------------------------------------- the store */

static ls_errlog_rec_t s_slot[LS_ERRLOG_RECORDS];
static bool            s_full[LS_ERRLOG_RECORDS];
static uint32_t        s_seq;

static bool st_load(int slot, ls_errlog_rec_t *out)
{
    if (!s_full[slot]) return false;
    *out = s_slot[slot];
    return true;
}
static bool st_save(int slot, const ls_errlog_rec_t *rec) { s_slot[slot] = *rec; s_full[slot] = true; return true; }
static bool st_erase(int slot) { s_full[slot] = false; return true; }
static uint32_t st_get_seq(void) { return s_seq; }
static bool st_set_seq(uint32_t seq) { s_seq = seq; return true; }

static const ls_errlog_store_t s_store = { st_load, st_save, st_erase, st_get_seq, st_set_seq };

static void fresh_store(void)
{
    memset(s_slot, 0, sizeof(s_slot));
    memset(s_full, 0, sizeof(s_full));
    s_seq = 0;
    ls_errlog_init(&s_store);
}

static void save_one(const char *reason)
{
    ls_errlog_rec_t r;
    memset(&r, 0, sizeof(r));
    snprintf(r.reason, sizeof(r.reason), "%s", reason);
    snprintf(r.fw, sizeof(r.fw), "2.8.2-test");
    LS_CHECK(ls_errlog_save(&r));
}

LS_CASE(records_read_back_newest_first_and_the_oldest_give_way)
{
    fresh_store();
    LS_EQ_INT(ls_errlog_count(), 0);
    char name[16];
    for (int i = 1; i <= 6; i++) {
        snprintf(name, sizeof(name), "crash %d", i);
        save_one(name);
    }
    LS_EQ_INT((int)ls_errlog_total(), 6);
    LS_EQ_INT(ls_errlog_count(), LS_ERRLOG_RECORDS);
    ls_errlog_rec_t r;
    LS_CHECK(ls_errlog_get(0, &r));
    LS_EQ_STR(r.reason, "crash 6");
    LS_EQ_INT((int)r.seq, 6);
    LS_CHECK(ls_errlog_get(LS_ERRLOG_RECORDS - 1, &r));
    LS_EQ_STR(r.reason, "crash 3");
    LS_CHECK(!ls_errlog_get(LS_ERRLOG_RECORDS, &r));
}

LS_CASE(a_cleared_log_is_empty_and_numbers_from_one_again)
{
    fresh_store();
    save_one("first");
    save_one("second");
    LS_CHECK(ls_errlog_clear());
    LS_EQ_INT(ls_errlog_count(), 0);
    save_one("after");
    ls_errlog_rec_t r;
    LS_CHECK(ls_errlog_get(0, &r));
    LS_EQ_INT((int)r.seq, 1);
    LS_EQ_STR(r.reason, "after");
}

LS_CASE(a_slot_that_holds_another_record_is_not_passed_off)
{
    fresh_store();
    save_one("one");
    s_slot[0].seq = 99;                       /* a write that never finished */
    ls_errlog_rec_t r;
    LS_CHECK(!ls_errlog_get(0, &r));
}

LS_CASE(a_record_reads_as_text_with_its_last_words)
{
    fresh_store();
    ls_errlog_rec_t r;
    memset(&r, 0, sizeof(r));
    r.wall = 0;
    snprintf(r.reason, sizeof(r.reason), "watchdog");
    snprintf(r.fw, sizeof(r.fw), "2.8.2");
    snprintf(r.crumb, sizeof(r.crumb), "core1 stopped first: 'IDLE1'");
    snprintf(r.tail, sizeof(r.tail), "E (5) x: y\n");
    LS_CHECK(ls_errlog_save(&r));
    char text[512];
    ls_errlog_format(&r, text, sizeof(text));
    ls_note("%s", text);
    LS_CHECK(strstr(text, "#1  no clock  watchdog  (fw 2.8.2)") != NULL);
    LS_CHECK(strstr(text, "core1 stopped first") != NULL);
    LS_CHECK(strstr(text, "last words:\nE (5) x: y\n") != NULL);
}

LS_CASE(the_newest_record_is_read_once_and_follows_a_save_and_a_clear)
{
    fresh_store();
    LS_CHECK(ls_errlog_newest() == NULL);
    save_one("first");
    ls_errlog_init(&s_store);                 /* a new boot: read from the store */
    const ls_errlog_rec_t *r = ls_errlog_newest();
    LS_CHECK(r != NULL);
    LS_EQ_STR(r->reason, "first");
    s_seq = 40;                               /* read once, then kept */
    LS_EQ_INT((int)ls_errlog_total(), 1);
    s_seq = 1;
    save_one("second");
    LS_EQ_STR(ls_errlog_newest()->reason, "second");
    LS_CHECK(ls_errlog_clear());
    LS_CHECK(ls_errlog_newest() == NULL);
    LS_EQ_INT(ls_errlog_count(), 0);
}
