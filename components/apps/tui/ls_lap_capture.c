/* API 2.7: raw Wi-Fi and Bluetooth capture. Receive only: nothing here
 * transmits. All state is one PSRAM block; the rings are PSRAM heap, held
 * only while a capture is open.
 *
 * Wi-Fi is the coprocessor behind esp_hosted. esp_hosted 2.12.9 keeps
 * esp_wifi_remote_set_promiscuous* inside a comment in esp_hosted_api.c, its
 * slave has no handler for Req_WifiSetPromiscuous and no path that carries
 * monitor frames to the host, so esp_wifi_remote's weak stub answers
 * ESP_ERR_NOT_SUPPORTED. That is reported as LAP_CAPTURE_NO_TRANSPORT. The
 * calls stay so a coprocessor firmware that forwards frames works unchanged.
 *
 * The simulator replays the pcapng named by LAPSIM_CAPTURE. It never makes
 * frames up. */
#include "ls_lap_capture.h"
#include "../../../app-sdk/include/lakeshark_app.h"
#include "ls_lap_api.h"
#include "ls_lap_context.h"
#include "ls_lap_radio.h"
#include "ls_lap_sys.h"
#include <stdatomic.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CAP_SLOTS 256u
#define CAP_BLE_BYTES 255u
#define CAP_PEND (LAP_CAPTURE_SNAP_MAX + 64u)

typedef struct {
    atomic_bool lock;
    bool open;
    uint8_t source, filter, channel, replay;
    unsigned owner;
    uint8_t *ring;                      /* CAP_SLOTS x (meta + snap), then CAP_PEND for the replay */
    uint8_t *pend;
    uint32_t snap, head, tail, sequence, dropped, frames;
    int32_t reason;
    /* Wi-Fi tuning */
    uint8_t channels[LAP_WIFI_HOP_MAX], count, at, home, retuned;
    uint32_t dwell_ms, hop_at;
    /* replay */
    void *file;
    uint64_t first_us, start_ms;
    uint32_t tsdiv[8];
    uint16_t link[8];
    uint8_t links, have_first, pending;
    lap_capture_meta_t next;
} cap_t;

static LS_LAP_POOL cap_t caps[2];

static cap_t *cap_of(int h) { return h == LAP_CAPTURE_WIFI || h == LAP_CAPTURE_BLE ? &caps[h - 1] : NULL; }
static bool take(cap_t *c) { return !atomic_exchange(&c->lock, true); }
static void give(cap_t *c) { atomic_store(&c->lock, false); }
static size_t slot_bytes(const cap_t *c) { return sizeof(lap_capture_meta_t) + c->snap; }

static void push(cap_t *c, lap_capture_meta_t *m, const uint8_t *data, size_t n)
{
    if (!c->open) return;
    if (!take(c)) { c->dropped++; return; }
    if (c->open && c->ring) {
        if (c->head - c->tail >= CAP_SLOTS) c->dropped++;
        else {
            uint8_t *slot = c->ring + (size_t)(c->head % CAP_SLOTS) * slot_bytes(c);
            if (n > c->snap) n = c->snap;
            m->size = sizeof(*m); m->version = 1;
            m->length = (uint16_t)n;
            m->sequence = ++c->sequence;
            m->dropped = c->dropped;
            m->source = c->source;
            memcpy(slot, m, sizeof(*m));
            if (n) memcpy(slot + sizeof(*m), data, n);
            c->head++; c->frames++;
        }
    }
    give(c);
}

#if defined(ESP_PLATFORM) && !defined(LS_LAP_HOST_TEST)
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "ls_wireless.h"
static EXT_RAM_BSS_ATTR ls_wireless_snapshot_t wl;
static void *ring_alloc(size_t n) { return heap_caps_malloc(n, MALLOC_CAP_SPIRAM); }
static void ring_free(void *p) { heap_caps_free(p); }
static uint32_t now_ms(void) { return (uint32_t)(esp_timer_get_time() / 1000); }
static void nap(void) { vTaskDelay(1); }

