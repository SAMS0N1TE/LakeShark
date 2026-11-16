/* WIFI WATCH engine: frames in, alerts out. Fixed tables, bounded work per
   frame (each lookup is a scan of at most 64 entries), no drawing, no
   radio calls. Key material is never copied: EAPOL-Key frames are reduced
   to "message n seen" and "a PMKID is present". */
#include "watch.h"

const char *const watch_kind_name[K_COUNT] = {"DEAUTH", "DISASSOC", "FLOOD", "TWIN", "HIDDEN", "HANDSHAKE", "PMKID"};

void watch_init(watch_t *w)
{
    memset(w, 0, sizeof(*w));
    w->cfg.burst_frames = 10;
    w->cfg.burst_ms = 2000;
    w->cfg.flood_new = 30;
    w->cfg.flood_ms = 3000;
}

static void mac(char *o, const uint8_t *a)
{
    if (lap_dot11_is_broadcast(a)) snprintf(o, 18, "broadcast");
    else lap_dot11_mac(o, 18, a);
}
static int say(watch_t *w, uint32_t now, int sev, int kind, uint8_t ch, const uint8_t *who, const uint8_t *whom, const char *fmt, ...)
{
    watch_alert_t *a = &w->alert[w->nalerts % W_ALERTS];
    memset(a, 0, sizeof(*a));
    a->ms = now; a->unix = w->unix_now;
    a->sev = (uint8_t)sev; a->kind = (uint8_t)kind; a->channel = ch;
    if (who) mac(a->who, who);
    if (whom) mac(a->whom, whom);
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(a->text, sizeof(a->text), fmt, ap);
    va_end(ap);
    w->nalerts++;
    w->by_kind[kind]++;
    return 1;
}
const watch_alert_t *watch_alert(const watch_t *w, uint32_t i)
{
    if (i >= watch_alerts_kept(w)) return NULL;
    return &w->alert[(w->nalerts - 1 - i) % W_ALERTS];
}
uint32_t watch_alerts_kept(const watch_t *w) { return w->nalerts < W_ALERTS ? w->nalerts : W_ALERTS; }

/* The least recently heard entry of a full table: the one to reuse. */
static int oldest(const void *base, size_t stride, int n, size_t at)
{
    int o = 0;
    uint32_t best = 0;
    for (int i = 0; i < n; i++) {
        uint32_t v;
        memcpy(&v, (const uint8_t *)base + (size_t)i * stride + at, sizeof(v));
        if (!i || v < best) { best = v; o = i; }
    }
    return o;
}
#define OLDEST(arr, n, field) oldest((arr), sizeof((arr)[0]), (n), offsetof(__typeof__((arr)[0]), field))

