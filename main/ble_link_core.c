#include "ble_link_core.h"

#include <stdio.h>
#include <string.h>

const char *ble_link_state_name_of(ble_link_state_t s)
{
    switch (s) {
    case BLE_LINK_OFF:         return "off";
    case BLE_LINK_SYNCING:     return "syncing";
    case BLE_LINK_SCANNING:    return "scanning";
    case BLE_LINK_CONNECTING:  return "connecting";
    case BLE_LINK_DISCOVERING: return "discovering";
    case BLE_LINK_READY:       return "ready";
    }
    return "?";
}

/* ----------------------------------------------------------------- framing */

void ble_link_rx_reset(ble_link_rx_t *rx)
{
    if (!rx) return;
    rx->pos     = 0;
    rx->lines   = 0;
    rx->drops   = 0;
    rx->line[0] = '\0';
}

void ble_link_rx_feed(ble_link_rx_t *rx, const uint8_t *data, int len,
                      ble_link_line_cb_t cb, void *user)
{
    if (!rx || !data || len <= 0) return;
    for (int i = 0; i < len; i++) {
        char c = (char)data[i];
        if (c == '\n' || c == '\r') {
            if (rx->pos > 0) {
                rx->line[rx->pos] = '\0';
                rx->lines++;
                if (cb) cb(rx->line, user);
                rx->pos = 0;
            }
        } else if (rx->pos < BLE_LINK_LINE_MAX - 1) {
            rx->line[rx->pos++] = c;
        } else {
            /* Overlong. Match the on-device behavior exactly: reset the
               buffer, count the drop, and let subsequent bytes start a fresh
               line at pos=0. */
            rx->pos = 0;
            rx->drops++;
        }
    }
}

/* ---------------------------------------------------------------- payload */

int ble_link_payload_cap(uint16_t mtu)
{
    int cap = (int)mtu - 3;
    if (mtu == 0 || cap < BLE_LINK_MIN_PAYLOAD) cap = BLE_LINK_MIN_PAYLOAD;
    /* Never offer the head more than its characteristic will hold. */
    if (cap > BLE_LINK_HEAD_ATT_MAX) cap = BLE_LINK_HEAD_ATT_MAX;
    return cap;
}

/**/
int ble_link_tx_chunk(size_t largest, size_t overhead, int want)
{
    if (want <= 0) return 0;
    if (largest <= overhead) return 0;
    size_t room = largest - overhead;
    if (room < (size_t)BLE_LINK_MIN_PAYLOAD) return 0;
    if (room >= (size_t)want) return want;
    return (int)room;
}

/* ---------------------------------------------------------------- backoff */

uint32_t ble_link_backoff_next(uint32_t cur)
{
    if (cur < BLE_LINK_RECONNECT_BACKOFF_MS) return BLE_LINK_RECONNECT_BACKOFF_MS;
    if (cur > BLE_LINK_RECONNECT_BACKOFF_MAX_MS / 2u) {
        return BLE_LINK_RECONNECT_BACKOFF_MAX_MS;
    }
    uint32_t next = cur * 2u;
    if (next > BLE_LINK_RECONNECT_BACKOFF_MAX_MS) {
        next = BLE_LINK_RECONNECT_BACKOFF_MAX_MS;
    }
    return next;
}

/* ---------------------------------------------------- disconnect classify */

ble_link_disc_class_t ble_link_classify_disc(bool disc_is_auth_fail_or_pinkey,
                                             ble_link_enc_kind_t last_enc)
{
    /* An AUTHREQ refusal at ENC_CHANGE takes precedence over the raw HCI
       reason: the point of is that AUTH_FAIL covers BOTH cases and the
       enc status is the only way to tell them apart. */
    if (last_enc == BLE_LINK_ENC_AUTHREQ_REFUSED) {
        return BLE_LINK_DISC_AUTHREQ_REFUSAL;
    }
    if (disc_is_auth_fail_or_pinkey) {
        return BLE_LINK_DISC_KEY_REJECT;
    }
    return BLE_LINK_DISC_UNRELATED;
}

bool ble_link_enc_kind_wipes_bond(ble_link_enc_kind_t k)
{
    /* AUTHREQ joins the list. If the peer rejects our requirements
       there is nothing usable in whatever we stored for it, and keeping it
       only guarantees the same rejection next time. */
    return k == BLE_LINK_ENC_KEY_REJECTED || k == BLE_LINK_ENC_AUTHREQ_REFUSED;
}

bool ble_link_enc_kind_needs_teardown(ble_link_enc_kind_t k)
{
    /* The link does not need encryption to be useful, so a failure to
       encrypt is not a reason to throw the connection away. Only a timeout is
       - that one means the head is waiting for a human who is not coming. */
    return k == BLE_LINK_ENC_TIMEOUT;
}

/* -------------------------------------------------------- state machine */

