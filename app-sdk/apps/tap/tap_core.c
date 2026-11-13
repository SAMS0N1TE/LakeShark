/* TAP: dissectors, display filters, statistics and the pcapng writer.
   Nothing here draws or touches a radio. Encrypted payloads are named,
   never opened. */
#include "tap.h"

const char *const tap_leaf_name[L_COUNT] = {"Beacon", "Probe", "Management", "Control", "Data", "EAPOL",
    "Advertising", "iBeacon", "Eddystone", "Scan response", "MeshCore", "Meshtastic", "LoRa raw", "Decodes"};
const uint8_t tap_leaf_radio[L_COUNT] = {R_WIFI, R_WIFI, R_WIFI, R_WIFI, R_WIFI, R_WIFI, R_BLE, R_BLE, R_BLE, R_BLE,
    R_LORA, R_LORA, R_LORA, R_DEC};
const char *const tap_radio_name[R_COUNT] = {"802.11", "Bluetooth LE", "LoRa", "Decodes"};

static void line(tap_dis_t *o, int depth, int off, int len, const char *fmt, ...)
{
    if (!o->lines || o->nlines >= o->cap) return;
    tap_line_t *l = &o->lines[o->nlines++];
    l->depth = (uint8_t)depth; l->off = (uint16_t)off; l->len = (uint16_t)len;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(l->text, sizeof(l->text), fmt, ap);
    va_end(ap);
}
static void mac(char *out, size_t cap, const uint8_t *a)
{
    static const uint8_t bc[6] = {255, 255, 255, 255, 255, 255};
    if (!memcmp(a, bc, 6)) snprintf(out, cap, "Broadcast");
    else snprintf(out, cap, "%02x:%02x:%02x:%02x:%02x:%02x", a[0], a[1], a[2], a[3], a[4], a[5]);
}
static unsigned be16(const uint8_t *p) { return (unsigned)p[0] << 8 | p[1]; }
static unsigned le16(const uint8_t *p) { return (unsigned)p[1] << 8 | p[0]; }
static uint32_t le32(const uint8_t *p) { return (uint32_t)p[3] << 24 | (uint32_t)p[2] << 16 | (uint32_t)p[1] << 8 | p[0]; }
/* Printable text out of n bytes; anything else becomes '.'. */
static void text(char *out, size_t cap, const uint8_t *p, size_t n)
{
    size_t i = 0;
    for (; i < n && i + 1 < cap; i++) out[i] = p[i] >= 32 && p[i] < 127 ? (char)p[i] : '.';
    out[i] = 0;
}
static void hex(char *out, size_t cap, const uint8_t *p, size_t n)
{
    size_t at = 0;
    out[0] = 0;
    for (size_t i = 0; i < n && at + 3 < cap; i++) at += (size_t)snprintf(out + at, cap - at, "%02x", p[i]);
    if (n * 2 >= cap && cap > 3) memcpy(out + cap - 3, "..", 3);
}

