/* LR433: rtl_433 on the LR2021. Tyre pressure sensors at 315 MHz, weather
   sensors at 433.92 and 915 MHz and Itron ERT utility meters near 912.6 MHz,
   heard by the chip's own packet engines run as samplers and decoded with
   rtl_433's decoders (lr433_dec.c).

   A round robin over a small plan, each session held for the dwell time: an
   OOK session samples the on/off waveform several times a pulse, an FSK
   session samples mark and space four times a chip, and the ERT session
   reads the meters' Manchester chips at their own rate. Every device heard
   goes in a table, newest first, with the ID it carries (rtl_433 shows the
   same), its readings, the level it came in at, how often and how long ago.
   A burst that keyed the receiver and decoded as nothing is counted against
   the session that caught it, so a transmitter none of the decoders knows is
   still seen. On an LR2021 the PKT sessions stream I/Q instead, keep each
   burst and decode it as a CC1101 or SX1231 style 2-FSK packet: preamble,
   sync word and a CRC that checks (fsk_packet.c), at any rate the preamble
   gives. OFFSET moves every session off its catalogue frequency, for a
   transmitter that is not quite where its band says. Receive only:
   nothing here transmits or replays. */
#include "../ls_experiments.h"
#include "lr433_dec.h"
#include "lr433_history.h"
#include "fsk_packet.h"
#include "rec_ism.h"
#include "rec_watch.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_attr.h"
#include "esp_err.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "ls_lora.h"
#include "ls_textpack.h"
#ifdef ESP_PLATFORM
#include "lr_wf_session.h"
#include "ls_mesh.h"
#include <stdatomic.h>
#endif

extern const ls_experiment_t exp_lr433;

/* ----------------------------------------------------------------- plan -- */

typedef enum { BAND_315, BAND_433, BAND_915, BAND_ERT } band_t;

/* Not an lr433_in_t: the LR2021's I/Q, sliced and decoded here. */
#define KIND_IQ 9

typedef struct {
    const char *tag;       /* short name, in the PLAN line                    */
    const char *name;      /* the NOW line                                    */
    const char *what;      /* what it is for                                  */
    uint8_t band;
    uint8_t kind;          /* lr433_in_t                                      */
    uint32_t freq_hz;
    uint32_t rate;         /* decisions a second                              */
    uint32_t bw_hz;
    uint32_t dev_hz;       /* FSK: the deviation the demodulator expects      */
    /* FSK: the sync word, MSB first on air. OOK: the detector pattern, bit 0
       first on air. */
    uint16_t sync;
    uint8_t sync_bits;
    /* What goes back in front of the payload, in time order (the first of
       lead_bits is the top one): what the chip matched, and for PMV-107J the
       rest of the preamble it is known to have had. */
    uint32_t lead;
    uint8_t lead_bits;
    uint8_t frame;         /* payload bytes                                   */
} plan_t;

/* OOK: a quiet decision then 3 high ones, the start of any burst whose
   first pulse is at least three decisions long. The part takes a detector
   pattern only when its length is even and its first two chips differ
   (measured on the LR2021), so it cannot also ask for a run of quiet before
   the edge. FSK: 0x0F0F, two cycles of an
   alternating preamble four decisions a chip, which reads the same in either
   polarity. The 19.2k TPMS session hears the 20k ones too: the chip's clock
   recovery follows a few percent. */
static const plan_t PLAN[] = {
    { "315o", "315 OOK", "TPMS: Schrader, GM, Subaru", BAND_315, LR433_IN_OOK,
      315000000u, 40000, 200000, 0, 0x000E, 4, 0x7, 4, 252 },
    { "315f", "315 FSK", "TPMS: Ford, Toyota, Hyundai/Kia", BAND_315, LR433_IN_FSK,
      315000000u, 76800, 250000, 38000, 0x0F0F, 16, 0x0F0F, 16, 128 },
    { "315p", "315 PMV", "TPMS: Toyota/Lexus PMV-107J", BAND_315, LR433_IN_FSK,
      315000000u, 40000, 166667, 25000, 0xFFF0, 16, 0xFFFFFF0, 28, 72 },
    { "433o", "433 OOK", "Acurite, LaCrosse, Ambient F007", BAND_433, LR433_IN_OOK,
      433920000u, 16000, 250000, 0, 0x000E, 4, 0x7, 4, 252 },
    { "433f", "433 FSK", "TPMS: Citroen/VDO, Ford, Toyota", BAND_433, LR433_IN_FSK,
      433920000u, 76800, 250000, 38000, 0x0F0F, 16, 0x0F0F, 16, 128 },
    { "915f", "915 FSK", "Ambient/Ecowitt WH31, WH24/65", BAND_915, LR433_IN_FSK,
      915000000u, 68966, 250000, 35000, 0x0F0F, 16, 0x0F0F, 16, 112 },
    { "ert", "ERT", "Itron meters: SCM, SCM+, IDM", BAND_ERT, LR433_IN_ERT,
      912600000u, 32768, 2222222, 0, 0x2999, 14, 0x2665, 14, 252 },
    { "433p", "433 PKT", "FSK packets: CC1101, RFM69, Si443x", BAND_433, KIND_IQ,
      433920000u, 0, 0, 0, 0, 0, 0, 0, 0 },
    { "915p", "915 PKT", "FSK packets: CC1101, RFM69, Si443x", BAND_915, KIND_IQ,
      915000000u, 0, 0, 0, 0, 0, 0, 0, 0 },
};
#define N_PLAN ((int)(sizeof(PLAN) / sizeof(PLAN[0])))

