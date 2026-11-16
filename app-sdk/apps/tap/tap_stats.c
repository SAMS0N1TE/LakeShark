/* TAP: statistics. Everything is counted as each packet arrives, at a fixed
   cost, so the tables stay right when the store ages packets out. */
#include "tap.h"

const tap_expert_kind_t tap_expert_kind[X_COUNT] = {
    {SEV_ERROR, "Malformed or short frame", "bad"},
    {SEV_ERROR, "Deauth/disassoc flood, 10+/s", "deauth"},
    {SEV_WARN, "Same SSID, different security", "beacon"},
    {SEV_WARN, "Beacon: BSSID changed its SSID", "beacon"},
    {SEV_WARN, "Beacon: BSSID changed security", "beacon"},
    {SEV_WARN, "Deauth or disassociation", "deauth"},
    {SEV_NOTE, "Beacon: BSSID changed channel", "beacon"},
    {SEV_NOTE, "Retransmission (retry bit)", "wlan.fc.retry == 1"},
    {SEV_NOTE, "Frame cut to the snap length", "cut"},
    {SEV_CHAT, "EAPOL 4-way handshake complete", "eapol"},
    {SEV_CHAT, "EAPOL key message", "eapol"}};

static void hexmac(char *out, size_t cap, const uint8_t *a)
{
    snprintf(out, cap, "%02x:%02x:%02x:%02x:%02x:%02x", a[0], a[1], a[2], a[3], a[4], a[5]);
}
static void expert(tap_stats_t *s, int kind, const tap_pkt_t *p, const char *fmt, ...)
{
    tap_expert_t *x = &s->x[kind];
    x->n++; x->last_no = p->no;
    if (!fmt) return;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(x->note, sizeof(x->note), fmt, ap);
    va_end(ap);
}
static int endpoint(tap_stats_t *s, const char *addr, int radio, uint32_t ms)
{
    if (!addr[0] || !strcmp(addr, "Broadcast")) return -1;
    for (int i = 0; i < s->eps; i++) if (s->ep[i].radio == radio && !strcmp(s->ep[i].addr, addr)) return i;
    if (s->eps >= TAP_EPS) { s->ep_over++; return -1; }
    tap_ep_t *e = &s->ep[s->eps];
    memset(e, 0, sizeof(*e));
    snprintf(e->addr, sizeof(e->addr), "%s", addr);
    e->radio = (uint8_t)radio;
    e->first_ms = ms;
    e->rssi_min = 127; e->rssi_max = -128;
    return s->eps++;
}
/* The I/O graph: a packet into its bucket; a full graph folds in half. */
static void io_add(tap_stats_t *s, int radio, uint32_t ms)
{
    if (!s->io_step_ms) s->io_step_ms = 1000;
    uint32_t i = ms / s->io_step_ms;
    while (i >= TAP_IO) {
        for (int r = 0; r < R_COUNT; r++) {
            for (int k = 0; k < TAP_IO / 2; k++) {
                const uint32_t v = (uint32_t)s->io[r][2 * k] + s->io[r][2 * k + 1];
                s->io[r][k] = (uint16_t)(v > 0xFFFFu ? 0xFFFFu : v);
            }
            memset(&s->io[r][TAP_IO / 2], 0, sizeof(s->io[r][0]) * (TAP_IO / 2));
        }
        s->io_step_ms *= 2;
        i = ms / s->io_step_ms;
    }
    if (s->io[radio][i] < 0xFFFFu) s->io[radio][i]++;
    if ((int)i + 1 > s->io_n) s->io_n = (int)i + 1;
}
/* Beacons: what each BSSID says about itself, and when that changes. */
static void beacon(tap_stats_t *s, const tap_pkt_t *p, const tap_dis_t *d)
{
    tap_bss_t *b = NULL, *spare = NULL;
    char m[20];
    hexmac(m, sizeof(m), d->bss);
    for (int i = 0; i < TAP_BSS && !b; i++) {
        if (!s->bss[i].used) { if (!spare) spare = &s->bss[i]; }
        else if (!memcmp(s->bss[i].bss, d->bss, 6)) b = &s->bss[i];
    }
    if (!b) {
        if (!spare) return;
        /* a new access point with a name another one has, secured differently */
        if (d->name[0])
            for (int i = 0; i < TAP_BSS; i++)
                if (s->bss[i].used && !strcmp(s->bss[i].ssid, d->name) && strcmp(s->bss[i].sec, d->sec) &&
                    (!strcmp(s->bss[i].sec, "open") || !strcmp(d->sec, "open"))) {
                    expert(s, X_TWIN, p, "%.20s: %s is %s", d->name, m + 9, d->sec);
                    break;
                }
        memset(spare, 0, sizeof(*spare));
        spare->used = 1;
        memcpy(spare->bss, d->bss, 6);
        snprintf(spare->ssid, sizeof(spare->ssid), "%s", d->name);
        snprintf(spare->sec, sizeof(spare->sec), "%s", d->sec);
        spare->ch = d->ds_ch;
        return;
    }
    if (d->name[0] && b->ssid[0] && strcmp(b->ssid, d->name)) expert(s, X_BCN_SSID, p, "%s: %.14s > %.14s", m + 9, b->ssid, d->name);
    if (d->sec[0] && b->sec[0] && strcmp(b->sec, d->sec)) expert(s, X_BCN_SEC, p, "%s: %s > %s", m, b->sec, d->sec);
    if (d->ds_ch && b->ch && b->ch != d->ds_ch) expert(s, X_BCN_CH, p, "%s: channel %u > %u", m, b->ch, d->ds_ch);
    if (d->name[0]) snprintf(b->ssid, sizeof(b->ssid), "%s", d->name);
    if (d->sec[0]) snprintf(b->sec, sizeof(b->sec), "%s", d->sec);
    if (d->ds_ch) b->ch = d->ds_ch;
}
/* The 4-way handshake per access point and station. */
static void handshake(tap_stats_t *s, const tap_pkt_t *p, const tap_dis_t *d)
{
    expert(s, X_EAPOL, p, NULL);
    if (!d->eapol_msg || !(d->have & H_BSS)) return;
    const uint8_t *sta = !memcmp(d->sa, d->bss, 6) ? d->da : d->sa;
    tap_hs_t *h = NULL, *spare = NULL;
    for (int i = 0; i < TAP_HS && !h; i++) {
        if (!s->hs[i].used) { if (!spare) spare = &s->hs[i]; }
        else if (!memcmp(s->hs[i].ap, d->bss, 6) && !memcmp(s->hs[i].sta, sta, 6)) h = &s->hs[i];
    }
    if (!h) {
        if (!spare) spare = &s->hs[p->no % TAP_HS];
        h = spare;
        memset(h, 0, sizeof(*h));
        h->used = 1;
        memcpy(h->ap, d->bss, 6); memcpy(h->sta, sta, 6);
    }
    if (d->eapol_msg == 1 && h->mask == 15) h->mask = 0;        /* a new handshake */
    const uint8_t before = h->mask;
    h->mask |= (uint8_t)(1u << (d->eapol_msg - 1));
    if (h->mask == 15 && before != 15) {
        char a[20], b[20];
        hexmac(a, sizeof(a), h->ap); hexmac(b, sizeof(b), h->sta);
        expert(s, X_HS_OK, p, "AP %s STA %s", a, b);
    }
    snprintf(s->x[X_EAPOL].note, sizeof(s->x[X_EAPOL].note), "message %u of 4 last", d->eapol_msg);
}
void tap_stats_add(tap_stats_t *s, const tap_pkt_t *p, const tap_dis_t *d)
{
    if (!s->total) s->t0 = p->ts_us;
    const uint32_t ms = p->ts_us > s->t0 ? (uint32_t)((p->ts_us - s->t0) / 1000u) : 0;
    if (ms > s->last_ms) s->last_ms = ms;
    s->total++; s->bytes += p->orig;
    s->pk[d->leaf]++; s->by[d->leaf] += p->orig;
    io_add(s, p->radio, ms);
    if (p->radio == R_WIFI) s->wifi_ch[p->ch >= 1 && p->ch <= 14 ? p->ch : 0]++;
    if (p->radio == R_BLE) s->ble_ch[p->ch >= 37 && p->ch <= 39 ? p->ch - 37 : 3]++;
    if (p->radio != R_DEC) {
        int bin = (p->rssi + 100) / 10 + 1;
        if (p->rssi < -100) bin = 0;
        s->rssi[p->radio][bin < 0 ? 0 : bin > 8 ? 8 : bin]++;
    }
    /* expert information */
    if (d->tags & T_BAD) expert(s, X_BAD, p, "%.50s", d->info);
    if (d->tags & T_CUT) expert(s, X_CUT, p, "%u of %u bytes kept", p->len, p->orig);
    if (p->radio == R_WIFI) {
        s->wifi_frames++;
        if (d->tags & T_RETRY) expert(s, X_RETRY, p, "%u%% of 802.11 frames", (unsigned)((s->x[X_RETRY].n + 1) * 100u / s->wifi_frames));
        if (d->tags & T_DEAUTH) {
            expert(s, X_DEAUTH, p, "%s, reason %u, to %.17s", d->fc_sub == 10 ? "disassoc" : "deauth", d->reason, d->dst);
            const uint32_t sec = ms / 1000u;
            if (sec != s->flood_sec || !s->flood_n) { s->flood_sec = sec; s->flood_n = 0; }
            if (++s->flood_n >= 10) {
                if (s->flood_n > s->flood_peak) s->flood_peak = s->flood_n;
                s->x[X_FLOOD].n += s->flood_n == 10 ? 10 : 1;
                s->x[X_FLOOD].last_no = p->no;
                snprintf(s->x[X_FLOOD].note, sizeof(s->x[X_FLOOD].note), "peak %u a second, BSSID %s", (unsigned)s->flood_peak, d->bssid);
            }
        }
        if ((d->tags & T_BEACON) && d->fc_sub == 8 && (d->have & H_BSS) && !(d->tags & T_BAD)) beacon(s, p, d);
        if (d->tags & T_EAPOL) handshake(s, p, d);
    }
    const int a = endpoint(s, d->src, p->radio, ms), b = endpoint(s, d->dst, p->radio, ms);
    if (a >= 0) {
        tap_ep_t *e = &s->ep[a];
        e->tx++; e->bytes += p->orig; e->rssi = p->rssi; e->last_ms = ms;
        if (p->rssi < e->rssi_min) e->rssi_min = p->rssi;
        if (p->rssi > e->rssi_max) e->rssi_max = p->rssi;
        if (d->name[0] && (d->tags & (T_BEACON | T_BLE | T_MESHCORE))) snprintf(e->label, sizeof(e->label), "%.19s", d->name);
    }
    if (b >= 0) { s->ep[b].rx++; s->ep[b].bytes += p->orig; s->ep[b].last_ms = ms; }
    if (a >= 0 && b >= 0) {
        int i = 0;
        for (; i < s->cvs; i++)
            if ((s->cv[i].a == a && s->cv[i].b == b) || (s->cv[i].a == b && s->cv[i].b == a)) break;
        if (i == s->cvs) {
            if (s->cvs >= TAP_CVS) { s->cv_over++; return; }
            s->cv[s->cvs++] = (tap_cv_t){.a = (uint8_t)a, .b = (uint8_t)b, .first_ms = ms};
        }
        tap_cv_t *c = &s->cv[i];
        c->pk++; c->bytes += p->orig; c->last_ms = ms; c->rssi = p->rssi;
        if (c->a == a) c->ab++; else c->ba++;
    }
}