static void wifi_rx(void *buf, wifi_promiscuous_pkt_type_t type)
{
    cap_t *c = &caps[0];
    if (!c->open || !buf) return;
    const wifi_promiscuous_pkt_t *p = buf;
    const uint8_t kind = type == WIFI_PKT_MGMT ? LAP_WIFI_MGMT : type == WIFI_PKT_CTRL ? LAP_WIFI_CTRL :
                         type == WIFI_PKT_DATA ? LAP_WIFI_DATA : 0;
    if (!kind || !(c->filter & kind)) return;
    static const uint8_t rate[16] = {2, 4, 11, 22, 0, 4, 11, 22, 96, 48, 24, 12, 108, 72, 36, 18};
    unsigned len = p->rx_ctrl.sig_len;
    if (len >= 4) len -= 4;                         /* the FCS */
    lap_capture_meta_t m = {.timestamp_us = (uint64_t)esp_timer_get_time(), .original_length = (uint16_t)len,
        .rssi = (int8_t)p->rx_ctrl.rssi, .channel = (uint8_t)p->rx_ctrl.channel, .kind = kind,
        .rate_500kbps = rate[p->rx_ctrl.rate & 15]};
    push(c, &m, p->payload, len);
}

static int wifi_open(cap_t *c, const lap_wifi_capture_config_t *cfg)
{
    ls_wireless_get(&wl);
    if (!wl.wifi_available) { c->reason = LAP_CAPTURE_NO_RADIO; return LAP_IO_ENOTSUP; }
    if (cfg->channel_count && wl.wifi_connected && !(cfg->flags & LAP_CAPTURE_DROP_LINK)) {
        c->reason = LAP_CAPTURE_LINK_IN_USE; return LAP_IO_EBUSY;
    }
    wifi_promiscuous_filter_t f = {.filter_mask = (c->filter & LAP_WIFI_MGMT ? WIFI_PROMIS_FILTER_MASK_MGMT : 0) |
        (c->filter & LAP_WIFI_CTRL ? WIFI_PROMIS_FILTER_MASK_CTRL : 0) |
        (c->filter & LAP_WIFI_DATA ? WIFI_PROMIS_FILTER_MASK_DATA : 0)};
    esp_err_t e = esp_wifi_set_promiscuous_rx_cb(wifi_rx);
    if (e == ESP_OK) e = esp_wifi_set_promiscuous_filter(&f);
    if (e == ESP_OK) e = esp_wifi_set_promiscuous(true);
    if (e != ESP_OK) {
        c->reason = e == ESP_ERR_NOT_SUPPORTED ? LAP_CAPTURE_NO_TRANSPORT : LAP_CAPTURE_NO_RADIO;
        return LAP_IO_ENOTSUP;
    }
    uint8_t primary = 0; wifi_second_chan_t second;
    if (esp_wifi_get_channel(&primary, &second) == ESP_OK) c->home = c->channel = primary;
    if (c->count) {
        c->retuned = 1;
        c->channel = c->channels[0];
        esp_wifi_set_channel(c->channel, WIFI_SECOND_CHAN_NONE);
        c->hop_at = now_ms();
    }
    return 0;
}
static void wifi_close(cap_t *c)
{
    esp_wifi_set_promiscuous(false);
    esp_wifi_set_promiscuous_rx_cb(NULL);
    if (c->retuned && c->home) esp_wifi_set_channel(c->home, WIFI_SECOND_CHAN_NONE);
}
/* On the app's task, from its poll: one retune per dwell, no timer or task. */
static void service(cap_t *c)
{
    if (c->source == LAP_CAPTURE_BLE) {
        if (now_ms() - c->hop_at >= 1000) { c->hop_at = now_ms(); ls_wireless_find_ble(true); }
        return;
    }
    if (c->count > 1 && now_ms() - c->hop_at >= c->dwell_ms) {
        c->hop_at = now_ms();
        c->at = (uint8_t)((c->at + 1) % c->count);
        c->channel = c->channels[c->at];
        esp_wifi_set_channel(c->channel, WIFI_SECOND_CHAN_NONE);
    }
}
static int ble_open(cap_t *c)
{
    if (!(ls_lap_sys_caps() & LAP_HW_BLE)) { c->reason = LAP_CAPTURE_NO_RADIO; return LAP_IO_ENOTSUP; }
    ls_wireless_find_ble(true);
    c->hop_at = now_ms();
    return 0;
}
static void ble_close(cap_t *c) { (void)c; }       /* ls_lap_sys_end settles the scanner */

