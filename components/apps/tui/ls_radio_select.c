/* See ls_radio_select.h. The tables of who can do what, the defaults, and
   the one picker. The board is asked through ls_rsel_hw, so this file is the
   same on the bench as on the device. */
#include "ls_radio_select.h"

#include <stdio.h>
#include <string.h>

#include "esp_attr.h"
#include "esp_timer.h"
#include "ls_lora.h"
#include "ls_picker.h"
#include "settings.h"

/* ------------------------------------------------------------ the board -- */

/* The board, as last asked. Presence changes when a dongle is plugged in or
   a co-processor answers, so a quarter of a second old is fresh enough, and
   it spares a frame from copying the wireless snapshot for every button. */
#define HW_FRESH_US 250000
static EXT_RAM_BSS_ATTR ls_rsel_hw_t s_hw;
static int64_t s_hw_us;
static bool s_hw_have;

static const ls_rsel_hw_t *hw(void)
{
    const int64_t now = esp_timer_get_time();
    if (!s_hw_have || now - s_hw_us >= HW_FRESH_US || now < s_hw_us) {
        /* Asked into a copy and stored whole: FIND's worker names its radio
           too, and must not catch the table half filled. */
        ls_rsel_hw_t fresh;
        memset(&fresh, 0, sizeof(fresh));
        ls_rsel_hw(&fresh);
        s_hw = fresh;
        s_hw_us = now;
        s_hw_have = true;
    }
    return &s_hw;
}

void ls_rsel_forget(void) { s_hw_have = false; }

static bool radio_ok(ls_rsel_radio_t r) { return r >= 0 && r < LS_RSEL_RADIOS; }
static bool job_ok(ls_rsel_job_t j) { return j >= 0 && j < LS_RSEL_JOBS; }

/* ------------------------------------------------------------- names -- */

const char *ls_rsel_name(ls_rsel_radio_t r)
{
    static const char *const NAME[LS_RSEL_RADIOS] = {
        "RTL-SDR", "HackRF", NULL, "CC1101", "nRF24", "NFC", "Wi-Fi", "Bluetooth", "GPS" };
    if (!radio_ok(r)) return "none";
    if (r != LS_RSEL_LORA) return NAME[r];
    /* The socket takes either part, and the name on the button should be
       the one on the chip. ls_lora calls an SX1262 "SX126x" because the
       driver covers the family; this board fits the SX1262. */
    const ls_rsel_hw_t *h = hw();
    if (!h->present[LS_RSEL_LORA]) return "LoRa";
    if (h->lora_lr20xx && h->lora_name && h->lora_name[0]) return h->lora_name;
    return "SX1262";
}

const char *ls_rsel_absent(ls_rsel_radio_t r)
{
    if (!radio_ok(r)) return "No such radio";
    const ls_rsel_hw_t *h = hw();
    if (h->present[r]) return NULL;
    /* The keyboard board is probed when something first asks for one of
       its radios, so before that answer they count as there: choosing one
       is what starts the probe, and the app says what it found. */
    if ((r == LS_RSEL_CC1101 || r == LS_RSEL_NRF24 || r == LS_RSEL_NFC) && h->mixrf_unknown) return NULL;
    switch (r) {
    case LS_RSEL_SDR_RTL:    return "Plug an RTL-SDR into the USB-A port";
    case LS_RSEL_SDR_HACKRF: return "Plug a HackRF into the USB-A port";
    case LS_RSEL_LORA:       return "No LoRa chip answered at boot";
    case LS_RSEL_CC1101:
    case LS_RSEL_NRF24:
    case LS_RSEL_NFC:        return "Needs the T-MixRF keyboard board";
    case LS_RSEL_WIFI:       return "Co-processor is not answering";
    case LS_RSEL_BLE:        return "Off: turn it on in RADIOS";
    case LS_RSEL_GPS:        return "No GPS on this board";
    default:                 return "Not fitted";
    }
}

/* -------------------------------------------------------- capabilities -- */

/* What a radio is, for the jobs it is plainly the wrong kind of radio for. */
static const char *not_a_receiver(ls_rsel_radio_t r)
{
    switch (r) {
    case LS_RSEL_NRF24: return "2.4 GHz energy survey only";
    case LS_RSEL_NFC:   return "Reads tags a few centimetres away";
    case LS_RSEL_WIFI:  return "A network link, not a receiver";
    case LS_RSEL_BLE:   return "A device link, not a receiver";
    case LS_RSEL_GPS:   return "Hears satellites only";
    default:            return NULL;
    }
}