/* ---- 802.11 ------------------------------------------------------------- */
static const char *mgmt_name(unsigned sub)
{
    static const char *const n[16] = {"Association request", "Association response", "Reassociation request",
        "Reassociation response", "Probe request", "Probe response", "Timing advertisement", "Reserved", "Beacon",
        "ATIM", "Disassociation", "Authentication", "Deauthentication", "Action", "Action no ack", "Reserved"};
    return n[sub & 15];
}
static const char *ctrl_name(unsigned sub)
{
    static const char *const n[16] = {"Control", "Control", "Trigger", "TACK", "Beamforming report poll",
        "NDP announcement", "Control extension", "Control wrapper", "Block ack request", "Block ack", "PS-Poll",
        "RTS", "CTS", "ACK", "CF-End", "CF-End + CF-Ack"};
    return n[sub & 15];
}
static const char *suite(const uint8_t *s, bool akm)
{
    if (s[0] != 0x00 || s[1] != 0x0F || s[2] != 0xAC) return "vendor";
    if (akm) switch (s[3]) { case 1: return "802.1X"; case 2: return "PSK"; case 3: return "FT-802.1X";
        case 4: return "FT-PSK"; case 6: return "PSK-SHA256"; case 8: return "SAE"; case 9: return "FT-SAE";
        case 18: return "OWE"; default: return "AKM?"; }
    switch (s[3]) { case 1: return "WEP-40"; case 2: return "TKIP"; case 4: return "CCMP"; case 5: return "WEP-104";
        case 8: return "GCMP"; case 9: return "GCMP-256"; case 10: return "CCMP-256"; default: return "cipher?"; }
}
/* Tagged parameters of a management frame from `at` on. */
static void wifi_tags(const uint8_t *d, int n, int at, tap_dis_t *o, unsigned capab, char *sec, size_t seccap)
{
    bool rsn = false, wpa = false;
    int channel = 0;
    line(o, 1, at, n - at, "Tagged parameters (%d bytes)", n - at);
    while (at + 2 <= n) {
        const int id = d[at], len = d[at + 1];
        const uint8_t *v = d + at + 2;
        if (at + 2 + len > n) { line(o, 2, at, n - at, "Tag %d: cut short", id); break; }
        switch (id) {
        case 0:
            if (len && len <= 32 && v[0]) text(o->name, sizeof(o->name), v, (size_t)len);
            line(o, 2, at, len + 2, "SSID: %s", len && v[0] ? o->name : "<hidden>");
            break;
        case 1: case 50: {
            char r[60]; size_t w = 0; r[0] = 0;
            for (int i = 0; i < len && w + 8 < sizeof(r); i++)
                w += (size_t)snprintf(r + w, sizeof(r) - w, "%s%u%s", i ? " " : "", (v[i] & 127) / 2, (v[i] & 1) ? ".5" : "");
            line(o, 2, at, len + 2, "%s rates: %s Mb/s", id == 1 ? "Supported" : "Extended", r);
            break;
        }
        case 3: if (len >= 1) { channel = v[0]; line(o, 2, at, len + 2, "DS channel: %d", channel); } break;
        case 5: if (len >= 2) line(o, 2, at, len + 2, "TIM: DTIM count %u, period %u", v[0], v[1]); break;
        case 7: if (len >= 2) line(o, 2, at, len + 2, "Country: %c%c", v[0] >= 32 ? v[0] : '?', v[1] >= 32 ? v[1] : '?'); break;
        case 45: line(o, 2, at, len + 2, "HT capabilities (802.11n)"); break;
        case 61: line(o, 2, at, len + 2, "HT operation: primary channel %d", len ? v[0] : 0); break;
        case 191: line(o, 2, at, len + 2, "VHT capabilities (802.11ac)"); break;
        case 48:
            rsn = true;
            if (len >= 8) {
                int a = 10 + 4 * (int)le16(v + 6);
                const char *pair = le16(v + 6) && len >= 12 ? suite(v + 8, false) : "?";
                const char *akm = a + 4 <= len && le16(v + a - 2) ? suite(v + a, true) : "?";
                line(o, 2, at, len + 2, "RSN: group %s, pairwise %s, AKM %s", suite(v + 2, false), pair, akm);
                snprintf(sec, seccap, "%s", !strcmp(akm, "SAE") || !strcmp(akm, "FT-SAE") ? "WPA3" :
                         !strcmp(akm, "OWE") ? "OWE" : !strcmp(akm, "802.1X") ? "WPA2-EAP" : "WPA2");
            } else line(o, 2, at, len + 2, "RSN");
            break;
        case 221:
            if (len >= 4 && v[0] == 0x00 && v[1] == 0x50 && v[2] == 0xF2 && v[3] == 1) { wpa = true; line(o, 2, at, len + 2, "Vendor: Microsoft WPA"); }
            else if (len >= 4 && v[0] == 0x00 && v[1] == 0x50 && v[2] == 0xF2 && v[3] == 4) line(o, 2, at, len + 2, "Vendor: WPS");
            else if (len >= 4 && v[0] == 0x00 && v[1] == 0x50 && v[2] == 0xF2 && v[3] == 2) line(o, 2, at, len + 2, "Vendor: WMM");
            else if (len >= 3) line(o, 2, at, len + 2, "Vendor: OUI %02x:%02x:%02x (%d bytes)", v[0], v[1], v[2], len);
            break;
        case 255: line(o, 2, at, len + 2, len && v[0] == 35 ? "HE capabilities (802.11ax)" : "Extension element %d", len ? v[0] : 0); break;
        default: line(o, 2, at, len + 2, "Tag %d (%d bytes)", id, len); break;
        }
        at += 2 + len;
    }
    if (!rsn) snprintf(sec, seccap, "%s", wpa ? "WPA" : (capab & 0x10) ? "WEP" : "open");
    (void)channel;
}
static void wifi_llc(const uint8_t *d, int n, int at, tap_dis_t *o)
{
    if (at + 8 > n || d[at] != 0xAA || d[at + 1] != 0xAA || d[at + 2] != 3) {
        snprintf(o->info, sizeof(o->info), "Data, %d bytes", n - at);
        return;
    }
    const unsigned type = be16(d + at + 6);
    line(o, 1, at, 8, "LLC/SNAP: EtherType 0x%04x", type);
    at += 8;
    if (type == 0x888E && at + 4 <= n) {
        o->tags |= T_EAPOL; o->leaf = L_EAPOL;
        snprintf(o->proto, sizeof(o->proto), "EAPOL");
        const unsigned ptype = d[at + 1];
        line(o, 1, at, 4, "802.1X: version %u, type %u, length %u", d[at], ptype, be16(d + at + 2));
        if (ptype == 3 && at + 4 + 13 <= n) {
            const unsigned ki = be16(d + at + 5);
            const bool ackb = ki & 0x80, micb = ki & 0x100, secure = ki & 0x200, install = ki & 0x40;
            const int msg = ackb && !micb ? 1 : ackb && micb && install ? 3 : micb && !ackb && !secure ? 2 : micb && !ackb ? 4 : 0;
            line(o, 2, at + 4, 1, "Descriptor type: %u", d[at + 4]);
            line(o, 2, at + 5, 2, "Key information: 0x%04x%s%s%s%s%s", ki, (ki & 8) ? " pairwise" : " group",
                 install ? " install" : "", ackb ? " ack" : "", micb ? " MIC" : "", secure ? " secure" : "");
            line(o, 2, at + 9, 8, "Replay counter: %u", (unsigned)(be16(d + at + 13) << 16 | be16(d + at + 15)));
            if (msg) snprintf(o->info, sizeof(o->info), "Key (message %d of 4)", msg);
            else snprintf(o->info, sizeof(o->info), "Key (group or unknown, info 0x%04x)", ki);
            line(o, 2, at + 4, n - at - 4, "Key material is not shown or stored apart from the frame");
        } else
            snprintf(o->info, sizeof(o->info), "%s", ptype == 0 ? "EAP packet" : ptype == 1 ? "Start" : ptype == 2 ? "Logoff" : "802.1X");
    } else if (type == 0x0800 && at + 20 <= n) {
        snprintf(o->proto, sizeof(o->proto), "IPv4");
        snprintf(o->info, sizeof(o->info), "%u.%u.%u.%u > %u.%u.%u.%u proto %u", d[at + 12], d[at + 13], d[at + 14],
                 d[at + 15], d[at + 16], d[at + 17], d[at + 18], d[at + 19], d[at + 9]);
        line(o, 1, at, 20, "IPv4: %s", o->info);
    } else if (type == 0x0806) {
        snprintf(o->proto, sizeof(o->proto), "ARP");
        snprintf(o->info, sizeof(o->info), at + 8 <= n && be16(d + at + 6) == 2 ? "ARP reply" : "ARP request");
        line(o, 1, at, n - at, "%s", o->info);
    } else if (type == 0x86DD) {
        snprintf(o->proto, sizeof(o->proto), "IPv6");
        snprintf(o->info, sizeof(o->info), "IPv6, %d bytes", n - at);
        line(o, 1, at, n - at, "%s", o->info);
    } else snprintf(o->info, sizeof(o->info), "EtherType 0x%04x, %d bytes", type, n - at);
}
static void wifi(const tap_pkt_t *p, const uint8_t *d, tap_dis_t *o)
{
    const int n = p->len;
    o->tags = T_WIFI; o->leaf = L_MGMT; o->colour = LAP_WHITE;
    snprintf(o->proto, sizeof(o->proto), "802.11");
    if (p->rate) line(o, 0, 0, 0, "Radio: channel %u, %d dBm, %u%s Mb/s", p->ch, p->rssi, p->rate / 2, (p->rate & 1) ? ".5" : "");
    else line(o, 0, 0, 0, "Radio: channel %u, %d dBm", p->ch, p->rssi);
    if (n < 10) { snprintf(o->info, sizeof(o->info), "Short frame (%d bytes)", n); return; }
    const unsigned fc0 = d[0], fc1 = d[1], type = (fc0 >> 2) & 3, sub = fc0 >> 4;
    const bool tods = fc1 & 1, fromds = fc1 & 2, prot = fc1 & 0x40;
    line(o, 0, 0, n, "IEEE 802.11 %s, %d bytes%s", type == 0 ? "management" : type == 1 ? "control" : type == 2 ? "data" : "extension",
         p->orig, p->orig > p->len ? " (cut)" : "");
    line(o, 1, 0, 2, "Frame control: type %u subtype %u%s%s%s%s", type, sub, tods ? " toDS" : "", fromds ? " fromDS" : "",
         (fc1 & 8) ? " retry" : "", prot ? " protected" : "");
    line(o, 1, 2, 2, "Duration: %u", le16(d + 2));
    if (type == 1) {
        o->tags |= T_CTRL; o->leaf = L_CTRL; o->colour = LAP_BLACK | LAP_BRIGHT;
        mac(o->dst, sizeof(o->dst), d + 4);
        line(o, 1, 4, 6, "Receiver: %s", o->dst);
        if (n >= 16 && (sub == 8 || sub == 9 || sub == 10 || sub == 11 || sub == 14)) {
            mac(o->src, sizeof(o->src), d + 10);
            line(o, 1, 10, 6, "Transmitter: %s", o->src);
        }
        snprintf(o->info, sizeof(o->info), "%s", ctrl_name(sub));
        return;
    }
    if (n < 24) { snprintf(o->info, sizeof(o->info), "Short frame (%d bytes)", n); return; }
    const uint8_t *a1 = d + 4, *a2 = d + 10, *a3 = d + 16;
    const unsigned seq = le16(d + 22) >> 4;
    if (type == 0) {
        o->tags |= T_MGMT;
        mac(o->dst, sizeof(o->dst), a1); mac(o->src, sizeof(o->src), a2); mac(o->bssid, sizeof(o->bssid), a3);
        line(o, 1, 4, 6, "Destination: %s", o->dst);
        line(o, 1, 10, 6, "Source: %s", o->src);
        line(o, 1, 16, 6, "BSSID: %s", o->bssid);
        line(o, 1, 22, 2, "Sequence: %u", seq);
        char sec[12] = "";
        if (sub == 8 || sub == 5) {
            o->tags |= sub == 8 ? T_BEACON : T_PROBE; o->leaf = sub == 8 ? L_BEACON : L_PROBE;
            o->colour = sub == 8 ? LAP_CYAN : LAP_GREEN;
            if (n >= 36) {
                const unsigned capab = le16(d + 34);
                line(o, 1, 24, 12, "Fixed: interval %u TU, capabilities 0x%04x%s%s", le16(d + 32), capab,
                     (capab & 1) ? " ESS" : "", (capab & 0x10) ? " privacy" : "");
                wifi_tags(d, n, 36, o, capab, sec, sizeof(sec));
            }
            snprintf(o->info, sizeof(o->info), "%s, SSID=%s, %s", mgmt_name(sub), o->name[0] ? o->name : "<hidden>", sec);
        } else if (sub == 4) {
            o->tags |= T_PROBE; o->leaf = L_PROBE; o->colour = LAP_GREEN | LAP_BRIGHT;
            wifi_tags(d, n, 24, o, 0, sec, sizeof(sec));
            snprintf(o->info, sizeof(o->info), "Probe request, SSID=%s", o->name[0] ? o->name : "<any>");
        } else if (sub == 10 || sub == 12) {
            o->tags |= T_DEAUTH; o->colour = LAP_RED | LAP_BRIGHT;
            const unsigned reason = n >= 26 ? le16(d + 24) : 0;
            line(o, 1, 24, 2, "Reason code: %u", reason);
            snprintf(o->info, sizeof(o->info), "%s, reason %u", mgmt_name(sub), reason);
        } else if (sub == 11 && n >= 30) {
            o->tags |= T_AUTH; o->colour = LAP_YELLOW;
            line(o, 1, 24, 6, "Algorithm %u, sequence %u, status %u", le16(d + 24), le16(d + 26), le16(d + 28));
            snprintf(o->info, sizeof(o->info), "Authentication, %s, seq %u, status %u",
                     le16(d + 24) == 3 ? "SAE" : le16(d + 24) == 0 ? "open system" : "other", le16(d + 26), le16(d + 28));
        } else if (sub <= 3) {
            o->tags |= T_AUTH; o->colour = LAP_YELLOW;
            const int at = sub == 0 ? 28 : sub == 2 ? 34 : 30;
            if (n > at) wifi_tags(d, n, at, o, 0, sec, sizeof(sec));
            if (o->name[0]) snprintf(o->info, sizeof(o->info), "%s, SSID=%s", mgmt_name(sub), o->name);
            else snprintf(o->info, sizeof(o->info), "%s", mgmt_name(sub));
        } else if (sub == 13 || sub == 14) {
            line(o, 1, 24, 1, "Category: %u", n > 24 ? d[24] : 0);
            snprintf(o->info, sizeof(o->info), "%s, category %u%s", mgmt_name(sub), n > 24 ? d[24] : 0, prot ? " (protected)" : "");
        } else snprintf(o->info, sizeof(o->info), "%s", mgmt_name(sub));
        return;
    }
    /* data */
    o->tags |= T_DATA; o->leaf = L_DATA;
    const uint8_t *da = a1, *sa = a2, *bss = a3;
    int hdr = 24;
    if (tods && fromds) { da = a3; sa = n >= 30 ? d + 24 : a2; bss = a2; hdr = 30; }
    else if (tods) { bss = a1; sa = a2; da = a3; }
    else if (fromds) { da = a1; bss = a2; sa = a3; }
    if (sub & 8) hdr += 2;
    if ((sub & 8) && (fc1 & 0x80)) hdr += 4;
    mac(o->dst, sizeof(o->dst), da); mac(o->src, sizeof(o->src), sa); mac(o->bssid, sizeof(o->bssid), bss);
    line(o, 1, 4, 18, "Destination %s, source %s", o->dst, o->src);
    line(o, 1, 4, 18, "BSSID: %s", o->bssid);
    line(o, 1, 22, 2, "Sequence: %u", seq);
    snprintf(o->proto, sizeof(o->proto), (sub & 8) ? "802.11 QoS" : "802.11");
    if (sub & 4) { snprintf(o->info, sizeof(o->info), "%s", (sub & 8) ? "QoS null (no data)" : "Null (no data)"); o->colour = LAP_BLACK | LAP_BRIGHT; }
    else if (prot) {
        o->tags |= T_PROTECTED;
        snprintf(o->info, sizeof(o->info), "Protected data, %d bytes (not decrypted)", p->orig > hdr ? p->orig - hdr : 0);
        if (n > hdr) line(o, 1, hdr, n - hdr, "Encrypted payload: %d bytes, left as received", n - hdr);
    } else if (n > hdr) wifi_llc(d, n, hdr, o);
    else snprintf(o->info, sizeof(o->info), "Data");
    if (o->tags & T_EAPOL) o->colour = LAP_YELLOW | LAP_BRIGHT;
}

