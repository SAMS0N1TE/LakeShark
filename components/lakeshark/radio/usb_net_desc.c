/* SPDX-License-Identifier: GPL-3.0-or-later
   LakeShark original. See usb_net_desc.h. References: USB CDC 1.2 and its
   ECM subclass 1.2 (functional descriptors, notifications), and Microsoft's
   Remote NDIS specification (message layouts). */
#include "usb_net_desc.h"

#include <string.h>

#define DT_CONFIG     0x02
#define DT_INTERFACE  0x04
#define DT_ENDPOINT   0x05
#define DT_STRING     0x03
#define DT_CS_INTF    0x24
#define CDC_UNION     0x06
#define CDC_ETHERNET  0x0F
#define CLASS_CDC_DATA 0x0A

static uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static uint32_t rd32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static void wr32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}

static ls_usbnet_kind_t ctrl_kind(uint8_t cls, uint8_t sub, uint8_t proto)
{
    if (cls == 0x02 && sub == 0x06) return LS_USBNET_ECM;
    /* RNDIS as Windows binds it: CDC ACM with the vendor protocol (older
       Android and Linux gadgets), the wireless-controller class (newer
       Android), and the misc "RNDIS over Ethernet" triple. */
    if (cls == 0x02 && sub == 0x02 && proto == 0xFF) return LS_USBNET_RNDIS;
    if (cls == 0xE0 && sub == 0x01 && proto == 0x03) return LS_USBNET_RNDIS;
    if (cls == 0xEF && sub == 0x04 && proto == 0x01) return LS_USBNET_RNDIS;
    return LS_USBNET_NONE;
}

/* Every descriptor must fit and be at least two bytes, or nothing is trusted. */
static size_t walkable(const uint8_t *cfg, size_t len)
{
    if (!cfg || len < 9 || cfg[0] < 9 || cfg[1] != DT_CONFIG) return 0;
    size_t total = rd16(cfg + 2);
    if (total > len) total = len;
    for (size_t off = 0; off < total;) {
        const uint8_t bl = cfg[off];
        if (bl < 2 || off + bl > total) return 0;
        off += bl;
    }
    return total;
}

/* Bulk IN/OUT pair in the first alt setting of `intf` that has both. */
static bool find_data(const uint8_t *cfg, size_t total, ls_usbnet_desc_t *d)
{
    bool in_intf = false;
    uint8_t in_ep = 0, out_ep = 0, alt = 0;
    uint16_t in_mps = 0, out_mps = 0;
    for (size_t off = cfg[0]; off < total; off += cfg[off]) {
        const uint8_t *p = cfg + off;
        if (p[1] == DT_INTERFACE && p[0] >= 9) {
            if (in_intf && in_ep && out_ep) break;
            in_intf = p[2] == d->data_intf && p[5] == CLASS_CDC_DATA;
            alt = p[3];
            in_ep = out_ep = 0;
        } else if (in_intf && p[1] == DT_ENDPOINT && p[0] >= 7 && (p[3] & 3) == 2) {
            const uint16_t mps = rd16(p + 4) & 0x7FF;
            if (p[2] & 0x80) { if (!in_ep) { in_ep = p[2]; in_mps = mps; } }
            else if (!out_ep) { out_ep = p[2]; out_mps = mps; }
        }
    }
    if (!in_intf || !in_ep || !out_ep || !in_mps || !out_mps) return false;
    d->data_alt = alt;
    d->in_ep = in_ep; d->out_ep = out_ep;
    d->in_mps = in_mps; d->out_mps = out_mps;
    return true;
}

bool ls_usbnet_parse_config(const uint8_t *cfg, size_t len, ls_usbnet_desc_t *out)
{
    const size_t total = walkable(cfg, len);
    if (!total) return false;
    for (size_t off = cfg[0]; off < total; off += cfg[off]) {
        const uint8_t *p = cfg + off;
        if (p[1] != DT_INTERFACE || p[0] < 9 || p[3] != 0) continue;
        const ls_usbnet_kind_t kind = ctrl_kind(p[5], p[6], p[7]);
        if (kind == LS_USBNET_NONE) continue;
        ls_usbnet_desc_t d = {.kind = kind, .config_value = cfg[5], .ctrl_intf = p[2],
                              .data_intf = (uint8_t)(p[2] + 1)};
        /* The function's own descriptors run up to the next interface. */
        for (size_t f = off + p[0]; f < total; f += cfg[f]) {
            const uint8_t *q = cfg + f;
            if (q[1] == DT_INTERFACE) break;
            if (q[1] == DT_CS_INTF && q[0] >= 5 && q[2] == CDC_UNION) {
                d.data_intf = q[4];
            } else if (q[1] == DT_CS_INTF && q[0] >= 13 && q[2] == CDC_ETHERNET) {
                d.mac_string = q[3];
                d.max_segment = rd16(q + 8);
            } else if (q[1] == DT_ENDPOINT && q[0] >= 7 && (q[3] & 3) == 3 && (q[2] & 0x80) &&
                       !d.notify_ep) {
                d.notify_ep = q[2];
                d.notify_mps = rd16(q + 4) & 0x7FF;
            }
        }
        if (find_data(cfg, total, &d)) {
            if (out) *out = d;
            return true;
        }
    }
    return false;
}

