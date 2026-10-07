#include "ls_mesh_inspect.h"
#include <string.h>
#include <stdio.h>

void ls_inspect_init(ls_inspect_tracker_t *t)
{
    memset(t, 0, sizeof(*t));
    t->options = (ls_inspect_options_t){30, 60, 0};
}
void ls_inspect_options(ls_inspect_tracker_t *t, ls_inspect_options_t o)
{
    if (o.timeout_s < 5) o.timeout_s = 5;
    if (o.timeout_s > 120) o.timeout_s = 120;
    if (o.interval_s < 30) o.interval_s = 30;
    if (o.interval_s > 3600) o.interval_s = 3600;
    if (o.repeat_minutes > 60) o.repeat_minutes = 60;
    t->options = o;
}
void ls_inspect_finish(ls_inspect_tracker_t *t, uint32_t now, ls_inspect_state_t state)
{
    if (t->result.state != LS_INSPECT_WAITING || state < LS_INSPECT_OK || state > LS_INSPECT_NOT_PERMITTED) return;
    t->result.state = state;
    t->result.rtt_ms = now - t->result.started_ms;
    /* No answer to a session request: the node may have rebooted and lost us. */
    if (state == LS_INSPECT_TIMEOUT && (t->result.kind == LS_INSPECT_TELEMETRY ||
        t->result.kind == LS_INSPECT_STATUS)) ls_inspect_session_drop(t, t->result.peer);
    if (state == LS_INSPECT_OK && t->result.kind < LS_INSPECT_KINDS) t->latest[t->result.kind] = t->result;
    memmove(t->history + 1, t->history, sizeof(t->history[0]) * (LS_INSPECT_HISTORY - 1));
    ls_inspect_history_t *h = t->history;
    h->tag = t->result.tag; h->rtt_ms = t->result.rtt_ms;
    h->state = state; h->kind = t->result.kind;
    memcpy(h->peer, t->result.peer, sizeof(h->peer));
    if (t->history_count < LS_INSPECT_HISTORY) t->history_count++;
}
void ls_inspect_tick(ls_inspect_tracker_t *t, uint32_t now)
{
    if (t->result.state == LS_INSPECT_WAITING &&
        now - t->result.started_ms >= (uint32_t)t->options.timeout_s * 1000)
        ls_inspect_finish(t, now, LS_INSPECT_TIMEOUT);
}
void ls_inspect_transmitted(ls_inspect_tracker_t *t, uint32_t now)
{
    if (t->result.state != LS_INSPECT_WAITING) return;
    t->result.started_ms = now;
    t->last_ms = now;
}
bool ls_inspect_repeat_due(const ls_inspect_tracker_t *t, uint32_t now)
{
    uint32_t interval = (uint32_t)t->options.repeat_minutes * 60000;
    if (interval < (uint32_t)t->options.interval_s * 1000)
        interval = (uint32_t)t->options.interval_s * 1000;
    return t->sent && t->options.repeat_minutes && t->result.kind != LS_INSPECT_LOGIN && t->result.state != LS_INSPECT_WAITING &&
        now - t->last_ms >= interval;
}
bool ls_inspect_route(ls_inspect_tracker_t *t, const uint8_t *relays,
    uint8_t count, uint8_t width, const uint8_t *target)
{
    if (count > 24 || (width != 1 && width != 2) || !target || (count && !relays)) return false;
    ls_inspect_result_t *r = &t->result;
    r->hash_size = width; r->hop_count = count * 2 + 1;
    if (count) memcpy(r->hashes, relays, count * width);
    memcpy(r->hashes + count * width, target, width);
    for (int i = 0; i < count; i++)
        memcpy(r->hashes + (count + 1 + i) * width, relays + (count - 1 - i) * width, width);
    return true;
}
ls_inspect_state_t ls_inspect_begin(ls_inspect_tracker_t *t, uint32_t now,
    uint32_t tag, const char *peer, ls_inspect_kind_t kind, bool tx_allowed)
{
    ls_inspect_tick(t, now);
    if (t->result.state == LS_INSPECT_WAITING) return LS_INSPECT_REFUSED;
    if (!tx_allowed) return LS_INSPECT_NOT_PERMITTED;
    if (t->sent && now - t->last_ms < (uint32_t)t->options.interval_s * 1000)
        return LS_INSPECT_REFUSED;
    if (!peer || strlen(peer) != 16 || !tag || (unsigned)kind > LS_INSPECT_LOGIN)
        return LS_INSPECT_REFUSED;
    memset(&t->result, 0, sizeof(t->result));
    t->result.tag = tag; t->result.started_ms = now;
    t->result.kind = kind; t->result.state = LS_INSPECT_WAITING;
    memcpy(t->result.peer, peer, 17);
    t->last_ms = now; t->sent = true;
    return LS_INSPECT_WAITING;
}
bool ls_inspect_trace(ls_inspect_tracker_t *t, uint32_t now, uint32_t tag,
    uint8_t flags, const uint8_t *hashes, size_t bytes, const int8_t *snrs,
    size_t snr_count, float rssi, float snr)
{
    ls_inspect_tick(t, now);
    size_t width = 1u << (flags & 3), count = bytes / width;
    if (t->result.state != LS_INSPECT_WAITING || t->result.kind != LS_INSPECT_TRACE ||
        tag != t->result.tag || !hashes || !snrs || bytes % width || !count ||
        count > LS_INSPECT_HOPS || bytes > sizeof(t->result.hashes) || snr_count != count) return false;
    /* A tag alone cannot identify an unrelated or malformed route. */
    if (t->result.hop_count != count || t->result.hash_size != width ||
        memcmp(t->result.hashes, hashes, bytes)) return false;
    memcpy(t->result.snr_q4, snrs, count);
    t->result.reply_rssi = rssi; t->result.reply_snr = snr;
    ls_inspect_finish(t, now, LS_INSPECT_OK);
    return true;
}
/* Cayenne LPP lengths: unsupported fields are skipped without inventing values. */
static int field_size(uint8_t type)
{
    switch (type) {
    case 0: case 1: case 102: case 104: case 120: case 142: return 1;
    case 2: case 3: case 101: case 103: case 115: case 116: case 117: case 121: case 125: case 128: case 132: return 2;
    case 100: case 118: case 130: case 131: case 133: return 4;
    case 113: case 134: return 6;
    case 135: return 3;
    case 136: return 9;
    default: return -1;
    }
}
static float field_value(const uint8_t *data, int bytes, bool signed_value, float divisor)
{
    uint32_t raw = 0;
    for (int i = 0; i < bytes; i++) raw = (raw << 8) | data[i];
    int32_t value = (int32_t)raw;
    if (signed_value && bytes < 4 && (raw & (1u << (bytes * 8 - 1))))
        value = (int32_t)(raw | (UINT32_MAX << (bytes * 8)));
    return (signed_value ? (float)value : (float)raw) / divisor;
}
static uint32_t little_u32(const uint8_t *data)
{
    return (uint32_t)data[0] | (uint32_t)data[1] << 8 |
        (uint32_t)data[2] << 16 | (uint32_t)data[3] << 24;
}
/* RESP_SERVER_LOGIN_OK: server clock (4), 0, keep-alive, is-admin, permissions,
   random (4), firmware level. Authenticated by the shared secret already. */
