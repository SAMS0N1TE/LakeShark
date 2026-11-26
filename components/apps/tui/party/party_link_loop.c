/* PARTY's loopback link (party_link.h): the air as two queues in memory.
   For lssim and the bench only; the board build does not carry it. The
   rates are those of the board's modes, so the airtime budget and the
   transfer times the screen shows are the ones a real party would see. */
#include "party_link.h"

#include <string.h>

#define RXQ 32
#define TXQ 64

typedef struct { uint8_t d[PARTY_DGRAM_MAX]; uint8_t len; int8_t rssi, snr; bool bulk; } slot_t;

static slot_t s_rx[RXQ], s_tx[TXQ];
static int s_rx_h, s_rx_n, s_tx_h, s_tx_n;
static bool s_open, s_lr = true;
static party_link_cfg_t s_cfg;
static party_link_stats_t s_st;

static uint32_t airtime(int len, bool bulk)
{
    if (bulk && s_cfg.mode != PARTY_MODE_LONG) {
        if (s_lr && (s_cfg.bulk_caps & PARTY_CAP_FLRC)) return 2 + (uint32_t)len * 8 / 650;   /* FLRC 650 kb/s */
        return 3 + (uint32_t)len * 8 / 50;                                                    /* GFSK 50 kb/s */
    }
    if (s_cfg.mode == PARTY_MODE_LONG) return 120 + (uint32_t)len * 8000 / 537;  /* LoRa SF11 BW125 */
    if (s_cfg.mode == PARTY_MODE_FAST) return 8 + (uint32_t)len * 8000 / 21875;  /* LoRa SF7 BW500 */
    return 25 + (uint32_t)len * 8000 / 1760;                                     /* LoRa SF9 BW125 */
}

static bool lp_open(const party_link_cfg_t *cfg)
{
    s_cfg = *cfg;
    s_open = true;
    s_rx_h = s_rx_n = s_tx_h = s_tx_n = 0;
    memset(&s_st, 0, sizeof(s_st));
    return true;
}
static void lp_reconfig(const party_link_cfg_t *cfg) { s_cfg = *cfg; }
static void lp_close(void) { s_open = false; }

static bool lp_send(const uint8_t *d, int len, uint8_t dst, bool bulk)
{
    (void)dst;
    if (!s_open || len <= 0 || len > PARTY_DGRAM_MAX) { s_st.tx_fail++; return false; }
    if (s_tx_n == TXQ) { s_tx_h = (s_tx_h + 1) % TXQ; s_tx_n--; }     /* nobody is reading: drop the oldest */
    slot_t *s = &s_tx[(s_tx_h + s_tx_n++) % TXQ];
    memcpy(s->d, d, (size_t)len);
    s->len = (uint8_t)len;
    s->bulk = bulk;
    s_st.tx++;
    return true;
}

static bool lp_poll(party_rx_t *out)
{
    if (!s_open || !s_rx_n) return false;
    const slot_t *s = &s_rx[s_rx_h];
    s_rx_h = (s_rx_h + 1) % RXQ;
    s_rx_n--;
    memcpy(out->data, s->d, s->len);
    out->len = s->len;
    out->rssi = s->rssi;
    out->snr = s->snr;
    out->via = PARTY_VIA_LOOP;
    s_st.rx++;
    return true;
}

static void lp_stats(party_link_stats_t *out)
{
    s_st.up = s_open;
    s_st.radio = s_lr ? "LR2021" : "SX1262";
    s_st.why = s_open ? NULL : "not started";
    s_st.caps = PARTY_CAP_LORA | PARTY_CAP_GFSK | (s_lr ? PARTY_CAP_FLRC : 0);
    s_st.mtu = PARTY_DGRAM_MAX;
    s_st.bulk_via = s_cfg.mode == PARTY_MODE_LONG ? PARTY_VIA_LORA
                  : s_lr && (s_cfg.bulk_caps & PARTY_CAP_FLRC) ? PARTY_VIA_FLRC : PARTY_VIA_GFSK;
    s_st.beacon_bps = s_cfg.mode == PARTY_MODE_LONG ? 537 : s_cfg.mode == PARTY_MODE_FAST ? 21875 : 1760;
    s_st.bulk_bps = s_st.bulk_via == PARTY_VIA_FLRC ? 650000 : s_st.bulk_via == PARTY_VIA_GFSK ? 50000 : s_st.beacon_bps;
    *out = s_st;
}

static const party_link_ops_t OPS = {
    .open = lp_open, .reconfig = lp_reconfig, .close = lp_close, .send = lp_send,
    .poll = lp_poll, .stats = lp_stats, .airtime_ms = airtime,
};
const party_link_ops_t *party_link_loop(void) { return &OPS; }

bool party_loop_inject(const uint8_t *d, int len, int8_t rssi, int8_t snr)
{
    if (!s_open || len <= 0 || len > PARTY_DGRAM_MAX || s_rx_n == RXQ) return false;
    slot_t *s = &s_rx[(s_rx_h + s_rx_n++) % RXQ];
    memcpy(s->d, d, (size_t)len);
    s->len = (uint8_t)len;
    s->rssi = rssi;
    s->snr = snr;
    return true;
}

bool party_loop_take(uint8_t *d, int *len, bool *bulk)
{
    if (!s_tx_n) return false;
    const slot_t *s = &s_tx[s_tx_h];
    s_tx_h = (s_tx_h + 1) % TXQ;
    s_tx_n--;
    memcpy(d, s->d, s->len);
    *len = s->len;
    if (bulk) *bulk = s->bulk;
    return true;
}

void party_loop_radio(bool lr2021) { s_lr = lr2021; }