static int hex(uint8_t c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

bool ls_usbnet_parse_mac_string(const uint8_t *desc, size_t len, uint8_t mac[6])
{
    if (!desc || len < 26 || desc[0] < 26 || desc[1] != DT_STRING) return false;
    uint8_t m[6];
    for (int i = 0; i < 12; ++i) {
        const int v = desc[3 + 2 * i] ? -1 : hex(desc[2 + 2 * i]);
        if (v < 0) return false;
        if (i & 1) m[i / 2] = (uint8_t)(m[i / 2] | v);
        else m[i / 2] = (uint8_t)(v << 4);
    }
    /* All-zero or multicast is no unicast station address. */
    if ((m[0] & 1) || !(m[0] | m[1] | m[2] | m[3] | m[4] | m[5])) return false;
    memcpy(mac, m, 6);
    return true;
}

unsigned ls_usbnet_parse_notification(const uint8_t *buf, size_t len, bool *link,
                                      uint32_t *down_bps)
{
    unsigned set = 0;
    /* RNDIS RESPONSE_AVAILABLE is a bare little-endian 1 followed by zeros. */
    if (buf && len >= 8 && rd32(buf) == 1 && rd32(buf + 4) == 0)
        return LS_USBNET_NOTE_RESPONSE;
    for (size_t off = 0; buf && len - off >= 8;) {
        const uint8_t *p = buf + off;
        if (p[0] != 0xA1) break;
        const uint16_t value = rd16(p + 2), extra = rd16(p + 6);
        if (p[1] == 0x00) {
            if (link) *link = value != 0;
            set |= LS_USBNET_NOTE_LINK;
        } else if (p[1] == 0x01) {
            set |= LS_USBNET_NOTE_RESPONSE;
        } else if (p[1] == 0x2A && extra >= 8 && len - off >= 16) {
            if (down_bps) *down_bps = rd32(p + 8);
            set |= LS_USBNET_NOTE_SPEED;
        }
        if (extra > len - off - 8) break;
        off += 8u + extra;
    }
    return set;
}

ls_usb_route_t ls_usbnet_route(bool hackrf, uint8_t num_configs,
                               const uint8_t *active_cfg, size_t len, bool learned)
{
    if (hackrf) return LS_USB_ROUTE_HACKRF;
    if (active_cfg && ls_usbnet_parse_config(active_cfg, len, NULL)) return LS_USB_ROUTE_NET;
    /* RTL-SDR dongles have one configuration: they never take this branch. */
    if (num_configs > 1 && !learned) return LS_USB_ROUTE_NET_PROBE;
    return LS_USB_ROUTE_SDR;
}

#define LEARNED_MAX 4
static struct { uint16_t vid, pid; uint8_t config; } s_learned[LEARNED_MAX];
static unsigned s_learned_next;

static int learned_index(uint16_t vid, uint16_t pid)
{
    for (int i = 0; i < LEARNED_MAX; ++i)
        if (s_learned[i].config && s_learned[i].vid == vid && s_learned[i].pid == pid) return i;
    return -1;
}
void ls_usbnet_learn_config(uint16_t vid, uint16_t pid, uint8_t config_value)
{
    int i = learned_index(vid, pid);
    if (i < 0) i = (int)(s_learned_next++ % LEARNED_MAX);
    s_learned[i].vid = vid; s_learned[i].pid = pid;
    __atomic_store_n(&s_learned[i].config, config_value, __ATOMIC_RELEASE);
}
bool ls_usbnet_config_learned(uint16_t vid, uint16_t pid) { return learned_index(vid, pid) >= 0; }
uint8_t ls_usbnet_pick_config(uint16_t vid, uint16_t pid, uint8_t num_configs)
{
    const int i = learned_index(vid, pid);
    const uint8_t c = i < 0 ? 0 : __atomic_load_n(&s_learned[i].config, __ATOMIC_ACQUIRE);
    return c >= 1 && c <= num_configs ? c : 1;
}
void ls_usbnet_forget_configs(void)
{
    memset(s_learned, 0, sizeof(s_learned));
    s_learned_next = 0;
}

size_t ls_rndis_build_init(uint8_t *buf, size_t cap, uint32_t req_id, uint32_t max_transfer)
{
    if (!buf || cap < 24) return 0;
    wr32(buf, RNDIS_MSG_INIT); wr32(buf + 4, 24); wr32(buf + 8, req_id);
    wr32(buf + 12, 1); wr32(buf + 16, 0); wr32(buf + 20, max_transfer);
    return 24;
}
size_t ls_rndis_build_query(uint8_t *buf, size_t cap, uint32_t req_id, uint32_t oid)
{
    if (!buf || cap < 28) return 0;
    wr32(buf, RNDIS_MSG_QUERY); wr32(buf + 4, 28); wr32(buf + 8, req_id); wr32(buf + 12, oid);
    wr32(buf + 16, 0); wr32(buf + 20, 20); wr32(buf + 24, 0);
    return 28;
}
size_t ls_rndis_build_set_u32(uint8_t *buf, size_t cap, uint32_t req_id, uint32_t oid,
                              uint32_t value)
{
    if (!buf || cap < 32) return 0;
    wr32(buf, RNDIS_MSG_SET); wr32(buf + 4, 32); wr32(buf + 8, req_id); wr32(buf + 12, oid);
    wr32(buf + 16, 4); wr32(buf + 20, 20); wr32(buf + 24, 0); wr32(buf + 28, value);
    return 32;
}

bool ls_rndis_parse_reply(const uint8_t *buf, size_t len, uint32_t type, uint32_t req_id,
                          uint32_t *status, const uint8_t **info, size_t *info_len)
{
    if (!buf || len < 16 || rd32(buf) != type) return false;
    const uint32_t msg_len = rd32(buf + 4);
    if (msg_len < 16 || msg_len > len || rd32(buf + 8) != req_id) return false;
    if (type == RNDIS_MSG_QUERY_C) {
        if (msg_len < 24) return false;
        const uint32_t n = rd32(buf + 16), at = rd32(buf + 20);
        /* Offsets count from the RequestId field. */
        if (at > msg_len || n > msg_len - 8 || 8u + at > msg_len - n) return false;
        if (info) *info = buf + 8 + at;
        if (info_len) *info_len = n;
    }
    if (status) *status = rd32(buf + 12);
    return true;
}

int ls_rndis_parse_indicate(const uint8_t *buf, size_t len)
{
    if (!buf || len < 12 || rd32(buf) != RNDIS_MSG_INDICATE || rd32(buf + 4) < 12 ||
        rd32(buf + 4) > len)
        return -1;
    const uint32_t status = rd32(buf + 8);
    if (status == RNDIS_STATUS_MEDIA_CONNECT) return 1;
    if (status == RNDIS_STATUS_MEDIA_DISCONNECT) return 0;
    return -1;
}

void ls_rndis_packet_header(uint8_t hdr[LS_RNDIS_HEADER], size_t payload)
{
    memset(hdr, 0, LS_RNDIS_HEADER);
    wr32(hdr, RNDIS_MSG_PACKET);
    wr32(hdr + 4, (uint32_t)(LS_RNDIS_HEADER + payload));
    wr32(hdr + 8, LS_RNDIS_HEADER - 8);  /* from the DataOffset field */
    wr32(hdr + 12, (uint32_t)payload);
}

unsigned ls_rndis_unwrap(const uint8_t *buf, size_t len, ls_usbnet_frame_cb cb, void *ctx)
{
    unsigned delivered = 0;
    for (size_t off = 0; buf && len - off >= LS_RNDIS_HEADER;) {
        const uint8_t *p = buf + off;
        const uint32_t msg_len = rd32(p + 4);
        if (rd32(p) != RNDIS_MSG_PACKET || msg_len < LS_RNDIS_HEADER || msg_len > len - off) break;
        const uint32_t at = rd32(p + 8), n = rd32(p + 12);
        if (at > msg_len || n > msg_len || 8u + at > msg_len - n) break;
        if (ls_usbnet_frame_ok(n)) {
            if (cb) cb(p + 8 + at, n, ctx);
            ++delivered;
        }
        off += msg_len;
    }
    return delivered;
}