/* Which entries run. */
typedef enum { SEL_ALL, SEL_315, SEL_433, SEL_915, SEL_ERT, SEL_COUNT } sel_t;
static const char *const SEL_NAMES[SEL_COUNT] = { "ALL", "315 TPMS", "433", "915", "ERT METERS" };
static const char *const SEL_ARGS[SEL_COUNT] = { "all", "315", "433", "915", "ert" };

static bool sel_has(int sel, const plan_t *p)
{
    switch (sel) {
    case SEL_315: return p->band == BAND_315;
    case SEL_433: return p->band == BAND_433;
    case SEL_915: return p->band == BAND_915;
    case SEL_ERT: return p->band == BAND_ERT;
    default:      return true;
    }
}

/* What the next start uses; words, so the console and OPTIONS can set them
   while the worker reads them. */
static volatile int s_sel = SEL_ALL;
static volatile int s_dwell_s = 10;
/* The OOK sessions' gain: the fixed top step, as the Mode S session runs it
   on air, or the part's AGC. */
static volatile int s_agc;
/* Every session tunes this far off its catalogue frequency. */
static volatile int32_t s_offset_hz;
#define OFFSET_MAX_KHZ 2000

static uint32_t tuned_hz(const plan_t *p) { return (uint32_t)((int64_t)p->freq_hz + s_offset_hz); }

/* The PKT sessions need the LR2021's I/Q stream; its extended receive
   range is what marks the part. */
static bool iq_capable(uint32_t caps) { return (caps & LS_LORA_CAP_RX_EXT) != 0; }

#define DWELL_MIN 2
#define DWELL_MAX 600
#define RETRY_US  1000000

/* ---------------------------------------------------------------- state -- */

#define MAX_DEVS 32

/* A device row for a decoded FSK packet: not one of rtl_433's protocols. */
#define PROTO_PACKET LR433_P_COUNT

typedef struct {
    bool used;
    uint8_t proto;
    int8_t channel, battery_ok;
    uint8_t plan;
    char id[16];
    float kpa, temp_c, humidity, rssi;
    int64_t consumption;
    char packet[16];      /* PROTO_PACKET: rate and CRC state */
    uint32_t count;
    int64_t last_us;
} heard_t;

typedef struct {
    uint32_t bursts;     /* captures that decoded as nothing */
    float last, max;
    int64_t last_us;
} unk_t;

/* What the readout shows, written by the worker under s_lock. */
typedef struct {
    int cur;                       /* the plan entry running, -1 none       */
    int64_t since_us;              /* when it began                         */
    bool on[N_PLAN];               /* in the running plan                   */
    bool refused[N_PLAN];          /* the chip would not start it last time */
    uint32_t caps[N_PLAN], decoded[N_PLAN];
    unk_t unk[N_PLAN];
    heard_t devs[MAX_DEVS];
    char fail[48];                 /* why the last session would not start  */
} view_t;

static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static EXT_RAM_BSS_ATTR view_t s_view;

/* The worker's own. */
static EXT_RAM_BSS_ATTR uint8_t s_buf[256];
static EXT_RAM_BSS_ATTR lr433_work_t s_work;
static EXT_RAM_BSS_ATTR lr433_msg_t s_msgs[8];
static bool s_on[N_PLAN];
static int s_n_on;
static int s_cur = -1;
static int64_t s_dwell_end, s_retry_at;

/* ------------------------------------------------------------ the table -- */

static bool same_device(const heard_t *d, const lr433_msg_t *m)
{
    return d->used && d->proto == m->proto && d->channel == m->channel && !strcmp(d->id, m->id);
}

/* Record one message. Under s_lock. */
static void note_msg(view_t *v, const lr433_msg_t *m, float rssi, int plan, int64_t now)
{
    int slot = -1, oldest = 0;
    for (int i = 0; i < MAX_DEVS; i++) {
        if (same_device(&v->devs[i], m)) { slot = i; break; }
        if (slot < 0 && !v->devs[i].used) slot = i;
        if (v->devs[i].last_us < v->devs[oldest].last_us) oldest = i;
    }
    if (slot < 0) slot = oldest;
    heard_t *d = &v->devs[slot];
    if (!same_device(d, m)) {
        memset(d, 0, sizeof(*d));
        d->used = true;
        d->proto = m->proto;
        d->channel = m->channel;
        d->battery_ok = -1;
        d->kpa = d->temp_c = d->humidity = NAN;
        d->consumption = -1;
        snprintf(d->id, sizeof(d->id), "%s", m->id);
    }
    /* A message carries what it carries: an Acurite 5-in-1 sends wind and
       rain in one and temperature in the next, and the table keeps both. */
    d->plan = (uint8_t)plan;
    if (lr433_proto_class(m->proto) == LR433_TPMS && isnan(m->kpa) && isnan(m->temp_c))
        d->kpa = d->temp_c = NAN;
    if (m->battery_ok >= 0) d->battery_ok = m->battery_ok;
    if (!isnan(m->kpa)) d->kpa = m->kpa;
    if (!isnan(m->temp_c)) d->temp_c = m->temp_c;
    if (!isnan(m->humidity)) d->humidity = m->humidity;
    if (m->consumption >= 0) d->consumption = m->consumption;
    d->rssi = rssi;
    d->count++;
    d->last_us = now;
}