static watch_ap_t *ap_find(watch_t *w, const uint8_t *b)
{
    for (int i = 0; i < w->naps; i++) if (!memcmp(w->ap[i].bssid, b, 6)) return &w->ap[i];
    return NULL;
}
static void flood_tick(watch_t *w, uint32_t now)
{
    const uint32_t sec = now / 1000u;
    if (sec - w->new_sec >= 8u) memset(w->new_bss, 0, sizeof(w->new_bss));
    else for (uint32_t s = w->new_sec + 1; s <= sec; s++) w->new_bss[s % 8u] = 0;
    w->new_sec = sec;
}
static int new_bss(watch_t *w, uint32_t now, uint8_t ch)
{
    flood_tick(w, now);
    w->new_bss[w->new_sec % 8u]++;
    const unsigned span = w->cfg.flood_ms / 1000u ? w->cfg.flood_ms / 1000u : 1u;
    unsigned sum = 0;
    for (unsigned s = 0; s < span && s < 8u; s++) sum += w->new_bss[(w->new_sec - s) % 8u];
    if (sum >= w->cfg.flood_new && (!w->flood_said_ms || now - w->flood_said_ms >= 30000u)) {
        w->flood_said_ms = now;
        return say(w, now, SEV_ALERT, K_FLOOD, ch, NULL, NULL, "Beacon flood: %u new BSSIDs in %u s", sum, span);
    }
    return 0;
}
/* A beacon or probe response: the network table, hidden names, twins. */
static int network(watch_t *w, const lap_dot11_t *f, int8_t rssi, uint8_t ch, uint32_t now)
{
    int raised = 0;
    watch_ap_t *a = ap_find(w, f->bssid);
    const uint8_t chan = f->channel ? f->channel : ch;
    if (!a) {
        if (f->subtype == LAP_DOT11_PROBE_RESP && !f->ssid_len) return 0;
        raised += new_bss(w, now, chan);
        if (w->naps < W_APS) a = &w->ap[w->naps++];
        else { w->aps_over++; a = &w->ap[OLDEST(w->ap, w->naps, last_ms)]; }
        memset(a, 0, sizeof(*a));
        memcpy(a->bssid, f->bssid, 6);
        a->first_ms = now;
        a->sec = f->security;
        a->hidden = f->subtype == LAP_DOT11_BEACON && f->ssid_hidden;
        if (!f->ssid_hidden) snprintf(a->ssid, sizeof(a->ssid), "%s", f->ssid);
    }
    a->last_ms = now; a->rssi = rssi; a->channel = chan;
    if (f->subtype == LAP_DOT11_BEACON) a->beacons++;
    /* a hidden network named by its own probe response */
    if (a->hidden && !a->revealed && f->subtype == LAP_DOT11_PROBE_RESP && f->ssid_len && !f->ssid_hidden) {
        a->revealed = true;
        snprintf(a->ssid, sizeof(a->ssid), "%s", f->ssid);
        raised += say(w, now, SEV_INFO, K_HIDDEN, chan, f->bssid, NULL, "Hidden network is \"%s\" (probe response)", a->ssid);
    }
    if (!a->ssid[0] && !f->ssid_hidden && f->ssid_len) snprintf(a->ssid, sizeof(a->ssid), "%s", f->ssid);
    /* the same BSSID now claiming other protection */
    if (f->security && a->sec && f->security != a->sec && !a->changed_said) {
        a->changed_said = true;
        raised += say(w, now, SEV_ALERT, K_TWIN, chan, f->bssid, NULL, "\"%s\" changed %s to %s on one BSSID",
                      a->ssid, lap_dot11_sec_name(a->sec), lap_dot11_sec_name(f->security));
    }
    if (f->security) a->sec = f->security;
    /* the same name from another BSSID with other protection */
    if (a->ssid[0] && !a->twin_said)
        for (int i = 0; i < w->naps; i++) {
            const watch_ap_t *b = &w->ap[i];
            if (b == a || !b->sec || !a->sec || b->sec == a->sec || strcmp(b->ssid, a->ssid)) continue;
            a->twin_said = true;
            const watch_ap_t *weak = a->sec < b->sec ? a : b, *strong = weak == a ? b : a;
            char sb[18];
            lap_dot11_mac(sb, sizeof(sb), strong->bssid);
            raised += say(w, now, SEV_ALERT, K_TWIN, chan, weak->bssid, strong->bssid, "Evil twin? \"%s\" %s here, %s on %s",
                          a->ssid, lap_dot11_sec_name(weak->sec), lap_dot11_sec_name(strong->sec), sb);
            break;
        }
    return raised;
}
static void probe(watch_t *w, const lap_dot11_t *f, int8_t rssi, uint32_t now)
{
    watch_client_t *c = NULL;
    for (int i = 0; i < w->ncl && !c; i++) if (!memcmp(w->cl[i].mac, f->ta, 6)) c = &w->cl[i];
    if (!c) {
        if (w->ncl < W_CLIENTS) c = &w->cl[w->ncl++];
        else { w->clients_over++; c = &w->cl[OLDEST(w->cl, w->ncl, last_ms)]; }
        memset(c, 0, sizeof(*c));
        memcpy(c->mac, f->ta, 6);
        c->random = lap_dot11_is_random(f->ta);
    }
    c->probes++; c->last_ms = now; c->rssi = rssi;
    if (!f->ssid_len || f->ssid_hidden) { c->wildcard = true; return; }
    for (int i = 0; i < c->nssid; i++) if (!strcmp(c->ssid[i], f->ssid)) return;
    if (c->nssid < 4) snprintf(c->ssid[c->nssid++], sizeof(c->ssid[0]), "%s", f->ssid);
}
static int kickoff(watch_t *w, const lap_dot11_t *f, uint8_t ch, uint32_t now)
{
    const bool dis = f->subtype == LAP_DOT11_DISASSOC;
    watch_pair_t *p = NULL;
    for (int i = 0; i < w->npairs && !p; i++)
        if (!memcmp(w->pair[i].src, f->ta, 6) && !memcmp(w->pair[i].dst, f->ra, 6) && w->pair[i].disassoc == dis) p = &w->pair[i];
    if (!p) {
        p = w->npairs < W_PAIRS ? &w->pair[w->npairs++] : &w->pair[OLDEST(w->pair, w->npairs, last_ms)];
        memset(p, 0, sizeof(*p));
        memcpy(p->src, f->ta, 6); memcpy(p->dst, f->ra, 6);
        p->disassoc = dis;
        p->window_ms = now;
    }
    p->total++; p->last_ms = now; p->reason = f->reason; p->channel = ch;
    if (now - p->window_ms > w->cfg.burst_ms) {
        p->rate = (uint16_t)(p->window * 1000u / (now - p->window_ms ? now - p->window_ms : 1u));
        p->window = 0; p->window_ms = now;
    }
    p->window++;
    const uint32_t span = now - p->window_ms;
    if (p->window >= w->cfg.burst_frames) {
        p->rate = (uint16_t)(p->window * 1000u / (span > 250u ? span : 250u));
        if (!p->said_ms || now - p->said_ms >= 10000u) {
            p->said_ms = now;
            char to[18];
            mac(to, f->ra);
            return say(w, now, SEV_ALERT, dis ? K_DISASSOC : K_DEAUTH, ch, f->ta, f->ra, "%s burst to %s: %u in %u ms, reason %u %s",
                       dis ? "Disassoc" : "Deauth", to, (unsigned)p->window, (unsigned)span, f->reason, lap_dot11_reason_name(f->reason));
        }
    }
    return 0;
}
static int handshake(watch_t *w, const lap_dot11_t *f, uint8_t ch, uint32_t now)
{
    if (!f->have_bssid) return 0;
    const uint8_t *sta = memcmp(f->a1, f->bssid, 6) ? f->a1 : f->a2;
    watch_shake_t *s = NULL;
    for (int i = 0; i < w->nshakes && !s; i++) if (!memcmp(w->shake[i].ap, f->bssid, 6) && !memcmp(w->shake[i].sta, sta, 6)) s = &w->shake[i];
    if (!s) {
        s = w->nshakes < W_SHAKES ? &w->shake[w->nshakes++] : &w->shake[OLDEST(w->shake, w->nshakes, last_ms)];
        memset(s, 0, sizeof(*s));
        memcpy(s->ap, f->bssid, 6); memcpy(s->sta, sta, 6);
    }
    s->last_ms = now;
    if (f->eapol_msg) s->msgs |= (uint8_t)(1u << (f->eapol_msg - 1));
    int raised = 0;
    if (f->eapol_pmkid && !s->pmkid_said) {
        s->pmkid = s->pmkid_said = true;
        raised += say(w, now, SEV_WARN, K_PMKID, ch, f->bssid, sta, "PMKID present in message 1 (noted, not kept)");
    }
    if (!s->said && ((s->msgs & 0x0F) == 0x0F || (s->msgs & 0x06) == 0x06)) {
        s->said = true;
        raised += say(w, now, SEV_WARN, K_HANDSHAKE, ch, f->bssid, sta, "4-way handshake seen (messages %s%s%s%s)",
                      s->msgs & 1 ? "1" : "", s->msgs & 2 ? "2" : "", s->msgs & 4 ? "3" : "", s->msgs & 8 ? "4" : "");
    }
    return raised;
}

