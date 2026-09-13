#include "ls_field.h"
#include "esp_timer.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static ls_field_state_t s;
static ls_journal_entry_t entry;
bool ls_field_start(void)
{
    if (s.ready) return true;
    s.ready = true;
    s.config = (ls_lora_cfg_t){.freq_hz=910525000, .sf=7, .bw_hz=125000, .cr=5, .power_dbm=10, .preamble=16, .sync_word=0x12, .crc_on=true};
    s.sample = (ls_field_sample_t){.source=LS_FIELD_MESH, .gps_valid=true, .imu_valid=true, .radio_valid=true,
        .lat=43.20, .lon=-71.65, .heading=72, .rssi=-88, .frequency=910525000,
        .imu={.ax=.02, .ay=.06, .az=.99, .gx=.3, .gy=-.2, .gz=.1, .mx=22, .my=31, .mz=-19, .mag_valid=true}};
    snprintf(s.sample.utc, sizeof(s.sample.utc), "2026-09-12T14:20:00Z");
    snprintf(s.status, sizeof(s.status), "Mesh keeps control until DIRECT is enabled");
    snprintf(s.storage, sizeof(s.storage), "Preview fixture | /sdcard/journal");
    entry = (ls_journal_entry_t){.id=1, .sample=s.sample, .saved=true};
    snprintf(entry.title, sizeof(entry.title), "Ridge trail / north overlook");
    snprintf(entry.text, sizeof(entry.text), "Mesh reception returned near the overlook. Clear sky and a steady GPS fix. Repeat this walk with the same antenna and compare the eastern path.\n\nThe radio sample, my position and all nine motion axes are attached to this note.");
    s.journal_count = 1;
    for (int i = 0; i < LS_FIELD_BINS; i++) {
        s.trace[i] = -105 + sinf(i * .32f) * 9 + (i > 36 && i < 44 ? 30 : 0);
        s.spectrum[i] = -120 + 65 * expf(-powf((i - 31) / 4.0f, 2)) + 20 * expf(-powf((i - 49) / 6.0f, 2));
    }
    for (int i = 0; i < 36; i++) { s.bearing[i] = -85 + 20 * cosf(i * .17453f); s.bearing_count[i] = 3; }
    return true;
}
void ls_field_snapshot(ls_field_state_t *out) { ls_field_start(); s.sequence = (uint32_t)(esp_timer_get_time() / 100000); if (out) *out = s; }
bool ls_field_direct(bool on) { s.requested = s.direct = on; snprintf(s.status, sizeof(s.status), "%s", on ? "Direct control; mesh paused" : "Mesh control restored"); return true; }
bool ls_field_owned(void) { return s.direct; }
bool ls_field_configure(const ls_lora_cfg_t *cfg) { s.config = *cfg; return true; }
bool ls_field_mode(ls_lab_mode_t mode) { s.mode = mode; return true; }
void lssim_field_calibration(int step, int hold, bool failed)
{
    s.calibration_step = (uint8_t)step; s.calibration_hold = (uint8_t)hold;
    s.calibration_aligned = hold > 0; s.calibration_failed = failed;
    s.calibrating = step < 6 || failed;
    if (step == 6 && !failed) s.calibrated = s.calibration_saved = true;
}
bool ls_field_calibrate(int action)
{
    if (action == 0) {
        const char *fixture = getenv("LSSIM_CAL_STEP");
        int step=fixture?atoi(fixture):0;
        if(step<0 || step>6)step=0;
        lssim_field_calibration(step,fixture?11:0,false);
    } else if(action == 2) s.calibrating=false;
    return true;
}
bool ls_field_calibrating(void) { return s.calibrating; }
bool ls_field_clear_plot(void) { memset(s.bearing_count, 0, sizeof(s.bearing_count)); for (int i = 0; i < LS_FIELD_BINS; i++) s.trace[i] = s.spectrum[i] = -140; return true; }
bool ls_field_transmit(const char *text) { (void)text; s.tx++; return s.direct; }
bool ls_field_source(ls_field_source_t source) { s.sample.source = source; s.sample.radio_valid = source > LS_FIELD_NONE && source < LS_FIELD_CC1101; return true; }
const char *ls_field_source_name(ls_field_source_t source) { static const char *const names[] = {"NOTES ONLY","MESH","LORA LABS","RTL","CC1101","NRF24","NFC","WI-FI","BLUETOOTH"}; return source >= 0 && source < LS_FIELD_SOURCES ? names[source] : "UNKNOWN"; }
bool ls_field_record(bool on) { s.recording = on; return true; }
void ls_field_watch(bool on) { (void)on; }
bool ls_field_mark_lora(void) { return true; }
bool ls_field_mark_radio(const char *title,const char *text,ls_field_source_t source,uint32_t frequency,uint32_t count)
{ (void)source;(void)frequency;(void)count;return ls_field_note(0,title,text); }
bool ls_field_note(uint32_t id, const char *title, const char *text) { (void)id; snprintf(entry.title, sizeof(entry.title), "%s", title); snprintf(entry.text, sizeof(entry.text), "%s", text); return true; }
bool ls_field_entry(int index, ls_journal_entry_t *out) { if (index != 0) return false; *out = entry; return true; }
bool ls_field_provider(ls_field_source_t source, ls_field_provider_t provider) { (void)source; (void)provider; return true; }