/* ---- Bluetooth LE advertising ------------------------------------------ */
static const char *company(unsigned id)
{
    static const struct { uint16_t id; const char *name; } k[] = {
        {0x004C, "Apple"}, {0x0006, "Microsoft"}, {0x0075, "Samsung"}, {0x00E0, "Google"}, {0x0059, "Nordic Semi"},
        {0x02E5, "Espressif"}, {0x0087, "Garmin"}, {0x038F, "Xiaomi"}, {0x009E, "Bose"}, {0x012D, "Sony"},
        {0x000F, "Broadcom"}, {0x000D, "Texas Instruments"}, {0x0002, "Intel"}, {0x0157, "Huami"}, {0x0171, "Amazon"},
        {0x01DA, "Logitech"}, {0x0499, "Ruuvi"}, {0x000A, "Qualcomm (CSR)"}, {0x001D, "Qualcomm"}, {0x0001, "Nokia"}};
    for (unsigned i = 0; i < sizeof(k) / sizeof(k[0]); i++) if (k[i].id == id) return k[i].name;
    return NULL;
}
static const char *uuid16(unsigned u)
{
    switch (u) {
    case 0x1800: return "Generic Access"; case 0x1801: return "Generic Attribute"; case 0x180A: return "Device Information";
    case 0x180D: return "Heart Rate"; case 0x180F: return "Battery"; case 0x1812: return "HID"; case 0x181A: return "Environmental Sensing";
    case 0xFEAA: return "Eddystone"; case 0xFD6F: return "Exposure Notification"; case 0xFE2C: return "Google Fast Pair";
    case 0xFE9F: return "Google"; case 0xFEED: return "Tile"; case 0xFD5A: return "Samsung SmartTag"; case 0xFFF6: return "Matter";
    case 0xFFFA: return "Remote ID (ASTM F3411)"; case 0x1816: return "Cycling Speed and Cadence"; case 0x1809: return "Health Thermometer";
    default: return NULL;
    }
}
static void eddystone(const uint8_t *v, int len, int at, tap_dis_t *o)
{
    o->tags |= T_EDDYSTONE; o->leaf = L_EDDYSTONE; o->colour = LAP_MAGENTA | LAP_BRIGHT;
    const unsigned frame = len ? v[0] : 0xFF;
    if (frame == 0x00 && len >= 18) {
        char ns[24], inst[16];
        hex(ns, sizeof(ns), v + 2, 10); hex(inst, sizeof(inst), v + 12, 6);
        line(o, 2, at, len, "Eddystone-UID: tx %d dBm, namespace %s, instance %s", (int8_t)v[1], ns, inst);
        snprintf(o->info, sizeof(o->info), "Eddystone-UID %s/%s", ns, inst);
    } else if (frame == 0x10 && len >= 3) {
        static const char *const scheme[4] = {"http://www.", "https://www.", "http://", "https://"};
        static const char *const tld[14] = {".com/", ".org/", ".edu/", ".net/", ".info/", ".biz/", ".gov/", ".com", ".org",
                                            ".edu", ".net", ".info", ".biz", ".gov"};
        char url[64]; size_t w = (size_t)snprintf(url, sizeof(url), "%s", v[2] < 4 ? scheme[v[2]] : "");
        for (int i = 3; i < len && w + 8 < sizeof(url); i++) {
            if (v[i] < 14) w += (size_t)snprintf(url + w, sizeof(url) - w, "%s", tld[v[i]]);
            else if (v[i] > 32 && v[i] < 127) url[w++] = (char)v[i], url[w] = 0;
        }
        line(o, 2, at, len, "Eddystone-URL: tx %d dBm, %s", (int8_t)v[1], url);
        snprintf(o->info, sizeof(o->info), "Eddystone-URL %s", url);
    } else if (frame == 0x20 && len >= 14) {
        line(o, 2, at, len, "Eddystone-TLM: %u mV, %d.%u C, %u adverts, up %u s", be16(v + 2), (int8_t)v[4], v[5] * 100u / 256u,
             (unsigned)(be16(v + 6) << 16 | be16(v + 8)), (unsigned)((be16(v + 10) << 16 | be16(v + 12)) / 10));
        snprintf(o->info, sizeof(o->info), "Eddystone-TLM %u mV", be16(v + 2));
    } else {
        line(o, 2, at, len, "Eddystone frame 0x%02x", frame);
        snprintf(o->info, sizeof(o->info), "Eddystone %s", frame == 0x30 ? "EID" : "frame");
    }
}
static void ble(const tap_pkt_t *p, const uint8_t *d, tap_dis_t *o)
{
    static const char *const pdu[8] = {"ADV_IND", "ADV_DIRECT_IND", "ADV_SCAN_IND", "ADV_NONCONN_IND", "SCAN_RSP", "PDU 5", "PDU 6", "ADV_EXT_IND"};
    const int n = p->len;
    const uint8_t *a = p->addr;
    o->tags = T_BLE; o->leaf = L_ADV; o->colour = LAP_BLUE | LAP_BRIGHT;
    if (p->kind == LAP_BLE_SCAN_RSP) { o->tags |= T_SCANRSP; o->leaf = L_SCANRSP; o->colour = LAP_BLUE; }
    snprintf(o->proto, sizeof(o->proto), "BLE ADV");
    snprintf(o->src, sizeof(o->src), "%02x:%02x:%02x:%02x:%02x:%02x", a[5], a[4], a[3], a[2], a[1], a[0]);
    snprintf(o->dst, sizeof(o->dst), "Broadcast");
    const char *kind = !p->addr_type ? "public" : (a[5] >> 6) == 3 ? "random static" : (a[5] >> 6) == 1 ? "random resolvable" : "random non-resolvable";
    if (p->ch) line(o, 0, 0, 0, "Radio: channel %u, %d dBm", p->ch, p->rssi);
    else line(o, 0, 0, 0, "Radio: %d dBm, advertising channel not reported", p->rssi);
    line(o, 0, 0, n, "Bluetooth LE %s, %d bytes of advertising data", pdu[p->kind & 7], n);
    line(o, 1, 0, 0, "Advertiser: %s (%s)", o->src, kind);
    char what[64] = "";
    for (int at = 0; at + 1 < n;) {
        const int len = d[at];
        if (!len) break;
        if (at + 1 + len > n) { line(o, 1, at, n - at, "AD structure cut short"); break; }
        const unsigned type = d[at + 1];
        const uint8_t *v = d + at + 2;
        const int vl = len - 1;
        switch (type) {
        case 0x01:
            line(o, 1, at, len + 1, "Flags: 0x%02x%s%s%s", vl ? v[0] : 0, vl && (v[0] & 1) ? " limited" : "", vl && (v[0] & 2) ? " general" : "",
                 vl && (v[0] & 4) ? " LE only" : "");
            break;
        case 0x02: case 0x03:
            for (int i = 0; i + 2 <= vl; i += 2) {
                const char *nm = uuid16(le16(v + i));
                line(o, 1, at + 2 + i, 2, "Service UUID16: 0x%04x%s%s", le16(v + i), nm ? " " : "", nm ? nm : "");
                if (nm && !what[0]) snprintf(what, sizeof(what), "%s", nm);
            }
            break;
        case 0x04: case 0x05:
            for (int i = 0; i + 4 <= vl; i += 4) line(o, 1, at + 2 + i, 4, "Service UUID32: 0x%08x", (unsigned)le32(v + i));
            break;
        case 0x06: case 0x07:
            for (int i = 0; i + 16 <= vl; i += 16)
                line(o, 1, at + 2 + i, 16, "Service UUID128: %08x-%04x-%04x-%04x-%04x%08x", (unsigned)le32(v + i + 12), le16(v + i + 10),
                     le16(v + i + 8), le16(v + i + 6), le16(v + i + 4), (unsigned)le32(v + i));
            break;
        case 0x08: case 0x09:
            text(o->name, sizeof(o->name), v, (size_t)(vl < 32 ? vl : 32));
            line(o, 1, at, len + 1, "%s name: %s", type == 9 ? "Complete" : "Short", o->name);
            break;
        case 0x0A: line(o, 1, at, len + 1, "Tx power: %d dBm", vl ? (int8_t)v[0] : 0); break;
        case 0x19: if (vl >= 2) line(o, 1, at, len + 1, "Appearance: 0x%04x", le16(v)); break;
        case 0x16:
            if (vl >= 2) {
                const unsigned u = le16(v);
                const char *nm = uuid16(u);
                line(o, 1, at, len + 1, "Service data: 0x%04x%s%s (%d bytes)", u, nm ? " " : "", nm ? nm : "", vl - 2);
                if (u == 0xFEAA) eddystone(v + 2, vl - 2, at + 4, o);
                else if (u == 0xFFFA) { o->tags |= T_RID; snprintf(what, sizeof(what), "Remote ID"); }
                else if (nm && !what[0]) snprintf(what, sizeof(what), "%s", nm);
            }
            break;
        case 0xFF:
            if (vl >= 2) {
                const unsigned id = le16(v);
                const char *nm = company(id);
                line(o, 1, at, len + 1, "Manufacturer data: 0x%04x %s (%d bytes)", id, nm ? nm : "unknown company", vl - 2);
                if (id == 0x004C && vl >= 25 && v[2] == 0x02 && v[3] == 0x15) {
                    o->tags |= T_IBEACON; o->leaf = L_IBEACON; o->colour = LAP_MAGENTA | LAP_BRIGHT;
                    char u[36];
                    hex(u, sizeof(u), v + 4, 16);
                    line(o, 2, at + 6, 16, "iBeacon UUID: %s", u);
                    line(o, 2, at + 22, 5, "Major %u, minor %u, measured power %d dBm", be16(v + 20), be16(v + 22), (int8_t)v[24]);
                    snprintf(o->info, sizeof(o->info), "iBeacon %.8s.. major %u minor %u", u, be16(v + 20), be16(v + 22));
                } else if (id == 0x004C && vl >= 3) {
                    const char *t = v[2] == 0x10 ? "Nearby Info" : v[2] == 0x0C ? "Handoff" : v[2] == 0x12 ? "Find My" : v[2] == 0x07 ? "AirPods" :
                                    v[2] == 0x09 ? "AirPlay" : v[2] == 0x0F ? "Nearby Action" : v[2] == 0x05 ? "AirDrop" : "message";
                    line(o, 2, at + 4, vl - 2, "Apple %s (type 0x%02x)", t, v[2]);
                    if (!what[0]) snprintf(what, sizeof(what), "Apple %s", t);
                } else if (!what[0]) snprintf(what, sizeof(what), "%s", nm ? nm : "manufacturer data");
            }
            break;
        default: line(o, 1, at, len + 1, "AD type 0x%02x (%d bytes)", type, vl); break;
        }
        at += len + 1;
    }
    if (!o->info[0]) {
        if (o->name[0] && what[0]) snprintf(o->info, sizeof(o->info), "%s \"%s\" %s", pdu[p->kind & 7], o->name, what);
        else if (o->name[0]) snprintf(o->info, sizeof(o->info), "%s \"%s\"", pdu[p->kind & 7], o->name);
        else if (what[0]) snprintf(o->info, sizeof(o->info), "%s %s", pdu[p->kind & 7], what);
        else snprintf(o->info, sizeof(o->info), "%s, %d bytes", pdu[p->kind & 7], n);
    }
}

