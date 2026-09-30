#include "ls_test.h"
#include "ls_df_beacon.h"
#include <math.h>
#include <string.h>

/* What the beacon sends is what FIND reads back, to a centimetre. */
LS_CASE(a_beacon_packet_round_trips)
{
    uint8_t p[LS_DFS_BEACON_LEN];
    LS_EQ_INT(ls_dfs_beacon_encode(p, 0xBEEF, true, -33.8567844, 151.2152967, 9, 10), LS_DFS_BEACON_LEN);
    ls_dfs_beacon_t b;
    memset(&b, 0, sizeof(b));
    LS_CHECK(ls_dfs_beacon_parse(p, sizeof(p), &b));
    LS_CHECK(b.fix);
    LS_EQ_INT(b.seq, 0xBEEF);
    LS_CHECK_MSG(fabs(b.lat - -33.8567844) < 2e-7, "lat %.7f", b.lat);
    LS_CHECK_MSG(fabs(b.lon - 151.2152967) < 2e-7, "lon %.7f", b.lon);
    LS_EQ_INT(b.sats, 9);
    LS_EQ_INT(b.tx_dbm, 10);
    /* Western and negative power survive the sign. */
    ls_dfs_beacon_encode(p, 1, true, 43.45, -71.65, 7, -4);
    LS_CHECK(ls_dfs_beacon_parse(p, sizeof(p), &b));
    LS_CHECK(fabs(b.lon - -71.65) < 2e-7);
    LS_EQ_INT(b.tx_dbm, -4);
}

/* Anything else on the channel is not the beacon. */
LS_CASE(other_packets_are_not_the_beacon)
{
    uint8_t p[LS_DFS_BEACON_LEN];
    ls_dfs_beacon_t b;
    ls_dfs_beacon_encode(p, 1, true, 10, 10, 5, 10);
    LS_CHECK(!ls_dfs_beacon_parse(p, LS_DFS_BEACON_LEN - 1, &b));   /* short */
    p[4] = 2;
    LS_CHECK(!ls_dfs_beacon_parse(p, sizeof(p), &b));               /* another version */
    ls_dfs_beacon_encode(p, 1, true, 10, 10, 5, 10);
    p[0] = 'X';
    LS_CHECK(!ls_dfs_beacon_parse(p, sizeof(p), &b));
    LS_CHECK(!ls_dfs_beacon_parse(NULL, 18, &b));
}

/* A fix flag over a zeroed position is not a position. */
LS_CASE(a_zero_position_is_no_fix)
{
    uint8_t p[LS_DFS_BEACON_LEN];
    ls_dfs_beacon_t b;
    ls_dfs_beacon_encode(p, 3, true, 0, 0, 0, 10);
    LS_CHECK(ls_dfs_beacon_parse(p, sizeof(p), &b));
    LS_CHECK(!b.fix);
    ls_dfs_beacon_encode(p, 3, false, 43.4, -71.6, 0, 10);
    LS_CHECK(ls_dfs_beacon_parse(p, sizeof(p), &b));
    LS_CHECK(!b.fix);
}