/* One capture from plan entry `i`: decode it and file what it said. */
static void take(int i, const uint8_t *buf, int n, float rssi, int64_t now)
{
    const plan_t *p = &PLAN[i];
    const lr433_capture_t cap = {
        .kind = (lr433_in_t)p->kind,
        .rate = p->rate,
        .lead = p->lead,
        .lead_bits = p->lead_bits,
        .data = buf,
        .bits = n * 8,
    };
    const int k = lr433_decode(&cap, &s_work, s_msgs, (int)(sizeof(s_msgs) / sizeof(s_msgs[0])));
    for (int j = 0; j < k; j++) {
        /* Everyone else who wants decoded devices (METERS), before the
           TPMS readings are blanked for the table below. */
        const rec_ism_msg_t pub = { .m = s_msgs[j], .hz = tuned_hz(p), .rssi = rssi, .source = REC_SOURCE_SX1262 };
        rec_ism_publish(&pub, now);
        lr433_history_receive(&s_msgs[j]);
        if (lr433_proto_class(s_msgs[j].proto) == LR433_TPMS && !lr433_history_owned(&s_msgs[j]))
            s_msgs[j].kpa = s_msgs[j].temp_c = NAN;
    }
    portENTER_CRITICAL(&s_lock);
    s_view.caps[i]++;
    if (k > 0) {
        s_view.decoded[i]++;
        for (int j = 0; j < k; j++) note_msg(&s_view, &s_msgs[j], rssi, i, now);
    } else {
        unk_t *u = &s_view.unk[i];
        if (!isnan(rssi)) {
            if (!u->bursts || isnan(u->max) || rssi > u->max) u->max = rssi;
        } else if (!u->bursts) {
            u->max = NAN;
        }
        u->last = rssi;
        u->last_us = now;
        u->bursts++;
    }
    portEXIT_CRITICAL(&s_lock);
}

/* ---------------------------------------------------------- I/Q session -- */

#define IQ_RATE 125000u
#define IQ_PAIRS 32768u        /* 262 ms */
#define IQ_PRE 1250u           /* 10 ms kept from before the trigger */
#define IQ_EDGES 4096

#ifdef ESP_PLATFORM
static EXT_RAM_BSS_ATTR int16_t s_iq[2 * IQ_PAIRS];
static EXT_RAM_BSS_ATTR int32_t s_iq_edges[IQ_EDGES];
static EXT_RAM_BSS_ATTR fsk_pkt_t s_pkts[4];

/* A packet the PKT session decoded: its payload is its ID. Under no lock. */
static void take_packet(int i, const fsk_pkt_t *pk, float rssi, int64_t now)
{
    lr433_msg_t m;
    memset(&m, 0, sizeof(m));
    m.proto = PROTO_PACKET;
    m.channel = -1;
    m.battery_ok = -1;
    m.kpa = m.temp_c = m.humidity = m.rain_mm = m.wind_ms = NAN;
    m.consumption = -1;
    size_t h = 0;
    for (int k = 0; k < pk->len && h + 2 < sizeof(m.id); k++) h += (size_t)snprintf(m.id + h, sizeof(m.id) - h, "%02X", pk->data[k]);
    portENTER_CRITICAL(&s_lock);
    s_view.caps[i]++;
    s_view.decoded[i]++;
    note_msg(&s_view, &m, rssi, i, now);
    for (int k = 0; k < MAX_DEVS; k++) {
        heard_t *d = &s_view.devs[k];
        if (d->used && d->proto == PROTO_PACKET && !strcmp(d->id, m.id))
            snprintf(d->packet, sizeof(d->packet), "%lu %s", (unsigned long)(pk->baud_std ? pk->baud_std : pk->baud),
                     pk->crc_kind == FSK_CRC_NONE ? "noCRC" : "CRC ok");
    }
    portEXIT_CRITICAL(&s_lock);
}

/* A burst that keyed the PKT trigger and held no packet. */
static void take_nothing(int i, float rssi, int64_t now)
{
    portENTER_CRITICAL(&s_lock);
    s_view.caps[i]++;
    unk_t *u = &s_view.unk[i];
    if (!u->bursts || isnan(u->max) || rssi > u->max) u->max = rssi;
    u->last = rssi;
    u->last_us = now;
    u->bursts++;
    portEXIT_CRITICAL(&s_lock);
}

/* The consumer runs on the stream's own task: it finds a burst 10 dB over
   the quietest millisecond of its first 20, keeps 10 ms before it, and
   stops 30 ms after the burst ends or when the buffer is full. */
static EXT_RAM_BSS_ATTR int16_t s_iq_ring[2 * IQ_PRE];
static atomic_bool s_iq_full;
static size_t s_iq_used, s_ring_at, s_ring_n;
static unsigned s_warm, s_rise, s_quiet;
static double s_floor, s_power;
static bool s_trig, s_iq_on, s_held;

static void iq_reset(void)
{
    s_iq_used = s_ring_at = s_ring_n = 0;
    s_warm = s_rise = s_quiet = 0;
    s_floor = s_power = 0;
    s_trig = false;
    atomic_store(&s_iq_full, false);
}

