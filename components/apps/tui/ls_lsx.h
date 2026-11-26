/* LSX, the LakeShark link: two LakeShark boards with a LoRa chip find each
   other, open an encrypted session and move data, on the fastest modulation
   both chips share and the link will carry.

   WHO TALKS TO WHOM. A board that listens (the home base on the desk, a
   PARTY host) holds the radio on the discovery channel; a board out in the
   world (a field unit) borrows the radio for a moment every few seconds,
   sends a PROBE and listens 300 ms for a WELCOME. The answer opens a session:
   the listener is its master, the prober its slave, and both move to a data
   channel.

   MODES. LoRa SF7/125 kHz finds and falls back (both chips); GFSK 50 and
   200 kbps when one end is an SX1262; FLRC 130, 325 and 975 kbps (effective,
   after FEC) when both are LR2021s. The master picks the first mode from the
   probe's RSSI, drops to LoRa after three silent turns, and climbs one mode
   at a time while both ends report a level that mode carries with margin.

   TURNS. Half duplex, master and slave, like Bluetooth's polling: the master
   sends up to a window of frames, the last marked POLL; the slave answers
   with its own window, the last marked OVER. Acks ride in the turns.

   FRAMES (LSX_HDR + body + LSX_TAG bytes, at most 255):
     [0] 0x40 | type   [1] flags   [2..5] src id   [6..9] dst id
     [10..13] counter  [14] body length   body (AES-128-CTR)   tag (8 bytes of
     AES-CMAC over header and ciphertext). On GFSK and FLRC every frame is
     255 bytes (both drivers run fixed-length packets); the rest is cipher
     noise so the bit stream has no long runs without whitening. LoRa frames
     are their own length.

   KEYS. A 16-byte network key (the home key; a PARTY key comes from its
   code through lsx_phrase_key). Probes, welcomes, beacons and broadcast
   datagrams use keys derived from it; a session uses a fresh key derived
   from both ends' random nonces, so its counters start at zero and a
   replayed frame is refused.

   OBJECTS. Reliable transfers up to LSX_OBJ_MAX bytes on a port: a BEGIN
   (length, CRC-32, up to LSX_META_MAX bytes of meta), chunks, and acks with
   the first missing chunk and a 32-chunk bitmap after it. A transfer cut off
   by a lost session is kept by the receiver and resumed from where it was
   when the same object (same CRC and length) is offered again.

   DATAGRAMS. Unreliable, authenticated, to the session peer or broadcast on
   the discovery channel: what PARTY's live positions and detection pings
   use.

   The engine is plain C with no radio and no clock of its own: a phy
   (ls_lsx_phy_t) and ls_lsx_step(now) drive it, so the bench runs two of
   them against each other over a lossy loopback. ls_lsx_svc.c runs one on
   the board's LoRa chip. */
#ifndef LS_LSX_H
#define LS_LSX_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define LSX_FRAME_MAX   255
#define LSX_HDR         15
#define LSX_TAG         8
#define LSX_BODY_MAX    (LSX_FRAME_MAX - LSX_HDR - LSX_TAG)   /* 232 */
#define LSX_OBJ_MAX     32768u
#define LSX_META_MAX    24
#define LSX_BCAST       0xFFFFFFFFu
#define LSX_NAME        13

/* Modes, slowest first. */
enum { LSX_M_LORA, LSX_M_GFSK50, LSX_M_GFSK200, LSX_M_FLRC130, LSX_M_FLRC325, LSX_M_FLRC975, LSX_M_N };
enum { LSX_K_LORA, LSX_K_GFSK, LSX_K_FLRC };

typedef struct {
    const char *name;
    uint8_t  kind;          /* LSX_K_* */
    uint32_t bitrate;       /* raw bits a second on air (LoRa: the PHY rate)    */
    uint32_t net_bps;       /* what the payload moves at, after FEC             */
    uint32_t deviation_hz;  /* GFSK                                             */
    uint32_t bandwidth_hz;  /* receive filter (GFSK), signal width (FLRC, LoRa) */
    uint8_t  flrc;          /* FLRC rate code + 1 (ls_lora.h LS_LORA_FLRC_*)    */
    uint8_t  flrc_cr;       /* FLRC coding rate code                            */
    int8_t   min_rssi;      /* below this the mode is not tried (dBm)           */
    uint8_t  window;        /* frames a turn                                    */
} ls_lsx_mode_info_t;

const ls_lsx_mode_info_t *ls_lsx_mode_info(int mode);
/* Air time of a frame of `len` bytes in `mode` (GFSK and FLRC: always 255). */
uint32_t ls_lsx_airtime_us(int mode, int len);

