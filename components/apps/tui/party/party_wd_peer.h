/* Members' finds as WARDRIVE sightings: what WARDRIVE's feed (the board's
   wd_feed_board.c and the simulator's lssim_wardrive.c) does with
   party_peer_since. Header only, so WARDRIVE's module needs nothing of
   PARTY's but this, and the call by name.

   Each find becomes a WD_RADIO_PEER sighting tagged WD_T_PEER plus its
   class's tags, placed where the member said it was (own_pos). Finds keep
   their kind; wd_core leaves WD_RADIO_PEER devices out of the WiGLE CSV,
   which is only what this board heard. Unknown kinds file as SUB-GHZ.
   A find with no address (a MARK, a HELP) gets one from its name and
   place, so two marks are two devices. PARTY never reads a WD_T_PEER
   device back (party_wd.c), so nothing echoes. */
#ifndef PARTY_WD_PEER_H
#define PARTY_WD_PEER_H

#include <string.h>

#include "party_core.h"
#include "../wardrive/wd_core.h"

#define PARTY_PEER_SUB 0xFE

static inline int party_peers_to_obs(const party_find_t *f, int n, wd_obs_t *out, int cap)
{
    static const uint8_t z[6];
    int k = 0;
    for (int i = 0; i < n && k < cap; i++) {
        wd_obs_t *o = &out[k++];
        memset(o, 0, sizeof(*o));
        const bool wiggle = f[i].kind >= WD_KINDS;
        o->kind = wiggle ? WD_RF : f[i].kind;
        o->sub = wiggle ? PARTY_PEER_SUB : 0;
        o->radio = WD_RADIO_PEER;
        o->tags = (uint16_t)(f[i].tags | WD_T_PEER);
        o->rssi = f[i].rssi;
        o->own_pos = true;
        o->lat = f[i].lat;
        o->lon = f[i].lon;
        memcpy(o->name, f[i].name, sizeof(o->name));
        o->name[sizeof(o->name) - 1] = 0;
        memcpy(o->addr, f[i].addr, 6);
        if (!memcmp(o->addr, z, 6)) {
            uint32_t h = 2166136261u;
            for (const char *p = f[i].name; *p; p++) h = (h ^ (uint8_t)*p) * 16777619u;
            h ^= (uint32_t)(int32_t)(f[i].lat * 1e4) * 31u + (uint32_t)(int32_t)(f[i].lon * 1e4);
            o->addr[0] = 0x50;               /* 'P': not a real address */
            o->addr[1] = 0x52;
            memcpy(o->addr + 2, &h, 4);
        }
    }
    return k;
}

#endif