static bool login_reply(ls_inspect_tracker_t *t, uint32_t now, const char *peer,
    const uint8_t *data, size_t len, float rssi, float snr)
{
    if (strcmp(peer, t->result.peer) || len < 13 || data[4] != 0) return false;
    ls_inspect_session_t *s = NULL;
    for (int i = 0; i < LS_INSPECT_SESSIONS; i++)
        if (!strcmp(t->sessions[i].peer, peer)) { s = &t->sessions[i]; break; }
    for (int i = 0; !s && i < LS_INSPECT_SESSIONS; i++)
        if (!t->sessions[i].peer[0]) s = &t->sessions[i];
    if (!s) { /* Replace the oldest. */
        s = &t->sessions[0];
        for (int i = 1; i < LS_INSPECT_SESSIONS; i++)
            if (t->sessions[i].since_ms < s->since_ms) s = &t->sessions[i];
    }
    memset(s, 0, sizeof(*s));
    memcpy(s->peer, peer, 16);
    s->admin = data[6] != 0; s->perms = data[7]; s->fw_level = data[12];
    s->since_ms = now ? now : 1;
    t->result.reply_rssi = rssi; t->result.reply_snr = snr;
    ls_inspect_finish(t, now, LS_INSPECT_OK);
    return true;
}
bool ls_inspect_telemetry(ls_inspect_tracker_t *t, uint32_t now, const char *peer,
    const uint8_t *data, size_t len, float rssi, float snr)
{
    ls_inspect_tick(t, now);
    uint32_t tag;
    if (!data || len < 4 || len > 184 || !peer) return false;
    if (t->result.state == LS_INSPECT_WAITING && t->result.kind == LS_INSPECT_LOGIN)
        return login_reply(t, now, peer, data, len, rssi, snr);
    memcpy(&tag, data, 4);
    if (t->result.state != LS_INSPECT_WAITING || t->result.kind == LS_INSPECT_TRACE ||
        tag != t->result.tag || strcmp(peer, t->result.peer)) return false;
    if (t->result.kind == LS_INSPECT_STATUS) {
        /* RepeaterStats starts with millivolts, queue, noise and RSSI,
           followed by four little-endian counters. Later fields are optional. */
        if (len < 28) return false;
        ls_inspect_result_t *r = &t->result;
        r->field_count = 1;
        r->fields[0].channel = 1; r->fields[0].type = 116;
        r->fields[0].value = ((unsigned)data[4] | (unsigned)data[5] << 8) / 1000.0f;
        r->rx_packets = little_u32(data + 12); r->tx_packets = little_u32(data + 16);
        r->airtime_s = little_u32(data + 20); r->uptime_s = little_u32(data + 24);
        r->has_status = true; r->reply_rssi = rssi; r->reply_snr = snr;
        ls_inspect_finish(t, now, LS_INSPECT_OK);
        return true;
    }
    /* Validate the entire payload before changing the visible result. */
    size_t end = 4;
    bool unknown = false;
    while (end < len) {
        size_t tail = end;
        while (tail < len && data[tail] == 0) tail++;
        if (tail == len) break; /* Cipher block padding. */
        if (end + 2 > len) return false;
        int n = field_size(data[end + 1]);
        if (n < 0) { unknown = true; break; }
        if (end + 2 + (size_t)n > len) return false;
        end += 2 + n;
    }
    t->result.telemetry_truncated = unknown;
    for (size_t i = 4; i < end;) {
        uint8_t channel = data[i++], type = data[i++];
        int n = field_size(type);
        if (t->result.field_count < LS_INSPECT_FIELDS) {
            ls_inspect_field_t *f = &t->result.fields[t->result.field_count++];
            f->channel = channel; f->type = type;
            bool sign = type == 2 || type == 3 || type == 103 || type == 121;
            float divisor = type == 2 || type == 3 || type == 116 ? 100 :
                type == 103 || type == 115 ? 10 : type == 104 ? 2 :
                type == 117 || type == 130 || type == 131 ? 1000 : 1;
            if (type == 113 || type == 134 || type == 135 || type == 136) {
                int bytes = n / 3;
                sign = type != 135;
                divisor = type == 113 ? 1000 : type == 134 ? 100 : type == 136 ? 10000 : 1;
                f->value = field_value(data + i, bytes, sign, divisor);
                f->value2 = field_value(data + i + bytes, bytes, sign, divisor);
                f->value3 = field_value(data + i + 2 * bytes, bytes, sign, type == 136 ? 100 : divisor);
            } else {
                f->value = field_value(data + i, n, sign, divisor);
                for (int j = 0; j < n; j++) f->raw = (f->raw << 8) | data[i + j];
            }
        } else t->result.telemetry_truncated = true;
        i += n;
    }
    t->result.reply_rssi = rssi; t->result.reply_snr = snr;
    ls_inspect_finish(t, now, LS_INSPECT_OK);
    return true;
}