void ls_lap_capture_ble(const uint8_t addr[6], uint8_t addr_type, uint8_t pdu, int rssi,
                        const uint8_t *data, size_t n, int64_t at_us)
{
    cap_t *c = &caps[1];
    if (!c->open || (pdu < 8 && !(c->filter & (1u << pdu)))) return;
    lap_capture_meta_t m = {.timestamp_us = (uint64_t)at_us, .original_length = (uint16_t)n,
        .rssi = (int8_t)rssi, .kind = pdu, .addr_type = addr_type};
    memcpy(m.addr, addr, 6);
    push(c, &m, data, n);
}
#else   /* ---- the simulator: replay a pcapng, never invent ------------------ */
static void *ring_alloc(size_t n) { return malloc(n); }
static void ring_free(void *p) { free(p); }
extern uint32_t (*ls_lap_api_host_clock)(void);
static uint32_t now_ms(void) { return ls_lap_api_host_clock ? ls_lap_api_host_clock() : 0; }
static void nap(void) {}
static uint32_t le32(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }
static unsigned le16(const uint8_t *p) { return p[0] | p[1] << 8; }

/* One radiotap record to a frame. False when it cannot be read. */
static bool from_radiotap(const uint8_t *p, size_t n, lap_capture_meta_t *m, const uint8_t **data, size_t *len)
{
    if (n < 8 || p[0]) return false;
    const size_t hl = le16(p + 2);
    if (hl < 8 || hl > n) return false;
    uint32_t present = le32(p + 4);
    size_t at = 8;
    for (uint32_t more = present; more & 0x80000000u; at += 4) { if (at + 4 > hl) return false; more = le32(p + at); }
    uint8_t flags = 0;
    if (present & 1) { at = (at + 7) & ~(size_t)7; at += 8; }
    if (present & 2) { if (at < hl) flags = p[at]; at++; }
    if (present & 4) { if (at < hl) m->rate_500kbps = p[at]; at++; }
    if (present & 8) {
        at = (at + 1) & ~(size_t)1;
        if (at + 4 <= hl) {
            const unsigned f = le16(p + at);
            m->channel = (uint8_t)(f == 2484 ? 14 : f >= 5000 ? (f - 5000) / 5 : f >= 2412 ? (f - 2407) / 5 : 0);
        }
        at += 4;
    }
    if (present & 16) at += 2;
    if ((present & 32) && at < hl) m->rssi = (int8_t)p[at];
    size_t body = n - hl;
    if ((flags & 0x10) && body >= 4) body -= 4;     /* FCS at end */
    if (body < 2) return false;
    const unsigned type = (p[hl] >> 2) & 3;
    m->kind = type == 0 ? LAP_WIFI_MGMT : type == 1 ? LAP_WIFI_CTRL : LAP_WIFI_DATA;
    *data = p + hl; *len = body;
    return true;
}
/* One LINKTYPE_BLUETOOTH_LE_LL_WITH_PHDR record to an advertising report. */
static bool from_ble_ll(const uint8_t *p, size_t n, lap_capture_meta_t *m, const uint8_t **data, size_t *len)
{
    static const int8_t hci[8] = {LAP_BLE_ADV_IND, LAP_BLE_ADV_DIRECT_IND, LAP_BLE_ADV_NONCONN_IND, -1,
                                  LAP_BLE_SCAN_RSP, -1, LAP_BLE_ADV_SCAN_IND, LAP_BLE_ADV_EXT_IND};
    if (n < 10 + 4 + 2 + 6) return false;
    const uint8_t rf = p[0];
    if (rf != 0 && rf != 12 && rf != 39) return false;          /* advertising channels only */
    if (le32(p + 10) != 0x8E89BED6u) return false;
    const uint8_t *pdu = p + 14;
    const int kind = hci[pdu[0] & 7];
    size_t body = pdu[1];
    if (kind < 0 || (pdu[0] & 8) || body < 6 || 16 + body > n) return false;
    m->channel = rf == 0 ? 37 : rf == 12 ? 38 : 39;
    m->rssi = (p[8] & 2) ? (int8_t)p[1] : 0;
    m->kind = (uint8_t)kind;
    m->addr_type = (pdu[0] >> 6) & 1;
    memcpy(m->addr, pdu + 2, 6);
    *data = pdu + 8; *len = body - 6;
    return true;
}
/* Read on to the next record of this source; false at the end of the file. */
static bool replay_next(cap_t *c)
{
    FILE *f = c->file;
    uint8_t h[28];
    while (f && fread(h, 1, 8, f) == 8) {
        const uint32_t type = le32(h), total = le32(h + 4);
        if (total < 12 || (total & 3)) break;
        long body = (long)total - 8;
        if (type == 1 && body >= 12) {                          /* interface description */
            if (fread(h, 1, 8, f) != 8) break;
            body -= 8;
            if (c->links < 8) {
                c->link[c->links] = (uint16_t)le16(h);
                c->tsdiv[c->links] = 1;                         /* microseconds unless if_tsresol says so */
                long left = body - 4;
                while (left >= 4) {
                    uint8_t o[4];
                    if (fread(o, 1, 4, f) != 4) return false;
                    body -= 4; left -= 4;
                    const unsigned code = le16(o), ol = le16(o + 2), pad = (ol + 3u) & ~3u;
                    if (!code || (long)pad > left) break;
                    uint8_t v[4] = {0};
                    if (pad <= 4) { if (fread(v, 1, pad, f) != pad) return false; }
                    else if (fseek(f, (long)pad, SEEK_CUR)) return false;
                    body -= (long)pad; left -= (long)pad;
                    if (code == 9 && ol == 1 && v[0] == 9) c->tsdiv[c->links] = 1000;   /* nanoseconds */
                    if (code == 9 && ol == 1 && v[0] == 3) c->tsdiv[c->links] = 0;      /* milliseconds */
                }
                c->links++;
            }
        } else if (type == 6 && body >= 24) {                   /* enhanced packet */
            if (fread(h, 1, 20, f) != 20) break;
            body -= 20;
            const uint32_t ifc = le32(h), caplen = le32(h + 12);
            const uint64_t raw = (uint64_t)le32(h + 4) << 32 | le32(h + 8);
            const unsigned want = c->source == LAP_CAPTURE_WIFI ? 127 : 256;
            if (!c->have_first && ifc < c->links) {             /* the file's clock starts at its first record */
                c->have_first = 1;
                c->first_us = c->tsdiv[ifc] ? raw / c->tsdiv[ifc] : raw * 1000;
            }
            if (ifc < c->links && c->link[ifc] == want && caplen <= CAP_PEND && (long)caplen <= body - 4) {
                if (fread(c->pend, 1, caplen, f) != caplen) break;
                body -= (long)caplen;
                lap_capture_meta_t m = {0};
                const uint8_t *data = NULL; size_t len = 0;
                const bool ok = want == 127 ? from_radiotap(c->pend, caplen, &m, &data, &len)
                                            : from_ble_ll(c->pend, caplen, &m, &data, &len);
                const unsigned bit = want == 127 ? m.kind : 1u << m.kind;
                if (ok && (c->filter & bit)) {
                    m.timestamp_us = c->tsdiv[ifc] ? raw / c->tsdiv[ifc] : raw * 1000;
                    m.original_length = (uint16_t)(len + (le32(h + 16) - caplen));
                    m.length = (uint16_t)len;
                    memmove(c->pend, data, len);
                    c->next = m;
                    if (fseek(f, body, SEEK_CUR)) return false;
                    return true;
                }
            }
        }
        if (fseek(f, body, SEEK_CUR)) break;
    }
    return false;
}
static int replay_open(cap_t *c)
{
    const char *path = getenv("LAPSIM_CAPTURE");
    uint8_t h[12];
    FILE *f = path && path[0] ? fopen(path, "rb") : NULL;
    if (!f || fread(h, 1, 12, f) != 12 || le32(h) != 0x0A0D0D0Au || le32(h + 8) != 0x1A2B3C4Du ||
        fseek(f, (long)le32(h + 4), SEEK_SET)) {
        if (f) fclose(f);
        c->reason = LAP_CAPTURE_NO_FILE;
        return LAP_IO_ENOTSUP;
    }
    c->file = f; c->replay = 1; c->links = 0; c->have_first = 0;
    c->start_ms = now_ms();
    c->pending = replay_next(c);
    return 0;
}
static int wifi_open(cap_t *c, const lap_wifi_capture_config_t *cfg)
{
    if (!(ls_lap_sys_caps() & LAP_HW_WIFI)) { c->reason = LAP_CAPTURE_NO_RADIO; return LAP_IO_ENOTSUP; }
    const char *joined = getenv("LAPSIM_CAPTURE_JOINED");   /* the board is on a network */
    if (cfg->channel_count && joined && joined[0] == '1' && !(cfg->flags & LAP_CAPTURE_DROP_LINK)) {
        c->reason = LAP_CAPTURE_LINK_IN_USE; return LAP_IO_EBUSY;
    }
    return replay_open(c);
}
static int ble_open(cap_t *c)
{
    if (!(ls_lap_sys_caps() & LAP_HW_BLE)) { c->reason = LAP_CAPTURE_NO_RADIO; return LAP_IO_ENOTSUP; }
    return replay_open(c);
}
static void replay_close(cap_t *c) { if (c->file) fclose(c->file); c->file = NULL; }
static void wifi_close(cap_t *c) { replay_close(c); }
static void ble_close(cap_t *c) { replay_close(c); }
/* Records whose time has come move into the ring, as a radio would fill it. */
static void service(cap_t *c)
{
    const char *pace = getenv("LAPSIM_CAPTURE_PACE");
    const bool paced = !pace || pace[0] != '0';
    while (c->pending) {
        if (paced && (c->next.timestamp_us - c->first_us) / 1000 > now_ms() - c->start_ms) return;
        if (c->head - c->tail >= CAP_SLOTS) return;             /* a file waits; nothing is lost */
        lap_capture_meta_t m = c->next;
        if (m.channel) c->channel = m.channel;
        push(c, &m, c->pend, m.length);
        c->pending = replay_next(c);
    }
    if (c->file && c->head == c->tail) c->reason = LAP_CAPTURE_REPLAY_END;
}
void ls_lap_capture_ble(const uint8_t addr[6], uint8_t addr_type, uint8_t pdu, int rssi,
                        const uint8_t *data, size_t n, int64_t at_us)
{ (void)addr; (void)addr_type; (void)pdu; (void)rssi; (void)data; (void)n; (void)at_us; }
#endif

