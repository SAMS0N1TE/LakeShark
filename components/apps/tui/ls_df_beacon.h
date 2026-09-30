/* The LS DF beacon's packet: a T-Beam running the FIND test beacon sends
   these back to back on a DF channel (915 MHz by default, at the Mesh
   radio's SF and bandwidth), each carrying its GPS fix, so FIND can be
   judged against where the transmitter really is.

     0  "LSDF"            4  version, 1        5  flags, bit 0 = GPS fix
     6  sequence, u16     8  latitude e7, i32  12 longitude e7, i32
     16 satellites        17 TX power dBm, i8

   All little-endian, 18 bytes. The beacon's firmware writes the same
   layout (examples/ls_df_beacon in the MeshCore tree it is built from). */

#ifndef LS_DF_BEACON_H
#define LS_DF_BEACON_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define LS_DFS_BEACON_LEN 18

typedef struct {
    bool heard, fix;
    int64_t us;
    uint16_t seq;
    double lat, lon;
    float rssi;
    uint8_t sats;
    int8_t tx_dbm;
    uint32_t freq_hz, packets;
} ls_dfs_beacon_t;

/* False for anything that is not a version 1 beacon packet. `fix` is also
   false for an exact 0,0 or a position off the globe. Only the packet's
   own fields are written. */
bool ls_dfs_beacon_parse(const uint8_t *p, int n, ls_dfs_beacon_t *out);
/* The same layout, for the bench. Returns LS_DFS_BEACON_LEN. */
int ls_dfs_beacon_encode(uint8_t out[LS_DFS_BEACON_LEN], uint16_t seq, bool fix,
                         double lat, double lon, uint8_t sats, int8_t tx_dbm);

#ifdef __cplusplus
}
#endif
#endif
