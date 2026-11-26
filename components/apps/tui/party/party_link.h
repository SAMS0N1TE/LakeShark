/* PARTY's transport: the one seam between PARTY and a radio.

   PARTY hands the link whole datagrams (party_proto.h, at most the link's
   MTU) and takes whole datagrams back. The link owns everything below that:
   claiming the LoRa chip through the radio selector, the frequencies for a
   channel number, LoRa for beacons and FLRC (LR2021) or GFSK (SX1262) for
   bulk, encryption and authentication with the 16-byte key, and refusing to
   go over the airtime it was given.

   Three backends:
     party_link_loop()   in memory, for the simulator and the tests: what is
                         sent is kept for the caller to look at, and the
                         caller injects what "the air" says (party_loop_*).
     party_link_board()  the board: a thin adapter that forwards each call to
                         ls_lsx (the LINK layer). Until it is wired, open()
                         fails with why = "LINK not wired".
   PARTY picks one with party_link_use() before starting.

   Threading: PARTY calls every function from one task (its service task on
   the board, the frame in the simulator). poll() must not block. */
#ifndef PARTY_LINK_H
#define PARTY_LINK_H

#include <stdbool.h>
#include <stdint.h>

#include "party_proto.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Radio mode the user prefers (profile). AUTO: LoRa beacons always, bulk
   (files) on FLRC when every member that answered has it, else GFSK, else
   LoRa. LONG: everything on LoRa at a long-range setting. FAST: everything
   on the fastest mode every member shares. */
enum { PARTY_MODE_AUTO, PARTY_MODE_LONG, PARTY_MODE_FAST, PARTY_MODES };

/* How a datagram travelled. */
enum { PARTY_VIA_LORA, PARTY_VIA_GFSK, PARTY_VIA_FLRC, PARTY_VIA_LOOP };

typedef struct {
    uint8_t key[16];           /* party_key_derive: the link encrypts and authenticates with it */
    uint16_t party;            /* the party id, for the link's own filtering if it has any */
    uint8_t channel;           /* 0..PARTY_CHANNELS-1 */
    uint8_t mode;              /* PARTY_MODE_* */
    uint8_t airtime_pct;       /* the most of each minute the link may transmit, 1..100 */
    uint8_t bulk_caps;         /* PARTY_CAP_FLRC / GFSK every known member has: what bulk may use */
} party_link_cfg_t;

typedef struct {
    uint8_t data[PARTY_DGRAM_MAX];
    uint8_t len;
    int8_t rssi;               /* dBm */
    int8_t snr;                /* dB, LoRa; 0 when the mode has none */
    uint8_t via;               /* PARTY_VIA_* */
} party_rx_t;

typedef struct {
    bool up;
    const char *radio;         /* "LR2021", "SX1262", "LOOP" */
    const char *why;           /* when !up: what is missing */
    uint8_t caps;              /* PARTY_CAP_LORA | GFSK | FLRC this radio has */
    uint8_t mtu;               /* largest datagram send() takes, <= PARTY_DGRAM_MAX */
    uint8_t bulk_via;          /* PARTY_VIA_* bulk is using now */
    uint32_t tx, rx, tx_fail, rx_bad;
    uint32_t air_ms_min;       /* transmit time in the last 60 s */
    uint32_t beacon_bps, bulk_bps;   /* raw rates of the two modes now */
} party_link_stats_t;

typedef struct {
    bool (*open)(const party_link_cfg_t *cfg);
    /* The configuration changed while open (members' caps, airtime). */
    void (*reconfig)(const party_link_cfg_t *cfg);
    void (*close)(void);
    /* Queue one datagram. dst is a member number or PARTY_ALL (the link may
       ignore it: everything is a broadcast on the air). bulk: a file chunk
       or ACK, which may go on the fast mode; false: a beacon or ping, which
       goes on the long-range mode. False when it was refused (not open,
       queue full, over the airtime). */
    bool (*send)(const uint8_t *d, int len, uint8_t dst, bool bulk);
    /* One received datagram into *out: true, or false when there is none. */
    bool (*poll)(party_rx_t *out);
    void (*stats)(party_link_stats_t *out);
    /* Air time of a datagram of len bytes, ms, for PARTY's own budget. */
    uint32_t (*airtime_ms)(int len, bool bulk);
} party_link_ops_t;

const party_link_ops_t *party_link_loop(void);
const party_link_ops_t *party_link_board(void);

/* ---- the loopback's air (simulator and tests) ---- */
/* Put a datagram on the air for the party to hear. */
bool party_loop_inject(const uint8_t *d, int len, int8_t rssi, int8_t snr);
/* Take the oldest datagram the party sent, true when there was one. */
bool party_loop_take(uint8_t *d, int *len, bool *bulk);
/* What the loop says its radio is: "LR2021" (FLRC) or "SX1262" (GFSK). */
void party_loop_radio(bool lr2021);

#ifdef __cplusplus
}
#endif
#endif