/* Where on the band. US 902-928: discovery at 919.1 MHz, data on three
   channels clear of the MeshCore and Meshtastic defaults; a self-imposed
   30 % airtime cap. EU 869.4-869.65 MHz (10 % duty, 500 mW): LoRa and
   GFSK 50 only, the band is too narrow for the rest. */
enum { LSX_REGION_US915, LSX_REGION_EU868, LSX_REGION_N };
typedef struct {
    const char *name;
    uint32_t disc_hz;
    uint32_t data_hz[3];
    uint8_t  n_data;
    uint16_t duty_permille;     /* airtime cap over any 60 s */
    int8_t   max_dbm;
    uint8_t  modes;             /* bit per LSX_M_* allowed */
} ls_lsx_region_t;
const ls_lsx_region_t *ls_lsx_region(int region);

/* Roles. */
enum { LSX_ROLE_OFF, LSX_ROLE_HOME, LSX_ROLE_FIELD, LSX_ROLE_PARTY };
/* What a chip offers (ls_lsx_phy_t.caps and the probe). */
#define LSX_CAP_LORA  1u
#define LSX_CAP_GFSK  2u
#define LSX_CAP_FLRC  4u

/* ---- the radio underneath --------------------------------------------- */

typedef struct {
    int8_t rssi;            /* dBm                      */
    int8_t snr;             /* dB; 127 when not known   */
} ls_lsx_rxmeta_t;

typedef struct {
    void *ctx;
    uint32_t caps;          /* LSX_CAP_* */
    /* on: ask for the chip (and keep asking); true once it is ours.
       off: let it go (end any session on it, park it). */
    bool (*claim)(void *ctx, bool on);
    /* Someone with a better right wants the chip: let go soon. */
    bool (*yield)(void *ctx);
    /* Configure `mode` at `hz` and `dbm` and listen. 0 or negative. */
    int  (*tune)(void *ctx, int mode, uint32_t hz, int8_t dbm);
    /* Start sending one frame; it is `len` bytes (255 for GFSK and FLRC). */
    int  (*send)(void *ctx, const uint8_t *frame, int len);
    /* 1 when the frame has gone and the radio listens again, 0 still
       sending, negative on a failure (the radio then listens again). */
    int  (*tx_done)(void *ctx);
    /* A received frame's length, 0 for none, negative for a bad one. */
    int  (*recv)(void *ctx, uint8_t *buf, int cap, ls_lsx_rxmeta_t *m);
} ls_lsx_phy_t;

/* ---- what the engine tells its owner ------------------------------------ */

#define LSX_ROLE_MASK   0x7F
#define LSX_ROLE_FORCED 0x80    /* a probe asked for by hand (ls_lsx_probe_now) */

typedef struct {
    uint32_t id;
    char     name[LSX_NAME];
    uint8_t  role, caps;    /* role: LSX_ROLE_* | LSX_ROLE_FORCED */
    int8_t   rssi, snr;
    uint32_t hint;          /* the probe's or beacon's app word */
    uint32_t heard_ms;
} ls_lsx_peer_t;

/* Session states, as ls_lsx_status_t.state and on_session report them. */
enum { LSX_ST_OFF, LSX_ST_WAIT_RADIO, LSX_ST_LISTEN, LSX_ST_PROBE, LSX_ST_SESSION, LSX_ST_N };
/* on_session events. */
enum { LSX_EV_OPEN = 1, LSX_EV_CLOSED, LSX_EV_LOST, LSX_EV_MODE };
/* on_sent results. */
enum { LSX_SENT_OK = 0, LSX_SENT_LOST = -1, LSX_SENT_REFUSED = -2 };

typedef struct {
    void *ctx;
    /* A listener heard a probe: answer it (open a session) or not. */
    bool (*on_probe)(void *ctx, const ls_lsx_peer_t *p);
    /* Any probe, welcome or beacon heard. */
    void (*on_peer)(void *ctx, const ls_lsx_peer_t *p);
    void (*on_session)(void *ctx, uint32_t peer, int event);
    void (*on_object)(void *ctx, uint32_t peer, uint8_t port, const uint8_t *meta, int mlen,
                      const uint8_t *data, uint32_t len);
    void (*on_sent)(void *ctx, uint32_t peer, uint8_t port, void *tag, int result);
    void (*on_dgram)(void *ctx, uint32_t peer, uint8_t port, const uint8_t *data, int len);
    /* The word a probe or beacon carries (a field unit: its atlas seq). */
    uint32_t (*hint)(void *ctx);
} ls_lsx_app_t;

