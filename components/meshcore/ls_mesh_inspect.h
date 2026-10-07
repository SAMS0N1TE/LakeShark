#ifndef LS_MESH_INSPECT_H
#define LS_MESH_INSPECT_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
#define LS_INSPECT_HOPS 49
#define LS_INSPECT_FIELDS 8
#define LS_INSPECT_HISTORY 4
typedef enum { LS_INSPECT_IDLE, LS_INSPECT_WAITING, LS_INSPECT_OK,
    LS_INSPECT_TIMEOUT, LS_INSPECT_REFUSED, LS_INSPECT_NOT_PERMITTED,
    /* Never a finished state: TELEM or STATUS to a repeater before LOGIN. */
    LS_INSPECT_NEEDS_LOGIN } ls_inspect_state_t;
typedef enum { LS_INSPECT_TRACE, LS_INSPECT_TELEMETRY, LS_INSPECT_STATUS,
    LS_INSPECT_LOGIN } ls_inspect_kind_t;
#define LS_INSPECT_KINDS 3 /* results kept: TRACE, TELEMETRY, STATUS; a login is the session */
#define LS_INSPECT_SESSIONS 4
#define LS_INSPECT_PASSWORD_MAX 15 /* MeshCore stores 15 characters */
typedef struct { uint8_t channel, type; float value, value2, value3; uint32_t raw; } ls_inspect_field_t;
typedef struct {
    uint32_t tag, started_ms, rtt_ms;
    char peer[17];
    ls_inspect_state_t state;
    ls_inspect_kind_t kind;
    uint8_t hop_count, hash_size, hashes[LS_INSPECT_HOPS * 2];
    int8_t snr_q4[LS_INSPECT_HOPS];
    float reply_rssi, reply_snr;
    uint8_t field_count;
    bool telemetry_truncated;
    ls_inspect_field_t fields[LS_INSPECT_FIELDS];
    bool has_status;
    uint32_t uptime_s, rx_packets, tx_packets, airtime_s;
} ls_inspect_result_t;
typedef struct { uint32_t tag, rtt_ms; ls_inspect_state_t state; ls_inspect_kind_t kind; char peer[17]; } ls_inspect_history_t;
typedef struct { uint16_t timeout_s, interval_s, repeat_minutes; } ls_inspect_options_t;
/* A repeater session: what the node said at login, kept per node. */
typedef struct { char peer[17]; bool admin; uint8_t perms, fw_level; uint32_t since_ms; } ls_inspect_session_t;
typedef struct {
    ls_inspect_result_t result;
    ls_inspect_session_t sessions[LS_INSPECT_SESSIONS];
    ls_inspect_result_t latest[LS_INSPECT_KINDS]; /* Last successful result of each kind. */
    ls_inspect_history_t history[LS_INSPECT_HISTORY];
    ls_inspect_options_t options;
    uint32_t last_ms;
    bool sent;
    uint8_t history_count;
} ls_inspect_tracker_t;
void ls_inspect_init(ls_inspect_tracker_t *t);
void ls_inspect_options(ls_inspect_tracker_t *t, ls_inspect_options_t options);
void ls_inspect_tick(ls_inspect_tracker_t *t, uint32_t now);
bool ls_inspect_repeat_due(const ls_inspect_tracker_t *t, uint32_t now);
bool ls_inspect_route(ls_inspect_tracker_t *t, const uint8_t *relays,
    uint8_t count, uint8_t width, const uint8_t *target);
/* Reservation and the transmit gate are checked together, before queueing. */
ls_inspect_state_t ls_inspect_begin(ls_inspect_tracker_t *t, uint32_t now,
    uint32_t tag, const char *peer, ls_inspect_kind_t kind, bool tx_allowed);
void ls_inspect_finish(ls_inspect_tracker_t *t, uint32_t now, ls_inspect_state_t state);
/* Start RTT and cooldown when RF starts, rather than when it was queued. */
void ls_inspect_transmitted(ls_inspect_tracker_t *t, uint32_t now);
bool ls_inspect_trace(ls_inspect_tracker_t *t, uint32_t now, uint32_t tag,
    uint8_t flags, const uint8_t *hashes, size_t bytes, const int8_t *snrs,
    size_t snr_count, float rssi, float snr);
/* Also takes the reply to LOGIN, which is not tagged: it starts with the
   repeater's clock and is matched on the waiting probe and the peer. */
bool ls_inspect_telemetry(ls_inspect_tracker_t *t, uint32_t now, const char *peer,
    const uint8_t *data, size_t len, float rssi, float snr);

/* ---- repeater login (ANON_REQ) and requests, MeshCore wire formats ------ */
/* Login payload: timestamp (4, LE) then the password, no terminator; an empty
   password asks for the guest role. 0 when the password cannot be sent. */
size_t ls_inspect_login_build(uint8_t *out, size_t cap, uint32_t ts, const char *password);
/* REQ payload (13): tag (4), type (1: 1 status, 3 telemetry), reserved (4), nonce (4). */
size_t ls_inspect_request_build(uint8_t out[13], uint32_t tag, ls_inspect_kind_t kind,
    const uint8_t nonce[4]);
const ls_inspect_session_t *ls_inspect_session(const ls_inspect_tracker_t *t, const char *peer);
/* NULL, "" or "all" drops every session. */
void ls_inspect_session_drop(ls_inspect_tracker_t *t, const char *peer);
/* True when a repeater or room needs a session this probe does not have. */
bool ls_inspect_needs_login(const ls_inspect_tracker_t *t, const char *peer,
    ls_inspect_kind_t kind, bool login_role);

/* ---- answering TELEM/STATUS for allowed peers --------------------------- */
#define LS_INSPECT_ALLOW_MAX 8
#define LS_INSPECT_ANSWER_GAP_MS 10000
typedef struct {
    char id[LS_INSPECT_ALLOW_MAX][17];
    uint32_t last_tag[LS_INSPECT_ALLOW_MAX], last_ms[LS_INSPECT_ALLOW_MAX];
    bool answered[LS_INSPECT_ALLOW_MAX];
} ls_inspect_allow_t;
typedef struct {
    uint16_t millivolts; int8_t percent; /* percent < 0: unknown */
    uint32_t uptime_s, rx_packets, tx_packets, airtime_s;
    int16_t noise_dbm;
    bool has_loc; int32_t lat_e6, lon_e6; /* only when the user shares location */
} ls_inspect_self_t;
bool ls_inspect_allowed(const ls_inspect_allow_t *a, const char *peer);
/* False when the list is full or the id is not 16 hex digits. */
bool ls_inspect_allow_set(ls_inspect_allow_t *a, const char *peer, bool allow);
int ls_inspect_allow_count(const ls_inspect_allow_t *a);
/* Reply for a decrypted REQ from `peer`, or 0 to stay silent: not allowed,
   repeated tag, asked again within the gap, unknown type, or short buffer.
   rx_* describe how the request was heard. */
size_t ls_inspect_answer(ls_inspect_allow_t *a, uint32_t now_ms, const char *peer,
    const uint8_t *req, size_t len, const ls_inspect_self_t *self,
    float rx_rssi, float rx_snr, uint8_t *out, size_t cap);
#ifdef __cplusplus
}
#endif
#endif
