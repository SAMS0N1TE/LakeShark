#ifndef BLE_LINK_CORE_H
#define BLE_LINK_CORE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Pure protocol logic under the NimBLE transport. Free of ESP-IDF and NimBLE
   so the bench can exercise it on the host, in the same shape ls_wifi_sta_core
   was carved out for ls_wifi.c. ble_link.c owns the transport (scan, connect,
   discovery, CCCD, notify) and hands the pieces here that do not need a
   radio: line reassembly, payload sizing, backoff, disconnect classification,
   the //invariants, and the HELLO line format. */

typedef enum {
    BLE_LINK_OFF,
    BLE_LINK_SYNCING,
    BLE_LINK_SCANNING,
    BLE_LINK_CONNECTING,
    BLE_LINK_DISCOVERING,
    BLE_LINK_READY,
} ble_link_state_t;

const char *ble_link_state_name_of(ble_link_state_t s);

/* ----------------------------------------------------------------- framing */

#define BLE_LINK_LINE_MAX 192

/* Emit callback: called with a completed line (NUL-terminated, no newline).
   `user` is the caller's cookie and is not touched here. */
typedef void (*ble_link_line_cb_t)(const char *line, void *user);

/* Frame reassembler.

   Bytes arriving in any chunking - one GATT notification, a frame split
   across two notifications, or byte-at-a-time - are buffered until '\n' or
   '\r' delimits a line, at which point `cb` is invoked with the completed
   line. A line longer than BLE_LINK_LINE_MAX-1 has its buffer reset per
   overflowing byte (drops++ each time), matching the current on-device
   feed_rx exactly; the reader resynchronises at the next delimiter. */
typedef struct {
    char     line[BLE_LINK_LINE_MAX];
    int      pos;
    uint32_t lines;
    uint32_t drops;
} ble_link_rx_t;

void ble_link_rx_reset(ble_link_rx_t *rx);
void ble_link_rx_feed(ble_link_rx_t *rx, const uint8_t *data, int len,
                      ble_link_line_cb_t cb, void *user);

/* ---------------------------------------------------------------- payload */

/* how many bytes we can put on the wire per ATT write.
   MTU 0 (not exchanged yet) or below the 23-byte default returns the 20-byte
   floor; every larger MTU pays the 3-byte ATT header. */
#define BLE_LINK_MIN_PAYLOAD 20
/* The head's RX characteristic is 244 bytes wide, and a write wider than that is thrown away by its GATT server without a word. */

#define BLE_LINK_HEAD_ATT_MAX 244

int ble_link_payload_cap(uint16_t mtu);

/* How much of `want` may be written given the largest free block in the DMA-capable heap, once `overhead` (header + alignment slack + the transport's safety margin) is taken out. */

int ble_link_tx_chunk(size_t largest, size_t overhead, int want);

/* ---------------------------------------------------------------- backoff */

/* reconnect backoff. Starts at RECONNECT_BACKOFF_MS on the first
   failure, doubles on each subsequent one, and stops at
   RECONNECT_BACKOFF_MAX_MS. Overflow-safe: a value close to UINT32_MAX
   clamps to the ceiling instead of wrapping. */
#define BLE_LINK_RECONNECT_BACKOFF_MS      1500u
#define BLE_LINK_RECONNECT_BACKOFF_MAX_MS  20000u
uint32_t ble_link_backoff_next(uint32_t cur);

/* ---------------------------------------------------- disconnect classify */

/* two flavours of AUTH_FAIL disconnect that need different reactions.

   The head can reject the KEYS we stored (worth forgetting the bond), OR it
   can refuse our security level outright (SM_ERR_AUTHREQ - a head with the
   app closed looks like this, and it has nothing to do with the keys). The
   caller classifies the raw enc_change status into this enum before handing
   it here, so the core has no NimBLE dependency. */
typedef enum {
    BLE_LINK_ENC_NONE = 0,         /* no enc_change seen before the disconnect */
    BLE_LINK_ENC_OK,               /* enc_change status == 0 */
    BLE_LINK_ENC_AUTHREQ_REFUSED,  /* SM peer AUTHREQ - head said "no" */
    BLE_LINK_ENC_KEY_REJECTED,     /* PINKEY_MISSING / ENC_KEY_SZ - keys bad */
    BLE_LINK_ENC_TIMEOUT,          /* passkey window elapsed */
    BLE_LINK_ENC_OTHER_FAIL,       /* anything else non-zero */
} ble_link_enc_kind_t;

typedef enum {
    BLE_LINK_DISC_UNRELATED = 0,     /* not AUTH_FAIL / PINKEY_MISSING */
    BLE_LINK_DISC_AUTHREQ_REFUSAL,   /* head said "no" - KEEP the bond */
    BLE_LINK_DISC_KEY_REJECT,        /* stored keys bad - DROP the bond */
} ble_link_disc_class_t;

/* Classify a disconnect. `disc_is_auth_fail_or_pinkey` is true when the
   HCI disconnect reason is AUTH_FAIL or PINKEY_MISSING; `last_enc` is the
   last enc_change kind seen on this connection (BLE_LINK_ENC_NONE if none). */
ble_link_disc_class_t ble_link_classify_disc(bool disc_is_auth_fail_or_pinkey,
                                             ble_link_enc_kind_t last_enc);

/* At ENC_CHANGE time, some statuses mean "the stored keys were bad" and the
   bond must be forgotten immediately, before any disconnect arrives. */