static void iq_consume(void *ctx, const uint8_t *data, size_t bytes, bool spare)
{
    (void)ctx; (void)spare;
    if (!data || atomic_load(&s_iq_full)) return;
    for (size_t at = 0; at + 3 < bytes; at += 4) {
        const int16_t i = (int16_t)(data[at] | data[at + 1] << 8);
        const int16_t q = (int16_t)(data[at + 2] | data[at + 3] << 8);
        s_power += (double)i * i + (double)q * q;
        s_warm++;
        if (!s_trig) {
            s_iq_ring[2 * s_ring_at] = i;
            s_iq_ring[2 * s_ring_at + 1] = q;
            s_ring_at = (s_ring_at + 1) % IQ_PRE;
            if (s_ring_n < IQ_PRE) s_ring_n++;
        } else if (s_iq_used < IQ_PAIRS) {
            s_iq[2 * s_iq_used] = i;
            s_iq[2 * s_iq_used + 1] = q;
            s_iq_used++;
        }
        if (s_warm % 125) continue;          /* 1 ms windows */
        const double w = s_power / 125;
        s_power = 0;
        if (s_warm <= 2500) {
            if (s_warm == 125 || w < s_floor) s_floor = w;
            continue;
        }
        const double fl = s_floor > 1 ? s_floor : 1;
        if (!s_trig) {
            s_rise = w >= 10 * fl ? s_rise + 1 : 0;
            if (s_rise < 3) continue;
            s_trig = true;
            for (size_t k = 0; k < s_ring_n; k++) {
                const size_t r = (s_ring_at + IQ_PRE - s_ring_n + k) % IQ_PRE;
                s_iq[2 * s_iq_used] = s_iq_ring[2 * r];
                s_iq[2 * s_iq_used + 1] = s_iq_ring[2 * r + 1];
                s_iq_used++;
            }
            continue;
        }
        s_quiet = w < 3 * fl ? s_quiet + 1 : 0;
        if (s_quiet >= 30 || s_iq_used >= IQ_PAIRS) { atomic_store(&s_iq_full, true); return; }
    }
}

static esp_err_t iq_begin(const plan_t *p)
{
    if (!s_held) {
        const int64_t until = esp_timer_get_time() + 1000000;
        while (!ls_mesh_radio_hold(true) && esp_timer_get_time() < until) vTaskDelay(1);
        if (!ls_mesh_radio_held()) { ls_mesh_radio_hold(false); return ESP_ERR_INVALID_STATE; }
        s_held = true;
    }
    iq_reset();
    const esp_err_t err = lr_wf_start_consumer(tuned_hz(p), iq_consume, NULL);
    s_iq_on = err == ESP_OK;
    if (!s_iq_on) { ls_mesh_radio_hold(false); s_held = false; }
    return err;
}

static void iq_end(void)
{
    if (s_iq_on) {
        lr_wf_stats st;
        lr_wf_stop_stats(&st);
        s_iq_on = false;
    }
    if (s_held) { ls_mesh_radio_hold(false); s_held = false; }
}

/* A finished burst: decode it, file it, listen again. */
static void iq_poll(int i, int64_t now)
{
    if (!s_iq_on || !atomic_load(&s_iq_full)) return;
    lr_wf_stats st;
    lr_wf_stop_stats(&st);
    s_iq_on = false;
    const size_t pairs = s_iq_used;
    fsk_pkt_rf_t rf;
    const size_t ne = fsk_pkt_iq16_edges(s_iq, pairs, IQ_RATE, s_iq_edges, IQ_EDGES, &rf);
    const int k = ne ? fsk_pkt_from_edges(s_iq_edges, ne, s_pkts, 4) : 0;
    /* uncalibrated: the level over the floor, not dBm */
    const float level = rf.snr_db > 0 ? rf.snr_db : NAN;
    if (k > 0) for (int j = 0; j < k; j++) take_packet(i, &s_pkts[j], level, now);
    else take_nothing(i, level, now);
    iq_reset();
    s_iq_on = lr_wf_start_consumer(tuned_hz(&PLAN[i]), iq_consume, NULL) == ESP_OK;
}
#else
static esp_err_t iq_begin(const plan_t *p) { (void)p; return ESP_ERR_NOT_SUPPORTED; }
static void iq_end(void) {}
static void iq_poll(int i, int64_t now) { (void)i; (void)now; }
#endif

/* ------------------------------------------------------------- sessions -- */

static esp_err_t session_begin(int i)
{
    const plan_t *p = &PLAN[i];
    if (p->kind == KIND_IQ) return iq_begin(p);
    if (p->kind == LR433_IN_FSK) {
        const ls_fsk_cfg_t cfg = {
            .freq_hz = tuned_hz(p),
            .bitrate = p->rate,
            .deviation_hz = p->dev_hz,
            .bandwidth_hz = ls_lora_fsk_bw_snap(p->bw_hz),
            .sync_word = (uint32_t)p->sync << (32 - p->sync_bits),
            .payload_bytes = p->frame,
            .sync_bits = p->sync_bits,
        };
        return ls_lora_fsk_begin(&cfg);
    }
    const ls_ook_cfg_t cfg = {
        .freq_hz = tuned_hz(p),
        .bitrate = p->rate,
        .bandwidth_hz = p->bw_hz,
        .pattern = p->sync,
        .pattern_bits = p->sync_bits,
        .repeats = 0,
        .frame_bytes = p->frame,
        .gain_step = (uint8_t)(s_agc ? 0 : 13),
        .boost = -1,
    };
    return ls_lora_ook_begin(&cfg);
}

