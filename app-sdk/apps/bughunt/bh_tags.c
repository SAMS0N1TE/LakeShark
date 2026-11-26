/* BUGHUNT's tag engine (it began as BLE WATCH's): advert parsing, tracker
   classes, the device table, rotation grouping and the following alert.
   See bh_tags.h.

   Identifiers marked (uncertain) below come from public reverse-engineering
   write-ups rather than a published specification; they are used to label a
   device, never to decide anything that could not be undone. */
#include "bh_tags.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

/* ---- names ------------------------------------------------------------------- */

static const struct { uint8_t cls; uint8_t level; const char *tag, *name, *note; } classes[BW_C_COUNT] = {
    {BW_C_NONE, 0, "-", "Unclassified", ""},
    {BW_C_FINDMY, 2, "FINDMY", "Apple Find My (offline finding)",
     "AirTags, AirPods and Find My tags. Separated: away from its owner."},
    {BW_C_APPLE_PP, 1, "APL-PP", "Apple proximity pairing",
     "AirPods out of the case, or an AirTag being set up."},
    {BW_C_TILE, 2, "TILE", "Tile tracker", "Tile keeps one address: grouping is exact."},
    {BW_C_SMARTTAG, 2, "SMTAG", "Samsung SmartTag (offline finding)",
     "Rotates its address about every 15 minutes."},
    {BW_C_FMDN, 2, "GFMDN", "Google Find My Device tag",
     "Frame 0x41: unwanted-tracking mode, away from its owner."},
    {BW_C_DULT, 2, "DULT", "Tracker in DULT separated mode",
     "A cross-platform tag that says it is away from its owner."},
    {BW_C_CHIPOLO, 2, "CHIPLO", "Chipolo tracker", ""},
    {BW_C_FASTPAIR, 1, "FPAIR", "Google Fast Pair", "Headphones mostly; some tags too."},
    {BW_C_IBEACON, 0, "IBCN", "iBeacon", ""},
    {BW_C_EDDYSTONE, 0, "EDDY", "Eddystone beacon", ""},
    {BW_C_APPLE, 0, "APPLE", "Apple continuity", "Phones, watches and Macs talking to each other."},
    {BW_C_MICROSOFT, 0, "MSFT", "Microsoft beacon", ""},
};

const char *bw_class_tag(int c) { return c >= 0 && c < BW_C_COUNT ? classes[c].tag : "?"; }
const char *bw_class_name(int c) { return c >= 0 && c < BW_C_COUNT ? classes[c].name : "?"; }
const char *bw_class_note(int c) { return c >= 0 && c < BW_C_COUNT ? classes[c].note : ""; }

/* Bluetooth SIG company identifiers, a small table of the common ones. */
static const struct { uint16_t id; const char *name; } companies[] = {
    {0x0000, "Ericsson"}, {0x0001, "Nokia"}, {0x0002, "Intel"}, {0x0003, "IBM"}, {0x0006, "Microsoft"},
    {0x0008, "Motorola"}, {0x000A, "Qualcomm"}, {0x000D, "Texas Instr."}, {0x000F, "Broadcom"},
    {0x001D, "Qualcomm"}, {0x0030, "STMicro"}, {0x0046, "MediaTek"}, {0x004C, "Apple"}, {0x0059, "Nordic"},
    {0x0075, "Samsung"}, {0x0087, "Garmin"}, {0x00C4, "LG"}, {0x00E0, "Google"}, {0x0118, "Radius Netw."},
    {0x012D, "Sony"}, {0x0131, "Cypress"}, {0x0157, "Huami"}, {0x0171, "Amazon"}, {0x02E5, "Espressif"},
    {0x02FF, "Silicon Labs"},
    {0x038F, "Xiaomi"},             /* (uncertain) */
    {0x0499, "Ruuvi"},
};
const char *bw_company(uint16_t id)
{
    for (size_t i = 0; i < sizeof(companies) / sizeof(companies[0]); i++)
        if (companies[i].id == id) return companies[i].name;
    return NULL;
}

