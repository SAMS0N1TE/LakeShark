/* TAP: capture files. Writes pcapng that Wireshark opens; reads pcapng and
   classic pcap back into TAP's records. */
#include "tap.h"

static unsigned le16(const uint8_t *p) { return (unsigned)p[1] << 8 | p[0]; }
static uint32_t le32(const uint8_t *p) { return (uint32_t)p[3] << 24 | (uint32_t)p[2] << 16 | (uint32_t)p[1] << 8 | p[0]; }
static size_t w16(uint8_t *o, unsigned v) { o[0] = (uint8_t)v; o[1] = (uint8_t)(v >> 8); return 2; }
static size_t w32(uint8_t *o, uint32_t v) { o[0] = (uint8_t)v; o[1] = (uint8_t)(v >> 8); o[2] = (uint8_t)(v >> 16); o[3] = (uint8_t)(v >> 24); return 4; }
static size_t option(uint8_t *o, unsigned code, const void *v, unsigned n)
{
    const unsigned pad = (n + 3u) & ~3u;
    w16(o, code); w16(o + 2, n);
    memset(o + 4, 0, pad);
    memcpy(o + 4, v, n);
    return 4 + pad;
}
static size_t idb(uint8_t *o, unsigned linktype, const char *name, const char *what)
{
    static const uint8_t usec = 6;
    size_t n = 8;
    n += w16(o + n, linktype); n += w16(o + n, 0); n += w32(o + n, LAP_CAPTURE_SNAP_MAX + 64);
    n += option(o + n, 2, name, (unsigned)strlen(name));       /* if_name */
    n += option(o + n, 3, what, (unsigned)strlen(what));       /* if_description */
    n += option(o + n, 9, &usec, 1);                            /* if_tsresol: microseconds */
    n += w32(o + n, 0);                                         /* opt_endofopt */
    w32(o, 1); w32(o + 4, (uint32_t)n + 4); w32(o + n, (uint32_t)n + 4);
    return n + 4;
}
size_t tap_pcapng_header(uint8_t *o, size_t cap)
{
    static const char app[] = "LakeShark TAP", hw[] = "LakeShark handheld (ESP32-P4)";
    if (cap < 400) return 0;
    size_t n = 8;
    n += w32(o + n, 0x1A2B3C4Du); n += w16(o + n, 1); n += w16(o + n, 0);
    memset(o + n, 0xFF, 8); n += 8;                             /* section length unknown */
    n += option(o + n, 2, hw, sizeof(hw) - 1);                  /* shb_hardware */
    n += option(o + n, 4, app, sizeof(app) - 1);                /* shb_userappl */
    n += w32(o + n, 0);
    w32(o, 0x0A0D0D0Au); w32(o + 4, (uint32_t)n + 4); w32(o + n, (uint32_t)n + 4);
    n += 4;
    n += idb(o + n, 127, "wifi0", "802.11 monitor, radiotap");
    n += idb(o + n, 256, "ble0", "Bluetooth LE advertising, passive scanner");
    n += idb(o + n, 270, "lora0", "LoRa, LoRaTap");
    return n;
}
/* The advertising channels' CRC-24 over the PDU (header and payload), as it
   goes on air: start value 0x555555, bits least significant first. */