/* ---- LoRa: MeshCore, Meshtastic, raw ------------------------------------- */
static void lora(const tap_pkt_t *p, const uint8_t *d, tap_dis_t *o)
{
    const int n = p->len;
    o->tags = T_LORA; o->leaf = L_LORA; o->colour = LAP_WHITE;
    snprintf(o->proto, sizeof(o->proto), "LoRa");
    line(o, 0, 0, 0, "Radio: %u.%03u MHz, SF%u, sync 0x%02x, %d dBm, SNR %d.%d dB", (unsigned)(p->freq_khz / 1000), (unsigned)(p->freq_khz % 1000),
         p->sf, p->sync, p->rssi, p->snr10 / 10, abs(p->snr10) % 10);
    if (p->sync == 0x2B && n >= 16) {
        o->tags |= T_MESHTASTIC; o->leaf = L_MESHTASTIC; o->colour = LAP_YELLOW;
        snprintf(o->proto, sizeof(o->proto), "Meshtastic");
        const uint32_t to = le32(d), from = le32(d + 4), id = le32(d + 8);
        const unsigned fl = d[12];
        snprintf(o->src, sizeof(o->src), "!%08x", (unsigned)from);
        if (to == 0xFFFFFFFFu) snprintf(o->dst, sizeof(o->dst), "Broadcast"); else snprintf(o->dst, sizeof(o->dst), "!%08x", (unsigned)to);
        line(o, 0, 0, n, "Meshtastic packet, %d bytes", n);
        line(o, 1, 0, 4, "To: %s", o->dst);
        line(o, 1, 4, 4, "From: %s", o->src);
        line(o, 1, 8, 4, "Packet id: 0x%08x", (unsigned)id);
        line(o, 1, 12, 1, "Flags: hop limit %u, hop start %u%s%s", fl & 7, fl >> 5, (fl & 8) ? ", want ack" : "", (fl & 16) ? ", via MQTT" : "");
        line(o, 1, 13, 1, "Channel hash: 0x%02x", d[13]);
        line(o, 1, 14, 2, "Next hop 0x%02x, relay 0x%02x", d[14], d[15]);
        if (n > 16) line(o, 1, 16, n - 16, "Encrypted payload: %d bytes, left as received", n - 16);
        snprintf(o->info, sizeof(o->info), "id %08x ch 0x%02x hops %u/%u, %d bytes payload", (unsigned)id, d[13], fl & 7, fl >> 5, n - 16);
        return;
    }
    if (n >= 2 && (d[0] >> 6) == 0) {
        static const char *const pt[16] = {"REQ", "RESPONSE", "TXT_MSG", "ACK", "ADVERT", "GRP_TXT", "GRP_DATA", "ANON_REQ", "PATH", "TRACE",
                                           "MULTIPART", "CONTROL", "type 12", "type 13", "type 14", "RAW_CUSTOM"};
        static const char *const rt[4] = {"transport flood", "flood", "direct", "transport direct"};
        const unsigned route = d[0] & 3, type = (d[0] >> 2) & 15;
        int at = 1 + ((route == 0 || route == 3) ? 4 : 0);
        if (at < n && d[at] <= 64 && at + 1 + d[at] <= n) {
            const int plen = d[at], pay = at + 1 + plen, pn = n - pay;
            o->tags |= T_MESHCORE; o->leaf = L_MESHCORE; o->colour = LAP_GREEN | LAP_BRIGHT;
            snprintf(o->proto, sizeof(o->proto), "MeshCore");
            line(o, 0, 0, n, "MeshCore packet, %d bytes", n);
            line(o, 1, 0, 1, "Header: %s, %s, version %u", rt[route], pt[type], d[0] >> 6);
            if (at > 1) line(o, 1, 1, 4, "Transport codes: %04x %04x", le16(d + 1), le16(d + 3));
            line(o, 1, at, 1 + plen, "Path: %d hop%s", plen, plen == 1 ? "" : "s");
            snprintf(o->dst, sizeof(o->dst), "Broadcast");
            snprintf(o->info, sizeof(o->info), "%s (%s), path %d, %d bytes", pt[type], rt[route], plen, pn);
            const uint8_t *v = d + pay;
            if (type == 4 && pn >= 100) {
                snprintf(o->src, sizeof(o->src), "%02x%02x%02x%02x", v[0], v[1], v[2], v[3]);
                line(o, 1, pay, 32, "Public key: %02x%02x%02x%02x...", v[0], v[1], v[2], v[3]);
                line(o, 1, pay + 32, 4, "Timestamp: %u", (unsigned)le32(v + 32));
                line(o, 1, pay + 36, 64, "Signature (64 bytes)");
                if (pn > 100) {
                    const unsigned fl = v[100];
                    static const char *const role[5] = {"node", "chat", "repeater", "room", "sensor"};
                    int q = 101;
                    line(o, 1, pay + 100, 1, "Flags: 0x%02x %s", fl, (fl & 15) < 5 ? role[fl & 15] : "node");
                    if ((fl & 0x10) && q + 8 <= pn) {
                        const int32_t la = (int32_t)le32(v + q), lo = (int32_t)le32(v + q + 4);
                        line(o, 1, pay + q, 8, "Position: %d.%06d, %d.%06d", (int)(la / 1000000), abs((int)(la % 1000000)), (int)(lo / 1000000), abs((int)(lo % 1000000)));
                        q += 8;
                    }
                    if (fl & 0x20) q += 2;
                    if (fl & 0x40) q += 2;
                    if ((fl & 0x80) && q < pn) {
                        text(o->name, sizeof(o->name), v + q, (size_t)(pn - q < 32 ? pn - q : 32));
                        line(o, 1, pay + q, pn - q, "Name: %s", o->name);
                        snprintf(o->info, sizeof(o->info), "ADVERT \"%s\" %s, path %d", o->name, (fl & 15) < 5 ? role[fl & 15] : "node", plen);
                    }
                }
            } else if ((type <= 2 || type == 8) && pn >= 4) {
                snprintf(o->dst, sizeof(o->dst), "%02x", v[0]);
                snprintf(o->src, sizeof(o->src), "%02x", v[1]);
                line(o, 1, pay, 1, "Destination hash: %02x", v[0]);
                line(o, 1, pay + 1, 1, "Source hash: %02x", v[1]);
                line(o, 1, pay + 2, 2, "MAC: %02x%02x", v[2], v[3]);
                line(o, 1, pay + 4, pn - 4, "Encrypted payload: %d bytes, left as received", pn - 4);
            } else if ((type == 5 || type == 6) && pn >= 3) {
                snprintf(o->dst, sizeof(o->dst), "chan %02x", v[0]);
                line(o, 1, pay, 1, "Channel hash: %02x", v[0]);
                line(o, 1, pay + 1, 2, "MAC: %02x%02x", v[1], v[2]);
                line(o, 1, pay + 3, pn - 3, "Encrypted payload: %d bytes, left as received", pn - 3);
            } else if (pn > 0) line(o, 1, pay, pn, "Payload: %d bytes", pn);
            if (!o->src[0] && plen) snprintf(o->src, sizeof(o->src), "via %02x", d[at + plen]);
            return;
        }
    }
    line(o, 0, 0, n, "LoRa payload, %d bytes, format not recognised", n);
    snprintf(o->info, sizeof(o->info), "%d bytes, SF%u sync 0x%02x", n, p->sf, p->sync);
}