const char *bw_pdu_name(uint8_t pdu)
{
    switch (pdu) {
    case 0: return "ADV_IND";
    case 1: return "ADV_DIRECT_IND";
    case 2: return "ADV_SCAN_IND";
    case 3: return "ADV_NONCONN_IND";
    case 4: return "SCAN_RSP";
    case 7: return "ADV_EXT_IND";
    default: return "unknown";
    }
}

/* The two top bits of a random address say what kind it is. */
const char *bw_addr_kind(const uint8_t a[6], uint8_t type)
{
    if (!(type & 1)) return "public";
    switch (a[5] >> 6) {
    case 3: return "random static";
    case 1: return "resolvable private";
    case 0: return "non-resolvable";
    default: return "random";
    }
}
void bw_addr_text(const uint8_t a[6], char out[18])
{
    snprintf(out, 18, "%02x:%02x:%02x:%02x:%02x:%02x", a[5], a[4], a[3], a[2], a[1], a[0]);
}

/* ---- parsing ---------------------------------------------------------------------- */

static void set_class(bw_ad_t *o, uint8_t cls, uint8_t level)
{
    if (o->cls == BW_C_NONE || level > o->level) { o->cls = cls; o->level = level; }
}
static void add_uuid(bw_ad_t *o, uint16_t u)
{
    for (int i = 0; i < o->n_uuid; i++) if (o->uuid16[i] == u) return;
    if (o->n_uuid < sizeof(o->uuid16) / sizeof(o->uuid16[0])) o->uuid16[o->n_uuid++] = u;
}
/* A 16-bit UUID, listed or carrying service data (v may be NULL). */
static void by_uuid(bw_ad_t *o, uint16_t u, const uint8_t *v, size_t n)
{
    switch (u) {
    case 0xFEED: case 0xFEEC: set_class(o, BW_C_TILE, 2); break;       /* both assigned to Tile, Inc. */
    case 0xFD5A: set_class(o, BW_C_SMARTTAG, 2); break;                 /* Samsung; SmartTag offline finding */
    case 0xFCB2: set_class(o, BW_C_DULT, 2); o->separated = 1; break;   /* DULT separated mode (uncertain: draft spec UUID) */
    case 0xFE33: case 0xFE65: set_class(o, BW_C_CHIPOLO, 2); break;     /* Chipolo d.o.o. (uncertain) */
    case 0xFE2C:                                                        /* Google Fast Pair */
        set_class(o, BW_C_FASTPAIR, 1);
        if (v && n == 3) o->model = (uint32_t)v[0] << 16 | (uint32_t)v[1] << 8 | v[2];   /* discoverable: the 24-bit model ID */
        break;
    case 0xFEAA:                                                        /* Eddystone UUID: beacons, and FMDN */
        if (!v || !n) break;
        if (v[0] == 0x40 || v[0] == 0x41) {                             /* FMDN frame; 0x41 = unwanted-tracking-protection mode */
            set_class(o, BW_C_FMDN, 2);
            o->sub = v[0];
            o->separated = v[0] == 0x41 ? 1 : -1;
        } else if (o->cls == BW_C_NONE) {
            set_class(o, BW_C_EDDYSTONE, 0);
            o->sub = v[0];                                              /* 0x00 UID, 0x10 URL, 0x20 TLM, 0x30 EID */
        }
        break;
    default: break;
    }
}
/* Apple manufacturer data: a run of continuity type/length/value records. */
static void apple(bw_ad_t *o, const uint8_t *v, size_t n)
{
    bool any = false;
    for (size_t k = 0; k + 2 <= n;) {
        const uint8_t t = v[k], l = v[k + 1];
        const uint8_t *b = v + k + 2;
        if (k + 2 + l > n) break;
        if (t == 0x12) {                    /* offline finding */
            set_class(o, BW_C_FINDMY, 2);
            o->sub = t;
            o->status = l >= 1 ? b[0] : 0;
            o->separated = l >= 25 ? 1 : l == 2 ? 0 : -1;    /* 25 bytes: the full key, sent when separated */
        } else if (t == 0x07) {             /* proximity pairing */
            set_class(o, BW_C_APPLE_PP, 1);
            if (o->cls == BW_C_APPLE_PP) {
                o->sub = t;
                o->model = l >= 3 ? (uint16_t)(b[1] << 8 | b[2]) : 0;
            }
        } else if (t == 0x02 && l == 0x15) {
            set_class(o, BW_C_IBEACON, 0);
        } else if (!any) {
            set_class(o, BW_C_APPLE, 0);
            if (o->cls == BW_C_APPLE) o->sub = t;
        }
        any = true;
        k += 2u + l;
    }
    if (!any) set_class(o, BW_C_APPLE, 0);
}

