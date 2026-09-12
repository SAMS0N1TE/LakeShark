#include "shell/ls_hub.h"
#include "shell/ls_hub_present.h"
#include "ls_board.h"

#include "lvgl.h"

#include <cstdio>
#include <cstring>

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "esp_timer.h"
/**/
#include "esp_adc/adc_oneshot.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"

extern "C" {
#include "app_registry.h"
#include "audio_out.h"
#include "event_bus.h"
#include "lakeshark_backend.h"
#include "fm_state.h"
#include "rec_state.h"
}

#define HUB_PERIOD_MS   250
#define HUB_MAX_SUBS    8
#define HUB_EVT_DEPTH   12

/**/
typedef struct {
    uint8_t kind;
    char    text[LS_HUB_LINE_MAX];
} hub_evt_t;

static ls_hub_state_t s_state;
static ls_hub_fn      s_fn[HUB_MAX_SUBS];
static void          *s_ud[HUB_MAX_SUBS];
static lv_timer_t    *s_timer;
static QueueHandle_t  s_evtq;
static char           s_line[LS_HUB_LINE_MAX];
static bool           s_started;

static int  s_sd     = -1;
static int  s_c6     = -1;
static bool s_prime  = true;
static char s_backend_mode[LS_HUB_MODE_MAX] = "--";

static void fanout(uint32_t dirty)
{
    if (!dirty) return;
    for (int i = 0; i < HUB_MAX_SUBS; i++)
        if (s_fn[i]) s_fn[i](&s_state, dirty, s_ud[i]);
}

/**/
static void bus_cb(const event_t *e, void *)
{
    if (!e || !s_evtq) return;

    hub_evt_t r;
    r.kind = (uint8_t)e->kind;
    r.text[0] = 0;

    switch (e->kind) {
    case EVT_DEVICE_ATTACHED:
        snprintf(r.text, sizeof(r.text), "RTL ATTACHED");
        break;
    case EVT_DEVICE_DETACHED:
        snprintf(r.text, sizeof(r.text), "RTL LOST");
        break;
    case EVT_TUNER_LOCKED:
        snprintf(r.text, sizeof(r.text), "TUNER LOCKED");
        break;
    case EVT_APP_SWITCHED:
        snprintf(r.text, sizeof(r.text), "MODE %s", e->u.sw.to);
        break;
    case EVT_CONTACT_NEW:
        snprintf(r.text, sizeof(r.text), "NEW %06lX %s",
                 (unsigned long)e->u.contact.icao, e->u.contact.callsign);
        break;
    case EVT_CONTACT_LOST:
        snprintf(r.text, sizeof(r.text), "LOST %06lX",
                 (unsigned long)e->u.contact.icao);
        break;
    case EVT_LOG:
        snprintf(r.text, sizeof(r.text), "%s", e->u.log.text);
        break;
    default:
        return;
    }

    xQueueSend(s_evtq, &r, 0);
}

static void drain_events(uint32_t *dirty)
{
    hub_evt_t r;
    bool got = false;
    while (s_evtq && xQueueReceive(s_evtq, &r, 0) == pdTRUE) {
        if (r.text[0]) { strncpy(s_line, r.text, sizeof(s_line) - 1);
                         s_line[sizeof(s_line) - 1] = 0; got = true; }
        if (r.kind == EVT_DEVICE_ATTACHED || r.kind == EVT_DEVICE_DETACHED ||
            r.kind == EVT_APP_SWITCHED)
            *dirty |= LS_HUB_RADIO;
    }
    if (got) *dirty |= LS_HUB_EVENT;
}

