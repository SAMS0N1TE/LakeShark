#include "AppACARS.hpp"

#include "sdr_ui/sdr_ui.h"
#include "ui/ls_ui.h"
#include "ui/ls_receiver_status.h"

#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <cmath>

extern "C" {
#include "acars.h"
#include "acars_app.h"
#include "fm_state.h"
#include "lakeshark_backend.h"
#include "esp_timer.h"
#include "ls_time.h"
}

namespace {
constexpr uint32_t ACARS_CHANS[] = {
    131550000UL, 130025000UL, 129125000UL, 131725000UL,
};
constexpr int ACARS_CHAN_N = (int)(sizeof(ACARS_CHANS) / sizeof(ACARS_CHANS[0]));
}

static void inject_seq(void)
{
    /* Round through a small canned set so a demo on the panel exercises the
       shape a real feed will produce.  These match the shapes covered by
       the bench cases in test_acars_app.c. */
    static int s = 0;
    static const struct {
        const char *reg;
        const char *label;
        const char *text;
    } K[] = {
        { ".N12345", "H1", "UA857 POS N43.5 W71.4 FL350" },
        { ".JA8089", "5Z", "JL7  DEP KSEA 1832Z" },
        { ".G-EUUU", "16", "BA123 WX REQ EGLL" },
        { ".D-AIBL", "Q0", "DLH441 ETA KJFK 2247Z" },
    };
    const auto &m = K[s++ % (int)(sizeof(K) / sizeof(K[0]))];
    acars_app_inject(m.reg, m.label, m.text);
}

AppACARS::AppACARS() : LsApp("ACARS", "acars") {}

bool AppACARS::pause(void)
{
    closeEntry();
    if (_timer) lv_timer_pause(_timer);
    /* Same shape as AppFM::pause - park the radio so the next foregrounded
       app (or a scan) can take the RTL session, and the ACARS mode stops
       consuming IQ while nobody is looking at the panel. */
    lakeshark_acars_stop();
    return true;
}

bool AppACARS::resume(void)
{
    lakeshark_acars_start();
    if (_timer) lv_timer_resume(_timer);
    refresh();
    return true;
}

bool AppACARS::close(void)
{
    closeEntry();
    /* the new log tree is empty on reconstruction, even if the
     * decoder counters have not changed while ACARS was closed. */
    _last_head = -1;
    _last_delivered = _last_bad_crc = 0;
    if (_timer) { lv_timer_del(_timer); _timer = nullptr; }
    _hdr = _tune_lbl = _log_col = _empty = nullptr;
    _freq = _gain = _signal = _entry_status = nullptr;
    _agc_btn = nullptr;
    _screen_readout = _screen_lamp = nullptr;
    lakeshark_acars_stop();
    return true;
}

bool AppACARS::run(lv_obj_t *parent)
{
    ls_ui_screen_t screen;
    ls_ui_screen_create(parent, nullptr, false, LS_UI_COLOR_ID_VIOLET, &screen);
    _screen_readout = screen.readout;
    _screen_lamp = screen.lamp;
    ls_ui_screen_set_readout(&screen, "131.550 MHz");
    parent = screen.content;

    buildHeader(parent);
    buildTuning(parent);
    buildLog(parent);
    lv_obj_clear_flag(screen.controls, LV_OBJ_FLAG_HIDDEN);
    buildFooter(screen.controls);

    lakeshark_acars_start();

    refresh();
    _timer = lv_timer_create(timerCb, 500, this);
    return true;
}