static bool is_sdr(ls_rsel_radio_t r) { return r == LS_RSEL_SDR_RTL || r == LS_RSEL_SDR_HACKRF; }

/* Whether the LoRa chip has the capability `cap`. An absent chip reports
   no capabilities at all, and that is ls_rsel_absent's to say, so an absent
   chip passes here. */
static bool lora_has(uint32_t cap)
{
    const ls_rsel_hw_t *h = hw();
    return !h->present[LS_RSEL_LORA] || (h->lora_caps & cap);
}

const char *ls_rsel_cant(ls_rsel_job_t job, ls_rsel_radio_t r)
{
    if (!job_ok(job) || !radio_ok(r)) return "Not a radio for this";
    const char *kind = not_a_receiver(r);
    switch (job) {
    case LS_RSEL_FM:
        if (is_sdr(r)) return NULL;
        return kind ? kind : "No IQ stream to demodulate audio";
    case LS_RSEL_PAGER:
        /* The LoRa chip's FSK demodulator with the paging decoder behind
           it: POCSAG, which is two-level FSK, and not FLEX. */
        if (is_sdr(r)) return NULL;
        if (r == LS_RSEL_LORA) return lora_has(LS_LORA_CAP_FSK) ? NULL : "No FSK receiver on this chip";
        return kind ? kind : "No pager decoder for this radio";
    case LS_RSEL_ACARS:
        if (is_sdr(r)) return NULL;
        return kind ? kind : "No AM receiver for 131 MHz";
    case LS_RSEL_P25:
        /* The LR2021 samples C4FM as a bit stream: Phase 1 voice. */
        if (is_sdr(r)) return NULL;
        if (r == LS_RSEL_LORA) return lora_has(LS_LORA_CAP_FSK_STREAM) ? NULL : "P25 needs the LR2021";
        return kind ? kind : "No IQ stream to decode P25";
    case LS_RSEL_ADSB:
        if (is_sdr(r)) return NULL;
        if (r == LS_RSEL_LORA) return lora_has(LS_LORA_CAP_MODES_RX) ? NULL : "No Mode S receiver on this chip";
        return kind ? kind : "Cannot tune 1090 MHz";
    case LS_RSEL_SUBGHZ_READ:
        if (is_sdr(r) || r == LS_RSEL_CC1101) return NULL;
        if (r == LS_RSEL_LORA) return lora_has(LS_LORA_CAP_FSK) ? NULL : "No FSK receiver on this chip";
        return kind;
    case LS_RSEL_SUBGHZ_SWEEP:
        if (r == LS_RSEL_LORA) return NULL;
        if (is_sdr(r)) return "ANALYZER sweeps with the LoRa chip";
        return kind ? kind : "ANALYZER sweeps with the LoRa chip";
    case LS_RSEL_REC:
        /* Raw pulses from the RTL-SDR and the CC1101, FSK captures from the
           LoRa chip, a track from the GPS, and receiver data from the rest. */
        if (r == LS_RSEL_LORA) return lora_has(LS_LORA_CAP_FSK) ? NULL : "No FSK receiver on this chip";
        return NULL;
    case LS_RSEL_WATERFALL:
        /* The SDRs and the LoRa chip draw a spectrum; the rest show what
           they measure. */
        return NULL;
    case LS_RSEL_LABS:
        return r == LS_RSEL_LORA ? NULL : "LoRa Labs drives the LoRa chip";
    case LS_RSEL_JOURNAL:
        /* GPS is the entry with no radio attached: position and motion. */
        return NULL;
    case LS_RSEL_FIND:
    case LS_RSEL_FIND2:
        if (r == LS_RSEL_NFC) return "NFC reaches a few centimetres: hold the board to the tag";
        if (r == LS_RSEL_GPS) return "Satellites are overhead: GPS places each bearing instead";
        return NULL;
    case LS_RSEL_CELL:
        if (r == LS_RSEL_SDR_RTL) return NULL;
        if (r == LS_RSEL_SDR_HACKRF) return "Surveys on the RTL-SDR; HackRF is HIGH RATE";
        return kind ? kind : "Cannot hear the LTE bands";
    case LS_RSEL_MIXRF:
        if (r == LS_RSEL_CC1101 || r == LS_RSEL_NRF24 || r == LS_RSEL_NFC) return NULL;
        return "Not on the T-MixRF keyboard board";
    case LS_RSEL_EXPERIMENT:
        return r == LS_RSEL_LORA ? NULL : "Experiments run on the LoRa chip";
    default:
        return "Not a radio for this";
    }
}

/* Why a radio cannot be used for the job now, or NULL. What it cannot do
   comes first: plugging in a radio that could not do the job would not
   help. */