static uint32_t ble_crc(const uint8_t *d, size_t n)
{
    uint32_t state = 0xAAAAAAu;         /* 0x555555, bit-reversed for this shift direction */
    for (size_t i = 0; i < n; i++) {
        uint8_t cur = d[i];
        for (int j = 0; j < 8; j++) {
            const uint32_t bit = (state ^ cur) & 1u;
            cur >>= 1;
            state >>= 1;
            if (bit) { state |= 1u << 23; state ^= 0x5A6000u; }
        }
    }
    return state;
}
size_t tap_pcapng_packet(const tap_pkt_t *p, const uint8_t *d, uint8_t *o, size_t cap, uint64_t epoch_us)
{
    if (p->radio == R_DEC || cap < (size_t)p->len + 96) return 0;
    /* an advertising PDU's length is one byte: the address and up to 249 more */
    size_t len = p->radio == R_BLE && p->len > 249 ? 249 : p->len;
    /* A directed advertisement is the advertiser and its target, nothing
       else. The controller's report leaves the target out: it is written as
       00:00:00:00:00:00 so the PDU is whole. */
    static const uint8_t no_target[6] = {0, 0, 0, 0, 0, 0};
    if (p->radio == R_BLE && (p->kind & 7) == LAP_BLE_ADV_DIRECT_IND && len != 6) { d = no_target; len = 6; }
    uint8_t *b = o + 28;                                        /* the packet data */
    size_t h = 0;
    if (p->radio == R_WIFI) {
        /* radiotap: flags, rate (0 when the radio gave none: HT and later), channel, antenna signal */
        const unsigned freq = p->ch == 14 ? 2484 : p->ch >= 1 && p->ch <= 13 ? 2407 + 5 * p->ch : 5000 + 5 * p->ch;
        b[0] = 0; b[1] = 0; w16(b + 2, 15);
        w32(b + 4, 0x2 | (p->rate ? 0x4 : 0) | 0x8 | 0x20);
        b[8] = 0; b[9] = (uint8_t)p->rate;
        w16(b + 10, freq); w16(b + 12, p->ch <= 14 ? 0x0080 : 0x0100);
        b[14] = (uint8_t)p->rssi;
        h = 15;
    } else if (p->radio == R_BLE) {
        static const uint8_t air[8] = {0, 1, 6, 2, 4, 5, 6, 7};  /* HCI report type to on-air PDU type */
        b[0] = p->ch == 38 ? 12 : p->ch == 39 ? 39 : 0;         /* RF channel; 0 is advertising channel 37 */
        b[1] = (uint8_t)p->rssi; b[2] = 0x80; b[3] = 0;
        w32(b + 4, 0x8E89BED6u);
        w16(b + 8, 0x0013);                                     /* dewhitened, signal valid, reference AA valid; CRC not checked */
        w32(b + 10, 0x8E89BED6u);
        b[14] = (uint8_t)(air[p->kind & 7] | (p->addr_type ? 0x40 : 0));
        b[15] = (uint8_t)(6 + len);
        memcpy(b + 16, p->addr, 6);
        h = 22;
    } else {
        /* LoRaTap v0 */
        int rs = p->rssi + 139;
        b[0] = 0; b[1] = 0; b[2] = 0; b[3] = 15;
        const uint32_t hz = p->freq_khz * 1000u;
        b[4] = (uint8_t)(hz >> 24); b[5] = (uint8_t)(hz >> 16); b[6] = (uint8_t)(hz >> 8); b[7] = (uint8_t)hz;
        b[8] = p->bw ? p->bw : 1; b[9] = p->sf;
        b[10] = (uint8_t)(rs < 0 ? 0 : rs > 255 ? 255 : rs); b[11] = b[10]; b[12] = b[10];
        b[13] = (uint8_t)(int8_t)(p->snr10 * 4 / 10);
        b[14] = p->sync;
        h = 15;
    }
    memcpy(b + h, d, len);
    size_t caplen = h + len, orig = h + (p->orig > len ? p->orig : len);
    if (p->radio == R_BLE) {
        /* The controller checks the CRC, reports only frames that pass and
           keeps the three bytes. They are worked out again here over the PDU
           as written, so the file holds a whole, self-consistent packet. */
        const uint32_t crc = ble_crc(b + 14, 2u + 6u + len);
        b[caplen] = (uint8_t)crc; b[caplen + 1] = (uint8_t)(crc >> 8); b[caplen + 2] = (uint8_t)(crc >> 16);
        caplen += 3; orig = caplen;
    }
    const size_t pad = (caplen + 3u) & ~(size_t)3;
    memset(b + caplen, 0, pad - caplen);
    const uint64_t ts = p->ts_us + epoch_us;
    const size_t total = 28 + pad + 4;
    w32(o, 6); w32(o + 4, (uint32_t)total);
    w32(o + 8, p->radio == R_WIFI ? 0 : p->radio == R_BLE ? 1 : 2);
    w32(o + 12, (uint32_t)(ts >> 32)); w32(o + 16, (uint32_t)ts);
    w32(o + 20, (uint32_t)caplen); w32(o + 24, (uint32_t)orig);
    w32(o + 28 + pad, (uint32_t)total);
    return total;
}