bool bw_parse(const uint8_t *ad, size_t n, bw_ad_t *o)
{
    memset(o, 0, sizeof(*o));
    o->company = 0xFFFF;
    o->separated = -1;
    bool named_full = false;
    for (size_t j = 0; j < n;) {
        const size_t len = ad[j];
        if (!len) break;                    /* padding: the rest is zero */
        if (j + 1 + len > n) return false;
        const uint8_t type = ad[j + 1], *v = ad + j + 2;
        const size_t vl = len - 1;
        switch (type) {
        case 0x01: if (vl >= 1) { o->flags = v[0]; o->has_flags = true; } break;
        case 0x02: case 0x03:
            for (size_t k = 0; k + 1 < vl; k += 2) { const uint16_t u = (uint16_t)(v[k] | v[k + 1] << 8); add_uuid(o, u); by_uuid(o, u, NULL, 0); }
            break;
        case 0x08: case 0x09:
            if (type == 0x09 || !named_full) {
                size_t m = vl < sizeof(o->name) - 1 ? vl : sizeof(o->name) - 1;
                for (size_t k = 0; k < m; k++) o->name[k] = v[k] >= 32 && v[k] < 127 && v[k] != ',' && v[k] != '"' ? (char)v[k] : '.';
                o->name[m] = 0;
                named_full = type == 0x09;
            }
            break;
        case 0x0A: if (vl >= 1) { o->tx_power = (int8_t)v[0]; o->has_tx = true; } break;
        case 0x19: if (vl >= 2) o->appearance = (uint16_t)(v[0] | v[1] << 8); break;
        case 0x16:
            if (vl >= 2) { const uint16_t u = (uint16_t)(v[0] | v[1] << 8); add_uuid(o, u); by_uuid(o, u, v + 2, vl - 2); }
            break;
        case 0xFF:
            if (vl >= 2) {
                o->company = (uint16_t)(v[0] | v[1] << 8);
                if (o->company == 0x004C) apple(o, v + 2, vl - 2);
                else if (o->company == 0x0006) set_class(o, BW_C_MICROSOFT, 0);
            }
            break;
        default: break;
        }
        j += len + 1;
    }
    o->level = classes[o->cls].level;
    return true;
}

/* ---- the table ------------------------------------------------------------------------ */

void bw_init(bw_t *w)
{
    memset(w, 0, sizeof(*w));
    w->next_uid = 1;
    w->follow_ms = 10u * 60u * 1000u;
    w->watch_ms = 120000u;
    w->follow_m = 250.0f;
    w->gap_ms = 5u * 60u * 1000u;
}

int bw_find_uid(const bw_t *w, uint32_t uid)
{
    if (!uid) return -1;
    for (int i = 0; i < w->n; i++) if (w->dev[i].uid == uid) return i;
    return -1;
}

static int find_addr(const bw_t *w, const uint8_t a[6], uint8_t type)
{
    for (int i = 0; i < w->n; i++)
        if (w->dev[i].addr_type == type && !memcmp(w->dev[i].addr, a, 6)) return i;
    return -1;
}

/* A full table gives up the device that matters least: not alerting, not a
   tracker, longest unheard. */
static int victim(const bw_t *w)
{
    int best = 0;
    long best_score = 0x7fffffffL;
    for (int i = 0; i < w->n; i++) {
        const bw_dev_t *d = &w->dev[i];
        long score = (long)(d->last_ms / 1000u);
        if (d->ad.level) score += 1000000L;
        if (d->alert) score += 2000000L;
        if (score < best_score) { best_score = score; best = i; }
    }
    return best;
}