static void poll_radio(uint32_t *dirty)
{
    const bool ready  = lakeshark_iq_receiver_ready();
    const bool parked = !lakeshark_radio_running();

    if (ready != s_state.rtl_ready) { s_state.rtl_ready = ready; *dirty |= LS_HUB_RADIO; }
    if (parked != s_state.parked)   { s_state.parked   = parked; *dirty |= LS_HUB_RADIO; }

    const app_t *cur  = app_current();
    const char  *name = (cur && cur->name) ? cur->name : "--";
    if (strncmp(name, s_backend_mode, sizeof(s_backend_mode) - 1) != 0) {
        strncpy(s_backend_mode, name, sizeof(s_backend_mode) - 1);
        s_backend_mode[sizeof(s_backend_mode) - 1] = 0;
        *dirty |= LS_HUB_RADIO | LS_HUB_TUNE | LS_HUB_SIGNAL;
    }

    if (s_sd >= 0 && (bool)s_sd != s_state.sd_present) {
        s_state.sd_present = (bool)s_sd; *dirty |= LS_HUB_RADIO;
    }
    if (s_c6 != s_state.c6_state) { s_state.c6_state = s_c6; *dirty |= LS_HUB_RADIO; }
}

static void poll_mode(uint32_t *dirty)
{
    ls_hub_observation_t o = {};
    o.backend_mode  = s_backend_mode;
    o.receiver_ready = s_state.rtl_ready;
    o.parked         = s_state.parked;

    if (strcmp(s_backend_mode, "P25") == 0) {
        lakeshark_p25_tel_t t;
        lakeshark_p25_telemetry(&t);
        o.freq_hz       = t.freq_hz;
        o.signal_pct    = t.iq_level / 10;
        o.active        = t.voice_active != 0;
        o.iq_bytes_sec  = t.iq_bytes_sec;
        o.p25_nac       = t.nac;
        o.p25_tg        = t.tg;
        o.p25_has_sync  = t.has_sync != 0;
        o.p25_ftype     = t.ftype;
    } else if (strcmp(s_backend_mode, "FM") == 0) {
        lakeshark_fm_tel_t t;
        lakeshark_fm_telemetry(&t);
        o.receiver_ready    = o.receiver_ready && t.receiver_streaming != 0;
        o.freq_hz           = t.freq_hz;
        o.signal_pct        = t.iq_level / 10;
        o.active            = t.squelch_open != 0;
        o.iq_bytes_sec      = t.iq_bytes_sec;
        o.fm_submode        = t.submode;
        o.fm_squelch_tenths = t.squelch_tenths;
    } else if (strcmp(s_backend_mode, "ADS-B") == 0) {
        lakeshark_adsb_tel_t t;
        lakeshark_adsb_telemetry(&t);
        o.freq_hz       = t.freq_hz;
        o.signal_pct    = t.mag_peak > 0 ? ((t.mag_peak >> 8) * 100) / 255 : 0;
        o.active        = t.last_msg_ms >= 0 && t.last_msg_ms < 3000;
        o.iq_bytes_sec  = t.iq_bytes_sec;
        o.adsb_tracked  = t.tracked;
        o.adsb_msgs_sec = t.msgs_sec;
    } else if (strcmp(s_backend_mode, "REC") == 0) {
        rec_hub_status_t t;
        rec_get_hub_status(&t);
        o.receiver_ready = o.receiver_ready && t.receiver_streaming;
        o.freq_hz        = t.freq_hz;
        o.iq_bytes_sec   = t.bytes_sec;
        o.rec_phase      = (int)t.phase;
        o.rec_edges      = t.edges;
        o.rec_mag_now    = t.mag_now;
        o.rec_mag_thresh = t.mag_thresh;
    }

    ls_hub_presentation_t p;
    ls_hub_present(&o, &p);

    if (strcmp(p.mode, s_state.mode) != 0 ||
        strcmp(p.target_app, s_state.target_app) != 0) {
        strncpy(s_state.mode, p.mode, sizeof(s_state.mode) - 1);
        s_state.mode[sizeof(s_state.mode) - 1] = 0;
        strncpy(s_state.target_app, p.target_app,
                sizeof(s_state.target_app) - 1);
        s_state.target_app[sizeof(s_state.target_app) - 1] = 0;
        *dirty |= LS_HUB_RADIO | LS_HUB_TUNE | LS_HUB_SIGNAL;
    }

    if (p.freq_hz != s_state.freq_hz) {
        s_state.freq_hz = p.freq_hz;
        *dirty |= LS_HUB_TUNE;
    }

    if (p.signal_pct != s_state.sig_pct || p.active != s_state.active ||
        p.contacts != s_state.contacts ||
        p.iq_bytes_sec != s_state.iq_bytes_sec ||
        strcmp(p.detail, s_state.detail) != 0) {
        s_state.sig_pct      = p.signal_pct;
        s_state.active       = p.active;
        s_state.contacts     = p.contacts;
        s_state.iq_bytes_sec = p.iq_bytes_sec;
        strncpy(s_state.detail, p.detail, sizeof(s_state.detail) - 1);
        s_state.detail[sizeof(s_state.detail) - 1] = 0;
        *dirty |= LS_HUB_SIGNAL;
    }
}

