#include "shell/ls_hub.h"

#include "lvgl.h"

#include <cstdio>
#include <cstring>

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "esp_timer.h"
/*LS-719*/
#include "esp_adc/adc_oneshot.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"

extern "C" {
#include "app_registry.h"
#include "audio_out.h"
#include "event_bus.h"
#include "lakeshark_backend.h"
}

#define HUB_PERIOD_MS   250
#define HUB_MAX_SUBS    8
#define HUB_EVT_DEPTH   12

/*LS-602*/
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

static void fanout(uint32_t dirty)
{
    if (!dirty) return;
    for (int i = 0; i < HUB_MAX_SUBS; i++)
        if (s_fn[i]) s_fn[i](&s_state, dirty, s_ud[i]);
}

/*LS-602*/
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
    const bool ready  = lakeshark_radio_device_ready();
    const bool parked = !lakeshark_radio_running();

    if (ready != s_state.rtl_ready) { s_state.rtl_ready = ready; *dirty |= LS_HUB_RADIO; }
    if (parked != s_state.parked)   { s_state.parked   = parked; *dirty |= LS_HUB_RADIO; }

    const app_t *cur  = app_current();
    const char  *name = (cur && cur->name) ? cur->name : "--";
    if (strncmp(name, s_state.mode, sizeof(s_state.mode) - 1) != 0) {
        strncpy(s_state.mode, name, sizeof(s_state.mode) - 1);
        s_state.mode[sizeof(s_state.mode) - 1] = 0;
        *dirty |= LS_HUB_RADIO | LS_HUB_TUNE | LS_HUB_SIGNAL;
    }

    if (s_sd >= 0 && (bool)s_sd != s_state.sd_present) {
        s_state.sd_present = (bool)s_sd; *dirty |= LS_HUB_RADIO;
    }
    if (s_c6 != s_state.c6_state) { s_state.c6_state = s_c6; *dirty |= LS_HUB_RADIO; }
}

static void poll_mode(uint32_t *dirty)
{
    uint32_t freq = 0;
    int      sig  = 0;
    bool     act  = false;
    int      contacts = s_state.contacts;
    uint32_t iq   = 0;
    char     det[sizeof(s_state.detail)];
    det[0] = 0;

    if (strcmp(s_state.mode, "P25") == 0) {
        lakeshark_p25_tel_t t;
        lakeshark_p25_telemetry(&t);
        freq = t.freq_hz;
        sig  = t.iq_level / 10;
        act  = t.voice_active != 0;
        iq   = t.iq_bytes_sec;
        if (t.nac)      snprintf(det, sizeof(det), "NAC %03X  TG %d", t.nac, t.tg);
        else if (t.has_sync) snprintf(det, sizeof(det), "SYNC  %s", t.ftype);
        else            snprintf(det, sizeof(det), "NO SYNC");
    } else if (strcmp(s_state.mode, "FM") == 0) {
        lakeshark_fm_tel_t t;
        lakeshark_fm_telemetry(&t);
        freq = t.freq_hz;
        sig  = t.iq_level / 10;
        act  = t.squelch_open != 0;
        iq   = t.iq_bytes_sec;
        snprintf(det, sizeof(det), "%s  SQL %d.%d",
                 act ? "OPEN" : "QUIET",
                 t.squelch_tenths / 10, t.squelch_tenths % 10);
    } else if (strcmp(s_state.mode, "ADS-B") == 0) {
        lakeshark_adsb_tel_t t;
        lakeshark_adsb_telemetry(&t);
        freq = t.freq_hz;
        sig  = t.mag_peak > 0 ? ((t.mag_peak >> 8) * 100) / 255 : 0;
        act  = t.last_msg_ms >= 0 && t.last_msg_ms < 3000;
        iq   = t.iq_bytes_sec;
        contacts = t.tracked;
        snprintf(det, sizeof(det), "%d TRACKED  %d/s", t.tracked, t.msgs_sec);
    } else {
        snprintf(det, sizeof(det), "IDLE");
    }

    if (sig < 0)   sig = 0;
    if (sig > 100) sig = 100;

    if (freq != s_state.freq_hz) { s_state.freq_hz = freq; *dirty |= LS_HUB_TUNE; }

    if (sig != s_state.sig_pct || act != s_state.active ||
        contacts != s_state.contacts || iq != s_state.iq_bytes_sec ||
        strcmp(det, s_state.detail) != 0) {
        s_state.sig_pct      = sig;
        s_state.active       = act;
        s_state.contacts     = contacts;
        s_state.iq_bytes_sec = iq;
        strncpy(s_state.detail, det, sizeof(s_state.detail) - 1);
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

/*LS-719*/
/* BAT_ADC is GPIO20 (ADC1 ch4), found empirically with `bat` - the net is in
   neither Waveshare's BSP header nor the vendor package, and appears exactly
   once in their schematic, at the divider. R12 200K / R15 100K off BAT, so the
   pin reads BAT/3. GPIO20 held to +/-2 raw counts across repeats while GPIO21
   and 22 drifted, which is a driven node against two floating ones.
   Do NOT widen this scan to GPIO16..19: those are the C6 SDIO bus. */
#define BATT_ADC_CHAN    ADC_CHANNEL_4
#define BATT_DIVIDER     3
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

/*LS-602*/
static void hub_tick(lv_timer_t *)
{
    uint32_t dirty = 0;

    drain_events(&dirty);
    poll_radio(&dirty);
    poll_mode(&dirty);
    poll_audio(&dirty);
    /*LS-719*/
    poll_battery(&dirty);

    if (s_prime) { s_prime = false; dirty = LS_HUB_ALL; }
    fanout(dirty);
}

/*LS-602*/
void ls_hub_start(void)
{
    if (s_started) return;
    s_started = true;

    memset(&s_state, 0, sizeof(s_state));
    strcpy(s_state.mode, "--");
    s_state.parked   = true;
    s_state.batt_pct = -1;
    s_state.c6_state = -1;
    s_state.volume   = audio_volume_get();
    s_state.muted    = audio_is_muted();

    /*LS-719*/
    batt_init();

    s_evtq = xQueueCreate(HUB_EVT_DEPTH, sizeof(hub_evt_t));

    event_bus_init();
    event_bus_subscribe(bus_cb, nullptr);

    s_timer = lv_timer_create(hub_tick, HUB_PERIOD_MS, nullptr);
}

/*LS-602*/
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

/*LS-602*/
void ls_hub_unsubscribe(int id)
{
    if (id < 0 || id >= HUB_MAX_SUBS) return;
    s_fn[id] = nullptr;
    s_ud[id] = nullptr;
}

const ls_hub_state_t *ls_hub_state(void) { return &s_state; }

/*LS-602*/
bool ls_hub_last_line(char *dst, int cap)
{
    if (!dst || cap <= 0 || !s_line[0]) return false;
    strncpy(dst, s_line, cap - 1);
    dst[cap - 1] = 0;
    return true;
}

void ls_hub_set_sd(bool present) { s_sd = present ? 1 : 0; }
void ls_hub_set_c6(int state)    { s_c6 = state; }