/* ---- the board's own decodes: text records, not frames ------------------- */
static void decode(const tap_pkt_t *p, const uint8_t *d, tap_dis_t *o)
{
    static const char *const kinds[13] = {"PAGE", "APRS", "AIS", "ADS-B", "MESH", "SAME", "ACARS", "SUB-GHZ", "LORA", "TEXT", "P25", "REC", "DETECT"};
    o->tags = T_DECODE; o->leaf = L_DECODE; o->colour = LAP_MAGENTA;
    snprintf(o->proto, sizeof(o->proto), "%s", p->kind < 13 ? kinds[p->kind] : "DECODE");
    const size_t fl = strnlen((const char *)d, p->len < 24 ? p->len : 24);
    text(o->src, sizeof(o->src), d, fl);
    if (p->freq_khz) snprintf(o->dst, sizeof(o->dst), "%u", (unsigned)p->freq_khz);
    if (p->len > 24) text(o->info, sizeof(o->info), d + 24, (size_t)(p->len - 24));
    line(o, 0, 0, p->len, "Decoded by the board (%s): a text record, not a frame", o->proto);
    line(o, 1, 0, 24, "From: %s", o->src);
    if (p->freq_khz) line(o, 1, 0, 0, "Address: %u", (unsigned)p->freq_khz);
    if (p->len > 24) line(o, 1, 24, p->len - 24, "Text: %.60s", o->info);
}

