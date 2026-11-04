/* DOPPLER RADAR: two boards, one room.

   The board with the SX1262 sends a plain carrier, low power, in the ISM band
   (ls_lora_cw_begin). The board with the LR2021 listens 1.5 kHz below it and
   hears that carrier straight across the room and again off everything in
   it: a hand, a fan, someone walking, a door. Whatever moves shifts its echo
   by f = 2 v / lambda, 6.1 Hz for each m/s at 915 MHz, so the readout is a
   waterfall of the few hundred hertz either side of the carrier (radar_dsp),
   newest row on top: right of the bar is coming closer, left is going away.

   Which board does what follows from its radio: start it on both. Both must
   use the same FREQ. The tone stops by itself after ten minutes. TX LIMIT
   SCALED charges it to the transmitter protection at its power (about 1/300
   of full power, so it runs); NONE is the plain ten minutes. */
#include "../ls_experiments.h"
#include "radar_dsp.h"

#include <math.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>

#include "esp_attr.h"
#include "esp_err.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "ls_lora.h"
#ifdef ESP_PLATFORM
#include "lr_wf_session.h"
#endif

extern const ls_experiment_t exp_radar;

#define TUNE_BELOW_HZ 1500          /* the LR sits this far under the tone   */
#define TONE_MAX_US   (600LL * 1000000)
#define ROWS          12
#define COLS          49            /* odd: the middle column is the carrier */
#define SPAN_HZ       240.0f
#define RING_PAIRS    16384         /* 131 ms at 125 kHz                     */

static const char *const FREQS[] = { "915.000 MHz", "903.000 MHz", "433.920 MHz" };
static const uint32_t FREQ_HZ[] = { 915000000u, 903000000u, 433920000u };
static const char *const POWERS[] = { "-9 dBm", "-6 dBm", "-3 dBm" };
static const int POWER_DBM[] = { -9, -6, -3 };
static const char *const BGS[] = { "ONLY WHAT CHANGES", "EVERYTHING" };
static const char *const LIMITS[] = { "SCALED BY POWER", "NONE" };
static volatile unsigned s_freq, s_power, s_bg, s_limit;

typedef enum { ROLE_NONE, ROLE_TONE, ROLE_RECEIVER } role_t;

/* What the readout needs of a row: a few numbers, not its 4 KB of bins. */
typedef struct {
    float carrier_dbfs, offset_hz, motion_db, peak_hz, peak_db;
    bool learning;
} summary_t;

static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static struct {
    role_t role;
    uint32_t hz;
    int dbm;
    int64_t since_us;
    bool tone_on, timed_out, refused;
    bool locked;
    summary_t last;
    uint32_t rows, lost_pairs, resets;
    char wf[ROWS][COLS + 1];
    int wf_n;
    float peak_hold_mps;
} s_view;

/* the receiver's state: big, so in PSRAM */
static EXT_RAM_BSS_ATTR radar_dsp_t s_dsp;
static EXT_RAM_BSS_ATTR radar_row_t s_rows[4];
static EXT_RAM_BSS_ATTR uint8_t s_ring[RING_PAIRS * 4];
static atomic_uint s_w, s_r;          /* pairs, free-running */
static atomic_bool s_gap;
static bool s_streaming;

#ifdef ESP_PLATFORM
/* On the LR session's owner task: copy and return. */
static void consume(void *ctx, const uint8_t *data, size_t bytes, bool spare)
{
    (void)ctx; (void)spare;
    if (!data || !bytes) { atomic_store(&s_gap, true); return; }
    unsigned w = atomic_load_explicit(&s_w, memory_order_relaxed);
    const unsigned r = atomic_load_explicit(&s_r, memory_order_acquire);
    const size_t pairs = bytes / 4;
    if (w - r + pairs > RING_PAIRS) { atomic_store(&s_gap, true); return; }   /* fell behind */
    for (size_t k = 0; k < pairs; k++, w++)
        memcpy(&s_ring[(w % RING_PAIRS) * 4], data + 4 * k, 4);
    atomic_store_explicit(&s_w, w, memory_order_release);
}
#endif

/* The radio, on the board. The host bench links the experiments without a
   radio: there it is an SX1262 that never transmits. */