static const char *why(int reason)
{
    switch (reason) {
    case LAP_CAPTURE_RUNNING: return "capturing";
    case LAP_CAPTURE_NO_RADIO: return "this radio is absent or switched off";
    case LAP_CAPTURE_NO_TRANSPORT: return "the Wi-Fi coprocessor firmware does not forward monitor frames";
    case LAP_CAPTURE_LINK_IN_USE: return "the board is on a Wi-Fi network; hopping would drop it";
    case LAP_CAPTURE_BUSY: return "another app is capturing";
    case LAP_CAPTURE_NO_MEMORY: return "no memory for the capture ring";
    case LAP_CAPTURE_NO_FILE: return "simulator: set LAPSIM_CAPTURE to a pcapng file to replay";
    case LAP_CAPTURE_REPLAY_END: return "simulator: the capture file has been replayed";
    default: return "stopped";
    }
}

static int start(int source, uint8_t filter, uint32_t snap, const lap_wifi_capture_config_t *wifi)
{
    cap_t *c = cap_of(source);
    if (!(ls_lap_granted() & LAP_CAP_RADIO_RX)) return LAP_IO_EACCES;
    if (c->open) {
        if (c->owner != ls_lap_context_index()) return LAP_IO_EBUSY;
        lap_capture_stop(source);
        if (c->open) return LAP_IO_EBUSY;
    }
    const bool held = atomic_load(&c->lock);
    memset(c, 0, sizeof(*c));
    atomic_store(&c->lock, held);
    c->source = (uint8_t)source; c->filter = filter; c->snap = snap;
    c->owner = ls_lap_context_index();
    c->reason = LAP_CAPTURE_STOPPED;
    if (wifi) {
        c->count = wifi->channel_count;
        memcpy(c->channels, wifi->channels, sizeof(c->channels));
        c->dwell_ms = wifi->dwell_ms ? wifi->dwell_ms : 250;
    }
    c->ring = ring_alloc((size_t)CAP_SLOTS * slot_bytes(c) + CAP_PEND);
    if (!c->ring) { c->reason = LAP_CAPTURE_NO_MEMORY; return LAP_IO_ELIMIT; }
    c->pend = c->ring + (size_t)CAP_SLOTS * slot_bytes(c);
    c->open = true;                                 /* the callback may run before open returns */
    const int e = wifi ? wifi_open(c, wifi) : ble_open(c);
    if (e) {
        c->open = false;
        for (int i = 0; i < 50 && !take(c); i++) nap();
        ring_free(c->ring); c->ring = NULL;
        give(c);
        return e;
    }
    c->reason = LAP_CAPTURE_RUNNING;
    return source;
}

