/* 802.11 frame reading; see lap_dot11.h. Read only, no allocation. */
#include "lap_dot11.h"
#include <stdio.h>
#include <string.h>

static unsigned le16(const uint8_t *p) { return (unsigned)p[0] | (unsigned)p[1] << 8; }
static unsigned be16(const uint8_t *p) { return (unsigned)p[0] << 8 | p[1]; }

/* One cipher/AKM suite selector of the RSN element (00-0F-AC) to AKM bits. */
static uint8_t akm_bit(const uint8_t *s)
{
    if (s[0] != 0x00 || s[1] != 0x0F || s[2] != 0xAC) return LAP_DOT11_AKM_OTHER;
    switch (s[3]) {
    case 1: case 5: case 11: case 12: case 13: return LAP_DOT11_AKM_8021X;
    case 3: return LAP_DOT11_AKM_8021X | LAP_DOT11_AKM_FT;
    case 2: case 6: return LAP_DOT11_AKM_PSK;
    case 4: return LAP_DOT11_AKM_PSK | LAP_DOT11_AKM_FT;
    case 8: case 24: return LAP_DOT11_AKM_SAE;
    case 9: case 25: return LAP_DOT11_AKM_SAE | LAP_DOT11_AKM_FT;
    case 18: return LAP_DOT11_AKM_OWE;
    default: return LAP_DOT11_AKM_OTHER;
    }
}
static void rsn(const uint8_t *v, int len, lap_dot11_t *o)
{
    o->rsn = true;
    if (len < 8) return;                                /* version + group suite only: AKM defaults to 802.1X */
    int at = 6;
    if (at + 2 > len) return;
    const int pairwise = (int)le16(v + at);
    at += 2 + 4 * pairwise;
    if (at + 2 > len) return;
    const int akms = (int)le16(v + at);
    at += 2;
    for (int i = 0; i < akms && at + 4 <= len; i++, at += 4) o->akm |= akm_bit(v + at);
    at += 2;                                            /* RSN capabilities */
    if (at + 2 <= len && le16(v + at) > 0 && at + 2 + 16 <= len) o->rsn_pmkid = true;
}
static void elements(const uint8_t *d, int n, int at, lap_dot11_t *o)
{
    while (at + 2 <= n) {
        const int id = d[at], len = d[at + 1];
        const uint8_t *v = d + at + 2;
        if (at + 2 + len > n) break;
        if (id == 0 && !o->ssid_present) {
            o->ssid_present = true;
            o->ssid_len = (uint8_t)(len > 32 ? 32 : len);
            bool zero = true;
            for (int i = 0; i < o->ssid_len; i++) {
                if (v[i]) zero = false;
                o->ssid[i] = v[i] >= 32 && v[i] < 127 ? (char)v[i] : '.';
            }
            o->ssid[o->ssid_len] = 0;
            o->ssid_hidden = zero;
            if (zero) o->ssid[0] = 0;
        } else if (id == 3 && len >= 1) o->channel = v[0];
        else if (id == 61 && len >= 1 && !o->channel) o->channel = v[0];
        else if (id == 48) rsn(v, len, o);
        else if (id == 221 && len >= 4 && v[0] == 0x00 && v[1] == 0x50 && v[2] == 0xF2 && v[3] == 1) {
            o->wpa = true;
            /* WPA1: version, group, pairwise list, AKM list (00-50-F2-1 802.1X, -2 PSK) */
            if (len >= 10) {
                int k = 10 + 4 * (int)le16(v + 8);
                if (k + 2 <= len) {
                    const int na = (int)le16(v + k);
                    for (int i = 0; i < na && k + 2 + 4 * i + 4 <= len; i++) {
                        const uint8_t t = v[k + 2 + 4 * i + 3];
                        if (!o->rsn) o->akm |= t == 1 ? LAP_DOT11_AKM_8021X : t == 2 ? LAP_DOT11_AKM_PSK : LAP_DOT11_AKM_OTHER;
                    }
                }
            }
        }
        at += 2 + len;
    }
}
static void security(lap_dot11_t *o)
{
    if (o->rsn) {
        const uint8_t a = o->akm ? o->akm : LAP_DOT11_AKM_8021X;
        if ((a & LAP_DOT11_AKM_SAE) && (a & LAP_DOT11_AKM_PSK)) o->security = LAP_DOT11_SEC_WPA2_WPA3;
        else if (a & LAP_DOT11_AKM_SAE) o->security = LAP_DOT11_SEC_WPA3;
        else if (a & LAP_DOT11_AKM_OWE) o->security = LAP_DOT11_SEC_OWE;
        else if ((a & LAP_DOT11_AKM_8021X) && !(a & LAP_DOT11_AKM_PSK)) o->security = LAP_DOT11_SEC_WPA2_EAP;
        else o->security = LAP_DOT11_SEC_WPA2;
    } else if (o->wpa) o->security = LAP_DOT11_SEC_WPA;
    else o->security = (o->capab & 0x10) ? LAP_DOT11_SEC_WEP : LAP_DOT11_SEC_OPEN;
}
static void eapol(const uint8_t *d, int n, int at, lap_dot11_t *o)
{
    if (at + 8 > n || d[at] != 0xAA || d[at + 1] != 0xAA || d[at + 2] != 0x03) return;
    o->ethertype = (uint16_t)be16(d + at + 6);
    if (o->ethertype != 0x888E) return;
    at += 8;
    if (at + 4 > n) return;
    o->eapol = true;
    o->eapol_type = d[at + 1];
    if (o->eapol_type != 3 || at + 7 > n) return;
    const unsigned ki = be16(d + at + 5);
    const bool ack = ki & 0x80, mic = ki & 0x100, secure = ki & 0x200, install = ki & 0x40;
    if (!(ki & 0x08)) o->eapol_msg = 0;                 /* group key handshake */
    else o->eapol_msg = (uint8_t)(ack && !mic ? 1 : ack && mic && install ? 3 : mic && !ack && !secure ? 2 : mic && !ack ? 4 : 0);
    /* Key data after a 16-byte MIC: descriptor 1, info 2, length 2, replay 8,
       nonce 32, IV 16, RSC 8, reserved 8, MIC 16, then its length. */
    const int kdl_at = at + 4 + 1 + 2 + 2 + 8 + 32 + 16 + 8 + 8 + 16;
    if (kdl_at + 2 > n) return;
    const int kdl = (int)be16(d + kdl_at);
    int k = kdl_at + 2;
    const int end = k + kdl < n ? k + kdl : n;
    while (k + 2 <= end) {
        const int t = d[k], l = d[k + 1];
        if (k + 2 + l > end) break;
        if (t == 0xDD && l >= 20 && d[k + 2] == 0x00 && d[k + 3] == 0x0F && d[k + 4] == 0xAC && d[k + 5] == 4) {
            bool any = false;
            for (int i = 0; i < 16; i++) any |= d[k + 6 + i] != 0;
            if (any) o->eapol_pmkid = true;              /* noted, never copied */
        }
        k += 2 + l;
    }
}