#ifdef ESP_PLATFORM
static ls_lora_chip_t radio_chip(void) { return ls_lora_present() ? ls_lora_chip() : ls_lora_fitted(); }
static esp_err_t tone_on(uint32_t hz, int dbm) { return ls_lora_cw_begin(hz, dbm, s_limit == 0); }
static esp_err_t tone_keep(void) { return ls_lora_cw_keep(); }
static void tone_off(void) { (void)ls_lora_cw_end(); }
#else
static ls_lora_chip_t radio_chip(void) { return LS_LORA_CHIP_SX126X; }
static esp_err_t tone_on(uint32_t hz, int dbm) { (void)hz; (void)dbm; return ESP_ERR_NOT_SUPPORTED; }
static esp_err_t tone_keep(void) { return ESP_OK; }
static void tone_off(void) {}
#endif

static role_t role_now(void)
{
    const ls_lora_chip_t chip = radio_chip();
    if (chip == LS_LORA_CHIP_LR20XX) return ROLE_RECEIVER;
    if (chip == LS_LORA_CHIP_SX126X) return ROLE_TONE;
    return ROLE_NONE;
}

static bool start(char *why, size_t n)
{
    const unsigned f = s_freq < 3 ? s_freq : 0, p = s_power < 3 ? s_power : 0;
    const role_t role = role_now();
    portENTER_CRITICAL(&s_lock);
    memset(&s_view, 0, sizeof(s_view));
    s_view.role = role; s_view.hz = FREQ_HZ[f]; s_view.dbm = POWER_DBM[p];
    s_view.since_us = esp_timer_get_time();
    portEXIT_CRITICAL(&s_lock);
    if (role == ROLE_TONE) {
        const esp_err_t e = tone_on(FREQ_HZ[f], POWER_DBM[p]);
        if (e != ESP_OK) { snprintf(why, n, "SX1262 tone: %s", esp_err_to_name(e)); return false; }
        portENTER_CRITICAL(&s_lock); s_view.tone_on = true; portEXIT_CRITICAL(&s_lock);
        return true;
    }
    if (role != ROLE_RECEIVER) { snprintf(why, n, "No LoRa radio answering"); return false; }
#ifdef ESP_PLATFORM
    radar_dsp_init(&s_dsp);
    radar_dsp_set_background(&s_dsp, s_bg == 0);
    atomic_store(&s_w, 0); atomic_store(&s_r, 0); atomic_store(&s_gap, false);
    const esp_err_t e = lr_wf_start_consumer(FREQ_HZ[f] - TUNE_BELOW_HZ, consume, NULL);
    if (e != ESP_OK) { snprintf(why, n, "LR2021 I/Q: %s", esp_err_to_name(e)); return false; }
    s_streaming = true;
    return true;
#else
    snprintf(why, n, "LR2021 I/Q needs the board"); return false;
#endif
}

static void stop(void)
{
    if (s_view.role == ROLE_TONE) tone_off();
#ifdef ESP_PLATFORM
    if (s_streaming) { lr_wf_stats st; lr_wf_stop_stats(&st); s_streaming = false; }
#endif
    portENTER_CRITICAL(&s_lock); s_view.tone_on = false; portEXIT_CRITICAL(&s_lock);
}

static float s_prev_peak_hz;
static void take_row(const radar_row_t *r)
{
    char text[COLS + 1];
    radar_row_text(r, SPAN_HZ, COLS, text);
    const float mps = radar_speed_mps(r->peak_hz, s_view.hz);
    const summary_t sum = { r->carrier_dbfs, r->offset_hz, r->motion_db, r->peak_hz, r->peak_db, r->learning };
    portENTER_CRITICAL(&s_lock);
    memmove(s_view.wf[1], s_view.wf[0], (ROWS - 1) * sizeof(s_view.wf[0]));
    memcpy(s_view.wf[0], text, sizeof(text));
    if (s_view.wf_n < ROWS) s_view.wf_n++;
    s_view.last = sum; s_view.rows++; s_view.locked = true;
    /* the fastest thing seen, decaying over ~10 s so a passer-by stays up;
       only an echo two rows running, within 20 Hz, so a burst from some
       other 915 MHz transmitter (one row, everywhere at once) is not one */
    s_view.peak_hold_mps *= 0.975f;
    if (r->peak_hz != 0 && s_prev_peak_hz != 0 && fabsf(r->peak_hz - s_prev_peak_hz) <= 20.0f &&
        fabsf(mps) > fabsf(s_view.peak_hold_mps)) s_view.peak_hold_mps = mps;
    s_prev_peak_hz = r->peak_hz;
    portEXIT_CRITICAL(&s_lock);
}