int lap_wifi_capture_start(const lap_wifi_capture_config_t *cfg)
{
    if (!ls_lap_api_inside() || !ls_lap_api_readable(cfg, sizeof(*cfg)) || cfg->size != sizeof(*cfg) ||
        cfg->version != 1 || cfg->types > 7 || cfg->channel_count > LAP_WIFI_HOP_MAX ||
        (cfg->flags & ~LAP_CAPTURE_DROP_LINK) || (cfg->dwell_ms && (cfg->dwell_ms < 50 || cfg->dwell_ms > 5000)) ||
        (cfg->snap_bytes && (cfg->snap_bytes < 64 || cfg->snap_bytes > LAP_CAPTURE_SNAP_MAX)))
        return LAP_IO_EINVAL;
    for (unsigned i = 0; i < cfg->channel_count; i++) {
        const unsigned ch = cfg->channels[i];
        if (!((ch >= 1 && ch <= 14) || (ch >= 36 && ch <= 177))) return LAP_IO_EINVAL;
    }
    return start(LAP_CAPTURE_WIFI, cfg->types ? cfg->types : 7, cfg->snap_bytes ? cfg->snap_bytes : 512, cfg);
}
int lap_ble_capture_start(const lap_ble_capture_config_t *cfg)
{
    if (!ls_lap_api_inside() || !ls_lap_api_readable(cfg, sizeof(*cfg)) || cfg->size != sizeof(*cfg) ||
        cfg->version != 1 || cfg->flags)
        return LAP_IO_EINVAL;
    return start(LAP_CAPTURE_BLE, cfg->pdu_types ? cfg->pdu_types : 0xFF, CAP_BLE_BYTES, NULL);
}
int lap_capture_read(int h, void *data, size_t cap, lap_capture_meta_t *meta)
{
    cap_t *c = cap_of(h);
    if (!ls_lap_api_inside() || !c || !ls_lap_api_readable(meta, sizeof(*meta)) ||
        (cap && !ls_lap_api_readable(data, cap)))
        return LAP_IO_EINVAL;
    if (!c->open || c->owner != ls_lap_context_index()) return LAP_IO_ECLOSED;
    service(c);
    if (!take(c)) return LAP_IO_EAGAIN;
    int r = LAP_IO_EAGAIN;
    if (c->head != c->tail) {
        const uint8_t *slot = c->ring + (size_t)(c->tail % CAP_SLOTS) * slot_bytes(c);
        memcpy(meta, slot, sizeof(*meta));
        if (meta->length > cap) meta->length = (uint16_t)cap;
        if (meta->length) memcpy(data, slot + sizeof(*meta), meta->length);
        r = meta->length;
        c->tail++;
    }
    give(c);
    return r;
}
int lap_capture_status(int h, lap_capture_status_t *out)
{
    cap_t *c = cap_of(h);
    if (!ls_lap_api_inside() || !c || !ls_lap_api_readable(out, sizeof(*out))) return LAP_IO_EINVAL;
    const bool mine = c->owner == ls_lap_context_index();
    if (c->open && mine) service(c);
    memset(out, 0, sizeof(*out));
    out->size = sizeof(*out); out->version = 1;
    out->source = (uint8_t)h;
    out->reason = c->open && !mine ? LAP_CAPTURE_BUSY : c->source ? c->reason : LAP_CAPTURE_STOPPED;
    if (c->open && mine) {
        out->frames = c->frames; out->dropped = c->dropped; out->queued = c->head - c->tail;
        out->channel = c->channel; out->replay = c->replay; out->hopping = c->count > 1;
    }
    snprintf(out->why, sizeof(out->why), "%s", why(out->reason));
    return 0;
}
static void stop(cap_t *c)
{
    if (!c->open) return;
    c->open = false;                                /* producers stop at their next look */
    if (c->source == LAP_CAPTURE_WIFI) wifi_close(c); else ble_close(c);
    for (int i = 0; i < 50 && !take(c); i++) nap();
    ring_free(c->ring); c->ring = NULL;
    c->head = c->tail = 0;
    c->reason = LAP_CAPTURE_STOPPED;
    give(c);
}
void lap_capture_stop(int h)
{
    cap_t *c = cap_of(h);
    if (ls_lap_api_inside() && c && c->owner == ls_lap_context_index()) stop(c);
}
void ls_lap_capture_end(void)
{
    for (int i = 0; i < 2; i++)
        if (caps[i].open && caps[i].owner == ls_lap_context_index()) stop(&caps[i]);
}