static void session_end(int i)
{
    if (i < 0) return;
    if (PLAN[i].kind == KIND_IQ) iq_end();
    else if (PLAN[i].kind == LR433_IN_FSK) ls_lora_fsk_end();
    else ls_lora_ook_end();
}

/* Start the first entry after `from` that the chip takes, recording why one
   would not. */
static void advance(int from, int64_t now)
{
    for (int step = 1; step <= N_PLAN; step++) {
        const int i = (from + step + N_PLAN) % N_PLAN;
        if (!s_on[i]) continue;
        const esp_err_t err = session_begin(i);
        if (err == ESP_OK) {
            s_cur = i;
            s_dwell_end = now + (int64_t)s_dwell_s * 1000000;
            portENTER_CRITICAL(&s_lock);
            s_view.cur = i;
            s_view.since_us = now;
            s_view.refused[i] = false;
            portEXIT_CRITICAL(&s_lock);
            return;
        }
        portENTER_CRITICAL(&s_lock);
        s_view.refused[i] = true;
        snprintf(s_view.fail, sizeof(s_view.fail), "%s refused: %s", PLAN[i].name, esp_err_to_name(err));
        portEXIT_CRITICAL(&s_lock);
    }
    s_cur = -1;
    s_retry_at = now + RETRY_US;
    portENTER_CRITICAL(&s_lock);
    s_view.cur = -1;
    portEXIT_CRITICAL(&s_lock);
}

static bool lr433_start(char *why, size_t n)
{
    lr433_history_start();
    const uint32_t caps = ls_lora_caps();
    const int sel = s_sel;
    s_n_on = 0;
    for (int i = 0; i < N_PLAN; i++) {
        const bool can = PLAN[i].kind == KIND_IQ      ? iq_capable(caps)
                       : PLAN[i].kind == LR433_IN_FSK ? (caps & LS_LORA_CAP_FSK) != 0
                                                      : (caps & LS_LORA_CAP_MODES_RX) != 0;
        s_on[i] = can && sel_has(sel, &PLAN[i]);
        if (s_on[i]) s_n_on++;
    }
    if (!s_n_on) {
        LS_SNPRINTF(t_exp_lr433_this_chip_has_no, why, n, "This chip has no OOK or FSK receiver for %s", SEL_NAMES[sel]);
        return false;
    }
    portENTER_CRITICAL(&s_lock);
    for (int i = 0; i < N_PLAN; i++) {
        s_view.on[i] = s_on[i];
        s_view.refused[i] = false;
    }
    s_view.fail[0] = 0;
    portEXIT_CRITICAL(&s_lock);
    s_cur = -1;
    const int64_t now = esp_timer_get_time();
    advance(-1, now);
    if (s_cur < 0) {
        portENTER_CRITICAL(&s_lock);
        snprintf(why, n, "%s", s_view.fail);
        portEXIT_CRITICAL(&s_lock);
        return false;
    }
    return true;
}

static void lr433_stop(void)
{
    session_end(s_cur);
    s_cur = -1;
    portENTER_CRITICAL(&s_lock);
    s_view.cur = -1;
    portEXIT_CRITICAL(&s_lock);
}

static void lr433_poll(void)
{
    const int64_t now = esp_timer_get_time();
    if (s_cur < 0) {
        if (now >= s_retry_at) advance(-1, now);
        return;
    }
    float rssi = NAN;
    if (PLAN[s_cur].kind == KIND_IQ) {
        iq_poll(s_cur, now);
    } else {
        const int n = PLAN[s_cur].kind == LR433_IN_FSK ? ls_lora_fsk_poll(s_buf, sizeof(s_buf), &rssi)
                                                       : ls_lora_ook_poll(s_buf, sizeof(s_buf), &rssi);
        if (n > 0) take(s_cur, s_buf, n, rssi <= -199.0f ? NAN : rssi, now);
    }
    /* The dwell is read live, so a change from OPTIONS counts from now. */
    const int64_t end = s_dwell_end;
    if (s_n_on > 1 && now >= end) {
        const int was = s_cur;
        session_end(was);
        advance(was, now);
    } else if (end - now > (int64_t)s_dwell_s * 1000000) {
        s_dwell_end = now + (int64_t)s_dwell_s * 1000000;
    }
}

/* -------------------------------------------------------------- readout -- */

/* The page shows 48 columns of each line, so nothing here is wider. */

static void fmt_ago(char *out, size_t n, int64_t us)
{
    const long s = us < 0 ? 0 : (long)(us / 1000000);
    if (s < 60) snprintf(out, n, "%lds", s);
    else if (s < 3600) snprintf(out, n, "%ldm", s / 60);
    else if (s < 86400) snprintf(out, n, "%ldh", s / 3600);
    else snprintf(out, n, "%ldd", s / 86400);
}

static void fmt_temp(char *out, size_t n, float c, bool metric)
{
    if (isnan(c)) { out[0] = 0; return; }
    if (metric) snprintf(out, n, "%.1fC", (double)c);
    else snprintf(out, n, "%.1fF", (double)(c * 9.0f / 5.0f + 32.0f));
}

