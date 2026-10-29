/* The two Golay (23,12) syndrome tables are built at start-up now (mbelib's
   golayMatrix, P25 Phase II's ls_p2_gly23127DecTbl), where they used to be
   8 KB constants each. These checks pin the built tables to the old ones:
   an FNV-1a hash of all 2048 entries, taken from the constant tables before
   they were removed, plus a few entries read off them. */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

extern int golayMatrix[2048];
extern uint32_t ls_p2_gly23127DecTbl[2048];

static int fails;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)

static uint32_t fnv(const uint32_t *w, unsigned n)
{
    uint32_t h = 0x811c9dc5u;
    for (unsigned i = 0; i < n; i++)
        for (unsigned b = 0; b < 4; b++) {
            h ^= (w[i] >> (8 * b)) & 0xFFu;
            h *= 0x01000193u;
        }
    return h;
}

int main(void)
{
    uint32_t gm[2048];
    for (unsigned i = 0; i < 2048; i++) gm[i] = (uint32_t)golayMatrix[i];
    CHECK(fnv(gm, 2048) == 0xc978d7bdu);
    CHECK(golayMatrix[0] == 0 && golayMatrix[15] == 72 && golayMatrix[23] == 2084);
    CHECK(golayMatrix[2047] == 80);

    CHECK(fnv(ls_p2_gly23127DecTbl, 2048) == 0x4749b605u);
    CHECK(ls_p2_gly23127DecTbl[0] == 0 && ls_p2_gly23127DecTbl[15] == 147459u);
    CHECK(ls_p2_gly23127DecTbl[23] == 4268035u && ls_p2_gly23127DecTbl[2047] == 163843u);

    printf("%s: golay tables\n", fails ? "FAIL" : "PASS");
    return fails != 0;
}