/* ---- reading ------------------------------------------------------------------ */
/* Radiotap: the fields up to the antenna signal, each aligned to its size. */
static bool radiotap(const uint8_t *b, size_t n, tap_pkt_t *p, size_t *hdr, bool *fcs)
{
    if (n < 8 || b[0]) return false;
    const size_t len = le16(b + 2);
    if (len < 8 || len > n) return false;
    size_t at = 4;
    const uint32_t present = le32(b + 4);
    for (uint32_t more = present; (more & 0x80000000u) && at + 8 <= len; ) { at += 4; more = le32(b + at); }
    at += 4;
#define FIELD(bit, size, align) ((present & (1u << (bit))) && ((at = (at + (align) - 1) & ~(size_t)((align) - 1)), at + (size) <= len))
    *fcs = false;
    if (FIELD(0, 8, 8)) at += 8;                                    /* TSFT */
    if (FIELD(1, 1, 1)) { *fcs = b[at] & 0x10; at += 1; }
    if (FIELD(2, 1, 1)) { p->rate = b[at]; at += 1; }
    if (FIELD(3, 4, 2)) {
        const unsigned f = le16(b + at);
        p->ch = (uint8_t)(f == 2484 ? 14 : f >= 2412 && f <= 2472 ? (f - 2407) / 5 : f >= 5955 ? (f - 5950) / 5 : f >= 5000 ? (f - 5000) / 5 : 0);
        at += 4;
    }
    if (FIELD(4, 2, 1)) at += 2;                                    /* FHSS */
    if (FIELD(5, 1, 1)) p->rssi = (int8_t)b[at];
#undef FIELD
    *hdr = len;
    return true;
}
/* One frame of link type `link` into p; false when TAP does not read it. */
static bool frame(unsigned link, const uint8_t *b, size_t cap, size_t orig, tap_pkt_t *p, const uint8_t **data)
{
    size_t h = 0, tail = 0;
    if (link == 127 || link == 105) {
        bool fcs = false;
        p->radio = R_WIFI;
        if (link == 127 && !radiotap(b, cap, p, &h, &fcs)) return false;
        if (fcs) { tail = cap == orig && cap >= h + 4 ? 4 : 0; orig = orig >= h + 4 ? orig - 4 : orig; }
        const unsigned type = cap > h ? (b[h] >> 2) & 3 : 0;
        p->kind = (uint8_t)(type == 0 ? LAP_WIFI_MGMT : type == 1 ? LAP_WIFI_CTRL : LAP_WIFI_DATA);
    } else if (link == 256) {
        static const uint8_t hci[8] = {0, 1, 3, 9, 4, 9, 2, 9};    /* on-air PDU type to the controller's report type */
        if (cap < 10 + 4 + 2 + 6 + 3 || le32(b + 10) != 0x8E89BED6u) return false;
        const unsigned type = hci[b[14] & 7];
        if (type == 9 || (b[14] & 0x0F) > 7) return false;         /* scan and connect requests: not advertising reports */
        const size_t plen = b[15];
        if (plen < 6 || 16 + plen > cap) return false;
        p->radio = R_BLE;
        p->ch = b[0] == 0 ? 37 : b[0] == 12 ? 38 : b[0] == 39 ? 39 : 0;
        p->rssi = (le16(b + 8) & 2) ? (int8_t)b[1] : 0;
        p->kind = (uint8_t)type;
        p->addr_type = (b[14] & 0x40) ? 1 : 0;
        memcpy(p->addr, b + 16, 6);
        *data = b + 22;
        p->len = p->orig = (uint16_t)(plen - 6);
        return true;
    } else if (link == 270) {
        if (cap < 15) return false;
        h = (size_t)b[2] << 8 | b[3];
        if (h < 15 || h > cap) return false;
        p->radio = R_LORA;
        p->freq_khz = (((uint32_t)b[4] << 24) | ((uint32_t)b[5] << 16) | ((uint32_t)b[6] << 8) | b[7]) / 1000u;
        p->bw = b[8]; p->sf = b[9];
        p->rssi = (int8_t)((int)b[10] - 139 < -128 ? -128 : (int)b[10] - 139);
        p->snr10 = (int16_t)((int8_t)b[13] * 10 / 4);
        p->sync = b[14];
    } else return false;
    if (cap < h + tail) return false;
    size_t n = cap - h - tail;
    orig = orig > h ? orig - h : n;
    if (n > LAP_CAPTURE_SNAP_MAX) n = LAP_CAPTURE_SNAP_MAX;
    *data = b + h;
    p->len = (uint16_t)n;
    p->orig = (uint16_t)(orig > 0xFFFFu ? 0xFFFFu : orig < n ? n : orig);
    return true;
}
static int bad(tap_reader_t *r, const char *why) { snprintf(r->err, sizeof(r->err), "%s", why); return -2; }
int tap_read_block(tap_reader_t *r, const uint8_t *b, size_t n, tap_pkt_t *p, const uint8_t **data, size_t *used)
{
    *used = 0;
    memset(p, 0, sizeof(*p));
    if (!r->fmt) {
        if (n < 24) return n ? -1 : bad(r, "the file is empty");
        const uint32_t magic = le32(b);
        if (magic == 0xA1B2C3D4u || magic == 0xA1B23C4Du) {
            r->fmt = 2; r->nif = 1; r->nsec = magic == 0xA1B23C4Du;
            r->link[0] = (uint16_t)le32(b + 20);
            *used = 24;
            return 0;
        }
        if (magic == 0xD4C3B2A1u || magic == 0x4D3CB2A1u) return bad(r, "a big-endian pcap: not read here");
        if (magic != 0x0A0D0D0Au) return bad(r, "not a pcapng or pcap file");
        if (le32(b + 8) != 0x1A2B3C4Du) return bad(r, le32(b + 8) == 0x4D3C2B1Au ? "a big-endian pcapng: not read here" : "not a pcapng file");
        r->fmt = 1;
    }
    if (r->fmt == 2) {
        if (n < 16) return -1;
        const size_t cap = le32(b + 8), orig = le32(b + 12);
        if (cap > 0x40000u) return bad(r, "the file is damaged (a record too long)");
        *used = 16 + cap;
        if (n < 16 + cap) return -1;
        p->ts_us = (uint64_t)le32(b) * 1000000u + (r->nsec ? le32(b + 4) / 1000u : le32(b + 4));
        if (!frame(r->link[0], b + 16, cap, orig, p, data)) { r->other++; return 0; }
        r->frames++;
        return 1;
    }
    if (n < 12) return -1;
    const uint32_t kind = le32(b), total = le32(b + 4);
    if (total < 12 || (total & 3)) return bad(r, "the file is damaged (a bad block length)");
    *used = total;
    if (n < total) return -1;
    if (kind == 0x0A0D0D0Au) {                  /* a new section: its interfaces start again */
        if (le32(b + 8) != 0x1A2B3C4Du) return bad(r, "a big-endian section: not read here");
        r->nif = 0;
    } else if (kind == 1 && total >= 20) {
        if (r->nif < 16) {
            uint32_t div = 1000000u;
            for (size_t at = 16; at + 4 <= total - 4; ) {       /* if_tsresol */
                const unsigned code = le16(b + at), len = le16(b + at + 2);
                if (!code) break;
                if (code == 9 && len == 1 && at + 5 <= total - 4) {
                    const unsigned v = b[at + 4];
                    div = 1;
                    if (v & 0x80) div = (v & 0x7F) < 32 ? 1u << (v & 0x7F) : 0; else for (unsigned i = 0; i < v && i < 9; i++) div *= 10u;
                }
                at += 4 + ((len + 3u) & ~3u);
            }
            r->link[r->nif] = (uint16_t)le16(b + 8);
            r->tsdiv[r->nif] = div ? div : 1000000u;
        }
        if (r->nif < 255) r->nif++;
    } else if (kind == 6 && total >= 32) {
        const uint32_t ifc = le32(b + 8), cap = le32(b + 20), orig = le32(b + 24);
        if (ifc >= r->nif || ifc >= 16 || 28 + (uint64_t)cap + 4 > total) { r->other++; return 0; }
        const uint64_t t = (uint64_t)le32(b + 12) << 32 | le32(b + 16);
        const uint32_t div = r->tsdiv[ifc];
        p->ts_us = div == 1000000u ? t : t / div * 1000000u + t % div * 1000000u / div;
        if (!frame(r->link[ifc], b + 28, cap, orig, p, data)) { r->other++; return 0; }
        r->frames++;
        return 1;
    } else if (kind == 3) r->other++;           /* a simple packet block: no interface to read it by */
    return 0;
}