static void put_le32(uint8_t *o, uint32_t v) { for (int i = 0; i < 4; i++) o[i] = (uint8_t)(v >> (8 * i)); }
size_t ls_inspect_login_build(uint8_t *out, size_t cap, uint32_t ts, const char *password)
{
    size_t n = password ? strlen(password) : 0;
    if (!out || n > LS_INSPECT_PASSWORD_MAX || cap < 4 + n) return 0;
    for (size_t i = 0; i < n; i++)
        if ((unsigned char)password[i] < ' ') return 0; /* below ' ' reads as another request type */
    put_le32(out, ts);
    if (n) memcpy(out + 4, password, n);
    return 4 + n;
}
size_t ls_inspect_request_build(uint8_t out[13], uint32_t tag, ls_inspect_kind_t kind, const uint8_t nonce[4])
{
    if (!out || !nonce || (kind != LS_INSPECT_TELEMETRY && kind != LS_INSPECT_STATUS)) return 0;
    memset(out, 0, 13);
    put_le32(out, tag);
    out[4] = kind == LS_INSPECT_STATUS ? 1 : 3; /* GET_STATUS / GET_TELEMETRY_DATA */
    memcpy(out + 9, nonce, 4);
    return 13;
}
const ls_inspect_session_t *ls_inspect_session(const ls_inspect_tracker_t *t, const char *peer)
{
    if (!t || !peer) return NULL;
    for (int i = 0; i < LS_INSPECT_SESSIONS; i++)
        if (t->sessions[i].peer[0] && !strcmp(t->sessions[i].peer, peer)) return &t->sessions[i];
    return NULL;
}
void ls_inspect_session_drop(ls_inspect_tracker_t *t, const char *peer)
{
    if (!t) return;
    bool all = !peer || !*peer || !strcmp(peer, "all");
    for (int i = 0; i < LS_INSPECT_SESSIONS; i++)
        if (all || !strcmp(t->sessions[i].peer, peer)) memset(&t->sessions[i], 0, sizeof(t->sessions[i]));
}
bool ls_inspect_needs_login(const ls_inspect_tracker_t *t, const char *peer,
    ls_inspect_kind_t kind, bool login_role)
{
    return login_role && (kind == LS_INSPECT_TELEMETRY || kind == LS_INSPECT_STATUS) &&
        !ls_inspect_session(t, peer);
}

