/*LS-746*/
/* The one scanner control surface. See scan_panel.hpp for why it exists. */

#include "scan_panel.hpp"

#include <cstdio>
#include <cstring>
#include <cstdint>

#include "scan_engine.h"
#include "scan_channels.h"
#include "lakeshark_backend.h"
#include "sdr_ui/sdr_ui.h"
#include "ui/ls_ui.h"

/*LS-746*/
/* Band presets. These are the ranges someone actually points a handheld at,
   and the STEP that band is channelised on - 12.5 kHz through the land mobile
   ranges, 25 kHz for air and the wide ham allocations. The step travels WITH
   the preset because picking "GMRS" and then being left on a 100 kHz grid
   walks straight past every channel in it, which is the kind of quiet wrong
   answer this project keeps having to unpick.
   151-152 is carved out separately from VHF HI on purpose: it is narrow
   enough to sweep in seconds when you are chasing one known signal, which is
   how this surface actually gets used in the field. */
static const struct {
    uint32_t a, b, step;
    const char *name;
} PRESETS[] = {
    { 150000000UL, 162000000UL, 12500UL, "VHF HI 150-162"   },
    { 151000000UL, 152000000UL, 12500UL, "VHF 151-152"      },
    { 154000000UL, 155000000UL, 12500UL, "FIRE/EMS 154-155" },
    { 144000000UL, 148000000UL, 12500UL, "2m HAM 144-148"   },
    { 162400000UL, 162550000UL, 25000UL, "NOAA WX"          },
    { 118000000UL, 137000000UL, 25000UL, "AIR 118-137"      },
    { 450000000UL, 460000000UL, 12500UL, "UHF 450-460"      },
    { 462000000UL, 468000000UL, 12500UL, "GMRS/FRS 462+"    },
    { 420000000UL, 450000000UL, 25000UL, "70cm HAM 420-450" },
};
static const int PRESET_N = (int)(sizeof(PRESETS) / sizeof(PRESETS[0]));

/* The channel steps a land-mobile radio actually offers. 12.5 is the one the
   handheld-style scan is named after; 6.25 exists because narrowbanding is
   still creeping downward. */
static const uint32_t STEPS_HZ[] = {
    5000UL, 6250UL, 10000UL, 12500UL, 15000UL, 20000UL, 25000UL, 30000UL, 50000UL
};
static const int STEP_N = (int)(sizeof(STEPS_HZ) / sizeof(STEPS_HZ[0]));

/*LS-736*/
/* How far one press moves a band edge. This used to be four buttons per edge
   - -1M -100k +100k +1M - which is eight controls to express two directions,
   and on the 480 px panel the pair of four-button groups is what ran past the
   right edge. One selectable amount and a previous/next pair per edge covers
   strictly more than the old cluster did: the old one could not move an edge
   by a channel step at all. */
static const uint32_t NUDGES_HZ[] = {
    6250UL, 12500UL, 25000UL, 100000UL, 1000000UL
};
static const int NUDGE_N = (int)(sizeof(NUDGES_HZ) / sizeof(NUDGES_HZ[0]));

/* mhz_str/khz_str: LVGL is built without LV_SPRINTF_USE_FLOAT, so a %f handed
   to any lv_*_fmt() corrupts every conversion after it - see LS-735. Every
   number on this panel is formatted with real snprintf and set as a finished
   string. Do not "simplify" these into lv_label_set_text_fmt. */
static void mhz_str(char *b, size_t n, uint32_t hz)
{
    snprintf(b, n, "%lu.%04lu",
             (unsigned long)(hz / 1000000UL),
             (unsigned long)((hz % 1000000UL) / 100UL));
}

static void khz_str(char *b, size_t n, uint32_t hz)
{
    unsigned long whole = hz / 1000UL;
    unsigned long frac  = (hz % 1000UL) / 100UL;
    if (frac) snprintf(b, n, "%lu.%01lu kHz", whole, frac);
    else      snprintf(b, n, "%lu kHz", whole);
}

static int nearest_step_idx(uint32_t hz)
{
    int best = 0;
    uint32_t bd = UINT32_MAX;
    for (int i = 0; i < STEP_N; i++) {
        uint32_t d = (STEPS_HZ[i] > hz) ? (STEPS_HZ[i] - hz) : (hz - STEPS_HZ[i]);
        if (d < bd) { bd = d; best = i; }
    }
    return best;
}

