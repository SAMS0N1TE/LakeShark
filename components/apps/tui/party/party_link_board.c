/* PARTY's link on the board (party_link.h), over ls_lsx (the LakeShark
   link) in its PARTY role: the engine holds the LoRa chip on the discovery
   channel the way a home base does (ls_mesh_radio_hold_svc: below every
   radio app and MeshCore, which take it back at once), encrypts and
   authenticates every frame with the party key, and carries PARTY's
   datagrams as broadcast datagrams on port PORT.

   Every PARTY datagram is a broadcast; party_proto's own header says who it
   is for. Files go the same way, as PARTY's CHUNK and ACK datagrams: an
   ls_lsx session (and with it GFSK and FLRC) is one master and one slave,
   which does not fit one sender and several receivers, so bulk stays on
   LoRa SF7/125 for now and the stats say so.

   Datagrams arrive on the link's task; a small ring hands them to PARTY's
   worker, which polls. */
#include "party_link.h"

#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_attr.h"

#include "../ls_lsx.h"
#include "../ls_lsx_svc.h"

#define PORT 0x50          /* 'P' */
#define RING 16

typedef struct { uint8_t d[PARTY_DGRAM_MAX]; uint8_t len; int8_t rssi, snr; } slot_t;

static EXT_RAM_BSS_ATTR slot_t s_ring[RING];
static int s_head, s_n;
static uint32_t s_dropped, s_rx;
static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
static party_link_cfg_t s_cfg;
static bool s_open;

/* On the link's task, under its lock: copy and go. */
static void on_rx(void *ctx, uint32_t src, uint8_t port, const uint8_t *data, int len, int8_t rssi, int8_t snr)
{
    (void)ctx; (void)src;
    if (port != PORT || len <= 0 || len > PARTY_DGRAM_MAX) return;
    taskENTER_CRITICAL(&s_mux);
    if (s_n == RING) { s_head = (s_head + 1) % RING; s_n--; s_dropped++; }
    slot_t *s = &s_ring[(s_head + s_n++) % RING];
    memcpy(s->d, data, (size_t)len);
    s->len = (uint8_t)len;
    s->rssi = rssi;
    s->snr = snr == 127 ? 0 : snr;
    s_rx++;
    taskEXIT_CRITICAL(&s_mux);
}

static bool bd_open(const party_link_cfg_t *cfg)
{
    s_cfg = *cfg;
    taskENTER_CRITICAL(&s_mux);
    s_head = s_n = 0;
    taskEXIT_CRITICAL(&s_mux);
    s_open = ls_lsx_svc_party(cfg->key, on_rx, NULL);
    return s_open;
}

/* LoRa on the discovery channel is the only mode a broadcast has: nothing to retune. */
static void bd_reconfig(const party_link_cfg_t *cfg) { s_cfg = *cfg; }

static void bd_close(void)
{
    if (s_open && ls_lsx_svc_role() == LSX_ROLE_PARTY) ls_lsx_svc_stop();
    s_open = false;
}

static bool bd_send(const uint8_t *d, int len, uint8_t dst, bool bulk)
{
    (void)dst; (void)bulk;
    return s_open && ls_lsx_svc_dgram(PORT, d, len) == 0;
}

static bool bd_poll(party_rx_t *out)
{
    bool got = false;
    taskENTER_CRITICAL(&s_mux);
    if (s_n) {
        const slot_t *s = &s_ring[s_head];
        memcpy(out->data, s->d, s->len);
        out->len = s->len;
        out->rssi = s->rssi;
        out->snr = s->snr;
        out->via = PARTY_VIA_LORA;
        s_head = (s_head + 1) % RING;
        s_n--;
        got = true;
    }
    taskEXIT_CRITICAL(&s_mux);
    return got;
}

static void bd_stats(party_link_stats_t *out)
{
    memset(out, 0, sizeof(*out));
    const uint32_t caps = ls_lsx_svc_caps();
    out->caps = (uint8_t)(((caps & LSX_CAP_LORA) ? PARTY_CAP_LORA : 0) | ((caps & LSX_CAP_GFSK) ? PARTY_CAP_GFSK : 0) |
                          ((caps & LSX_CAP_FLRC) ? PARTY_CAP_FLRC : 0));
    out->radio = !caps ? "NO LORA" : (caps & LSX_CAP_FLRC) ? "LR2021" : "SX1262";
    out->mtu = PARTY_DGRAM_MAX;
    out->bulk_via = PARTY_VIA_LORA;
    out->beacon_bps = out->bulk_bps = ls_lsx_mode_info(LSX_M_LORA)->net_bps;
    if (!s_open || ls_lsx_svc_role() != LSX_ROLE_PARTY) {
        out->why = s_open ? "link stopped" : ls_lsx_svc_why();
        return;
    }
    ls_lsx_status_t l;
    ls_lsx_svc_status(&l, NULL);
    out->up = true;
    out->why = l.held ? NULL : "radio busy";
    out->tx = l.frames_tx;
    out->rx = s_rx;
    out->rx_bad = l.frames_bad + s_dropped;
    out->air_ms_min = l.airtime_ms_60s;
}

static uint32_t bd_airtime(int len, bool bulk)
{
    (void)bulk;
    return ls_lsx_airtime_us(LSX_M_LORA, LSX_HDR + 1 + len + LSX_TAG) / 1000u + 1u;
}

static const party_link_ops_t OPS = {
    .open = bd_open, .reconfig = bd_reconfig, .close = bd_close, .send = bd_send,
    .poll = bd_poll, .stats = bd_stats, .airtime_ms = bd_airtime,
};
const party_link_ops_t *party_link_board(void) { return &OPS; }