bool lap_dot11_parse(const uint8_t *d, size_t len, lap_dot11_t *o)
{
    memset(o, 0, sizeof(*o));
    if (!d || len < 10) return false;
    const int n = len > 4096 ? 4096 : (int)len;
    const unsigned fc0 = d[0], fc1 = d[1];
    if (fc0 & 3) return false;                          /* protocol version 0 only */
    o->type = (uint8_t)((fc0 >> 2) & 3);
    o->subtype = (uint8_t)(fc0 >> 4);
    o->to_ds = fc1 & 1; o->from_ds = fc1 & 2; o->retry = fc1 & 8; o->protect = fc1 & 0x40;
    o->duration = (uint16_t)le16(d + 2);
    memcpy(o->a1, d + 4, 6);
    memcpy(o->ra, d + 4, 6);
    o->naddr = 1;
    o->hdr_len = 10;
    o->ok = true;
    if (o->type == LAP_DOT11_CTRL) {
        const unsigned s = o->subtype;
        if (n >= 16 && (s == 4 || s == 5 || s == 8 || s == 9 || s == 10 || s == 11 || s == 14 || s == 15)) {
            memcpy(o->a2, d + 10, 6); memcpy(o->ta, d + 10, 6);
            o->have_ta = true; o->naddr = 2; o->hdr_len = 16;
        }
        return true;
    }
    if (n < 24) return true;
    memcpy(o->a2, d + 10, 6); memcpy(o->a3, d + 16, 6);
    o->naddr = 3; o->hdr_len = 24;
    o->seq = (uint16_t)(le16(d + 22) >> 4);
    memcpy(o->ta, o->a2, 6); o->have_ta = true;
    if (o->type == LAP_DOT11_MGMT) {
        memcpy(o->bssid, o->a3, 6); o->have_bssid = true;
        const int s = o->subtype;
        int at = 24, ies = -1;
        if (s == LAP_DOT11_BEACON || s == LAP_DOT11_PROBE_RESP) {
            if (at + 12 <= n) { o->interval = (uint16_t)le16(d + at + 8); o->capab = (uint16_t)le16(d + at + 10); ies = at + 12; }
        } else if (s == LAP_DOT11_PROBE_REQ) ies = at;
        else if (s == LAP_DOT11_ASSOC_REQ) { if (at + 4 <= n) { o->capab = (uint16_t)le16(d + at); ies = at + 4; } }
        else if (s == LAP_DOT11_REASSOC_REQ) { if (at + 10 <= n) { o->capab = (uint16_t)le16(d + at); ies = at + 10; } }
        else if (s == LAP_DOT11_ASSOC_RESP || s == LAP_DOT11_REASSOC_RESP) {
            if (at + 6 <= n) { o->capab = (uint16_t)le16(d + at); o->status = (uint16_t)le16(d + at + 2); ies = at + 6; }
        } else if (s == LAP_DOT11_AUTH) {
            if (at + 6 <= n && !o->protect) {
                o->auth_alg = (uint16_t)le16(d + at); o->auth_seq = (uint16_t)le16(d + at + 2); o->status = (uint16_t)le16(d + at + 4);
            }
        } else if (s == LAP_DOT11_DEAUTH || s == LAP_DOT11_DISASSOC) {
            if (at + 2 <= n && !o->protect) o->reason = (uint16_t)le16(d + at);   /* protected (802.11w): sealed */
        }
        if (ies >= 0 && !o->protect) {
            elements(d, n, ies, o);
            if (s == LAP_DOT11_BEACON || s == LAP_DOT11_PROBE_RESP || s == LAP_DOT11_ASSOC_REQ || s == LAP_DOT11_REASSOC_REQ) security(o);
        }
        return true;
    }
    if (o->type != LAP_DOT11_DATA) return true;
    /* Data: the address roles follow toDS/fromDS. */
    if (!o->to_ds && !o->from_ds) { memcpy(o->bssid, o->a3, 6); o->have_bssid = true; }
    else if (o->to_ds && !o->from_ds) { memcpy(o->bssid, o->a1, 6); o->have_bssid = true; }
    else if (!o->to_ds && o->from_ds) { memcpy(o->bssid, o->a2, 6); o->have_bssid = true; }
    else if (n >= 30) { memcpy(o->a4, d + 24, 6); o->naddr = 4; o->hdr_len = 30; }
    else return true;
    if (o->subtype & 8) {
        o->qos = true;
        o->hdr_len = (uint8_t)(o->hdr_len + 2);
        if (fc1 & 0x80) o->hdr_len = (uint8_t)(o->hdr_len + 4);   /* HT control */
    }
    if (!o->protect && !(o->subtype & 4) && o->hdr_len < n) eapol(d, n, o->hdr_len, o);   /* subtype bit 2: no payload */
    return true;
}