int watch_frame(watch_t *w, const uint8_t *d, size_t n, int8_t rssi, uint8_t ch, uint32_t now)
{
    lap_dot11_t f;
    w->frames++;
    if (!lap_dot11_parse(d, n, &f)) { w->short_frames++; return 0; }
    if (f.type == LAP_DOT11_CTRL) { w->ctrl++; return 0; }
    if (f.type == LAP_DOT11_DATA) {
        w->data++;
        if (!f.eapol) return 0;
        w->eapol++;
        return handshake(w, &f, ch, now);
    }
    if (f.type != LAP_DOT11_MGMT || f.naddr < 3) return 0;
    w->mgmt++;
    if (f.protect) w->protected_mgmt++;
    switch (f.subtype) {
    case LAP_DOT11_BEACON: w->beacons++; return network(w, &f, rssi, ch, now);
    case LAP_DOT11_PROBE_RESP: w->proberesp++; return network(w, &f, rssi, ch, now);
    case LAP_DOT11_PROBE_REQ: w->probes++; probe(w, &f, rssi, now); return 0;
    case LAP_DOT11_DEAUTH: w->deauth++; return kickoff(w, &f, ch, now);
    case LAP_DOT11_DISASSOC: w->disassoc++; return kickoff(w, &f, ch, now);
    case LAP_DOT11_AUTH: w->auth++; return 0;
    case LAP_DOT11_ASSOC_REQ: case LAP_DOT11_REASSOC_REQ: {
        w->assoc++;
        /* a station naming the hidden network it joins */
        watch_ap_t *a = ap_find(w, f.bssid);
        if (a && a->hidden && !a->revealed && f.ssid_len && !f.ssid_hidden) {
            a->revealed = true;
            snprintf(a->ssid, sizeof(a->ssid), "%s", f.ssid);
            return say(w, now, SEV_INFO, K_HIDDEN, ch, f.bssid, f.ta, "Hidden network is \"%s\" (association)", a->ssid);
        }
        return 0;
    }
    default: return 0;
    }
}