static lv_obj_t *scan_button_group(ls_ui_value_t *row)
{
    if (!row || !row->controls) return nullptr;
    /* LS-734: ls_ui_value now gives controls the measured full row width.  A
     * group can divide that concrete line directly; overriding the parent with
     * another percentage recreated the content/percentage sizing cycle. */
    return ls_ui_button_group(row->controls);
}

void ScanPanel::build(lv_obj_t *parent)
{
    ls_ui_value_t r;

    /* The status line is the SAME string the console `scan status` prints, on
       purpose: the screen and the serial log cannot disagree about what the
       scanner is doing, which they did before LS-731. */
    lv_obj_t *hp = ls_ui_panel(parent, "CARRIER SCAN STATUS");
    _status = sdr_label(hp, sdr_font_mono(), LS_UI_ACCENT);
    lv_obj_set_width(_status, lv_pct(100));
    lv_label_set_long_mode(_status, LV_LABEL_LONG_WRAP);
    lv_label_set_text(_status, "scanner idle");

    ls_ui_value(parent, "CARRIER SCAN", &r);
    lv_label_set_text(r.value, "");
    lv_obj_t *group = scan_button_group(&r);
    ls_ui_group_button(group, "SCAN", LS_BTN_TOGGLE_OFF, toggleCb, this, &_run_lbl);
    ls_ui_group_button(group, "SKIP", LS_BTN_DEFAULT, skipCb, this, nullptr);

    /* PRESET walks the stored list filtered by zone; BAND ignores the store
       and walks a bare frequency grid. Everything downstream - hold, hang,
       squelch, carrier test - is shared, which is why this is a source and
       not a second scanner (LS-733). */
    ls_ui_value(parent, "SOURCE", &r);
    _src_val = r.value;
    group = scan_button_group(&r);
    ls_ui_group_button(group, "PRESET/BAND", LS_BTN_DEFAULT, srcCb, this, nullptr);

    ls_ui_section(parent, "BAND");

    ls_ui_value(parent, "RANGE", &r);
    _band_val = r.value;
    ls_ui_stepper(&r, presetPrevCb, this, presetNextCb, this);

    ls_ui_value(parent, "STEP", &r);
    _step_val = r.value;
    ls_ui_stepper(&r, stepPrevCb, this, stepNextCb, this);

    /*LS-736*/
    ls_ui_value(parent, "NUDGE", &r);
    _nudge_val = r.value;
    ls_ui_stepper(&r, nudgePrevCb, this, nudgeNextCb, this);

    ls_ui_value(parent, "START", &r);
    _start_val = r.value;
    ls_ui_stepper(&r, startPrevCb, this, startNextCb, this);

    ls_ui_value(parent, "STOP", &r);
    _stop_val = r.value;
    ls_ui_stepper(&r, stopPrevCb, this, stopNextCb, this);

    ls_ui_section(parent, "SQUELCH");

    /* AUTO SQ measures the floor and sits a margin above it. The default of 15
       has now caused three separate misdiagnoses because it is a constant and
       the floor is not - see LS-736. It belongs next to the scan controls
       because "the scanner will not stop" and "the scanner will not release"
       are both this number. */
    ls_ui_value(parent, "NFM SQUELCH", &r);
    _sq_val = r.value;
    group = scan_button_group(&r);
    ls_ui_group_button(group, "AUTO SQ", LS_BTN_DEFAULT, asqCb, this, nullptr);

    /* HANG belongs here rather than on one app's tab: it is how long the
       engine sits on a hit before resuming, which is the same question in
       both sources and both demodulators. */
    ls_ui_value(parent, "HANG", &r);
    _hang_val = r.value;
    ls_ui_stepper(&r, hangPrevCb, this, hangNextCb, this);

    uint32_t a = 0, b = 0, st = 0;
    scan_engine_get_band(&a, &b, &st);
    _step = nearest_step_idx(st ? st : 12500UL);

    refresh();
}

void ScanPanel::forget(void)
{
    _status = _run_lbl = _src_val = _band_val = _start_val = _stop_val =
        _step_val = _sq_val = _hang_val = _nudge_val = nullptr;
}