const char *lap_dot11_subtype_name(int type, int sub)
{
    static const char *const mgmt[16] = {"Association request", "Association response", "Reassociation request",
        "Reassociation response", "Probe request", "Probe response", "Timing advertisement", "Reserved", "Beacon",
        "ATIM", "Disassociation", "Authentication", "Deauthentication", "Action", "Action no ack", "Reserved"};
    static const char *const ctrl[16] = {"Control", "Control", "Trigger", "TACK", "Beamforming report poll",
        "NDP announcement", "Control extension", "Control wrapper", "Block ack request", "Block ack", "PS-Poll",
        "RTS", "CTS", "ACK", "CF-End", "CF-End + CF-Ack"};
    if (type == LAP_DOT11_MGMT) return mgmt[sub & 15];
    if (type == LAP_DOT11_CTRL) return ctrl[sub & 15];
    if (type == LAP_DOT11_DATA) return (sub & 4) ? "Null data" : (sub & 8) ? "QoS data" : "Data";
    return "Extension";
}
const char *lap_dot11_sec_name(int s)
{
    static const char *const n[LAP_DOT11_SEC_COUNT] = {"?", "open", "WEP", "WPA", "WPA2", "WPA2-EAP", "WPA2/3", "WPA3", "OWE"};
    return s >= 0 && s < LAP_DOT11_SEC_COUNT ? n[s] : "?";
}
const char *lap_dot11_reason_name(unsigned r)
{
    switch (r) {
    case 0: return "sealed";
    case 1: return "unspecified";
    case 2: return "auth no longer valid";
    case 3: return "station leaving";
    case 4: return "inactivity";
    case 5: return "AP full";
    case 6: return "class 2 from unauth";
    case 7: return "class 3 from unassoc";
    case 8: return "station left BSS";
    case 9: return "not authenticated";
    case 14: return "MIC failure";
    case 15: return "4-way timeout";
    case 23: return "802.1X failed";
    default: return "other";
    }
}
void lap_dot11_mac(char *out, size_t cap, const uint8_t a[6])
{
    snprintf(out, cap, "%02x:%02x:%02x:%02x:%02x:%02x", a[0], a[1], a[2], a[3], a[4], a[5]);
}
bool lap_dot11_is_broadcast(const uint8_t a[6])
{
    return (a[0] & a[1] & a[2] & a[3] & a[4] & a[5]) == 0xFF;
}
bool lap_dot11_is_multicast(const uint8_t a[6]) { return a[0] & 1; }
bool lap_dot11_is_random(const uint8_t a[6]) { return (a[0] & 0x02) && !(a[0] & 1); }
unsigned lap_dot11_channel_mhz(unsigned ch)
{
    if (ch >= 1 && ch <= 13) return 2407 + 5 * ch;
    if (ch == 14) return 2484;
    if (ch >= 32 && ch <= 177) return 5000 + 5 * ch;
    return 0;
}
unsigned lap_dot11_mhz_channel(unsigned mhz)
{
    if (mhz == 2484) return 14;
    if (mhz >= 2412 && mhz <= 2472 && (mhz - 2407) % 5 == 0) return (mhz - 2407) / 5;
    if (mhz >= 5160 && mhz <= 5885 && mhz % 5 == 0) return (mhz - 5000) / 5;
    return 0;
}
uint32_t lap_dot11_airtime_us(unsigned bytes, unsigned rate, unsigned channel)
{
    const uint32_t bits = 8u * (bytes + 4u);            /* with the FCS the radio stripped */
    if (channel && channel <= 14 && (rate == 2 || rate == 4 || rate == 11 || rate == 22))
        return 192u + (bits * 2u + rate - 1u) / rate;   /* DSSS/CCK, long preamble */
    if (!rate) rate = 12;                               /* unknown (HT and later): 6 Mb/s */
    const uint32_t per = 2u * rate;                     /* data bits per 4 us OFDM symbol */
    return 20u + 4u * ((16u + bits + 6u + per - 1u) / per);
}