bool ble_link_enc_kind_wipes_bond(ble_link_enc_kind_t k);

bool ble_link_enc_kind_needs_teardown(ble_link_enc_kind_t k);

/* -------------------------------------------------------- state machine */

typedef enum {
    BLE_LINK_EV_START_UP,           /* ble_link_start with the stack already up */
    BLE_LINK_EV_START_COLD,         /* ble_link_start with the stack not yet up */
    BLE_LINK_EV_SYNC,               /* controller synced */
    BLE_LINK_EV_SCAN_MATCH,         /* adv matched, ble_gap_connect started */
    BLE_LINK_EV_CONNECT_OK,         /* BLE_GAP_EVENT_CONNECT status == 0 */
    BLE_LINK_EV_CONNECT_FAIL,       /* BLE_GAP_EVENT_CONNECT status != 0 */
    BLE_LINK_EV_SUBSCRIBED,         /* CCCD write completed */
    BLE_LINK_EV_DISCONNECT_RUNNING, /* BLE_GAP_EVENT_DISCONNECT, still running */
    BLE_LINK_EV_DISCONNECT_STOPPING,/* BLE_GAP_EVENT_DISCONNECT while stopping */
    BLE_LINK_EV_STOP,               /* ble_link_stop */
} ble_link_event_t;

/* Pure state transition, used from ble_link.c at every event site. */

ble_link_state_t ble_link_state_step(ble_link_state_t prev,
                                     ble_link_event_t ev);

/* --------------------------------------------------- x invariants */

typedef struct {
    uint16_t scan_itvl;
    uint16_t scan_window;
    uint16_t itvl_min;
    uint16_t itvl_max;
    uint16_t latency;
    uint16_t supervision_timeout;
} ble_link_conn_params_t;

/* the parameters ble_gap_connect MUST be called with, so the link
   starts on our supervision timeout and does not sit on NimBLE's 2.56 s
   default. supervision_timeout = 400 units = 4 s. */
void ble_link_default_conn_params(ble_link_conn_params_t *out);

/* pin: after a successful connect, go straight to discovery. Do NOT
   initiate security - the head does not pair and the request would sit
   waiting for the supervision timeout. */
bool ble_link_ls823_should_pair_on_connect(void);

/* pin: after subscribe, do NOT ask for a CONN_UPDATE. The head cannot
   answer and the link dies at the 40 s LL response timeout. */
bool ble_link_ls824_should_request_conn_update(void);

/* ---------------------------------------------------- HELLO / sanitize */

/* transport sends this line as soon as the CCCD is subscribed, so a
   re-connecting head sees the firmware string without waiting for its first
   SYS reply. Format: "+HELLO <proto> LakeShark <version>\n". Returns the
   number of bytes written (excluding NUL), 0 on invalid arguments. */
#define BLE_LINK_HELLO_PROTO_VERSION 3
int ble_link_format_hello(char *buf, size_t len, int proto_version,
                          const char *version_str);

/* Same char-swap flipper_link.c applies to a telemetry field: replace ' ',
   '\t', '=', '\r', '\n' with '_' so a value survives the whitespace-split
   protocol without a special-case parser. */
void ble_link_sanitize_field(char *s);

/* -------------------------------------------------- passkey early hold */

/* A `ble pin <code>` typed before the PASSKEY_ACTION request arrives is
   stashed here and applied when the request does show up. */
typedef struct {
    uint32_t code;
    bool     valid;
} ble_link_pk_hold_t;

void ble_link_pk_hold_reset(ble_link_pk_hold_t *h);
void ble_link_pk_hold_set(ble_link_pk_hold_t *h, uint32_t code);
bool ble_link_pk_hold_take(ble_link_pk_hold_t *h, uint32_t *out);

/* -------------------------------------------------- peer identity */

typedef struct {
    uint8_t val[6];   /* little-endian, matches NimBLE ble_addr_t.val */
    uint8_t type;     /* BLE_ADDR_PUBLIC=0 / BLE_ADDR_RANDOM=1 / etc. */
    uint8_t valid;    /* zero means "no peer remembered" */
} ble_link_peer_addr_t;

typedef enum {
    BLE_LINK_SCAN_SKIP = 0,
    BLE_LINK_SCAN_CONNECT,
} ble_link_scan_decision_t;

/* Byte-compare val[] and type.  Two NULLs compare equal; NULL vs non-NULL is
   not equal.  `valid` is ignored - callers gate on it themselves. */
bool ble_link_peer_addr_equal(const ble_link_peer_addr_t *a,
                              const ble_link_peer_addr_t *b);

/* Render as "aa:bb:cc:dd:ee:ff" (big-endian, matching addr_str() in
   ble_link.c) into `out`, or "-" if a is NULL or !a->valid. */
void ble_link_peer_addr_format(const ble_link_peer_addr_t *a,
                               char *out, size_t len);

/* Decide whether to connect to this scan candidate. */

ble_link_scan_decision_t ble_link_scan_decide(
    bool has_our_service,
    bool name_matches_filter,
    const ble_link_peer_addr_t *adv_addr,
    const ble_link_peer_addr_t *pinned);

/* -------------------------------------------------- telemetry identity */

/* Emit the ` bd=<board>` field that identifies which board this telemetry frame came from. */

int ble_link_format_identity(char *out, size_t len, const char *board_name);

#ifdef __cplusplus
}
#endif

#endif