ble_link_state_t ble_link_state_step(ble_link_state_t prev,
                                     ble_link_event_t ev)
{
    (void)prev;
    switch (ev) {
    case BLE_LINK_EV_START_UP:            return BLE_LINK_SCANNING;
    case BLE_LINK_EV_START_COLD:          return BLE_LINK_SYNCING;
    case BLE_LINK_EV_SYNC:                return BLE_LINK_SCANNING;
    case BLE_LINK_EV_SCAN_MATCH:          return BLE_LINK_CONNECTING;
    case BLE_LINK_EV_CONNECT_OK:          return BLE_LINK_DISCOVERING;
    case BLE_LINK_EV_CONNECT_FAIL:        return BLE_LINK_SCANNING;
    case BLE_LINK_EV_SUBSCRIBED:          return BLE_LINK_READY;
    case BLE_LINK_EV_DISCONNECT_RUNNING:  return BLE_LINK_SCANNING;
    case BLE_LINK_EV_DISCONNECT_STOPPING: return BLE_LINK_OFF;
    case BLE_LINK_EV_STOP:                return BLE_LINK_OFF;
    }
    return prev;
}

/* --------------------------------------------------- x invariants */

void ble_link_default_conn_params(ble_link_conn_params_t *out)
{
    if (!out) return;
    /* scan_itvl/window keep NimBLE-like defaults; only the link
       timing is ours. supervision_timeout in NimBLE's default is
       0x0100 = 256 units = 2.56 s, which killed every pairing attempt
       against the Flipper on the timeout. 400 units = 4 s is the ours. */
    out->scan_itvl           = 0x0010;
    out->scan_window         = 0x0010;
    out->itvl_min            = 24;
    out->itvl_max            = 48;
    out->latency             = 0;
    out->supervision_timeout = 400;
}

bool ble_link_ls823_should_pair_on_connect(void)
{
    /* a Flipper-shaped head does not pair. Initiating security stalls
       the link on the supervision timer. This must stay false. */
    return false;
}

bool ble_link_ls824_should_request_conn_update(void)
{
    /* the head never answers a CONN_UPDATE and the LL response timer
       drops the link at 40 s. This must stay false. */
    return false;
}

/* ---------------------------------------------------- HELLO / sanitize */

void ble_link_sanitize_field(char *s)
{
    if (!s) return;
    for (char *p = s; *p; p++) {
        if (*p == ' ' || *p == '\t' || *p == '=' || *p == '\r' || *p == '\n') {
            *p = '_';
        }
    }
}

int ble_link_format_hello(char *buf, size_t len, int proto_version,
                          const char *version_str_sanitized)
{
    /* The caller sanitizes `version_str_sanitized` first (via
       ble_link_sanitize_field on the raw ls_version_line output), so this
       just splices it into the frame. Matches ble_link.c and flipper_link.c
       one-for-one. */
    if (!buf || len == 0) return 0;
    const char *v = version_str_sanitized ? version_str_sanitized : "";
    int n = snprintf(buf, len, "+HELLO %d LakeShark %s\n", proto_version, v);
    if (n < 0) { buf[0] = '\0'; return 0; }
    if ((size_t)n >= len) n = (int)len - 1;
    return n;
}

/* -------------------------------------------------- passkey early hold */

void ble_link_pk_hold_reset(ble_link_pk_hold_t *h)
{
    if (!h) return;
    h->code  = 0;
    h->valid = false;
}

void ble_link_pk_hold_set(ble_link_pk_hold_t *h, uint32_t code)
{
    if (!h) return;
    h->code  = code;
    h->valid = true;
}

bool ble_link_pk_hold_take(ble_link_pk_hold_t *h, uint32_t *out)
{
    if (!h || !h->valid) return false;
    if (out) *out = h->code;
    h->valid = false;
    return true;
}

/* -------------------------------------------------- peer identity */

bool ble_link_peer_addr_equal(const ble_link_peer_addr_t *a,
                              const ble_link_peer_addr_t *b)
{
    if (a == b) return true;
    if (!a || !b) return false;
    if (a->type != b->type) return false;
    for (int i = 0; i < 6; i++) {
        if (a->val[i] != b->val[i]) return false;
    }
    return true;
}

void ble_link_peer_addr_format(const ble_link_peer_addr_t *a,
                               char *out, size_t len)
{
    if (!out || len == 0) return;
    if (!a || !a->valid) {
        if (len > 1) { out[0] = '-'; out[1] = '\0'; }
        else out[0] = '\0';
        return;
    }
    snprintf(out, len, "%02x:%02x:%02x:%02x:%02x:%02x",
             a->val[5], a->val[4], a->val[3], a->val[2], a->val[1], a->val[0]);
}

ble_link_scan_decision_t ble_link_scan_decide(
    bool has_our_service,
    bool name_matches_filter,
    const ble_link_peer_addr_t *adv_addr,
    const ble_link_peer_addr_t *pinned)
{
    /* The service UUID is the hard gate, pin or no pin. */

    if (!has_our_service) return BLE_LINK_SCAN_SKIP;

    if (pinned && pinned->valid) {
        if (!adv_addr) return BLE_LINK_SCAN_SKIP;
        return ble_link_peer_addr_equal(adv_addr, pinned)
                   ? BLE_LINK_SCAN_CONNECT
                   : BLE_LINK_SCAN_SKIP;
    }
    return name_matches_filter ? BLE_LINK_SCAN_CONNECT : BLE_LINK_SCAN_SKIP;
}

int ble_link_format_identity(char *out, size_t len, const char *board_name)
{
    if (!out || len == 0) return 0;
    const char *name = (board_name && *board_name) ? board_name : "?";
    int n = snprintf(out, len, " bd=%s", name);
    if (n < 0) { out[0] = '\0'; return 0; }
    if ((size_t)n >= len) n = (int)len - 1;
    return n;
}