static void hist_advance(bw_dev_t *d, uint32_t now_ms)
{
    const uint32_t b = now_ms / BW_HIST_MS;
    if (b <= d->hist_at) return;
    const uint32_t shift = b - d->hist_at;
    if (shift >= BW_HIST) memset(d->hist, BW_NO_RSSI, sizeof(d->hist));
    else {
        memmove(d->hist, d->hist + shift, BW_HIST - shift);
        memset(d->hist + BW_HIST - shift, BW_NO_RSSI, shift);
    }
    d->hist_at = b;
}

static void take_position(bw_t *w, bw_dev_t *d)
{
    if (!w->fix) return;
    if (!d->has_pos0) { d->has_pos0 = true; d->lat0 = w->lat; d->lon0 = w->lon; return; }
    const float m = bw_distance_m(d->lat0, d->lon0, w->lat, w->lon);
    if (m > d->moved_m) d->moved_m = m;
}

static void merge_ad(bw_dev_t *d, const bw_ad_t *a, uint8_t pdu)
{
    bw_ad_t *o = &d->ad;
    /* The strongest tracker evidence stays; a scan response never demotes. */
    const bool take = o->cls == BW_C_NONE || a->level > o->level || (a->level == o->level && pdu != 4);
    if (take && a->cls != BW_C_NONE) {
        o->cls = a->cls; o->level = a->level; o->sub = a->sub; o->status = a->status;
        o->separated = a->separated; o->model = a->model;
    }
    if (a->name[0]) memcpy(o->name, a->name, sizeof(o->name));
    if (a->company != 0xFFFF) o->company = a->company;
    if (a->n_uuid) { memcpy(o->uuid16, a->uuid16, sizeof(o->uuid16)); o->n_uuid = a->n_uuid; }
    if (a->has_flags) { o->flags = a->flags; o->has_flags = true; }
    if (a->has_tx) { o->tx_power = a->tx_power; o->has_tx = true; }
    if (a->appearance) o->appearance = a->appearance;
}

int bw_add(bw_t *w, const uint8_t addr[6], uint8_t addr_type, uint8_t pdu, int rssi, const uint8_t *ad, size_t n,
           uint32_t now)
{
    bw_ad_t a;
    bw_parse(ad, n, &a);
    w->adverts++;
    if (rssi < -127) rssi = -127;
    if (rssi > 20) rssi = 20;
    int i = find_addr(w, addr, addr_type);
    bw_dev_t *d;
    if (i < 0) {
        if (w->n < BW_MAX_DEV) i = w->n++;
        else { i = victim(w); w->evicted++; }
        d = &w->dev[i];
        memset(d, 0, sizeof(*d));
        d->uid = w->next_uid++;
        memcpy(d->addr, addr, 6);
        d->addr_type = addr_type;
        d->pdu = 0xFF;
        d->ad = a;
        d->first_ms = d->last_ms = d->chain_ms = now;
        memset(d->hist, BW_NO_RSSI, sizeof(d->hist));
        d->hist_at = now / BW_HIST_MS;
        d->smooth = (float)rssi;
        d->peak = (int8_t)rssi;
    } else {
        d = &w->dev[i];
        if (now - d->last_ms > w->gap_ms) {       /* gone a while: presence starts again here */
            d->chain_ms = now;
            d->has_pos0 = false;
            d->moved_m = 0;
            d->alert = 0;
        }
        merge_ad(d, &a, pdu);
        d->smooth = d->smooth * 0.7f + (float)rssi * 0.3f;
        if (rssi > d->peak) d->peak = (int8_t)rssi;
    }
    if (pdu != 4 || !d->raw_len) {
        d->raw_len = (uint8_t)(n < BW_AD_KEEP ? n : BW_AD_KEEP);
        if (d->raw_len) memcpy(d->raw, ad, d->raw_len);
    }
    if (pdu != 4) d->pdu = pdu;
    d->count++;
    if (now > d->last_ms) d->last_ms = now;
    d->rssi = (int8_t)rssi;
    hist_advance(d, now);
    if (rssi > d->hist[BW_HIST - 1]) d->hist[BW_HIST - 1] = (int8_t)rssi;
    const uint32_t m = now / 60000u, s = m % BW_MINUTES;
    if (d->mins[s].minute != m || !d->mins[s].n) { d->mins[s].minute = m; d->mins[s].n = 0; }
    if (d->mins[s].n < 0xFFFF) d->mins[s].n++;
    take_position(w, d);
    return i;
}