/* The readings, short enough for the column. */
static void fmt_reading(char *out, size_t n, const heard_t *d, bool metric)
{
    char t[12];
    out[0] = 0;
    if (d->proto == PROTO_PACKET) { snprintf(out, n, "%s", d->packet); return; }
    switch (lr433_proto_class(d->proto)) {
    case LR433_TPMS: {
        lr433_msg_t key = { .proto = d->proto, .channel = d->channel };
        snprintf(key.id, sizeof(key.id), "%s", d->id);
        if (!lr433_history_owned(&key)) { snprintf(out, n, "MY ID off"); break; }
        char p[12] = "";
        if (!isnan(d->kpa)) {
            if (metric) snprintf(p, sizeof(p), "%.0fkPa", (double)d->kpa);
            else snprintf(p, sizeof(p), "%.1fpsi", (double)(d->kpa / 6.894757f));
        }
        if (isnan(d->temp_c)) t[0] = 0;
        else if (metric) snprintf(t, sizeof(t), "%.0fC", (double)d->temp_c);
        else snprintf(t, sizeof(t), "%.0fF", (double)(d->temp_c * 9.0f / 5.0f + 32.0f));
        snprintf(out, n, "%s%s%s", p, t[0] && p[0] ? " " : "", t);
        break;
    }
    case LR433_METER:
        if (d->consumption >= 0) snprintf(out, n, "%lld", (long long)d->consumption);
        break;
    default:
        fmt_temp(t, sizeof(t), d->temp_c, metric);
        if (!isnan(d->humidity)) snprintf(out, n, "%s%s%.0f%%", t, t[0] ? " " : "", (double)d->humidity);
        else if (t[0]) snprintf(out, n, "%s", t);
        else snprintf(out, n, "wind/rain");
        break;
    }
    if (d->battery_ok == 0) {
        const size_t len = strlen(out);
        snprintf(out + len, n - len, "%sBAT", len ? " " : "");
    }
}

static void fmt_id(char *out, size_t n, const heard_t *d)
{
    /* A long ID keeps its tail, which is what differs between neighbours;
       the channel, when there is one, after it. */
    const size_t len = strlen(d->id);
    const size_t room = d->channel < 0 ? 9 : 7;
    const char *id = len > room ? d->id + (len - room) : d->id;
    if (d->channel < 0) snprintf(out, n, "%s", id);
    else if (d->channel >= 'A') snprintf(out, n, "%s %c", id, d->channel);
    else snprintf(out, n, "%s %d", id, d->channel);
}

