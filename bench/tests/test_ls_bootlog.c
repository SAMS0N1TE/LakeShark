/* The boot log's buffer and its two files. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ls_bootlog.h"

static int fails;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)

static char text[LS_BOOTLOG_TEXT_MAX + 1];
static char back[LS_BOOTLOG_TEXT_MAX + 1];

static void add(const char *s) { ls_bootlog_add(s, strlen(s)); }

static void test_whole(void)
{
    ls_bootlog_reset();
    CHECK(ls_bootlog_text(text, sizeof(text)) == 0);
    add("I (1) a: one\n");
    add("E (2) b: two\n");
    CHECK(ls_bootlog_seq() == 2);
    const size_t n = ls_bootlog_text(text, sizeof(text));
    CHECK(n == strlen(text));
    CHECK(!strcmp(text, "I (1) a: one\nE (2) b: two\n"));
    /* a small buffer is cut, terminated, never overrun */
    char small[8];
    memset(small, 'x', sizeof(small));
    CHECK(ls_bootlog_text(small, sizeof(small)) == 7 && small[7] == '\0');
    CHECK(!ls_bootlog_add(NULL, 4) && !ls_bootlog_add("x", 0));
    ls_bootlog_skip();
    CHECK(ls_bootlog_skipped() == 1);
    ls_bootlog_text(text, sizeof(text));
    CHECK(strstr(text, "(1 lines not kept"));
}

/* Past 40 KB the start stays whole and the newest lines follow a note. */
static void test_rolls(void)
{
    ls_bootlog_reset();
    char s[64];
    const int total = 3000;
    size_t bytes = 0;
    for (int i = 0; i < total; i++) {
        const int k = snprintf(s, sizeof(s), "I (%d) boot: line number %d\n", i * 7, i);
        CHECK(ls_bootlog_add(s, (size_t)k));
        bytes += (size_t)k;
    }
    CHECK(bytes > LS_BOOTLOG_HEAD + 2 * LS_BOOTLOG_TAIL);
    const size_t n = ls_bootlog_text(text, sizeof(text));
    CHECK(n < sizeof(text) && n == strlen(text));
    CHECK(!strncmp(text, "I (0) boot: line number 0\n", 26));
    CHECK(strstr(text, "boot: line number 100\n"));
    const char *note = strstr(text, "... ");
    CHECK(note && strstr(note, "bytes left out here"));
    CHECK(note && note > text && note[-1] == '\n');
    snprintf(s, sizeof(s), "boot: line number %d\n", total - 1);
    const char *last = strstr(text, s);
    CHECK(last && last[strlen(s)] == '\0');             /* the newest line ends it */
    /* the first line after the note is a whole one */
    const char *after = note ? strchr(note, '\n') : NULL;
    CHECK(after && !strncmp(after + 1, "I (", 3));
    /* every line after the note is in order and whole */
    int prev = -1, lines = 0;
    for (const char *p = after ? after + 1 : NULL; p && *p; ) {
        int stamp, num;
        CHECK(sscanf(p, "I (%d) boot: line number %d\n", &stamp, &num) == 2);
        CHECK(prev < 0 || num == prev + 1);
        prev = num; lines++;
        p = strchr(p, '\n');
        if (p) p++;
    }
    CHECK(lines > 100 && prev == total - 1);
    /* one line longer than the tail keeps its end */
    static char big[LS_BOOTLOG_TAIL + 500];
    memset(big, 'z', sizeof(big));
    big[sizeof(big) - 1] = '\n';
    CHECK(ls_bootlog_add(big, sizeof(big)));
    CHECK(ls_bootlog_text(text, sizeof(text)) < sizeof(text));
    CHECK(text[strlen(text) - 1] == '\n');
}

static void test_files(const char *dir)
{
    char cur[300], old[300];
    snprintf(cur, sizeof(cur), "%s/%s", dir, LS_BOOTLOG_FILE);
    snprintf(old, sizeof(old), "%s/%s", dir, LS_BOOTLOG_PREV);
    remove(cur); remove(old);

    /* run one: nothing on the card yet */
    ls_bootlog_reset();
    add("I (1) run: one\n");
    CHECK(ls_bootlog_load(dir, true, back, sizeof(back)) == 0);
    CHECK(ls_bootlog_save(dir));
    CHECK(ls_bootlog_load(dir, false, back, sizeof(back)) && !strcmp(back, "I (1) run: one\n"));
    CHECK(ls_bootlog_load(dir, true, back, sizeof(back)) == 0);
    /* a second save in the same run replaces the file and does not roll it */
    add("W (2) run: one more\n");
    CHECK(ls_bootlog_save(dir));
    CHECK(ls_bootlog_load(dir, false, back, sizeof(back)) && strstr(back, "one more"));
    CHECK(ls_bootlog_load(dir, true, back, sizeof(back)) == 0);

    /* run two: before it saves, the file there is run one's */
    ls_bootlog_reset();
    add("I (1) run: two\n");
    CHECK(ls_bootlog_load(dir, true, back, sizeof(back)) && strstr(back, "one more"));
    CHECK(ls_bootlog_save(dir));
    CHECK(ls_bootlog_load(dir, false, back, sizeof(back)) && !strcmp(back, "I (1) run: two\n"));
    CHECK(ls_bootlog_load(dir, true, back, sizeof(back)) && strstr(back, "run: one\n") && strstr(back, "one more"));

    /* run three rolls again: run one's is gone, run two's is the one before */
    ls_bootlog_reset();
    add("I (1) run: three\n");
    CHECK(ls_bootlog_save(dir));
    CHECK(ls_bootlog_load(dir, true, back, sizeof(back)) && !strcmp(back, "I (1) run: two\n"));

    /* no card: no file, no crash, and the next save still rolls */
    ls_bootlog_reset();
    add("I (1) run: four\n");
    CHECK(!ls_bootlog_save("no-such-dir/deeper"));
    CHECK(ls_bootlog_save(dir));
    CHECK(ls_bootlog_load(dir, true, back, sizeof(back)) && !strcmp(back, "I (1) run: three\n"));
    remove(cur); remove(old);
}

int main(int argc, char **argv)
{
    test_whole();
    test_rolls();
    test_files(argc > 1 ? argv[1] : ".");
    if (fails) { printf("%d failed\n", fails); return 1; }
    printf("ok\n");
    return 0;
}