void bw_position(bw_t *w, bool fix, double lat, double lon)
{
    w->fix = fix;
    if (fix) { w->lat = lat; w->lon = lon; w->ever_fix = true; }
}

static bool rotates(int cls)
{
    return cls == BW_C_FINDMY || cls == BW_C_SMARTTAG || cls == BW_C_FMDN || cls == BW_C_DULT ||
           cls == BW_C_APPLE_PP || cls == BW_C_FASTPAIR;
}

/* A rotating tracker changes address at once: the old address stops and a
   new one of the same kind starts. Linked only when the old one has stayed
   silent since (checked 15 s later), their RSSI agree and, for Find My, the
   status byte matches. Two tags of one kind rotating within the same minute
   can still be swapped; that is the limit of what the air shows. */
static void link_rotation(bw_t *w, bw_dev_t *d)
{
    d->chain_checked = true;
    if (!rotates(d->ad.cls) || !(d->addr_type & 1) || d->pred) return;
    int best = -1;
    float best_diff = 15.0f;
    for (int i = 0; i < w->n; i++) {
        bw_dev_t *c = &w->dev[i];
        if (c == d || c->succ || c->ad.cls != d->ad.cls || c->first_ms >= d->first_ms) continue;
        if (c->last_ms + 60000u < d->first_ms || c->last_ms > d->first_ms + 2000u) continue;
        if (d->ad.cls == BW_C_FINDMY && (c->ad.status != d->ad.status || c->ad.separated != d->ad.separated)) continue;
        const float diff = fabsf(c->smooth - d->smooth);
        if (diff <= best_diff) { best_diff = diff; best = i; }
    }
    if (best < 0) return;
    bw_dev_t *c = &w->dev[best];
    d->pred = c->uid;
    c->succ = d->uid;
    d->rotations = (uint16_t)(c->rotations + 1);
    if (d->first_ms - c->last_ms <= w->gap_ms) d->chain_ms = c->chain_ms;
    if (c->has_pos0) {
        float m = c->moved_m;
        if (d->has_pos0) {
            const float a = bw_distance_m(c->lat0, c->lon0, d->lat0, d->lon0);
            if (a + d->moved_m > m) m = a + d->moved_m;
        }
        d->has_pos0 = true; d->lat0 = c->lat0; d->lon0 = c->lon0; d->moved_m = m;
    }
    for (int j = 0; j < BW_MINUTES; j++) {
        if (!c->mins[j].n) continue;
        const uint32_t mm = c->mins[j].minute, s = mm % BW_MINUTES;
        if (d->mins[s].minute == mm && d->mins[s].n) {
            const uint32_t t = (uint32_t)d->mins[s].n + c->mins[j].n;
            d->mins[s].n = (uint16_t)(t > 0xFFFF ? 0xFFFF : t);
        } else if (!d->mins[s].n || d->mins[s].minute < mm) d->mins[s] = c->mins[j];
    }
    d->count += c->count;
    if (c->peak > d->peak) d->peak = c->peak;
    if (c->alert) { d->alert = c->alert; d->alert_ms = c->alert_ms; }
    if (c->mark && !d->mark) d->mark = c->mark;
}