static void poll(void)
{
    if (s_view.role == ROLE_TONE) {
        if (s_view.tone_on && esp_timer_get_time() - s_view.since_us > TONE_MAX_US) {
            tone_off();
            portENTER_CRITICAL(&s_lock); s_view.tone_on = false; s_view.timed_out = true; portEXIT_CRITICAL(&s_lock);
        } else if (s_view.tone_on && tone_keep() != ESP_OK) {
            /* the protection refused the next second; the tone is already off */
            portENTER_CRITICAL(&s_lock); s_view.tone_on = false; s_view.refused = true; portEXIT_CRITICAL(&s_lock);
        }
        return;
    }
    if (!s_streaming) return;
    if (atomic_exchange(&s_gap, false)) {
        radar_dsp_reset(&s_dsp);
        portENTER_CRITICAL(&s_lock); s_view.resets++; s_view.locked = false; portEXIT_CRITICAL(&s_lock);
    }
    /* up to ~33 ms of samples a pass: poll runs every 2 ms */
    unsigned r = atomic_load_explicit(&s_r, memory_order_relaxed);
    const unsigned w = atomic_load_explicit(&s_w, memory_order_acquire);
    unsigned todo = w - r;
    if (todo > 4096) todo = 4096;
    while (todo) {
        const unsigned at = r % RING_PAIRS;
        unsigned run = RING_PAIRS - at < todo ? RING_PAIRS - at : todo;
        const int k = radar_dsp_feed(&s_dsp, &s_ring[at * 4], run, s_rows, 4);
        for (int j = 0; j < k; j++) take_row(&s_rows[j]);
        r += run; todo -= run;
        atomic_store_explicit(&s_r, r, memory_order_release);
    }
    if (!s_dsp.locked && s_view.locked) {
        portENTER_CRITICAL(&s_lock); s_view.locked = false; portEXIT_CRITICAL(&s_lock);
    }
}

static void fmt_clock(int64_t us, char *out, size_t n)
{
    const unsigned s = (unsigned)(us / 1000000);
    snprintf(out, n, "%u:%02u", s / 60, s % 60);
}

static int lines(char (*out)[LS_EXP_LINE], int max)
{
    portENTER_CRITICAL(&s_lock);
    const role_t role = s_view.role;
    const uint32_t hz = s_view.hz;
    const int dbm = s_view.dbm;
    const int64_t since = s_view.since_us;
    const bool tone_on = s_view.tone_on, timed_out = s_view.timed_out, locked = s_view.locked;
    const bool refused = s_view.refused;
    const summary_t last = s_view.last;
    const int wf_n = s_view.wf_n;
    const float hold = s_view.peak_hold_mps;
    char wf[ROWS][COLS + 1];            /* UI and console read at once: not static */
    memcpy(wf, s_view.wf, sizeof(wf));
    portEXIT_CRITICAL(&s_lock);
    int n = 0;
#define LINE(...) do { if (n < max) snprintf(out[n++], LS_EXP_LINE, __VA_ARGS__); } while (0)
    const role_t shown = role != ROLE_NONE ? role : role_now();
    if (shown == ROLE_TONE) {
        char on[12]; fmt_clock(esp_timer_get_time() - since, on, sizeof(on));
        LINE("TONE  %.3f MHz  %+d dBm", (hz ? hz : FREQ_HZ[s_freq % 3]) / 1e6, role ? dbm : POWER_DBM[s_power % 3]);
        if (tone_on) LINE("ON %s - stops by itself at 10:00", on);
        else if (timed_out) LINE("OFF: ten minutes are up. START again for more");
        else if (refused) LINE("OFF: the transmitter protection stopped it");
        else LINE("OFF. START sends the carrier");
        LINE("This board lights the room. Start DOPPLER");
        LINE("RADAR on the LR2021 board, same FREQ, to see");
        LINE("what moves. Plain carrier, ISM band, low power.");
        LINE("TX LIMIT %s", s_limit ? "none" : "scaled by power");
        return n;
    }
    if (shown != ROLE_RECEIVER) { LINE("Needs an SX1262 or an LR2021 radio"); return n; }
    LINE("RECEIVER %.3f MHz  watching +-%.0f Hz", (hz ? hz : FREQ_HZ[s_freq % 3]) / 1e6, (double)SPAN_HZ);
    if (!role) { LINE("START here and on the SX1262 board"); return n; }
    if (!locked) {
        LINE("SEARCHING for the other board's tone...");
        LINE("Is DOPPLER RADAR running there, on %.3f MHz?", hz / 1e6);
        return n;
    }
    /* where the tone landed against where it should: the two crystals */
    const float differ = last.offset_hz - TUNE_BELOW_HZ;
    LINE("TONE %.0f dBFS  boards differ %+.0f Hz (%+.2f ppm)", (double)last.carrier_dbfs,
         (double)differ, (double)(differ * 1e6f / (float)hz));
    /* how much is moving: -80 dBc (still) .. -20 dBc (a lot), in 30 steps */
    char bar[31];
    int fill = (int)lroundf((last.motion_db + 80.0f) / 2.0f);
    if (fill < 0) fill = 0;
    if (fill > 30) fill = 30;
    for (int i = 0; i < 30; i++) bar[i] = i < fill ? (i < 10 ? '-' : i < 20 ? '=' : '#') : '.';
    bar[30] = 0;
    if (last.learning) LINE("MOTION  learning the still room, keep still...");
    else if (last.motion_db <= -100.0f) LINE("MOTION [%s] none", bar);
    else LINE("MOTION [%s] %.0f dBc", bar, (double)last.motion_db);
    if (last.peak_hz != 0) {
        const float mps = radar_speed_mps(last.peak_hz, hz);
        LINE("ECHO %+6.1f Hz  %4.1f m/s %s  %.0f dB up", (double)last.peak_hz, (double)fabsf(mps),
             mps > 0 ? "closer" : "away  ", (double)last.peak_db);
    } else LINE(s_bg ? "ECHO none over the floor" : "ECHO none - nothing changed");
    LINE("FASTEST lately %.1f m/s %s", (double)fabsf(hold), hold > 0 ? "closer" : hold < 0 ? "away" : "");
    /* the bar sits under column COLS / 2 of the rows, after their marker */
    LINE(" away%*s|%*s closer", COLS / 2 - 4, "", COLS / 2 - 7, "");
    for (int i = 0; i < wf_n && i < ROWS; i++) LINE("%c%.49s", i ? ' ' : '>', wf[i]);
    LINE(" -%.0f%*s0%*s+%.0f", (double)SPAN_HZ, COLS / 2 - 4, "", COLS / 2 - 4, "", (double)SPAN_HZ);
    LINE(" Hz; one column %.0f Hz = %.1f m/s", (double)(SPAN_HZ / (COLS / 2)),
         (double)radar_speed_mps(SPAN_HZ / (COLS / 2), hz));
#undef LINE
    return n;
}