/* ---- alert delivery -------------------------------------------------------- */

void watch_prefs_default(watch_prefs_t *p)
{
    memset(p, 0, sizeof(*p));
    p->magic = WATCH_PREFS_MAGIC;
    p->size = (uint16_t)sizeof(*p);
    p->version = 1;
    p->mesh_to = MESH_OFF;              /* nothing goes on air until the owner says so */
    p->mesh_min_sev = SEV_ALERT;
    p->mesh_gap_min = 10;
    p->mesh_per_hour = 6;
    p->out[SEV_INFO] = OUT_NONE;
    p->out[SEV_WARN] = OUT_NONE;        /* handshakes go past whenever anyone joins */
    p->out[SEV_ALERT] = OUT_BOTH;
    p->banner = BANNER_OFF;
    p->quiet_from = 22;
    p->quiet_until = 7;
    p->quiet_mesh = 1;
}
bool watch_prefs_load(watch_prefs_t *p, const void *data, size_t len)
{
    watch_prefs_t in;
    watch_prefs_default(p);
    if (!data || len != sizeof(in)) return false;
    memcpy(&in, data, sizeof(in));
    if (in.magic != WATCH_PREFS_MAGIC || in.size != sizeof(in) || in.version != 1) return false;
    in.mesh_to = in.mesh_to < MESH_COUNT ? in.mesh_to : MESH_OFF;
    in.mesh_min_sev = in.mesh_min_sev <= SEV_ALERT ? in.mesh_min_sev : SEV_ALERT;
    in.mesh_gap_min = in.mesh_gap_min < 1 ? 1 : in.mesh_gap_min > WATCH_GAP_MIN_MAX ? WATCH_GAP_MIN_MAX : in.mesh_gap_min;
    in.mesh_per_hour = in.mesh_per_hour < 1 ? 1 : in.mesh_per_hour > WATCH_PER_HOUR_MAX ? WATCH_PER_HOUR_MAX : in.mesh_per_hour;
    for (int i = 0; i < 3; i++) if (in.out[i] >= OUT_COUNT) in.out[i] = OUT_NONE;
    in.speak = in.speak ? 1 : 0;
    in.banner = in.banner < BANNER_COUNT ? in.banner : BANNER_OFF;
    in.quiet_on = in.quiet_on ? 1 : 0;
    in.quiet_from %= 24; in.quiet_until %= 24;
    in.quiet_mesh = in.quiet_mesh ? 1 : 0;
    in.dm_id[16] = 0; in.dm_name[19] = 0;
    /* a node id is 16 hex digits; anything else is no node */
    bool hex = strlen(in.dm_id) == 16;
    for (int i = 0; hex && i < 16; i++) {
        const char c = in.dm_id[i];
        hex = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
    }
    if (!hex) { in.dm_id[0] = 0; in.dm_name[0] = 0; if (in.mesh_to == MESH_DM) in.mesh_to = MESH_OFF; }
    *p = in;
    return true;
}
bool watch_quiet(const watch_prefs_t *p, int hour)
{
    if (!p->quiet_on || hour < 0) return false;
    hour %= 24;
    if (p->quiet_from == p->quiet_until) return true;
    if (p->quiet_from < p->quiet_until) return hour >= p->quiet_from && hour < p->quiet_until;
    return hour >= p->quiet_from || hour < p->quiet_until;       /* across midnight */
}