uint16_t bw_kind_bit(int cls)
{
    if (cls == BW_C_APPLE_PP) cls = BW_C_FINDMY;
    return cls > 0 && cls < 16 ? (uint16_t)(1u << cls) : 0;
}
bool bw_alertable(const bw_t *w, const bw_dev_t *d)
{
    if (d->mark == BW_MARK_MINE || (w->kind_off & bw_kind_bit(d->ad.cls))) return false;
    return d->ad.level >= 2 || (d->ad.level == 1 && w->alert_possible);
}
void bw_mark(bw_t *w, int i, uint8_t mark)
{
    if (i < 0 || i >= w->n) return;
    /* the newest address of the chain, and every older one so a late link agrees */
    bw_dev_t *d = &w->dev[i];
    for (int guard = 0; d->succ && guard < BW_MAX_DEV; guard++) {
        const int k = bw_find_uid(w, d->succ);
        if (k < 0) break;
        d = &w->dev[k];
    }
    for (int guard = 0; d && guard < BW_MAX_DEV; guard++) {
        d->mark = mark;
        d->gone = 0;
        if (mark == BW_MARK_MINE && d->alert) d->alert = 0;
        const int k = d->pred ? bw_find_uid(w, d->pred) : -1;
        d = k >= 0 ? &w->dev[k] : NULL;
    }
}
int bw_watch(bw_t *w, uint32_t now, int *event)
{
    const uint32_t gone_ms = w->watch_ms ? w->watch_ms : 120000u;
    for (int i = 0; i < w->n; i++) {
        bw_dev_t *d = &w->dev[i];
        if (d->mark != BW_MARK_WATCH || d->succ) continue;
        /* a rotation's new address may not be linked yet: wait out the
           15 s check before calling a rotating tag gone */
        if (!d->gone && now - d->last_ms > gone_ms + 15000u) { d->gone = 1; if (event) *event = BW_EV_GONE; return i; }
        if (d->gone && now - d->last_ms < 3000u) { d->gone = 0; if (event) *event = BW_EV_BACK; return i; }
    }
    return -1;
}

int bw_tick(bw_t *w, uint32_t now)
{
    for (int i = 0; i < w->n; i++) {
        bw_dev_t *d = &w->dev[i];
        if (!d->chain_checked && now - d->first_ms >= 15000u) link_rotation(w, d);
    }
    for (int i = 0; i < w->n; i++) {
        bw_dev_t *d = &w->dev[i];
        if (d->succ || d->alert || !bw_alertable(w, d)) continue;
        if (now - d->last_ms > 60000u || bw_span_ms(d) < w->follow_ms) continue;
        if (d->ad.separated == 0) continue;     /* it says its owner is beside it: your own, or a companion's */
        /* With a position, a tag that stays while you stand still is the
           neighbourhood's; it has to come along. Without one, time alone. */
        if (d->has_pos0 && w->fix && d->moved_m < w->follow_m) continue;
        d->alert = 1;
        d->alert_ms = now;
        return i;
    }
    return -1;
}

/* ---- reading ------------------------------------------------------------------------- */

uint32_t bw_span_ms(const bw_dev_t *d) { return d->last_ms - d->chain_ms; }

int bw_window(const bw_dev_t *d, uint32_t now, int minutes)
{
    const uint32_t m = now / 60000u;
    int n = 0;
    for (int j = 0; j < BW_MINUTES; j++)
        if (d->mins[j].n && d->mins[j].minute <= m && m - d->mins[j].minute < (uint32_t)minutes) n += d->mins[j].n;
    return n;
}

uint32_t bw_presence(const bw_dev_t *d, uint32_t now)
{
    const uint32_t m = now / 60000u;
    uint32_t bits = 0;
    for (int j = 0; j < BW_MINUTES; j++)
        if (d->mins[j].n && d->mins[j].minute <= m && m - d->mins[j].minute < BW_MINUTES) bits |= 1u << (m - d->mins[j].minute);
    return bits;
}

void bw_hist(const bw_dev_t *d, uint32_t now, int8_t out[BW_HIST])
{
    const uint32_t b = now / BW_HIST_MS;
    const uint32_t shift = b > d->hist_at ? b - d->hist_at : 0;
    if (shift >= BW_HIST) { memset(out, BW_NO_RSSI, BW_HIST); return; }
    memcpy(out, d->hist + shift, BW_HIST - shift);
    memset(out + BW_HIST - shift, BW_NO_RSSI, shift);
}

int bw_trend(const bw_dev_t *d, uint32_t now)
{
    int8_t h[BW_HIST];
    bw_hist(d, now, h);
    int got = 0, a = 0, b = 0;
    for (int i = BW_HIST - 1; i >= BW_HIST - 20 && got < 6; i--) {
        if (h[i] == BW_NO_RSSI) continue;
        if (got < 3) a += h[i]; else b += h[i];
        got++;
    }
    if (got < 6) return 0;
    const int diff = (a - b) / 3;
    return diff >= 4 ? 1 : diff <= -4 ? -1 : 0;
}