void ScanPanel::applyPreset(int idx)
{
    if (idx < 0 || idx >= PRESET_N) return;
    _preset = idx;
    _step   = nearest_step_idx(PRESETS[idx].step);
    scan_engine_set_band(PRESETS[idx].a, PRESETS[idx].b, PRESETS[idx].step);
    /* Choosing a band IS the intent to scan it - the same reasoning that made
       the console `scan band` flip the source (LS-733). */
    scan_engine_set_source(SCAN_SRC_BAND);
}

void ScanPanel::syncStep(void)
{
    uint32_t a = 0, b = 0, st = 0;
    scan_engine_get_band(&a, &b, &st);
    scan_engine_set_band(a, b, STEPS_HZ[_step]);
}

void ScanPanel::refresh(void)
{
    char buf[96];

    if (_status) {
        char scan_status[160];
        scan_engine_status(scan_status, sizeof(scan_status));
        sdr_text_if_changed(_status, scan_status);
    }

    if (_run_lbl)
        sdr_text_if_changed(_run_lbl, scan_engine_active() ? "STOP" : "SCAN");

    uint32_t a = 0, b = 0, st = 0;
    scan_engine_get_band(&a, &b, &st);

    if (_src_val) {
        if (scan_engine_get_source() == SCAN_SRC_BAND)
            snprintf(buf, sizeof(buf), "BAND  %d steps", scan_engine_band_steps());
        else
            snprintf(buf, sizeof(buf), "PRESET  %d stored", scan_channels_count());
        sdr_text_if_changed(_src_val, buf);
    }

    if (_band_val) {
        char x[16], y[16];
        mhz_str(x, sizeof(x), a);
        mhz_str(y, sizeof(y), b);
        snprintf(buf, sizeof(buf), "%s  %s-%s",
                 PRESETS[_preset].name, x, y);
        sdr_text_if_changed(_band_val, buf);
    }

    if (_start_val) {
        mhz_str(buf, sizeof(buf), a);
        sdr_text_if_changed(_start_val, buf);
    }
    if (_stop_val) {
        mhz_str(buf, sizeof(buf), b);
        sdr_text_if_changed(_stop_val, buf);
    }

    if (_step_val) {
        char s[24];
        khz_str(s, sizeof(s), st);
        snprintf(buf, sizeof(buf), "%s  (%d steps)", s, scan_engine_band_steps());
        sdr_text_if_changed(_step_val, buf);
    }

    if (_hang_val) {
        int ms = scan_engine_get_hang_ms();
        snprintf(buf, sizeof(buf), "%d.%01d s", ms / 1000, (ms % 1000) / 100);
        sdr_text_if_changed(_hang_val, buf);
    }

    /*LS-736*/
    if (_nudge_val) {
        khz_str(buf, sizeof(buf), NUDGES_HZ[_nudge]);
        sdr_text_if_changed(_nudge_val, buf);
    }

    if (_sq_val) {
        int floor_pct = scan_engine_autosquelch_floor();
        if (floor_pct >= 0)
            snprintf(buf, sizeof(buf), "sq=%d   floor=%d",
                     lakeshark_fm_squelch_get(), floor_pct);
        else
            snprintf(buf, sizeof(buf), "sq=%d   floor=--",
                     lakeshark_fm_squelch_get());
        sdr_text_if_changed(_sq_val, buf);
    }
}

/*LS-736*/
static void hang_step(int direction)
{
    int ms = scan_engine_get_hang_ms() + direction * 500;
    if (ms < 0)     ms = 0;
    if (ms > 10000) ms = 10000;
    scan_engine_set_hang_ms(ms);
}

void ScanPanel::hangPrevCb(lv_event_t *e)
{
    ScanPanel *self = static_cast<ScanPanel *>(lv_event_get_user_data(e));
    hang_step(-1);
    if (self) self->refresh();
}

void ScanPanel::hangNextCb(lv_event_t *e)
{
    ScanPanel *self = static_cast<ScanPanel *>(lv_event_get_user_data(e));
    hang_step(+1);
    if (self) self->refresh();
}

void ScanPanel::toggleCb(lv_event_t *e)
{
    (void)e;
    if (scan_engine_active()) scan_engine_stop();
    else                      scan_engine_start();
}