void watch_relay_init(watch_relay_t *r, uint8_t min_sev, uint16_t gap_min, uint8_t per_hour)
{
    memset(r, 0, sizeof(*r));
    r->min_sev = min_sev;
    r->gap_min = gap_min ? gap_min : 1;
    r->per_hour = per_hour ? (per_hour > W_RELAY_RING ? W_RELAY_RING : per_hour) : 1;
}
bool watch_relay_note(watch_relay_t *r, const watch_alert_t *a)
{
    if (!a || a->sev < r->min_sev || a->kind >= K_COUNT) return false;
    watch_relay_kind_t *k = &r->k[a->kind];
    if (!k->pending) k->first_ms = a->ms;
    else r->coalesced++;
    if (k->pending < 0xFFFFu) k->pending++;
    k->latest = *a;
    r->noted++;
    return true;
}
static uint32_t relay_hour(const watch_relay_t *r, uint32_t now)
{
    uint32_t n = 0;
    const uint32_t kept = r->nsent < W_RELAY_RING ? r->nsent : W_RELAY_RING;
    for (uint32_t i = 0; i < kept; i++) {
        if (now - r->sent_ms[(r->nsent - 1 - i) % W_RELAY_RING] >= 3600000u) break;   /* newest first: the rest are older */
        n++;
    }
    return n;
}
static uint32_t relay_gap_left(const watch_relay_t *r, const watch_relay_kind_t *k, uint32_t now)
{
    const uint32_t gap = (uint32_t)r->gap_min * 60000u;
    if (!k->ever || now - k->last_ms >= gap) return 0;
    return gap - (now - k->last_ms);
}
int watch_relay_next(const watch_relay_t *r, uint32_t now)
{
    if (relay_hour(r, now) >= r->per_hour) return -1;
    int best = -1;
    for (int i = 0; i < K_COUNT; i++) {
        const watch_relay_kind_t *k = &r->k[i];
        if (!k->pending || relay_gap_left(r, k, now)) continue;
        if (best < 0 || (int32_t)(k->first_ms - r->k[best].first_ms) < 0) best = i;
    }
    return best;
}
void watch_relay_sent(watch_relay_t *r, int kind, uint32_t now)
{
    if (kind < 0 || kind >= K_COUNT) return;
    watch_relay_kind_t *k = &r->k[kind];
    k->last_ms = now; k->ever = true; k->pending = 0;
    r->sent_ms[r->nsent % W_RELAY_RING] = now;
    r->nsent++;
}
uint32_t watch_relay_waiting(const watch_relay_t *r)
{
    uint32_t n = 0;
    for (int i = 0; i < K_COUNT; i++) n += r->k[i].pending;
    return n;
}
uint32_t watch_relay_wait_ms(const watch_relay_t *r, uint32_t now)
{
    uint32_t best = 0;
    bool any = false;
    for (int i = 0; i < K_COUNT; i++) {
        if (!r->k[i].pending) continue;
        const uint32_t left = relay_gap_left(r, &r->k[i], now);
        if (!any || left < best) best = left;
        any = true;
    }
    if (!any) return 0;
    if (relay_hour(r, now) >= r->per_hour) {
        /* the cap frees a slot when the per_hour-th newest send ages out of the hour */
        const uint32_t t = r->sent_ms[(r->nsent - (uint32_t)r->per_hour) % W_RELAY_RING];
        const uint32_t free_in = 3600000u - (now - t);
        if (free_in > best) best = free_in;
    }
    return best;
}

/* The first quoted name in an alert's text: the SSID, for twins and hidden networks. */
static void quoted(char *o, size_t cap, const char *t)
{
    o[0] = 0;
    const char *a = strchr(t, '"');
    if (!a) return;
    const char *b = strchr(a + 1, '"');
    if (!b) return;
    size_t n = (size_t)(b - a - 1);
    if (n > 20) n = 20;                    /* short: a long name is cut */
    snprintf(o, cap, "\"%.*s\"", (int)n, a + 1);
}
/* "02:bb:00:00:00:10" -> "..00:00:10"; "broadcast" -> "bcast". */
static void short_mac(char *o, size_t cap, const char *m)
{
    if (!strcmp(m, "broadcast")) snprintf(o, cap, "bcast");
    else if (strlen(m) == 17) snprintf(o, cap, "..%s", m + 9);
    else snprintf(o, cap, "%s", m);
}
int watch_mesh_text(char *out, size_t cap, const watch_alert_t *a, const char *kind_name, unsigned count, const char *hhmm)
{
    if (!out || !cap) return -1;
    char x[16] = "", name[26] = "", whom[20] = "", ch[16] = "";
    if (count > 1) snprintf(x, sizeof(x), " x%u", count);
    if (a) {
        quoted(name, sizeof(name), a->text);
        if (a->whom[0]) { whom[0] = '>'; short_mac(whom + 1, sizeof(whom) - 1, a->whom); }
        if (a->channel) snprintf(ch, sizeof(ch), " ch%u", a->channel);
    }
    const char *kind = kind_name ? kind_name : a && a->kind < K_COUNT ? watch_kind_name[a->kind] : "ALERT";
    return snprintf(out, cap, "WIFI WATCH %s%s%s%s%s%s%s%s%s%s", kind, x, name[0] ? " " : "", name,
                    a && a->who[0] ? " " : "", a ? a->who : "", whom, ch, hhmm && hhmm[0] ? " " : "", hhmm ? hhmm : "");
}