const char *bw_label(const bw_dev_t *d, char *buf, size_t cap)
{
    const bw_ad_t *a = &d->ad;
    if (a->name[0]) { snprintf(buf, cap, "%s", a->name); return buf; }
    if (a->cls == BW_C_FINDMY) {
        /* Status bits 4-5 as AirGuard reads them (uncertain): which kind of
           Find My device. Shown with a question mark. */
        static const char *const kind[4] = {"Apple device?", "AirTag?", "AirPods?", "Find My tag?"};
        snprintf(buf, cap, "%s", kind[(a->status >> 4) & 3]);
        return buf;
    }
    if (a->cls == BW_C_APPLE_PP) {
        /* Model numbers from community continuity notes (uncertain). */
        const char *m = a->model == 0x0220 ? "AirPods" : a->model == 0x0F20 ? "AirPods 2" :
                        a->model == 0x0E20 ? "AirPods Pro" : a->model == 0x0A20 ? "AirPods Max" :
                        a->model == 0x0055 ? "AirTag setup?" : "Apple audio?";
        snprintf(buf, cap, "%s", m);
        return buf;
    }
    if (a->cls == BW_C_TILE) { snprintf(buf, cap, "Tile"); return buf; }
    if (a->cls == BW_C_SMARTTAG) { snprintf(buf, cap, "SmartTag"); return buf; }
    if (a->cls == BW_C_FMDN) { snprintf(buf, cap, "Find My Device"); return buf; }
    if (a->cls == BW_C_DULT) { snprintf(buf, cap, "Tracker (DULT)"); return buf; }
    if (a->cls == BW_C_CHIPOLO) { snprintf(buf, cap, "Chipolo"); return buf; }
    if (a->cls == BW_C_FASTPAIR) { snprintf(buf, cap, "Fast Pair %06lx", (unsigned long)a->model); return buf; }
    if (a->cls == BW_C_IBEACON) { snprintf(buf, cap, "iBeacon"); return buf; }
    if (a->cls == BW_C_EDDYSTONE) {
        snprintf(buf, cap, "Eddystone %s", a->sub == 0 ? "UID" : a->sub == 0x10 ? "URL" : a->sub == 0x20 ? "TLM" :
                 a->sub == 0x30 ? "EID" : "");
        return buf;
    }
    const char *c = a->company != 0xFFFF ? bw_company(a->company) : NULL;
    if (c) snprintf(buf, cap, "%s", c);
    else if (a->company != 0xFFFF) snprintf(buf, cap, "co 0x%04x", a->company);
    else if (a->n_uuid) snprintf(buf, cap, "svc %04x", a->uuid16[0]);
    else snprintf(buf, cap, "(no name)");
    return buf;
}

/* Metres on the ground between two points; an equirectangular sum is plenty
   at walking distances and needs only the float maths the board exports. */
float bw_distance_m(double lat1, double lon1, double lat2, double lon2)
{
    const float k = 0.01745329252f;
    const float mid = (float)((lat1 + lat2) * 0.5) * k;
    const float dx = (float)(lon2 - lon1) * k * cosf(mid);
    const float dy = (float)(lat2 - lat1) * k;
    return 6371000.0f * sqrtf(dx * dx + dy * dy);
}

int bw_csv(const bw_dev_t *d, const char *event, uint32_t now, char *out, size_t cap)
{
    char a[18], label[32];
    bw_addr_text(d->addr, a);
    bw_label(d, label, sizeof(label));
    for (char *p = label; *p; p++) if (*p == ',' || *p == '"') *p = ' ';
    return snprintf(out, cap, "%lu,%s,%s,%s,%s,%04x,%d,%d,%lu,%d,%u,%s\n", (unsigned long)(now / 1000u), event, a,
                    d->addr_type & 1 ? "random" : "public", bw_class_tag(d->ad.cls), d->ad.company, d->rssi,
                    bw_window(d, now, 5), (unsigned long)(bw_span_ms(d) / 1000u), (int)d->moved_m, d->rotations, label);
}