void ScanPanel::skipCb(lv_event_t *e)
{
    (void)e;
    if (scan_engine_active()) scan_engine_skip();
}

void ScanPanel::srcCb(lv_event_t *e)
{
    ScanPanel *self = static_cast<ScanPanel *>(lv_event_get_user_data(e));
    scan_engine_set_source(scan_engine_get_source() == SCAN_SRC_CHANNELS
                           ? SCAN_SRC_BAND : SCAN_SRC_CHANNELS);
    if (self) self->refresh();
}

void ScanPanel::presetPrevCb(lv_event_t *e)
{
    ScanPanel *self = static_cast<ScanPanel *>(lv_event_get_user_data(e));
    if (!self) return;
    self->applyPreset((self->_preset + PRESET_N - 1) % PRESET_N);
    self->refresh();
}

void ScanPanel::presetNextCb(lv_event_t *e)
{
    ScanPanel *self = static_cast<ScanPanel *>(lv_event_get_user_data(e));
    if (!self) return;
    self->applyPreset((self->_preset + 1) % PRESET_N);
    self->refresh();
}

void ScanPanel::stepPrevCb(lv_event_t *e)
{
    ScanPanel *self = static_cast<ScanPanel *>(lv_event_get_user_data(e));
    if (!self) return;
    self->_step = (self->_step + STEP_N - 1) % STEP_N;
    self->syncStep();
    self->refresh();
}

void ScanPanel::stepNextCb(lv_event_t *e)
{
    ScanPanel *self = static_cast<ScanPanel *>(lv_event_get_user_data(e));
    if (!self) return;
    self->_step = (self->_step + 1) % STEP_N;
    self->syncStep();
    self->refresh();
}

/*LS-736*/
void ScanPanel::nudgePrevCb(lv_event_t *e)
{
    ScanPanel *self = static_cast<ScanPanel *>(lv_event_get_user_data(e));
    if (!self) return;
    self->_nudge = (self->_nudge + NUDGE_N - 1) % NUDGE_N;
    self->refresh();
}

void ScanPanel::nudgeNextCb(lv_event_t *e)
{
    ScanPanel *self = static_cast<ScanPanel *>(lv_event_get_user_data(e));
    if (!self) return;
    self->_nudge = (self->_nudge + 1) % NUDGE_N;
    self->refresh();
}

/* Nudging either edge keeps the current step. scan_engine_set_band() refuses a
   range it cannot use, so a nudge that would invert start/stop is simply not
   applied rather than silently producing a zero-step grid. */
void ScanPanel::nudgeEdge(bool start_edge, int direction)
{
    const int64_t delta = (int64_t)NUDGES_HZ[_nudge] * direction;
    uint32_t a = 0, b = 0, st = 0;
    scan_engine_get_band(&a, &b, &st);
    if (start_edge) {
        int64_t na = (int64_t)a + delta;
        if (na < 1000000LL) return;
        scan_engine_set_band((uint32_t)na, b, st);
    } else {
        int64_t nb = (int64_t)b + delta;
        if (nb < 1000000LL) return;
        scan_engine_set_band(a, (uint32_t)nb, st);
    }
    refresh();
}

void ScanPanel::startPrevCb(lv_event_t *e)
{
    ScanPanel *self = static_cast<ScanPanel *>(lv_event_get_user_data(e));
    if (self) self->nudgeEdge(true, -1);
}

void ScanPanel::startNextCb(lv_event_t *e)
{
    ScanPanel *self = static_cast<ScanPanel *>(lv_event_get_user_data(e));
    if (self) self->nudgeEdge(true, +1);
}

void ScanPanel::stopPrevCb(lv_event_t *e)
{
    ScanPanel *self = static_cast<ScanPanel *>(lv_event_get_user_data(e));
    if (self) self->nudgeEdge(false, -1);
}

void ScanPanel::stopNextCb(lv_event_t *e)
{
    ScanPanel *self = static_cast<ScanPanel *>(lv_event_get_user_data(e));
    if (self) self->nudgeEdge(false, +1);
}

void ScanPanel::asqCb(lv_event_t *e)
{
    (void)e;
    /* Asynchronous - it tunes and blocks, so it runs on the scan task and the
       result turns up in the status line (LS-736). */
    scan_engine_autosquelch(-1);
}