/* ---- answering ---------------------------------------------------------- */
static bool valid_id(const char *id)
{
    if (!id || strlen(id) != 16) return false;
    for (int i = 0; i < 16; i++) {
        char c = id[i];
        if (!((c >= '0' && c <= '9') || (c >= 'A' && c <= 'F') || (c >= 'a' && c <= 'f'))) return false;
    }
    return true;
}
static char upper_hex(char c) { return (c >= 'a' && c <= 'f') ? (char)(c - 32) : c; }
static int allow_find(const ls_inspect_allow_t *a, const char *peer)
{
    if (!a || !valid_id(peer)) return -1;
    for (int i = 0; i < LS_INSPECT_ALLOW_MAX; i++) {
        if (!a->id[i][0]) continue;
        int j = 0;
        while (j < 16 && a->id[i][j] == upper_hex(peer[j])) j++;
        if (j == 16) return i;
    }
    return -1;
}
bool ls_inspect_allowed(const ls_inspect_allow_t *a, const char *peer) { return allow_find(a, peer) >= 0; }
int ls_inspect_allow_count(const ls_inspect_allow_t *a)
{
    int n = 0;
    for (int i = 0; a && i < LS_INSPECT_ALLOW_MAX; i++) if (a->id[i][0]) n++;
    return n;
}
bool ls_inspect_allow_set(ls_inspect_allow_t *a, const char *peer, bool allow)
{
    if (!a || !valid_id(peer)) return false;
    int at = allow_find(a, peer);
    if (!allow) {
        if (at >= 0) {
            memset(a->id[at], 0, sizeof(a->id[at]));
            a->last_tag[at] = a->last_ms[at] = 0; a->answered[at] = false;
        }
        return true;
    }
    if (at >= 0) return true;
    for (int i = 0; i < LS_INSPECT_ALLOW_MAX; i++)
        if (!a->id[i][0]) {
            for (int j = 0; j < 16; j++) a->id[i][j] = upper_hex(peer[j]);
            a->id[i][16] = 0;
            a->last_tag[i] = a->last_ms[i] = 0; a->answered[i] = false;
            return true;
        }
    return false;
}
static uint8_t *lpp_i16(uint8_t *o, uint8_t ch, uint8_t type, int32_t v)
{
    if (v > 32767) v = 32767;
    if (v < -32768) v = -32768;
    *o++ = ch; *o++ = type; *o++ = (uint8_t)((uint16_t)v >> 8); *o++ = (uint8_t)v;
    return o;
}
static uint8_t *be24(uint8_t *o, int32_t v)
{
    *o++ = (uint8_t)(v >> 16); *o++ = (uint8_t)(v >> 8); *o++ = (uint8_t)v;
    return o;
}
static int32_t round_scaled(float v, float scale) { return (int32_t)(v * scale + (v < 0 ? -0.5f : 0.5f)); }
size_t ls_inspect_answer(ls_inspect_allow_t *a, uint32_t now_ms, const char *peer,
    const uint8_t *req, size_t len, const ls_inspect_self_t *self,
    float rx_rssi, float rx_snr, uint8_t *out, size_t cap)
{
    int at = allow_find(a, peer);
    if (at < 0 || !req || !self || !out || len < 5 || cap < 64) return 0;
    uint32_t tag = (uint32_t)req[0] | (uint32_t)req[1] << 8 | (uint32_t)req[2] << 16 | (uint32_t)req[3] << 24;
    if (req[4] != 1 && req[4] != 3) return 0;
    if (a->answered[at] && (tag == a->last_tag[at] || now_ms - a->last_ms[at] < LS_INSPECT_ANSWER_GAP_MS))
        return 0;
    a->answered[at] = true; a->last_tag[at] = tag; a->last_ms[at] = now_ms;
    memcpy(out, req, 4); /* the tag is reflected, as a repeater does */
    uint8_t *o = out + 4;
    if (req[4] == 1) {
        /* RepeaterStats: the fields this node has; the rest stay zero. */
        memset(o, 0, 56);
        o[0] = (uint8_t)self->millivolts; o[1] = (uint8_t)(self->millivolts >> 8);
        o[4] = (uint8_t)self->noise_dbm; o[5] = (uint8_t)((uint16_t)self->noise_dbm >> 8);
        int16_t r = (int16_t)round_scaled(rx_rssi, 1);
        o[6] = (uint8_t)r; o[7] = (uint8_t)((uint16_t)r >> 8);
        put_le32(o + 8, self->rx_packets); put_le32(o + 12, self->tx_packets);
        put_le32(o + 16, self->airtime_s); put_le32(o + 20, self->uptime_s);
        int16_t q = (int16_t)round_scaled(rx_snr, 4);
        o[42] = (uint8_t)q; o[43] = (uint8_t)((uint16_t)q >> 8);
        return 60;
    }
    /* Cayenne LPP, as MeshCore sends it. */
    if (self->millivolts) o = lpp_i16(o, 1, 116, self->millivolts / 10);
    if (self->percent >= 0) { *o++ = 1; *o++ = 120; *o++ = (uint8_t)(self->percent > 100 ? 100 : self->percent); }
    o = lpp_i16(o, 2, 2, round_scaled(rx_rssi, 100));  /* analog: RSSI heard here */
    o = lpp_i16(o, 3, 2, round_scaled(rx_snr, 100));   /* analog: SNR heard here */
    o = lpp_i16(o, 4, 2, (int32_t)(self->uptime_s / 36)); /* analog: uptime, hours x100 */
    if (self->has_loc) {
        *o++ = 1; *o++ = 136;
        o = be24(o, round_scaled((float)(self->lat_e6 / 1e6), 10000));
        o = be24(o, round_scaled((float)(self->lon_e6 / 1e6), 10000));
        o = be24(o, 0);
    }
    return (size_t)(o - out);
}