void tap_dissect(const tap_pkt_t *p, const uint8_t *d, tap_dis_t *o)
{
    tap_line_t *lines = o->lines;
    const int cap = o->cap;
    memset(o, 0, sizeof(*o));
    o->lines = lines; o->cap = cap;
    if (p->radio == R_WIFI) wifi(p, d, o);
    else if (p->radio == R_BLE) ble(p, d, o);
    else if (p->radio == R_LORA) lora(p, d, o);
    else decode(p, d, o);
}

/* ---- display filters ------------------------------------------------------ */
enum { F_TAG, F_ADDR, F_SRC, F_DST, F_RSSI, F_LEN, F_CH, F_NO, F_CONTAINS, F_NAME, F_AND, F_OR, F_NOT };
enum { C_EQ, C_NE, C_GT, C_LT, C_GE, C_LE, C_HAS };
typedef struct { const char *s; tap_filter_t *f; char tok[40]; int kind; } parse_t;   /* kind: 0 end, 1 word, 2 op, 3 string */
static const struct { const char *w; uint32_t mask; } words[] = {
    {"wifi", T_WIFI}, {"wlan", T_WIFI}, {"802.11", T_WIFI}, {"mgmt", T_MGMT}, {"ctrl", T_CTRL}, {"data", T_DATA},
    {"beacon", T_BEACON}, {"probe", T_PROBE}, {"eapol", T_EAPOL}, {"deauth", T_DEAUTH}, {"auth", T_AUTH},
    {"ble", T_BLE}, {"bt", T_BLE}, {"ibeacon", T_IBEACON}, {"eddystone", T_EDDYSTONE}, {"scanrsp", T_SCANRSP},
    {"rid", T_RID}, {"lora", T_LORA}, {"meshcore", T_MESHCORE}, {"meshtastic", T_MESHTASTIC}, {"decode", T_DECODE},
    {"protected", T_PROTECTED}};