static const char *why_not(ls_rsel_job_t job, ls_rsel_radio_t r)
{
    const char *why = ls_rsel_cant(job, r);
    return why ? why : ls_rsel_absent(r);
}

static bool usable(ls_rsel_job_t job, ls_rsel_radio_t r)
{
    return radio_ok(r) && !why_not(job, r);
}

/* ------------------------------------------------------------ defaults -- */

/* Each job's own order: what it falls back to, best first. Radios that are
   not listed can still be chosen; they are only never picked for you. */
#define ORDER_MAX LS_RSEL_RADIOS
typedef struct { uint8_t n; uint8_t r[ORDER_MAX]; } order_t;

static const order_t ORDER[LS_RSEL_JOBS] = {
    [LS_RSEL_FM]           = { 2, { LS_RSEL_SDR_RTL, LS_RSEL_SDR_HACKRF } },
    [LS_RSEL_PAGER]        = { 3, { LS_RSEL_SDR_RTL, LS_RSEL_SDR_HACKRF, LS_RSEL_LORA } },
    [LS_RSEL_ACARS]        = { 2, { LS_RSEL_SDR_RTL, LS_RSEL_SDR_HACKRF } },
    [LS_RSEL_P25]          = { 3, { LS_RSEL_SDR_RTL, LS_RSEL_SDR_HACKRF, LS_RSEL_LORA } },
    [LS_RSEL_ADSB]         = { 3, { LS_RSEL_SDR_RTL, LS_RSEL_SDR_HACKRF, LS_RSEL_LORA } },
    [LS_RSEL_SUBGHZ_READ]  = { 3, { LS_RSEL_SDR_RTL, LS_RSEL_CC1101, LS_RSEL_LORA } },
    [LS_RSEL_SUBGHZ_SWEEP] = { 1, { LS_RSEL_LORA } },
    [LS_RSEL_REC]          = { 9, { LS_RSEL_SDR_RTL, LS_RSEL_CC1101, LS_RSEL_LORA, LS_RSEL_GPS,
                                    LS_RSEL_SDR_HACKRF, LS_RSEL_NRF24, LS_RSEL_NFC, LS_RSEL_WIFI,
                                    LS_RSEL_BLE } },
    [LS_RSEL_WATERFALL]    = { 9, { LS_RSEL_SDR_RTL, LS_RSEL_SDR_HACKRF, LS_RSEL_LORA, LS_RSEL_CC1101,
                                    LS_RSEL_NRF24, LS_RSEL_NFC, LS_RSEL_WIFI, LS_RSEL_BLE,
                                    LS_RSEL_GPS } },
    [LS_RSEL_LABS]         = { 1, { LS_RSEL_LORA } },
    [LS_RSEL_JOURNAL]      = { 1, { LS_RSEL_GPS } },
    [LS_RSEL_FIND]         = { 7, { LS_RSEL_LORA, LS_RSEL_SDR_RTL, LS_RSEL_SDR_HACKRF, LS_RSEL_CC1101,
                                    LS_RSEL_NRF24, LS_RSEL_WIFI, LS_RSEL_BLE } },
    [LS_RSEL_FIND2]        = { 7, { LS_RSEL_SDR_RTL, LS_RSEL_SDR_HACKRF, LS_RSEL_LORA, LS_RSEL_CC1101,
                                    LS_RSEL_NRF24, LS_RSEL_WIFI, LS_RSEL_BLE } },
    [LS_RSEL_CELL]         = { 1, { LS_RSEL_SDR_RTL } },
    [LS_RSEL_MIXRF]        = { 3, { LS_RSEL_CC1101, LS_RSEL_NRF24, LS_RSEL_NFC } },
    [LS_RSEL_EXPERIMENT]   = { 1, { LS_RSEL_LORA } },
};

static ls_rsel_radio_t fallback(ls_rsel_job_t job)
{
    const order_t *o = &ORDER[job];
    for (int i = 0; i < o->n; i++)
        if (usable(job, (ls_rsel_radio_t)o->r[i])) return (ls_rsel_radio_t)o->r[i];
    return o->n ? (ls_rsel_radio_t)o->r[0] : LS_RSEL_SDR_RTL;
}

/* ------------------------------------------------------------ choices -- */

static EXT_RAM_BSS_ATTR ls_rsel_radio_t (*s_track[LS_RSEL_JOBS])(void);

ls_rsel_radio_t ls_rsel_get(ls_rsel_job_t job)
{
    if (!job_ok(job)) return LS_RSEL_NONE;
    const ls_rsel_radio_t saved = ls_rsel_saved(job);
    return radio_ok(saved) ? saved : fallback(job);
}