void AppACARS::buildHeader(lv_obj_t *parent)
{
    lv_obj_t *panel = sdr_lcd_panel(parent, SDR_ROLE_COLOR(LS_UI_COLOR_ID_VIOLET));
    _freq = sdr_label(panel, &lv_font_montserrat_48, SDR_BRIGHT);
    lv_obj_set_width(_freq, lv_pct(100));
    lv_obj_set_style_text_align(_freq, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_add_flag(_freq, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(_freq, freqCb, LV_EVENT_CLICKED, this);
    _tune_lbl = sdr_label(panel, sdr_font_mono_sm(), SDR_LABEL);
    lv_obj_set_width(_tune_lbl, lv_pct(100));
    lv_label_set_long_mode(_tune_lbl, LV_LABEL_LONG_WRAP);
    _signal = sdr_label(panel, sdr_font_mono_sm(), SDR_DIM);
    lv_obj_set_width(_signal, lv_pct(100));
    lv_label_set_long_mode(_signal, LV_LABEL_LONG_WRAP);
    _hdr = sdr_label(panel, &lv_font_montserrat_16, SDR_PAS_GREEN);
    lv_obj_set_width(_hdr, lv_pct(100));
    lv_label_set_long_mode(_hdr, LV_LABEL_LONG_WRAP);
    lv_label_set_text(_hdr, "MSGS 0    BAD 0    SYNC 0");
}

void AppACARS::buildLog(lv_obj_t *parent)
{
    sdr_section(parent, "MESSAGES  (newest first)");

    /* A scrollable column that holds one card per message.  Cards are
       rebuilt on every refresh - the log is small (ACARS_MSG_LOG_MAX = 8)
       and the message text is short, so redraw cost stays well under the
       500 ms refresh cadence. */
    lv_obj_t *box = lv_obj_create(parent);
    lv_obj_set_width(box, lv_pct(100));
    lv_obj_set_height(box, 0);
    lv_obj_set_style_min_height(box, 150, 0);
    lv_obj_set_flex_grow(box, 1);
    ls_ui_style_scroll_panel(box);
    lv_obj_set_flex_flow(box, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_scroll_dir(box, LV_DIR_VER);

    lv_obj_set_scrollbar_mode(box, LV_SCROLLBAR_MODE_ON);

    _log_col = box;

    _empty = sdr_label(box, sdr_font_mono(), SDR_DIM);
    lv_obj_set_width(_empty, lv_pct(100));
    lv_label_set_text(_empty, "(listening for aircraft text...)");
}

void AppACARS::buildTuning(lv_obj_t *parent)
{
    /* two explicit equal-width groups keep manual tuning reachable
     * without a natural-width row escaping the 480 px receiver panel. */
    lv_obj_t *panel = ls_ui_panel(parent, nullptr);
    lv_obj_t *row = ls_ui_button_group(panel);
    ls_ui_group_button(row, "FREQ MHz", LS_BTN_PRIMARY, freqCb, this, nullptr);
    ls_ui_group_button(row, "PRESET", LS_BTN_DEFAULT, chanCb, this, nullptr);
    _gain = sdr_label(panel, sdr_font_mono_sm(), SDR_LABEL);
    lv_obj_set_width(_gain, lv_pct(100));
    lv_label_set_long_mode(_gain, LV_LABEL_LONG_WRAP);
    row = ls_ui_button_group(panel);
    ls_ui_group_button(row, "GAIN dB", LS_BTN_DEFAULT, gainCb, this, nullptr);
    _agc_btn = ls_ui_group_button(row, "AGC", LS_BTN_TOGGLE_OFF, agcCb, this, nullptr);
    _entry_status = sdr_label(panel, sdr_font_mono_sm(), SDR_AMBER);
    lv_obj_set_width(_entry_status, lv_pct(100));
    lv_label_set_long_mode(_entry_status, LV_LABEL_LONG_WRAP);
    lv_obj_add_flag(_entry_status, LV_OBJ_FLAG_HIDDEN);
    updateTuningLabel();
}

void AppACARS::updateTuningLabel(void)
{
    if (!_tune_lbl) return;
    uint32_t hz = lakeshark_acars_get_freq();
    ls_iq_control_status_t radio;
    fm_get_receiver_status(&radio);
    ls_receiver_presentation_t receiver;
    ls_receiver_present(&radio, &receiver);

    ls_ui_button_set_role(_agc_btn, radio.requested_gain_tenths_db == 0
        ? LS_BTN_TOGGLE_ON : LS_BTN_TOGGLE_OFF);
    char s[128];
    snprintf(s, sizeof(s), "%.4f", hz / 1e6);
    lv_label_set_text(_freq, s);
    if (radio.receiver_streaming) {
        snprintf(s, sizeof(s), "IQ %.0f%%   %lu kB/s   RX ERR %lu",
            (double)FM.iq_level * 100.0, (unsigned long)(FM.iq_bytes_sec / 1000),
            (unsigned long)FM.read_errors);
        lv_label_set_text(_signal, s);
    } else
        lv_label_set_text(_signal, "IQ --   RX idle");
    lv_label_set_text_fmt(_gain, "GAIN  %s", receiver.gain);
    lv_obj_set_style_text_color(_gain,
        receiver.gain_attention ? SDR_PAS_ROSE : SDR_LABEL, 0);
    if (radio.receiver_streaming)
        snprintf(s, sizeof(s), "%s  |  %s", receiver.frequency, receiver.connection);
    else
        snprintf(s, sizeof(s), "NO RX   %s",
                 ls_radio_err_name(radio.receiver_error));
    lv_label_set_text(_tune_lbl, s);
    lv_obj_set_style_text_color(_tune_lbl,
        radio.receiver_streaming && !receiver.tune_attention ? SDR_LABEL : SDR_PAS_ROSE, 0);
    if (radio.receiver_streaming)
        snprintf(s, sizeof(s), "%7.3f MHz", hz / 1e6);
    else
        snprintf(s, sizeof(s), "NO RX");
    ls_ui_readout_set(_screen_readout, s);
    ls_ui_lamp_set(_screen_lamp, radio.receiver_streaming,
                   LS_UI_COLOR_ACCENT);
}

void AppACARS::buildFooter(lv_obj_t *parent)
{
    lv_obj_t *row = ls_ui_button_group(parent);
    ls_ui_group_button(row, "DEMO MSG", LS_BTN_DEFAULT, injectCb, this, nullptr);
    ls_ui_group_button(row, "CLEAR LOG", LS_BTN_DANGER, clearCb, this, nullptr);
}

static lv_obj_t *make_card(lv_obj_t *parent, bool is_newest)
{
    lv_obj_t *c = lv_obj_create(parent);
    lv_obj_set_width(c, lv_pct(100));
    lv_obj_set_height(c, LV_SIZE_CONTENT);
    ls_ui_style_card(c, is_newest);
    lv_obj_set_flex_flow(c, LV_FLEX_FLOW_COLUMN);
    lv_obj_clear_flag(c, LV_OBJ_FLAG_SCROLLABLE);
    return c;
}

void AppACARS::refresh(void)
{
    const acars_state_t *s = acars_app_state();
    if (!_hdr || !_log_col) return;

    ls_iq_control_status_t radio;
    fm_get_receiver_status(&radio);
    char h[96];
    if (radio.receiver_streaming) {
        snprintf(h, sizeof(h), "MSGS %lu    BAD %lu    SYNC %lu",
                 (unsigned long)s->n_delivered,
                 (unsigned long)s->n_bad_crc,
                 (unsigned long)s->n_synced);
    } else {
        /* a selected ACARS submode is only intent.  Task creation can
           fail before one IQ byte is received, so the dedicated panel must
           not keep its green lamp and "listening" claim in that state. */
        snprintf(h, sizeof(h), "NO RX (%s)    MSGS %lu",
                 ls_radio_err_name(radio.receiver_error),
                 (unsigned long)s->n_delivered);
    }
    lv_label_set_text(_hdr, h);

    /* The radio may have been retuned from the shell or via CHAN; keep
       the tuning strip honest without polling it every frame if nothing
       moved. */
    updateTuningLabel();
    if (_empty) {
        lv_label_set_text(_empty, radio.receiver_streaming
            ? "(listening for aircraft text...)"
            : "(receiver unavailable - not listening)");
    }

    /* Skip a rebuild when nothing has changed - the timer runs twice a
       second and we would otherwise thrash the LVGL object tree even while
       the panel is idle. */
    if ((uint32_t)s->n_delivered == _last_delivered &&
        (uint32_t)s->n_bad_crc   == _last_bad_crc   &&
        s->msg_head              == _last_head)
        return;
    _last_delivered = (uint32_t)s->n_delivered;
    _last_bad_crc   = (uint32_t)s->n_bad_crc;
    _last_head      = s->msg_head;

    /* Rebuild the log column: clear children, then push cards newest-first.
       lv_obj_clean() also disposes _empty; recreate it on the empty path. */
    lv_obj_clean(_log_col);
    _empty = nullptr;

    if (s->msg_count == 0) {
        _empty = sdr_label(_log_col, sdr_font_mono(), SDR_DIM);
        lv_obj_set_width(_empty, lv_pct(100));
        lv_label_set_text(_empty, radio.receiver_streaming
            ? "(listening for aircraft text...)"
            : "(receiver unavailable - not listening)");
        return;
    }

    for (int k = 0; k < s->msg_count; k++) {
        int idx = (s->msg_head - 1 - k + ACARS_MSG_LOG_MAX * 2) % ACARS_MSG_LOG_MAX;
        const acars_msg_out_t *m = &s->msgs[idx];
        lv_obj_t *card = make_card(_log_col, k == 0);

        /* Row 1 - the top strip carries WHO: registration, extracted flight
           and message label.  Newest gets a colour bump so the eye lands on
           it without having to read the timestamp. */
        char stamp[LS_TIME_STAMP_MAX];
        ls_time_render_stamp_at(stamp, sizeof(stamp), (time_t)m->ts_epoch, m->ts_us);

        char flight[16];
        acars_flight_from_text(m->text, flight, sizeof(flight));

        char top[160];
        snprintf(top, sizeof(top),
                 "%s\nREG %s   FLT %s   LBL %s   BLK %c",
                 stamp,
                 m->reg[0]   ? m->reg   : "-------",
                 flight[0]   ? flight   : "-",
                 m->label[0] ? m->label : "--",
                 m->block_id ? m->block_id : ' ');
        lv_obj_t *strip = sdr_label(card, sdr_font_mono_sm(),
                                    k == 0 ? sdr_accent() : SDR_LABEL);
        lv_obj_set_width(strip, lv_pct(100));
        lv_label_set_long_mode(strip, LV_LABEL_LONG_WRAP);
        lv_label_set_text(strip, top);

        lv_obj_t *body = sdr_label(card, sdr_font_mono(),
                                   k == 0 ? SDR_BRIGHT : SDR_TEXT);
        lv_obj_set_width(body, lv_pct(100));
        lv_label_set_long_mode(body, LV_LABEL_LONG_WRAP);
        lv_label_set_text(body, m->text[0] ? m->text : "(no text)");

        if (m->parity_errors > 0) {
            char pe[48];
            snprintf(pe, sizeof(pe), "parity errors: %d", m->parity_errors);
            lv_obj_t *w = sdr_label(card, sdr_font_mono_sm(), SDR_AMBER);
            lv_label_set_text(w, pe);
        }
    }

    lv_obj_scroll_to_y(_log_col, 0, LV_ANIM_OFF);
}

void AppACARS::timerCb(lv_timer_t *t)
{
    static_cast<AppACARS *>(t->user_data)->refresh();
}

void AppACARS::injectCb(lv_event_t *)
{
    inject_seq();
}

void AppACARS::clearCb(lv_event_t *)
{
    acars_app_clear();
}

void AppACARS::chanCb(lv_event_t *e)
{
    auto *self = static_cast<AppACARS *>(lv_event_get_user_data(e));
    uint32_t cur = lakeshark_acars_get_freq();
    int idx = 0;
    for (int i = 0; i < ACARS_CHAN_N; i++) {
        if (ACARS_CHANS[i] == cur) { idx = (i + 1) % ACARS_CHAN_N; goto go; }
    }
go:
    lakeshark_acars_set_freq(ACARS_CHANS[idx]);
    if (self) self->updateTuningLabel();
}

void AppACARS::freqCb(lv_event_t *e)
{
    static_cast<AppACARS *>(lv_event_get_user_data(e))->openEntry(false);
}

void AppACARS::gainCb(lv_event_t *e)
{
    static_cast<AppACARS *>(lv_event_get_user_data(e))->openEntry(true);
}

void AppACARS::agcCb(lv_event_t *e)
{
    lakeshark_fm_agc();
    static_cast<AppACARS *>(lv_event_get_user_data(e))->updateTuningLabel();
}

void AppACARS::openEntry(bool gain)
{
    closeEntry();
    _entry_gain = gain;
    lv_obj_add_flag(_entry_status, LV_OBJ_FLAG_HIDDEN);
    char initial[24];
    if (gain) snprintf(initial, sizeof(initial), "%.1f", lakeshark_fm_gain_tenths() / 10.0);
    else snprintf(initial, sizeof(initial), "%.4f", lakeshark_acars_get_freq() / 1e6);
    const ls_text_entry_config_t config = {
        .title = gain ? "GAIN dB (0 = AGC, max 49.6)" : "FREQUENCY MHz (118 - 137)",
        .text = initial,
        .placeholder = gain ? "20.0" : "131.550",
        .accepted_chars = "0123456789.",
        .max_length = 10,
        .width = lv_pct(90),
        .mode = LS_TEXT_ENTRY_NUMBER,
        .large = true,
    };
    _entry = ls_text_entry_open(&config, entryDone, this);
}

void AppACARS::entryDone(bool accepted, const char *text, void *user_data)
{
    auto *self = static_cast<AppACARS *>(user_data);
    self->_entry = nullptr;
    if (!accepted) return;
    char *end = nullptr;
    const double value = strtod(text, &end);
    const bool valid = end != text && *end == '\0' && std::isfinite(value) &&
        (self->_entry_gain ? value >= 0.0 && value <= 49.6
                           : value >= 118.0 && value <= 137.0);
    if (!valid) {
        lv_label_set_text(self->_entry_status, self->_entry_gain
            ? "Enter 0 - 49.6 dB (0 = AGC)." : "Enter 118 - 137 MHz.");
        lv_obj_clear_flag(self->_entry_status, LV_OBJ_FLAG_HIDDEN);
        return;
    }
    if (self->_entry_gain) lakeshark_fm_set_gain((int)(value * 10.0 + 0.5));
    else lakeshark_acars_set_freq((uint32_t)(value * 1e6 + 0.5));
    self->updateTuningLabel();
}

void AppACARS::closeEntry()
{
    if (!_entry) return;
    auto *entry = _entry;
    _entry = nullptr;
    ls_text_entry_close(entry);
}