static char lower(char c) { return c >= 'A' && c <= 'Z' ? (char)(c + 32) : c; }
static void next(parse_t *p)
{
    while (*p->s == ' ') p->s++;
    size_t n = 0;
    p->tok[0] = 0; p->kind = 0;
    if (!*p->s) return;
    if (*p->s == '"') {
        p->s++;
        while (*p->s && *p->s != '"') { if (n + 1 < sizeof(p->tok)) p->tok[n++] = *p->s; p->s++; }
        if (*p->s == '"') p->s++;
        p->kind = 3;
    } else if (strchr("()=!<>&|", *p->s)) {
        p->tok[n++] = *p->s++;
        if (*p->s && strchr("=&|", *p->s) && p->tok[0] != '(' && p->tok[0] != ')') p->tok[n++] = *p->s++;
        p->kind = 2;
    } else {
        while (*p->s && *p->s != ' ' && !strchr("()=!<>&|\"", *p->s)) { if (n + 1 < sizeof(p->tok)) p->tok[n++] = lower(*p->s); p->s++; }
        p->kind = 1;
    }
    p->tok[n] = 0;
}
static bool fail(parse_t *p, const char *why)
{
    if (!p->f->err[0]) snprintf(p->f->err, sizeof(p->f->err), "%s%s%.16s", why, p->tok[0] ? " near " : "", p->tok);
    return false;
}
static tap_fnode_t *emit(parse_t *p, int op)
{
    if (p->f->count >= 24) { fail(p, "filter too long"); return NULL; }
    tap_fnode_t *n = &p->f->n[p->f->count++];
    memset(n, 0, sizeof(*n));
    n->op = (uint8_t)op;
    return n;
}
static int compare(parse_t *p)
{
    static const char *const ops[6] = {"==", "!=", ">", "<", ">=", "<="};
    if (p->kind == 1 && !strcmp(p->tok, "contains")) { next(p); return C_HAS; }
    if (p->kind == 1 && !strcmp(p->tok, "eq")) { next(p); return C_EQ; }
    if (p->kind == 2) {
        if (!strcmp(p->tok, "=")) { next(p); return C_EQ; }
        for (int i = 0; i < 6; i++) if (!strcmp(p->tok, ops[i])) { next(p); return i; }
    }
    return -1;
}
static bool expr(parse_t *p);
static bool primary(parse_t *p)
{
    if (p->kind == 2 && !strcmp(p->tok, "(")) {
        next(p);
        if (!expr(p)) return false;
        if (p->kind != 2 || strcmp(p->tok, ")")) return fail(p, "missing )");
        next(p);
        return true;
    }
    if ((p->kind == 2 && !strcmp(p->tok, "!")) || (p->kind == 1 && !strcmp(p->tok, "not"))) {
        next(p);
        return primary(p) && emit(p, F_NOT);
    }
    if (p->kind != 1) return fail(p, p->kind ? "unexpected" : "incomplete filter");
    char w[40];
    snprintf(w, sizeof(w), "%s", p->tok);
    next(p);
    int field = !strcmp(w, "addr") ? F_ADDR : !strcmp(w, "src") ? F_SRC : !strcmp(w, "dst") ? F_DST : !strcmp(w, "rssi") ? F_RSSI :
                !strcmp(w, "len") ? F_LEN : !strcmp(w, "ch") || !strcmp(w, "channel") ? F_CH : !strcmp(w, "no") ? F_NO :
                !strcmp(w, "ssid") || !strcmp(w, "name") ? F_NAME : !strcmp(w, "contains") ? F_CONTAINS :
                !strcmp(w, "proto") || !strcmp(w, "radio") ? F_TAG : -1;
    if (field < 0) {
        for (unsigned i = 0; i < sizeof(words) / sizeof(words[0]); i++)
            if (!strcmp(w, words[i].w)) { tap_fnode_t *n = emit(p, F_TAG); if (n) n->mask = words[i].mask; return n != NULL; }
        snprintf(p->tok, sizeof(p->tok), "%s", w);
        return fail(p, "unknown word");
    }
    int cmp = C_HAS;
    if (field != F_CONTAINS) {
        cmp = compare(p);
        if (cmp < 0) return fail(p, "expected a comparison");
    }
    if (p->kind != 1 && p->kind != 3) return fail(p, "expected a value");
    tap_fnode_t *n = emit(p, field);
    if (!n) return false;
    n->cmp = (uint8_t)cmp;
    if (field == F_TAG) {
        for (unsigned i = 0; i < sizeof(words) / sizeof(words[0]); i++) if (!strcmp(p->tok, words[i].w)) n->mask = words[i].mask;
        if (!n->mask) return fail(p, "unknown protocol");
        if (cmp != C_EQ && cmp != C_NE) return fail(p, "use == or !=");
    } else if (field == F_RSSI || field == F_LEN || field == F_CH || field == F_NO) {
        char *end;
        n->num = (int32_t)strtol(p->tok, &end, 0);
        if (*end || cmp == C_HAS) return fail(p, "expected a number");
    } else {
        for (n->slen = 0; p->tok[n->slen]; n->slen++) n->str[n->slen] = lower(p->tok[n->slen]);
        if (field != F_CONTAINS && field != F_NAME && cmp != C_EQ && cmp != C_NE && cmp != C_HAS) return fail(p, "use == or !=");
    }
    next(p);
    if (field == F_TAG && cmp == C_NE) return emit(p, F_NOT) != NULL;
    return true;
}
static bool conjn(parse_t *p)
{
    if (!primary(p)) return false;
    while ((p->kind == 1 && !strcmp(p->tok, "and")) || (p->kind == 2 && !strcmp(p->tok, "&&"))) {
        next(p);
        if (!primary(p) || !emit(p, F_AND)) return false;
    }
    return true;
}
static bool expr(parse_t *p)
{
    if (!conjn(p)) return false;
    while ((p->kind == 1 && !strcmp(p->tok, "or")) || (p->kind == 2 && !strcmp(p->tok, "||"))) {
        next(p);
        if (!conjn(p) || !emit(p, F_OR)) return false;
    }
    return true;
}
bool tap_filter_compile(const char *s, tap_filter_t *f)
{
    parse_t p = {.s = s ? s : "", .f = f};
    f->count = 0; f->err[0] = 0;
    next(&p);
    if (!p.kind) return true;
    if (!expr(&p)) { f->count = 0; return false; }
    if (p.kind) { fail(&p, "unexpected"); f->count = 0; return false; }
    return true;
}
/* Case-blind search of a lowered needle in text or bytes. */
static bool has(const char *hay, size_t hn, const char *needle, size_t nn)
{
    if (!nn) return true;
    for (size_t i = 0; i + nn <= hn; i++) {
        size_t k = 0;
        while (k < nn && lower(hay[i + k]) == needle[k]) k++;
        if (k == nn) return true;
    }
    return false;
}
static bool addr_is(const char *have, const tap_fnode_t *n)
{
    /* a prefix is enough: addr == aa:bb:cc */
    size_t k = 0;
    if (!have[0]) return false;
    while (k < n->slen && lower(have[k]) == n->str[k]) k++;
    return k == n->slen;
}
static bool num(int v, const tap_fnode_t *n)
{
    switch (n->cmp) { case C_EQ: return v == n->num; case C_NE: return v != n->num; case C_GT: return v > n->num;
        case C_LT: return v < n->num; case C_GE: return v >= n->num; default: return v <= n->num; }
}
bool tap_filter_match(const tap_filter_t *f, const tap_pkt_t *p, const uint8_t *d, const tap_dis_t *o)
{
    bool st[24];
    int sp = 0;
    if (!f->count) return true;
    for (int i = 0; i < f->count; i++) {
        const tap_fnode_t *n = &f->n[i];
        bool v = false;
        switch (n->op) {
        case F_AND: if (sp >= 2) { sp--; st[sp - 1] = st[sp - 1] && st[sp]; } continue;
        case F_OR: if (sp >= 2) { sp--; st[sp - 1] = st[sp - 1] || st[sp]; } continue;
        case F_NOT: if (sp >= 1) st[sp - 1] = !st[sp - 1]; continue;
        case F_TAG: v = (o->tags & n->mask) != 0; break;
        case F_ADDR: v = addr_is(o->src, n) || addr_is(o->dst, n) || addr_is(o->bssid, n); if (n->cmp == C_NE) v = !v; break;
        case F_SRC: v = addr_is(o->src, n); if (n->cmp == C_NE) v = !v; break;
        case F_DST: v = addr_is(o->dst, n); if (n->cmp == C_NE) v = !v; break;
        case F_RSSI: v = num(p->rssi, n); break;
        case F_LEN: v = num(p->orig, n); break;
        case F_CH: v = num(p->ch, n); break;
        case F_NO: v = num((int)p->no, n); break;
        case F_NAME:
            v = n->cmp == C_HAS ? has(o->name, strlen(o->name), n->str, n->slen)
                                : strlen(o->name) == n->slen && has(o->name, n->slen, n->str, n->slen);
            if (n->cmp == C_NE) v = !v;
            break;
        default:
            v = has((const char *)d, p->len, n->str, n->slen) || has(o->info, strlen(o->info), n->str, n->slen) ||
                has(o->name, strlen(o->name), n->str, n->slen);
            break;
        }
        if (sp < 24) st[sp++] = v;
    }
    return sp > 0 && st[sp - 1];
}