void ls_rsel_set(ls_rsel_job_t job, ls_rsel_radio_t r)
{
    if (!job_ok(job) || !radio_ok(r)) return;
    settings_set_radio_choice((int)job, (int)r);
}

ls_rsel_radio_t ls_rsel_resolved(ls_rsel_job_t job)
{
    if (!job_ok(job)) return LS_RSEL_NONE;
    const ls_rsel_radio_t saved = ls_rsel_saved(job);
    if (usable(job, saved)) return saved;
    return fallback(job);
}

ls_rsel_radio_t ls_rsel_effective(ls_rsel_job_t job)
{
    if (!job_ok(job)) return LS_RSEL_NONE;
    if (s_track[job]) {
        const ls_rsel_radio_t r = s_track[job]();
        if (radio_ok(r)) return r;
    }
    return ls_rsel_resolved(job);
}

void ls_rsel_track(ls_rsel_job_t job, ls_rsel_radio_t (*in_use)(void))
{
    if (job_ok(job)) s_track[job] = in_use;
}

ls_rsel_radio_t ls_rsel_sdr_held_by(const char *owner)
{
    if (!owner || !owner[0]) return LS_RSEL_NONE;
    const ls_rsel_hw_t *h = hw();
    for (int i = 0; i < 2; i++)
        if (!strncmp(h->sdr_owner[i], owner, sizeof(h->sdr_owner[i])))
            return i ? LS_RSEL_SDR_HACKRF : LS_RSEL_SDR_RTL;
    return LS_RSEL_NONE;
}

ls_btn_t ls_rsel_button(ls_rsel_job_t job)
{
    return (ls_btn_t){ "RADIO", ls_rsel_name(ls_rsel_effective(job)), 'r', false, false };
}

void ls_rsel_restart_sdr(void) { ls_rsel_hw_restart(); }

/* ------------------------------------------------------------- picker -- */

static ls_rsel_job_t s_open_job;
static void (*s_open_done)(ls_rsel_radio_t);
/* Rows an app adds below the radios, for what it records or shows that is
   not a radio of its own (REC's mesh traffic log). */
static const ls_rsel_extra_t *s_extra;
static int s_extra_n;
static void (*s_extra_done)(int);

static void fill(ls_rsel_job_t job)
{
    const ls_rsel_radio_t now = ls_rsel_effective(job);
    for (int i = 0; i < LS_RSEL_RADIOS; i++) {
        const ls_rsel_radio_t r = (ls_rsel_radio_t)i;
        const char *why = why_not(job, r);
        ls_picker_add(ls_rsel_name(r), why ? why : r == now ? "selected" : "ready");
    }
}

static void picked(int index)
{
    const ls_rsel_job_t job = s_open_job;
    void (*done)(ls_rsel_radio_t) = s_open_done;
    if (index >= LS_RSEL_RADIOS && index < LS_RSEL_RADIOS + s_extra_n) {
        if (s_extra_done) s_extra_done(index - LS_RSEL_RADIOS);
        return;
    }
    if (index < 0 || index >= LS_RSEL_RADIOS) return;
    const ls_rsel_radio_t r = (ls_rsel_radio_t)index;
    const char *why = why_not(job, r);
    if (why) {
        /* The list stays up, with the reason above it: the choice is
           unchanged and the next tap can be a radio that works. */
        ls_rsel_open_with(job, done, s_extra, s_extra_n, s_extra_done);
        ls_picker_select(index);
        char note[LS_PICKER_DETAIL + 16];
        snprintf(note, sizeof(note), "%s: %s", ls_rsel_name(r), why);
        ls_picker_note(note);
        return;
    }
    ls_rsel_set(job, r);
    if (done) done(r);
}

void ls_rsel_open(ls_rsel_job_t job, void (*done)(ls_rsel_radio_t chosen))
{
    ls_rsel_open_with(job, done, NULL, 0, NULL);
}

void ls_rsel_open_with(ls_rsel_job_t job, void (*done)(ls_rsel_radio_t chosen),
                       const ls_rsel_extra_t *extra, int n, void (*extra_done)(int row))
{
    if (!job_ok(job)) return;
    s_open_job = job;
    s_open_done = done;
    s_extra = extra;
    s_extra_n = extra && n > 0 ? n : 0;
    s_extra_done = extra_done;
    ls_picker_open("RADIO", picked);
    fill(job);
    for (int i = 0; i < s_extra_n; i++) ls_picker_add(extra[i].label, extra[i].detail);
}