static bool configure(int argc, char **argv, char *why, size_t n)
{
    for (int i = 0; i < argc; i++) {
        if (!strcmp(argv[i], "915")) s_freq = 0;
        else if (!strcmp(argv[i], "903")) s_freq = 1;
        else if (!strcmp(argv[i], "433")) s_freq = 2;
        else if (!strcmp(argv[i], "-9")) s_power = 0;
        else if (!strcmp(argv[i], "-6")) s_power = 1;
        else if (!strcmp(argv[i], "-3")) s_power = 2;
        else if (!strcmp(argv[i], "all")) s_bg = 1;
        else if (!strcmp(argv[i], "changes")) s_bg = 0;
        else if (!strcmp(argv[i], "limit")) s_limit = 0;
        else if (!strcmp(argv[i], "nolimit")) s_limit = 1;
        else { snprintf(why, n, "args: 915|903|433, -9|-6|-3 (dBm), changes|all, limit|nolimit"); return false; }
    }
    return true;
}

static int get(const ls_opt_t *o)
{
    return o->names == FREQS ? (int)s_freq : o->names == POWERS ? (int)s_power
         : o->names == LIMITS ? (int)s_limit : (int)s_bg;
}
static void set(const ls_opt_t *o, int v)
{
    if (v < 0 || v >= o->n) return;
    if (o->names == FREQS) s_freq = (unsigned)v;
    else if (o->names == POWERS) s_power = (unsigned)v;
    else if (o->names == LIMITS) s_limit = (unsigned)v;
    else s_bg = (unsigned)v;
    if (ls_exp_running() == &exp_radar) ls_exp_start(&exp_radar);
}
static const ls_opt_t OPTS[] = {
    {.label="FREQ", .kind=LS_OPT_CYCLE, .names=FREQS, .n=3, .get=get, .set=set},
    {.label="TONE POWER", .kind=LS_OPT_CYCLE, .names=POWERS, .n=3, .get=get, .set=set},
    {.label="SHOW", .kind=LS_OPT_CYCLE, .names=BGS, .n=2, .get=get, .set=set},
    {.label="TX LIMIT", .kind=LS_OPT_CYCLE, .names=LIMITS, .n=2, .get=get, .set=set},
};
const ls_experiment_t exp_radar = {
    .id="radar", .name="DOPPLER RADAR", .sub="SX1262 board sends a tone, LR2021 board sees what moves",
    .maturity=LS_EXP_TRYING, .needs="both boards, this on each, same FREQ",
    .start=start, .stop=stop, .poll=poll, .lines=lines, .configure=configure,
    .opts=OPTS, .n_opts=4,
};