static void poll_audio(uint32_t *dirty)
{
    const int  vol   = audio_volume_get();
    const bool muted = audio_is_muted();
    if (vol != s_state.volume) { s_state.volume = vol; *dirty |= LS_HUB_AUDIO; }
    if (muted != s_state.muted) { s_state.muted = muted; *dirty |= LS_HUB_AUDIO; }
}

/**/
/* BAT_ADC is GPIO20 (ADC1 ch4), found empirically with `bat` - the net is in
   neither Waveshare's BSP header nor the vendor package, and appears exactly
   once in their schematic, at the divider. R12 200K / R15 100K off BAT, so the
   pin reads BAT/3. GPIO20 held to +/-2 raw counts across repeats while GPIO21
   and 22 drifted, which is a driven node against two floating ones.
   Do NOT widen this scan to GPIO16..19: those are the C6 SDIO bus. */
/* The T-Display uses a fuel gauge. Configuring Waveshare's ADC1 channel 4
 * here disabled its codec SDA GPIO20: hardware dump showed InputEn=0 and
 * OpenDrain=0, and every volume write timed out after shell initialization. */
#if LS_HAS_BATTERY_ADC
#define BATT_ADC_CHAN    ((adc_channel_t)LS_BOARD_BATTERY_ADC_CHANNEL)
#define BATT_DIVIDER     LS_BOARD_BATTERY_DIVIDER
#define BATT_ABSENT_MV   2500
#define BATT_PERIOD_MS   5000

static adc_oneshot_unit_handle_t s_badc = nullptr;
static adc_cali_handle_t         s_bcal = nullptr;

static void batt_init(void)
{
    adc_oneshot_unit_init_cfg_t ucfg = { .unit_id = ADC_UNIT_1 };
    if (adc_oneshot_new_unit(&ucfg, &s_badc) != ESP_OK) { s_badc = nullptr; return; }
    adc_oneshot_chan_cfg_t c = { .atten = ADC_ATTEN_DB_12, .bitwidth = ADC_BITWIDTH_DEFAULT };
    if (adc_oneshot_config_channel(s_badc, BATT_ADC_CHAN, &c) != ESP_OK) {
        adc_oneshot_del_unit(s_badc); s_badc = nullptr; return;
    }
    adc_cali_curve_fitting_config_t ccfg = {
        .unit_id  = ADC_UNIT_1,
        .atten    = ADC_ATTEN_DB_12,
        .bitwidth = ADC_BITWIDTH_DEFAULT,
    };
    if (adc_cali_create_scheme_curve_fitting(&ccfg, &s_bcal) != ESP_OK) s_bcal = nullptr;
}

/* Resting LiPo curve. Rough by nature - under load it reads low, and while
   charging it reads high because this is the terminal voltage, not coulombs. */
