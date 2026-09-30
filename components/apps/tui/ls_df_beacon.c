#include "ls_df_beacon.h"

#include <math.h>
#include <string.h>

static int32_t rd32(const uint8_t *p)
{
    return (int32_t)((uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24);
}

static void wr32(uint8_t *p, int32_t v)
{
    const uint32_t u = (uint32_t)v;
    p[0] = (uint8_t)u; p[1] = (uint8_t)(u >> 8); p[2] = (uint8_t)(u >> 16); p[3] = (uint8_t)(u >> 24);
}

bool ls_dfs_beacon_parse(const uint8_t *p, int n, ls_dfs_beacon_t *out)
{
    if (!p || !out || n < LS_DFS_BEACON_LEN || memcmp(p, "LSDF", 4) || p[4] != 1) return false;
    const int32_t lat = rd32(p + 8), lon = rd32(p + 12);
    out->fix = (p[5] & 1) && !(lat == 0 && lon == 0) && lat >= -900000000 && lat <= 900000000 &&
               lon >= -1800000000 && lon <= 1800000000;
    out->seq = (uint16_t)(p[6] | p[7] << 8);
    out->lat = lat / 1e7;
    out->lon = lon / 1e7;
    out->sats = p[16];
    out->tx_dbm = (int8_t)p[17];
    return true;
}

int ls_dfs_beacon_encode(uint8_t out[LS_DFS_BEACON_LEN], uint16_t seq, bool fix,
                         double lat, double lon, uint8_t sats, int8_t tx_dbm)
{
    memcpy(out, "LSDF", 4);
    out[4] = 1;
    out[5] = fix ? 1 : 0;
    out[6] = (uint8_t)seq; out[7] = (uint8_t)(seq >> 8);
    wr32(out + 8, (int32_t)lround(lat * 1e7));
    wr32(out + 12, (int32_t)lround(lon * 1e7));
    out[16] = sats;
    out[17] = (uint8_t)tx_dbm;
    return LS_DFS_BEACON_LEN;
}