static int lr433_lines(char (*out)[LS_EXP_LINE], int max)
{
    const int64_t now = esp_timer_get_time();
    const bool metric = !lr433_history_fahrenheit();
    int cur;
    int64_t since;
    bool on[N_PLAN], refused[N_PLAN];
    uint32_t caps = 0, decoded = 0;
    unk_t unk[N_PLAN];
    char fail[48];
    uint8_t order[MAX_DEVS];
    int ndev = 0;
    portENTER_CRITICAL(&s_lock);
    cur = s_view.cur;
    since = s_view.since_us;
    for (int i = 0; i < N_PLAN; i++) {
        on[i] = s_view.on[i];
        refused[i] = s_view.refused[i];
        caps += s_view.caps[i];
        decoded += s_view.decoded[i];
        unk[i] = s_view.unk[i];
    }
    memcpy(fail, s_view.fail, sizeof(fail));
    /* Newest first. */
    bool taken[MAX_DEVS] = { false };
    for (;;) {
        int best = -1;
        for (int i = 0; i < MAX_DEVS; i++)
            if (s_view.devs[i].used && !taken[i] && (best < 0 || s_view.devs[i].last_us > s_view.devs[best].last_us))
                best = i;
        if (best < 0) break;
        taken[best] = true;
        order[ndev++] = (uint8_t)best;
    }
    portEXIT_CRITICAL(&s_lock);

    /* Before the first start, the plan the next one will use. */
    bool any_on = false;
    for (int i = 0; i < N_PLAN; i++) any_on |= on[i];
    if (!any_on)
        for (int i = 0; i < N_PLAN; i++) on[i] = sel_has(s_sel, &PLAN[i]);

    int n = 0;
    if (n < max) {
        if (cur >= 0) {
            char left[8] = "";
            if (s_n_on > 1) {
                const int64_t rem = since + (int64_t)s_dwell_s * 1000000 - now;
                snprintf(left, sizeof(left), " %lds", (long)(rem > 0 ? (rem + 999999) / 1000000 : 0));
            }
            LS_SNPRINTF(t_exp_lr433_now_8s_4f_mhz, out[n++], LS_EXP_LINE, "NOW  %-8s %.4f MHz%s", PLAN[cur].name, tuned_hz(&PLAN[cur]) / 1e6, left);
        } else {
            snprintf(out[n++], LS_EXP_LINE, "NOW  -- %.40s", fail[0] ? fail : "not listening");
        }
    }
    if (n < max && cur >= 0) snprintf(out[n++], LS_EXP_LINE, "     %s", PLAN[cur].what);
    if (n < max) {
        char line[LS_EXP_LINE] = "PLAN";
        for (int i = 0; i < N_PLAN; i++) {
            if (!on[i]) continue;
            const size_t len = strlen(line);
            /* "!" after one the chip would not start. */
            snprintf(line + len, sizeof(line) - len, i == cur ? " [%s]" : " %s%s", PLAN[i].tag,
                     refused[i] ? "!" : "");
        }
        snprintf(out[n++], LS_EXP_LINE, "%s", line);
    }
    if (n < max)
        LS_SNPRINTF(t_exp_lr433_heard_lu_bursts_lu, out[n++], LS_EXP_LINE, "HEARD %lu bursts, %lu decoded, %d device%s", (unsigned long)caps,
                 (unsigned long)decoded, ndev, ndev == 1 ? "" : "s");

    /* What keyed and did not decode keeps its lines at the bottom. */
    int nunk = 0;
    for (int i = 0; i < N_PLAN; i++) if (unk[i].bursts) nunk++;
    const int unk_lines = nunk ? nunk + 1 : 0;
    const bool tpms_note = ndev == 0;
    const int tail = unk_lines + (tpms_note ? 1 : 0);

    if (ndev && n < max - tail)
        LS_SNPRINTF(t_exp_lr433_9s_9s_15s_4s, out[n++], LS_EXP_LINE, "%-9s %-9s %-15s %4s %3s %3s", "DEVICE", "ID", "READING", "dBm", "N", "AGO");
    for (int k = 0; k < ndev && n < max - tail; k++) {
        heard_t d;
        portENTER_CRITICAL(&s_lock);
        d = s_view.devs[order[k]];
        portEXIT_CRITICAL(&s_lock);
        if (!d.used) continue;
        char id[24], rd[32], ago[8], lvl[8];
        fmt_id(id, sizeof(id), &d);
        fmt_reading(rd, sizeof(rd), &d, metric);
        fmt_ago(ago, sizeof(ago), now - d.last_us);
        if (isnan(d.rssi)) snprintf(lvl, sizeof(lvl), "--");
        /* a packet's level is dB over the floor (I/Q is uncalibrated) */
        else if (d.proto == PROTO_PACKET) snprintf(lvl, sizeof(lvl), "+%.0f", (double)d.rssi);
        else snprintf(lvl, sizeof(lvl), "%.0f", (double)d.rssi);
        LS_SNPRINTF(t_exp_lr433_9_9s_9_9s, out[n++], LS_EXP_LINE, "%-9.9s %-9.9s %-15.15s %4.4s %3lu %3.3s",
                 d.proto == PROTO_PACKET ? "FSK-PKT" : lr433_proto_label(d.proto),
                 id, rd, lvl, (unsigned long)(d.count > 999 ? 999 : d.count), ago);
    }
    if (unk_lines && n < max) snprintf(out[n++], LS_EXP_LINE, "KEYED, NOT DECODED");
    for (int i = 0; i < N_PLAN && n < max; i++) {
        if (!unk[i].bursts) continue;
        char ago[8], last[8], top[8];
        fmt_ago(ago, sizeof(ago), now - unk[i].last_us);
        if (isnan(unk[i].last)) snprintf(last, sizeof(last), "--");
        else snprintf(last, sizeof(last), "%.0f", (double)unk[i].last);
        if (isnan(unk[i].max)) snprintf(top, sizeof(top), "--");
        else snprintf(top, sizeof(top), "%.0f", (double)unk[i].max);
        LS_SNPRINTF(t_exp_lr433_8_8s_5lu_last, out[n++], LS_EXP_LINE, "%-8.8s %5lu  last %4.4s max %4.4s dBm %4.4s", PLAN[i].name,
                 (unsigned long)(unk[i].bursts > 99999 ? 99999 : unk[i].bursts), last, top, ago);
    }
    if (tpms_note && n < max)
        LS_SNPUTS(t_exp_lr433_tpms_send_about_once, out[n++], LS_EXP_LINE, "TPMS send about once a minute while driving");
    return n;
}

/* -------------------------------------------------------------- console -- */

static bool lr433_configure(int argc, char **argv, char *why, size_t n)
{
    int sel = -1;
    for (int i = 0; i < SEL_COUNT; i++)
        if (!strcmp(argv[0], SEL_ARGS[i])) sel = i;
    int dwell = s_dwell_s;
    int32_t offset = s_offset_hz;
    bool ok = sel >= 0 && argc <= 3;
    if (ok && argc >= 2) {
        char *end = NULL;
        const long v = strtol(argv[1], &end, 10);
        ok = end != argv[1] && !*end && v >= DWELL_MIN && v <= DWELL_MAX;
        dwell = (int)v;
    }
    /* A centre in MHz: the offset from the first of the band's entries. */
    if (ok && argc == 3) {
        char *end = NULL;
        const double mhz = strtod(argv[2], &end);
        ok = end != argv[2] && !*end && sel != SEL_ALL;
        const plan_t *first = NULL;
        for (int i = 0; ok && i < N_PLAN && !first; i++) if (sel_has(sel, &PLAN[i])) first = &PLAN[i];
        const double off = ok && first ? mhz * 1e6 - first->freq_hz : 1e12;
        ok = ok && off >= -OFFSET_MAX_KHZ * 1000.0 && off <= OFFSET_MAX_KHZ * 1000.0;
        offset = (int32_t)llround(off);
    }
    if (!ok) {
        LS_SNPRINTF(t_exp_lr433_all_315_433_915, why, n, "all|315|433|915|ert [dwell %d-%d s] [centre MHz]", DWELL_MIN, DWELL_MAX);
        return false;
    }
    s_sel = sel;
    s_dwell_s = dwell;
    s_offset_hz = offset;
    return true;
}

