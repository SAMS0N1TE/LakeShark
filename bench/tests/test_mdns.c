/* ls_mdns: the board's .local name. Queries as Windows, macOS and iOS
   send them, answers read back field by field. */

#include <stdio.h>
#include <string.h>

#include "ls_mdns.h"

static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static const uint8_t IP[4] = {192, 168, 16, 208};
static uint32_t ip(void) { uint32_t v; memcpy(&v, IP, 4); return v; }

/* A query with one question per name given, type and class each. */
static size_t query(uint8_t *b, unsigned id, int count, const char *const *names, const unsigned *types, const unsigned *classes)
{
    size_t n = 0;
    b[n++] = (uint8_t)(id >> 8); b[n++] = (uint8_t)id;
    b[n++] = 0; b[n++] = 0;
    b[n++] = 0; b[n++] = (uint8_t)count;
    memset(b + n, 0, 6); n += 6;
    for (int q = 0; q < count; q++) {
        const char *s = names[q];
        while (*s) {
            const char *dot = strchr(s, '.');
            const size_t k = dot ? (size_t)(dot - s) : strlen(s);
            b[n++] = (uint8_t)k;
            memcpy(b + n, s, k); n += k;
            s += k + (dot ? 1 : 0);
        }
        b[n++] = 0;
        b[n++] = (uint8_t)(types[q] >> 8); b[n++] = (uint8_t)types[q];
        b[n++] = (uint8_t)(classes[q] >> 8); b[n++] = (uint8_t)classes[q];
    }
    return n;
}

static const uint8_t NAME[] = "\x09lakeshark\x05local";   /* with the terminating zero */

static void answers_its_name(void)
{
    uint8_t q[256], a[256];
    bool uni;
    const char *names[] = {"LakeShark.LOCAL"};
    const unsigned types[] = {1}, classes[] = {1};
    size_t n = query(q, 0, 1, names, types, classes);
    size_t m = ls_mdns_answer(q, n, "lakeshark", ip(), false, a, sizeof(a), &uni);
    CHECK(m == 12 + sizeof(NAME) + 10 + 4, "answer length %d", (int)m);
    CHECK(!uni, "a QM question is answered to the group");
    CHECK(a[0] == 0 && a[1] == 0 && a[2] == 0x84 && a[3] == 0, "id zero, an authoritative answer");
    CHECK(a[5] == 0 && a[7] == 1, "no question, one answer");
    CHECK(!memcmp(a + 12, NAME, sizeof(NAME)), "the name");
    const uint8_t *r = a + 12 + sizeof(NAME);
    CHECK(r[0] == 0 && r[1] == 1 && r[2] == 0x80 && r[3] == 1, "A, IN, cache-flush");
    CHECK(r[4] == 0 && r[5] == 0 && r[6] == 0 && r[7] == 120 && r[8] == 0 && r[9] == 4, "ttl 120, four bytes");
    CHECK(!memcmp(r + 10, IP, 4), "the address");

    /* QU: iOS and macOS ask for the answer straight back the first time. */
    const unsigned qu[] = {0x8001};
    n = query(q, 0, 1, names, types, qu);
    CHECK(ls_mdns_answer(q, n, "lakeshark", ip(), false, a, sizeof(a), &uni) && uni, "QU");

    /* ANY is asked too. */
    const unsigned any[] = {255};
    n = query(q, 0, 1, names, any, classes);
    CHECK(ls_mdns_answer(q, n, "lakeshark", ip(), false, a, sizeof(a), &uni), "ANY");
}

static void legacy_asker(void)
{
    /* A plain resolver on another port: its id and question come back, short TTL, no cache-flush. */
    uint8_t q[256], a[256];
    bool uni;
    const char *names[] = {"lakeshark.local"};
    const unsigned types[] = {1}, classes[] = {1};
    const size_t n = query(q, 0xBEEF, 1, names, types, classes);
    const size_t m = ls_mdns_answer(q, n, "lakeshark", ip(), true, a, sizeof(a), &uni);
    CHECK(m == 12 + sizeof(NAME) + 4 + sizeof(NAME) + 10 + 4, "legacy length %d", (int)m);
    CHECK(a[0] == 0xBE && a[1] == 0xEF && a[5] == 1 && a[7] == 1, "id echoed, one question, one answer");
    const uint8_t *r = a + 12 + sizeof(NAME) + 4 + sizeof(NAME);
    CHECK(r[2] == 0 && r[3] == 1 && r[7] == 10, "IN without cache-flush, ttl 10");
}