static int batt_mv_to_pct(int mv)
{
    static const struct { int mv; int pct; } C[] = {
        { 4200, 100 }, { 4000, 80 }, { 3850, 60 }, { 3700, 40 },
        { 3550, 20 },  { 3300,  5 }, { 3000,  0 },
    };
    if (mv >= C[0].mv) return 100;
    const int n = (int)(sizeof(C) / sizeof(C[0]));
    if (mv <= C[n - 1].mv) return 0;
    for (int i = 0; i < n - 1; i++) {
        if (mv > C[i + 1].mv) {
            const int span = C[i].mv  - C[i + 1].mv;
            const int rise = C[i].pct - C[i + 1].pct;
            return C[i + 1].pct + ((mv - C[i + 1].mv) * rise + span / 2) / span;
        }
    }
    return 0;
}

static void poll_battery(uint32_t *dirty)
{
    if (!s_badc || !s_bcal) return;

    static uint32_t next_at = 0;
    const uint32_t now = (uint32_t)(esp_timer_get_time() / 1000);
    if (now < next_at) return;
    next_at = now + BATT_PERIOD_MS;

    int raw = 0, acc = 0, got = 0;
    for (int i = 0; i < 8; i++)
        if (adc_oneshot_read(s_badc, BATT_ADC_CHAN, &raw) == ESP_OK) { acc += raw; got++; }
    if (!got) return;

    int mv = 0;
    if (adc_cali_raw_to_voltage(s_bcal, acc / got, &mv) != ESP_OK) return;

    const int bat_mv = mv * BATT_DIVIDER;
    /* No pack fitted leaves the divider floating; do not invent a reading. */
    const int pct = (bat_mv < BATT_ABSENT_MV) ? -1 : batt_mv_to_pct(bat_mv);

    if (pct != s_state.batt_pct) { s_state.batt_pct = pct; *dirty |= LS_HUB_RADIO; }
}

#else
static void batt_init(void) {}
static void poll_battery(uint32_t *) {}
#endif

/**/
static void hub_tick(lv_timer_t *)
{
    uint32_t dirty = 0;

    drain_events(&dirty);
    poll_radio(&dirty);
    poll_mode(&dirty);
    poll_audio(&dirty);
    /**/
    poll_battery(&dirty);

    if (s_prime) { s_prime = false; dirty = LS_HUB_ALL; }
    fanout(dirty);
}

/**/
void ls_hub_start(void)
{
    if (s_started) return;
    s_started = true;

    memset(&s_state, 0, sizeof(s_state));
    strcpy(s_state.mode, "--");
    s_state.target_app[0] = 0;
    strcpy(s_backend_mode, "--");
    s_state.parked   = true;
    s_state.batt_pct = -1;
    s_state.c6_state = -1;
    s_state.volume   = audio_volume_get();
    s_state.muted    = audio_is_muted();

    /**/
    batt_init();

    s_evtq = xQueueCreate(HUB_EVT_DEPTH, sizeof(hub_evt_t));

    event_bus_init();
    event_bus_subscribe(bus_cb, nullptr);

    s_timer = lv_timer_create(hub_tick, HUB_PERIOD_MS, nullptr);
}

/**/
int ls_hub_subscribe(ls_hub_fn fn, void *ud)
{
    if (!fn) return -1;
    for (int i = 0; i < HUB_MAX_SUBS; i++) {
        if (s_fn[i]) continue;
        s_fn[i] = fn;
        s_ud[i] = ud;
        fn(&s_state, LS_HUB_ALL, ud);
        return i;
    }
    return -1;
}

/**/
void ls_hub_unsubscribe(int id)
{
    if (id < 0 || id >= HUB_MAX_SUBS) return;
    s_fn[id] = nullptr;
    s_ud[id] = nullptr;
}

const ls_hub_state_t *ls_hub_state(void) { return &s_state; }

/**/
bool ls_hub_last_line(char *dst, int cap)
{
    if (!dst || cap <= 0 || !s_line[0]) return false;
    strncpy(dst, s_line, cap - 1);
    dst[cap - 1] = 0;
    return true;
}

void ls_hub_set_sd(bool present) { s_sd = present ? 1 : 0; }
void ls_hub_set_c6(int state)    { s_c6 = state; }