/* ---- statistics ----------------------------------------------------------- */
static int endpoint(tap_stats_t *s, const char *addr, int radio)
{
    if (!addr[0] || !strcmp(addr, "Broadcast")) return -1;
    for (int i = 0; i < s->eps; i++) if (s->ep[i].radio == radio && !strcmp(s->ep[i].addr, addr)) return i;
    if (s->eps >= TAP_EPS) { s->ep_over++; return -1; }
    tap_ep_t *e = &s->ep[s->eps];
    memset(e, 0, sizeof(*e));
    snprintf(e->addr, sizeof(e->addr), "%s", addr);
    e->radio = (uint8_t)radio;
    return s->eps++;
}
void tap_stats_add(tap_stats_t *s, const tap_pkt_t *p, const tap_dis_t *d)
{
    s->total++; s->bytes += p->orig;
    s->pk[d->leaf]++; s->by[d->leaf] += p->orig;
    if (p->radio == R_WIFI) s->wifi_ch[p->ch >= 1 && p->ch <= 14 ? p->ch : 0]++;
    if (p->radio == R_BLE) s->ble_ch[p->ch >= 37 && p->ch <= 39 ? p->ch - 37 : 3]++;
    if (p->radio != R_DEC) {
        int bin = (p->rssi + 100) / 10 + 1;
        if (p->rssi < -100) bin = 0;
        s->rssi[p->radio][bin < 0 ? 0 : bin > 8 ? 8 : bin]++;
    }
    const int a = endpoint(s, d->src, p->radio), b = endpoint(s, d->dst, p->radio);
    if (a >= 0) {
        tap_ep_t *e = &s->ep[a];
        e->tx++; e->bytes += p->orig; e->rssi = p->rssi;
        if (d->name[0] && (d->tags & (T_BEACON | T_BLE | T_MESHCORE))) snprintf(e->label, sizeof(e->label), "%s", d->name);
    }
    if (b >= 0) s->ep[b].rx++;
    if (a >= 0 && b >= 0) {
        int i = 0;
        for (; i < s->cvs; i++)
            if ((s->cv[i].a == a && s->cv[i].b == b) || (s->cv[i].a == b && s->cv[i].b == a)) break;
        if (i == s->cvs) {
            if (s->cvs >= TAP_CVS) { s->cv_over++; return; }
            s->cv[s->cvs++] = (tap_cv_t){.a = (uint8_t)a, .b = (uint8_t)b};
        }
        s->cv[i].pk++; s->cv[i].bytes += p->orig;
    }
}

/* ---- pcapng ----------------------------------------------------------------- */
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
static size_t idb(uint8_t *o, unsigned linktype, const char *name)
{
    static const uint8_t usec = 6;
    size_t n = 8;
    n += w16(o + n, linktype); n += w16(o + n, 0); n += w32(o + n, LAP_CAPTURE_SNAP_MAX + 64);
    n += option(o + n, 2, name, (unsigned)strlen(name));       /* if_name */
    n += option(o + n, 9, &usec, 1);                            /* if_tsresol: microseconds */
    n += w32(o + n, 0);                                         /* opt_endofopt */
    w32(o, 1); w32(o + 4, (uint32_t)n + 4); w32(o + n, (uint32_t)n + 4);
    return n + 4;
}
size_t tap_pcapng_header(uint8_t *o, size_t cap)
{
    static const char app[] = "LakeShark TAP";
    if (cap < 256) return 0;
    size_t n = 8;
    n += w32(o + n, 0x1A2B3C4Du); n += w16(o + n, 1); n += w16(o + n, 0);
    memset(o + n, 0xFF, 8); n += 8;                             /* section length unknown */
    n += option(o + n, 4, app, sizeof(app) - 1);                /* shb_userappl */
    n += w32(o + n, 0);
    w32(o, 0x0A0D0D0Au); w32(o + 4, (uint32_t)n + 4); w32(o + n, (uint32_t)n + 4);
    n += 4;
    n += idb(o + n, 127, "wifi0 802.11 radiotap");
    n += idb(o + n, 256, "ble0 advertising");
    n += idb(o + n, 270, "lora0 LoRaTap");
    return n;
}
size_t tap_pcapng_packet(const tap_pkt_t *p, const uint8_t *d, uint8_t *o, size_t cap, uint64_t epoch_us)
{
    if (p->radio == R_DEC || cap < (size_t)p->len + 96) return 0;
    uint8_t *b = o + 28;                                        /* the packet data */
    size_t h = 0;
    if (p->radio == R_WIFI) {
        /* radiotap: flags, [rate], channel, antenna signal */
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
        b[15] = (uint8_t)(6 + p->len);
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
    memcpy(b + h, d, p->len);
    size_t caplen = h + p->len, orig = h + p->orig;
    if (p->radio == R_BLE) { memset(b + caplen, 0, 3); caplen += 3; orig = caplen; }   /* the CRC the controller kept */
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