typedef struct {
    uint8_t  role;
    uint32_t id;            /* this board, never 0 or LSX_BCAST */
    char     name[LSX_NAME];
    uint8_t  key[16];
    uint8_t  region;
    int8_t   power_dbm;
    uint8_t  max_mode;      /* highest mode allowed (LSX_M_N - 1 for all)  */
    uint32_t probe_ms;      /* field: how often to probe (0: 8000)         */
    const ls_lsx_phy_t *phy;
    ls_lsx_app_t app;
    uint32_t (*rand32)(void);
} ls_lsx_cfg_t;

typedef struct ls_lsx ls_lsx_t;

ls_lsx_t *ls_lsx_create(const ls_lsx_cfg_t *cfg);
void ls_lsx_destroy(ls_lsx_t *x);
void ls_lsx_set_key(ls_lsx_t *x, const uint8_t key[16]);
void ls_lsx_set_power(ls_lsx_t *x, int8_t dbm);
void ls_lsx_set_max_mode(ls_lsx_t *x, int mode);
/* Run: everything happens in here. Returns how many ms the owner may wait
   before calling again (1 while frames are moving). */
uint32_t ls_lsx_step(ls_lsx_t *x, uint32_t now_ms);

/* Field: probe at the next step instead of waiting. */
void ls_lsx_probe_now(ls_lsx_t *x);
/* End the session (BYE) and give the radio back. */
void ls_lsx_close(ls_lsx_t *x);
/* Queue an object for the session peer; `data` (len bytes) and `meta` stay
   the caller's until on_sent says what happened. 0, or -1 when there is no
   session, the queue is full or it is too big. */
int ls_lsx_send_object(ls_lsx_t *x, uint8_t port, const uint8_t *meta, int mlen,
                       const uint8_t *data, uint32_t len, void *tag);
/* A datagram for the session peer (dst = its id) or everyone (LSX_BCAST,
   sent on the discovery channel when the radio is ours). At most
   LSX_BODY_MAX - 1 bytes. 0 queued, -1 not. */
int ls_lsx_send_dgram(ls_lsx_t *x, uint32_t dst, uint8_t port, const uint8_t *data, int len);

typedef struct {
    uint8_t  state, role;
    bool     held;              /* the radio is ours now          */
    uint32_t peer;
    char     peer_name[LSX_NAME];
    bool     master;
    uint8_t  mode;              /* LSX_M_*                        */
    uint32_t hz;
    int8_t   rssi, snr;         /* last frame from the peer       */
    int8_t   rssi_at_peer;      /* what the peer reports of ours  */
    uint8_t  rssi_hist[32];     /* last frames' RSSI + 160, 0 none, newest last */
    uint32_t frames_rx, frames_tx, frames_bad, turns, misses;
    uint32_t bytes_rx, bytes_tx;          /* object payload, this session */
    uint32_t obj_rx_done, obj_rx_len;     /* the object coming in   */
    uint8_t  obj_rx_meta[LSX_META_MAX];   /* and its BEGIN's meta   */
    uint8_t  obj_rx_mlen;
    uint32_t obj_tx_done, obj_tx_len;     /* the object going out   */
    uint32_t sessions, resumed;
    uint32_t airtime_ms_60s;
    uint32_t session_ms;
    uint32_t last_heard_ms;     /* since a valid frame from any peer; UINT32_MAX never */
    uint8_t  mode_changes;
} ls_lsx_status_t;
void ls_lsx_status(const ls_lsx_t *x, ls_lsx_status_t *out);
const char *ls_lsx_state_name(int state);

/* Test hook: drop this share (0..255 of 256) of received frames. */
void ls_lsx_set_rx_drop(ls_lsx_t *x, uint8_t share);

/* ---- the loopback the bench and lssim use ------------------------------ */

/* Two phys joined by a simulated channel: frames take their air time,
   arrive only when both ends are tuned to the same mode and frequency and
   the receiver is not transmitting, and are lost at `loss` (0..255 of 256).
   `rssi` is what each end hears. */
typedef struct ls_lsx_loop ls_lsx_loop_t;
ls_lsx_loop_t *ls_lsx_loop_create(uint32_t caps_a, uint32_t caps_b, uint32_t seed);
void ls_lsx_loop_destroy(ls_lsx_loop_t *l);
const ls_lsx_phy_t *ls_lsx_loop_phy(ls_lsx_loop_t *l, int end);   /* end 0 or 1 */
void ls_lsx_loop_set(ls_lsx_loop_t *l, uint8_t loss, int8_t rssi);
void ls_lsx_loop_time(ls_lsx_loop_t *l, uint32_t now_ms);
/* Neither end holds the radio while this is set (the other app has it). */
void ls_lsx_loop_block(ls_lsx_loop_t *l, int end, bool blocked);
uint32_t ls_lsx_loop_frames(ls_lsx_loop_t *l, int end);   /* frames delivered to end */

#ifdef __cplusplus
}
#endif
#endif