/* -------------------------------------------------------------- OPTIONS -- */

static int o_get_sel(const ls_opt_t *o) { (void)o; return s_sel; }
static void o_set_sel(const ls_opt_t *o, int v)
{
    (void)o;
    s_sel = (v >= 0 && v < SEL_COUNT) ? v : SEL_ALL;
    if (ls_exp_running() == &exp_lr433) ls_exp_start(&exp_lr433);
}

static double o_dwell(const ls_opt_t *o) { (void)o; return s_dwell_s; }
static void o_set_dwell(const ls_opt_t *o, double v)
{
    (void)o;
    s_dwell_s = (int)(v + 0.5);
}
static void o_show_dwell(const ls_opt_t *o, char *out, size_t n) { (void)o; snprintf(out, n, "%d s", s_dwell_s); }

static const char *const UNIT_NAMES[] = { "PSI / F", "KPA / C" };
static int o_get_units(const ls_opt_t *o) { (void)o; return lr433_history_fahrenheit() ? 0 : 1; }
static void o_set_units(const ls_opt_t *o, int v) { (void)o; lr433_history_set_fahrenheit(!v); }

static const char *const GAIN_NAMES[] = { "TOP STEP", "AGC" };
static int o_get_gain(const ls_opt_t *o) { (void)o; return s_agc ? 1 : 0; }
static void o_set_gain(const ls_opt_t *o, int v)
{
    (void)o;
    s_agc = v ? 1 : 0;
    if (ls_exp_running() == &exp_lr433) ls_exp_start(&exp_lr433);
}

static void o_clear(const ls_opt_t *o)
{
    (void)o;
    portENTER_CRITICAL(&s_lock);
    memset(s_view.devs, 0, sizeof(s_view.devs));
    memset(s_view.unk, 0, sizeof(s_view.unk));
    memset(s_view.caps, 0, sizeof(s_view.caps));
    memset(s_view.decoded, 0, sizeof(s_view.decoded));
    portEXIT_CRITICAL(&s_lock);
}
static void o_show_clear(const ls_opt_t *o, char *out, size_t n) { (void)o; snprintf(out, n, "devices and counts"); }

static double o_offset(const ls_opt_t *o) { (void)o; return s_offset_hz / 1000.0; }
static void o_set_offset(const ls_opt_t *o, double v)
{
    (void)o;
    if (v < -OFFSET_MAX_KHZ) v = -OFFSET_MAX_KHZ;
    if (v > OFFSET_MAX_KHZ) v = OFFSET_MAX_KHZ;
    s_offset_hz = (int32_t)llround(v * 1000.0);
    if (ls_exp_running() == &exp_lr433) ls_exp_start(&exp_lr433);
}
static void o_show_offset(const ls_opt_t *o, char *out, size_t n)
{
    (void)o;
    if (!s_offset_hz) snprintf(out, n, "none");
    else snprintf(out, n, "%+.1f kHz", s_offset_hz / 1000.0);
}

static const ls_opt_t OPTS[] = {
    { .label = "BANDS", .kind = LS_OPT_CYCLE, .names = SEL_NAMES, .n = SEL_COUNT, .get = o_get_sel, .set = o_set_sel },
    { .label = "DWELL", .kind = LS_OPT_NUMBER, .num = o_dwell, .set_num = o_set_dwell, .lo = DWELL_MIN,
      .hi = DWELL_MAX, .unit = "seconds a session, 2 to 600", .show = o_show_dwell },
    { .label = "UNITS", .kind = LS_OPT_CYCLE, .names = UNIT_NAMES, .n = 2, .get = o_get_units, .set = o_set_units },
    { .label = "OOK GAIN", .kind = LS_OPT_CYCLE, .names = GAIN_NAMES, .n = 2, .get = o_get_gain, .set = o_set_gain },
    { .label = "HISTORY", .kind = LS_OPT_MENU, .sub = &lr433_history_ctx },
    { .label = "CLEAR", .kind = LS_OPT_ACTION, .act = o_clear, .show = o_show_clear },
    /* last, so the rows before keep their places */
    { .label = "OFFSET", .kind = LS_OPT_NUMBER, .num = o_offset, .set_num = o_set_offset,
      .lo = -OFFSET_MAX_KHZ, .hi = OFFSET_MAX_KHZ, .unit = "kHz off each band's frequency", .show = o_show_offset },
};

/* METERS keeps its pins file on this worker too; weak, so a build or a
   bench without METERS links. */
__attribute__((weak)) bool ls_meters_upkeep(void) { return false; }
static bool lr433_upkeep(void)
{
    const bool history = lr433_history_service();
    const bool meters = ls_meters_upkeep();
    return history || meters;
}

const ls_experiment_t exp_lr433 = {
    .id = "lr433",
    .name = "LR433",
    .sub = "TPMS, sensors, meters at 315-928 MHz",
    .maturity = LS_EXP_TRYING,
    .needs = "a sender nearby: a car driven, a sensor, a meter",
    .start = lr433_start,
    .stop = lr433_stop,
    .poll = lr433_poll,
    .lines = lr433_lines,
    .configure = lr433_configure,
    .opts = OPTS,
    .n_opts = (int)(sizeof(OPTS) / sizeof(OPTS[0])),
    .maintenance = lr433_upkeep,
};