static void leaves_the_rest(void)
{
    uint8_t q[256], a[256];
    bool uni;
    const unsigned a1[] = {1}, in[] = {1};
    const char *other[] = {"printer.local"};
    size_t n = query(q, 0, 1, other, a1, in);
    CHECK(!ls_mdns_answer(q, n, "lakeshark", ip(), false, a, sizeof(a), &uni), "another name");
    const char *longer[] = {"lakeshark.local.example"};
    n = query(q, 0, 1, longer, a1, in);
    CHECK(!ls_mdns_answer(q, n, "lakeshark", ip(), false, a, sizeof(a), &uni), "a longer name");
    const char *ours[] = {"lakeshark.local"};
    const unsigned aaaa[] = {28};
    n = query(q, 0, 1, ours, aaaa, in);
    CHECK(!ls_mdns_answer(q, n, "lakeshark", ip(), false, a, sizeof(a), &uni), "AAAA: no IPv6 here");
    n = query(q, 0, 1, ours, a1, in);
    q[2] = 0x84;
    CHECK(!ls_mdns_answer(q, n, "lakeshark", ip(), false, a, sizeof(a), &uni), "someone else's answer");
}

static void second_question_compressed(void)
{
    /* Windows asks A and AAAA together, the second name a pointer to the first. */
    uint8_t q[128] = {0, 0, 0, 0, 0, 2, 0, 0, 0, 0, 0, 0};
    size_t n = 12;
    memcpy(q + n, NAME, sizeof(NAME)); n += sizeof(NAME);
    const uint8_t aaaa[] = {0, 28, 0, 1, 0xC0, 12, 0, 1, 0, 1};
    memcpy(q + n, aaaa, sizeof(aaaa)); n += sizeof(aaaa);
    uint8_t a[256];
    bool uni;
    CHECK(ls_mdns_answer(q, n, "lakeshark", ip(), false, a, sizeof(a), &uni) > 0, "A behind a pointer");
}

static void malformed(void)
{
    uint8_t a[256];
    bool uni;
    uint8_t loop[] = {0, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0xC0, 12, 0, 1, 0, 1};
    CHECK(!ls_mdns_answer(loop, sizeof(loop), "lakeshark", ip(), false, a, sizeof(a), &uni), "a pointer to itself");
    uint8_t cut[] = {0, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 9, 'l', 'a', 'k'};
    CHECK(!ls_mdns_answer(cut, sizeof(cut), "lakeshark", ip(), false, a, sizeof(a), &uni), "cut short");
    CHECK(!ls_mdns_answer(cut, 5, "lakeshark", ip(), false, a, sizeof(a), &uni), "no header");
    uint8_t q[256];
    const char *names[] = {"lakeshark.local"};
    const unsigned types[] = {1}, classes[] = {1};
    const size_t n = query(q, 0, 1, names, types, classes);
    CHECK(!ls_mdns_answer(q, n, "lakeshark", ip(), false, a, 20, &uni), "no room for the answer");
}

static void announce(void)
{
    uint8_t a[128];
    const size_t m = ls_mdns_announce("lakeshark", ip(), a, sizeof(a));
    CHECK(m == 12 + sizeof(NAME) + 10 + 4 && a[2] == 0x84 && a[7] == 1, "announce");
    CHECK(!memcmp(a + m - 4, IP, 4), "announce's address");
}

int main(void)
{
    answers_its_name();
    legacy_asker();
    leaves_the_rest();
    second_question_compressed();
    malformed();
    announce();
    if (fails) { printf("%d failed\n", fails); return 1; }
    printf("mdns: all passed\n");
    return 0;
}
