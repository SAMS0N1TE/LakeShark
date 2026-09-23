#include "ls_test.h"
#include "ls_field.h"
#include "ls_gps.h"
#include "core/ls_time.h"
#include "ls_mesh.h"
#include "esp_timer.h"
#include "radio/radio_health.h"
#include "ls_wireless.h"
#include "apps/fm/pocsag.h"
#include <sys/stat.h>
#include <stdlib.h>
#include <math.h>

static ls_lora_cfg_t radio;
static bool held, held_ready, scan, receiving, tx_done, fail_config, absent;
static unsigned configurations, sends, polls;
static ls_gps_state_t gps;
static ls_field_state_t state;
static bool imu_busy, keyboard_attached;
static bool scan_complete=true;static int scan_bins=64;
bool ls_keypad_present(void) { return keyboard_attached; }
static ls_imu_sample_t imu_data;
static uint32_t mesh_received=42;
bool ls_lora_present(void) { return !absent; }
/* ---- the FSK session and the paging decoder, faked like the radio ------ */
static bool fsk_on;
static ls_fsk_cfg_t fsk_cfg;
static unsigned fsk_begins, fsk_ends;
static uint8_t fsk_inbound[8];
static int fsk_inbound_len;
static bool fsk_refuse;
bool ls_lora_fsk_active(void) { return fsk_on; }
esp_err_t ls_lora_fsk_begin(const ls_fsk_cfg_t *cfg)
{
    if (fsk_refuse) return ESP_FAIL;
    fsk_cfg = *cfg; fsk_on = true; fsk_begins++; return ESP_OK;
}
esp_err_t ls_lora_fsk_end(void) { fsk_on = false; fsk_ends++; return ESP_OK; }
int ls_lora_fsk_poll(uint8_t *buf, size_t max, float *rssi)
{
    *rssi = -95;
    if (fsk_inbound_len <= 0 || !buf) return 0;
    int n = fsk_inbound_len < (int)max ? fsk_inbound_len : (int)max;
    memcpy(buf, fsk_inbound, n);
    fsk_inbound_len = 0;
    return n;
}

struct pocsag_ctx { int baud; };
static struct pocsag_ctx fake_pocsag;
static fm_state_t *fake_pocsag_state;
static unsigned pocsag_creates, pocsag_destroys, pocsag_batches;
static uint32_t pocsag_pages_seen;
pocsag_ctx_t *pocsag_create(fm_state_t *out, int baud)
{
    fake_pocsag_state = out; fake_pocsag.baud = baud; pocsag_creates++;
    return (pocsag_ctx_t *)&fake_pocsag;
}
void pocsag_destroy(pocsag_ctx_t *c) { (void)c; pocsag_destroys++; fake_pocsag_state = NULL; }
void pocsag_set_baud(pocsag_ctx_t *c, int baud) { (void)c; fake_pocsag.baud = baud; }
uint32_t pocsag_n_pages(const pocsag_ctx_t *c) { (void)c; return pocsag_pages_seen; }
/* Every batch decodes to one page, so the copy-out path is exercised. */
bool pocsag_process_batch(pocsag_ctx_t *c, const uint8_t *data, int len,
                          bool inverted, bool contiguous)
{
    (void)c; (void)data; (void)len; (void)inverted; (void)contiguous;
    pocsag_batches++;
    if (!fake_pocsag_state) return false;
    fm_page_t *p = &fake_pocsag_state->pages[fake_pocsag_state->page_head];
    p->address = 1234567; snprintf(p->text, sizeof(p->text), "CALL THE OFFICE");
    fake_pocsag_state->page_head =
        (fake_pocsag_state->page_head + 1) % FM_PAGE_LOG_MAX;
    if (fake_pocsag_state->page_count < FM_PAGE_LOG_MAX)
        fake_pocsag_state->page_count++;
    pocsag_pages_seen++;
    return true;
}
bool ls_lora_scanning(void) { return scan; }
bool ls_mesh_radio_hold(bool on) { held = on; return held_ready; }
bool ls_mesh_radio_held(void) { return held_ready; }
bool ls_lora_is_receiving(void) { return receiving; }
const ls_lora_cfg_t *ls_lora_cfg(void) { return &radio; }
void ls_lora_cfg_default(ls_lora_cfg_t *out) { *out = (ls_lora_cfg_t){.freq_hz=910525000,.sf=7,.bw_hz=125000,.cr=5,.preamble=16}; }
esp_err_t ls_lora_configure(const ls_lora_cfg_t *cfg) { configurations++; if (fail_config) return ESP_FAIL; radio = *cfg; receiving = false; return ESP_OK; }
esp_err_t ls_lora_receive(void) { receiving = true; return ESP_OK; }
esp_err_t ls_lora_scan_begin(uint32_t lo, uint32_t hi) { (void)lo; (void)hi; scan = true; return ESP_OK; }
esp_err_t ls_lora_scan_end(void) { scan = false; return ESP_OK; }
int ls_lora_scan_pass(float *dbm, int n, bool *done) { for (int i=0;i<n;i++) dbm[i] = -110; *done = scan_complete; return scan_bins; }
bool ls_lora_send_done(void) { return tx_done; }
esp_err_t ls_lora_send(const uint8_t *data, size_t n) { (void)data; (void)n; sends++; tx_done = false; return ESP_OK; }
uint32_t ls_lora_airtime_ms(int len) { return (uint32_t)len * 10; }
/* One packet, handed over once, so a case can say exactly what arrived. */
static uint8_t inbound[8];
static int inbound_len;
int ls_lora_poll(uint8_t *buf, size_t max, float *rssi, float *snr)
{
    polls++; *rssi=-90; *snr=7;
    if (inbound_len <= 0 || !buf) return 0;
    int n = inbound_len < (int)max ? inbound_len : (int)max;
    memcpy(buf, inbound, n);
    inbound_len = 0;
    return n;
}
esp_err_t ls_lora_rssi_inst(float *dbm) { *dbm = -92; return ESP_OK; }
void ls_gps_get(ls_gps_state_t *out) { *out = gps; }
bool ls_imu_read(ls_imu_sample_t *out) { if (imu_busy) return false; *out=imu_data; return true; }
float ls_imu_heading(void) { return 45; }
void ls_mesh_get_stats(ls_mesh_stats_t *out) { *out=(ls_mesh_stats_t){.running=true,.rx_packets=mesh_received,.last_rssi=-98}; }
void ls_mesh_get_radio(ls_mesh_radio_t *out) { *out=(ls_mesh_radio_t){.freq_hz=910525000}; }
bool ls_mesh_peer_at(int rank,ls_mesh_peer_t *out) { (void)rank;(void)out;return false; }
bool radio_health_get(const char *id, radio_health_snapshot_t *out) { (void)id; (void)out; return false; }
int perf_get_msgs_total(void) { return 0; }
ls_radio_err_t ls_radio_endpoint_get(const char *id, ls_radio_endpoint_info_t *out) { (void)id; (void)out; return LS_RADIO_ERR_UNAVAILABLE; }
void ls_wireless_observe(bool on) { (void)on; }
void ls_wireless_get(ls_wireless_snapshot_t *out) { memset(out, 0, sizeof(*out)); }

static void reset(void)
{
    ls_test_mkdir("field-test");
    remove("field-test/entries.bin"); remove("field-test/notes.md"); remove("field-test/samples.csv");
    remove("field-test/packets.csv"); inbound_len = 0;
    fsk_on=fsk_refuse=false; fsk_begins=fsk_ends=0; fsk_inbound_len=0;
    pocsag_creates=pocsag_destroys=pocsag_batches=0; pocsag_pages_seen=0;
    remove("field-test/compass-solo0.cal"); remove("field-test/compass-solo1.cal");
    remove("field-test/compass-kbd0.cal");remove("field-test/compass-kbd1.cal");
    held=scan=fail_config=absent=false; held_ready=receiving=tx_done=true; configurations=sends=polls=0;
    scan_complete=true;scan_bins=64;imu_busy = keyboard_attached = false;
    mesh_received=42;
    imu_data = (ls_imu_sample_t){.az=1,.mx=25,.my=25,.mag_valid=true};
    ls_lora_cfg_default(&radio);
    memset(&gps, 0, sizeof(gps));
    ls_shim_time_set(1000000);
    ls_field_test_reset("field-test"); ls_field_step();
}

LS_CASE(signal_detections_require_new_packets_and_have_a_fixed_capacity)
{
    reset();ls_field_snapshot(&state);LS_EQ_UINT(state.detection_count,0);
    for(int i=0;i<20;i++){ls_shim_time_advance(100000);ls_field_step();}
    ls_field_snapshot(&state);LS_EQ_UINT(state.detection_count,0);
    for(int i=0;i<12;i++){mesh_received++;ls_shim_time_advance(100000);ls_field_step();}
    ls_field_snapshot(&state);LS_EQ_UINT(state.detection_count,LS_FIELD_DETECTIONS);
    LS_CHECK(state.detections[0].mesh);LS_NEAR(state.detections[0].rssi,-98,.001);
    LS_EQ_UINT(state.detections[0].frequency,910525000);
    LS_CHECK(state.detections[0].observed_us>state.detections[1].observed_us);
    unsigned dots=0;for(int i=0;i<36;i++)dots+=state.bearing_count[i];LS_EQ_UINT(dots,12);
    mesh_received=0;ls_shim_time_advance(100000);ls_field_step();ls_field_snapshot(&state);
    LS_EQ_UINT(state.detection_count,LS_FIELD_DETECTIONS);
}

LS_CASE(direct_waits_for_mesh_and_restores_original_config)
{
    reset(); held_ready=false; ls_lora_cfg_t before=radio;
    LS_CHECK(ls_field_direct(true)); ls_field_step();
    LS_CHECK(held); LS_EQ_UINT(configurations,0);
    held_ready=true; ls_field_step(); ls_field_snapshot(&state); LS_CHECK(state.direct);
    ls_lora_cfg_t changed=radio; changed.sf=10; changed.freq_hz=915000000;
    LS_CHECK(ls_field_configure(&changed)); ls_field_step(); LS_EQ_UINT(radio.sf,10);
    ls_field_direct(false); ls_field_step();
    LS_CHECK(!held); LS_CHECK(receiving); LS_EQ_UINT(radio.freq_hz,before.freq_hz); LS_EQ_UINT(radio.sf,before.sf);
}
LS_CASE(saved_radio_bookmark_does_not_inherit_another_radios_signal)
{
    reset();
    LS_CHECK(ls_field_source(LS_FIELD_LORA));
    ls_field_step();
    LS_CHECK(ls_field_mark_radio("SubGHz pattern","Sensors at bookmark time",LS_FIELD_RTL,433920000,42));
    ls_field_step();
    ls_journal_entry_t entry;
    LS_CHECK(ls_field_entry(0,&entry));
    LS_EQ_INT(entry.sample.source,LS_FIELD_RTL);
    LS_EQ_UINT(entry.sample.frequency,433920000);LS_EQ_UINT(entry.sample.packets,42);
    LS_CHECK(entry.sample.radio_valid);LS_CHECK(!entry.sample.signal_valid);
    LS_CHECK(isnan(entry.sample.rssi));LS_CHECK(isnan(entry.sample.snr));
    LS_CHECK(entry.saved);
    LS_CHECK(!ls_field_mark_radio("Bad","Bad source",LS_FIELD_SOURCES,433920000,1));
    LS_CHECK(ls_field_note(entry.id,"Edited bookmark","Keep attachment"));ls_field_step();
    LS_CHECK(ls_field_entry(0,&entry));LS_EQ_UINT(entry.sample.frequency,433920000);
    ls_shim_time_advance(2000000);
    LS_CHECK(ls_field_mark_radio("Late bookmark","No fresh sensors",LS_FIELD_RTL,315000000,1));
    ls_field_step();LS_CHECK(ls_field_entry(0,&entry));
    LS_CHECK(!entry.sample.imu_valid);LS_CHECK(!entry.sample.gps_valid);
}
LS_CASE(brief_imu_contention_keeps_the_panel_stable_but_old_data_expires)
{
    reset();
    imu_busy = true;
    ls_shim_time_advance(200000); ls_field_step(); ls_field_snapshot(&state);
    LS_CHECK(state.sample.imu_valid); LS_NEAR(state.sample.imu.mx, 25, .001);
    ls_shim_time_advance(200000); ls_field_step(); ls_field_snapshot(&state);
    LS_CHECK(!state.sample.imu_valid);
    imu_busy = false;
    ls_shim_time_advance(200000); ls_field_step(); ls_field_snapshot(&state);
    LS_CHECK(state.sample.imu_valid);
}

static void calibrate(float offset)
{
    LS_CHECK(ls_field_calibrate(0)); ls_field_step();
    const float poses[6][3]={{0,0,1},{0,1,0},{1,0,0},{0,-1,0},{-1,0,0},{0,0,-1}};
    for (int i = 0; i < 120; i++) {
        float x=poses[i/20][0], y=poses[i/20][1], z=poses[i/20][2];
        imu_data = (ls_imu_sample_t){.ax=x,.ay=y,.az=z,.mx=offset+45*x,
            .my=-80+45*y,.mz=100+45*z,.mag_valid=true};
        ls_shim_time_advance(100000); ls_field_step();
    }
    ls_shim_time_advance(100000); ls_field_step();
    ls_field_snapshot(&state);
    LS_CHECK(state.calibrated); LS_CHECK(state.calibration_saved); LS_CHECK(!state.calibrating);
    LS_EQ_UINT(state.calibration_step,6);
}

LS_CASE(compass_calibration_survives_restart_and_an_interrupted_new_save)
{
    reset(); calibrate(60); calibrate(70);
    FILE *f = fopen("field-test/compass-solo0.cal", "wb"); LS_CHECK(f != NULL);
    if (f) { fputs("interrupted", f); fclose(f); }
    ls_field_test_reset("field-test");
    imu_data = (ls_imu_sample_t){.az=1,.mx=85,.my=-80,.mz=100,.mag_valid=true};
    ls_shim_time_advance(100000); ls_field_step(); ls_field_snapshot(&state);
    LS_CHECK(state.calibrated); LS_CHECK(state.calibration_saved);
    /* Restored offset is 60: positive corrected mx points south. */
    LS_NEAR(state.sample.heading, 180, .01);
}

LS_CASE(incomplete_calibration_keeps_previous_values_and_does_not_hold_mesh)
{
    reset(); calibrate(60);
    LS_CHECK(ls_field_calibrate(0)); ls_field_step();
    LS_CHECK(ls_field_calibrate(1)); ls_field_step(); ls_field_snapshot(&state);
    LS_CHECK(state.calibrating); LS_CHECK(state.calibrated); LS_CHECK(!held);
    LS_CHECK(ls_field_calibrate(2)); ls_field_step(); ls_field_snapshot(&state);
    LS_CHECK(!state.calibrating); LS_CHECK(state.calibrated);
}

LS_CASE(guide_restarts_cleanly_and_rejects_a_bad_fit_without_overwriting)
{
    reset(); calibrate(60);
    ls_field_calibrate(0);ls_field_step();
    const float poses[6][3]={{0,0,1},{0,1,0},{1,0,0},{0,-1,0},{-1,0,0},{0,0,-1}};
    for(int i=0;i<120;i++) {
        imu_data=(ls_imu_sample_t){.ax=poses[i/20][0],.ay=poses[i/20][1],.az=poses[i/20][2],
            .mx=80,.my=-80,.mz=100,.mag_valid=true};
        ls_shim_time_advance(100000);ls_field_step();
    }
    ls_field_snapshot(&state);
    LS_CHECK(state.calibration_failed);LS_CHECK(state.calibrated);LS_CHECK(state.calibration_saved);
    LS_CHECK(ls_field_calibrating());LS_CHECK(!held);
    ls_field_calibrate(0);ls_field_step();ls_field_snapshot(&state);
    LS_EQ_UINT(state.calibration_step,0);LS_EQ_UINT(state.calibration_hold,0);LS_CHECK(!state.calibration_failed);
    ls_field_calibrate(2);ls_field_step();LS_CHECK(!ls_field_calibrating());
    ls_field_test_reset("field-test");
    imu_data=(ls_imu_sample_t){.az=1,.mx=85,.my=-80,.mz=100,.mag_valid=true};
    ls_shim_time_advance(100000);ls_field_step();ls_field_snapshot(&state);
    LS_NEAR(state.sample.heading,180,.01);
}
LS_CASE(exiting_waits_for_transmit_before_restoring)
{
    reset(); ls_field_direct(true); ls_field_step();
    LS_CHECK(ls_field_transmit("TEST")); ls_field_step(); LS_EQ_UINT(sends,1);
    ls_field_direct(false); ls_field_step(); LS_CHECK(held);
    tx_done=true; ls_field_step(); LS_CHECK(!held); LS_CHECK(receiving);
}
LS_CASE(failed_restore_keeps_mesh_held_until_retry_succeeds)
{
    reset(); ls_field_direct(true); ls_field_step(); fail_config=true;
    ls_field_direct(false); ls_field_step(); LS_CHECK(held); LS_CHECK(ls_field_owned());
    fail_config=false; ls_field_step(); LS_CHECK(!held);
}
LS_CASE(a_full_note_queue_cannot_block_radio_release)
{
    reset(); ls_field_direct(true); ls_field_step();
    for(int i=0;i<4;i++) LS_CHECK(ls_field_note(0,"queued","note"));
    LS_CHECK(!ls_field_note(0,"overflow","note"));
    LS_CHECK(ls_field_direct(false)); ls_field_step(); LS_CHECK(!held);
}
LS_CASE(notes_survive_restart_and_edits_keep_original_attachment)
{
    reset(); gps=(ls_gps_state_t){.fix=true,.alive=true,.lat_deg=42.36,.lon_deg=-71.06,.last_sentence_us=1000000};
    ls_shim_time_advance(200000); ls_field_step();
    LS_CHECK(ls_field_note(0,"Trail","First observation")); ls_field_step();
    ls_journal_entry_t e; LS_CHECK(ls_field_entry(0,&e)); LS_CHECK(e.saved); LS_NEAR(e.sample.lat,42.36,.00001);
    uint32_t id=e.id;
    ls_field_test_reset("field-test"); ls_field_step(); LS_CHECK(ls_field_entry(0,&e)); LS_EQ_STR(e.text,"First observation");
    gps.lat_deg=41; ls_shim_time_advance(200000); ls_field_step();
    LS_CHECK(ls_field_note(id,"Trail","Added detail")); ls_field_step(); LS_CHECK(ls_field_entry(0,&e));
    LS_EQ_STR(e.text,"Added detail"); LS_NEAR(e.sample.lat,42.36,.00001);
}
LS_CASE(stale_gps_and_unimplemented_radios_do_not_fabricate_data)
{
    reset(); gps=(ls_gps_state_t){.fix=true,.alive=true,.lat_deg=42,.last_sentence_us=1};
    ls_field_source(LS_FIELD_CC1101); ls_shim_time_advance(4000000); ls_field_step(); ls_field_snapshot(&state);
    LS_CHECK(!state.sample.gps_valid); LS_CHECK(!state.sample.radio_valid); LS_CHECK(state.sample.imu_valid);
}
LS_CASE(missing_card_keeps_notes_in_ram_and_stops_recording)
{
    reset(); ls_field_test_reset("nonexistent-parent/journal"); ls_field_step();
    LS_CHECK(ls_field_note(0,"Unsaved","Keep this in RAM")); ls_field_step();
    ls_journal_entry_t e; LS_CHECK(ls_field_entry(0,&e)); LS_CHECK(!e.saved);
    ls_field_record(true); ls_field_step(); ls_field_snapshot(&state); LS_CHECK(!state.recording);
}
LS_CASE(invalid_controls_never_reach_hardware)
{
    reset(); ls_lora_cfg_t bad=radio; bad.sf=0; LS_CHECK(!ls_field_configure(&bad));
    bad=radio; bad.freq_hz=0; LS_CHECK(!ls_field_configure(&bad));
    LS_CHECK(!ls_field_mode((ls_lab_mode_t)99));
    LS_CHECK(ls_field_transmit("TEST")); ls_field_step(); LS_EQ_UINT(sends,0);
}

LS_CASE(a_partial_sd_record_does_not_hide_later_notes)
{
    reset(); ls_field_note(0,"Before interruption","Retained"); ls_field_step();
    FILE *f=fopen("field-test/entries.bin","ab"); LS_CHECK(f != NULL);
    if (f) { fwrite("partial",7,1,f); fclose(f); }
    ls_field_test_reset("field-test"); ls_field_step();
    ls_field_note(0,"After interruption","Also retained"); ls_field_step();
    ls_field_test_reset("field-test"); ls_field_step();
    ls_journal_entry_t e; LS_CHECK(ls_field_entry(0,&e)); LS_EQ_STR(e.title,"After interruption");
    LS_CHECK(ls_field_entry(1,&e)); LS_EQ_STR(e.title,"Before interruption");
}
LS_CASE(a_stuck_transmitter_has_a_bounded_release)
{
    reset(); ls_field_direct(true); ls_field_step(); ls_field_transmit("TEST"); ls_field_step();
    ls_field_direct(false); ls_field_step(); LS_CHECK(held);
    ls_shim_time_advance(4000000); ls_field_step(); LS_CHECK(!held);
}
LS_CASE(ram_capacity_preserves_unsaved_notes)
{
    reset(); ls_field_test_reset("nonexistent-parent/journal");
    for (int i=0;i<LS_JOURNAL_CAP;i++) { LS_CHECK(ls_field_note(0,"Unsaved","Retain")); ls_field_step(); }
    LS_CHECK(!ls_field_note(0,"Overflow","Must be refused"));
    ls_field_snapshot(&state); LS_EQ_INT(state.journal_count, LS_JOURNAL_CAP);
}

LS_CASE(packet_polling_continues_between_graph_samples)
{
    reset(); ls_field_direct(true); ls_field_step();
    unsigned before = polls;
    ls_field_snapshot(&state); uint32_t sequence = state.sequence;
    for (int i=0;i<3;i++) { ls_shim_time_advance(25000); ls_field_step(); }
    LS_EQ_UINT(polls, before + 3);
    ls_field_snapshot(&state); LS_EQ_UINT(state.sequence, sequence);
}
LS_CASE(a_late_existing_archive_is_never_overwritten)
{
    reset(); ls_field_note(0,"Existing archive","Preserve"); ls_field_step();
    remove("field-test/inserted.bin");
    LS_EQ_INT(rename("field-test/entries.bin", "field-test/inserted.bin"), 0);
    ls_field_test_reset("field-test");
    LS_EQ_INT(rename("field-test/inserted.bin", "field-test/entries.bin"), 0);
    ls_field_note(0,"New RAM note","Retain separately"); ls_field_step();
    ls_journal_entry_t e; LS_CHECK(ls_field_entry(0,&e)); LS_CHECK(!e.saved);
    LS_CHECK(!ls_field_note(9999,"Bad edit","No such entry"));
    ls_field_test_reset("field-test");
    LS_CHECK(ls_field_entry(0,&e)); LS_EQ_STR(e.title,"Existing archive");
    LS_CHECK(!ls_field_entry(1,&e));
}
LS_CASE(unknown_archive_versions_are_not_truncated)
{
    reset();
    FILE *f=fopen("field-test/entries.bin","wb"); LS_CHECK(f != NULL);
    if(f) { fwrite("UNKNOWN JOURNAL FORMAT",21,1,f); fclose(f); }
    ls_field_test_reset("field-test"); ls_field_note(0,"New note","RAM"); ls_field_step();
    ls_journal_entry_t e; LS_CHECK(ls_field_entry(0,&e)); LS_CHECK(!e.saved);
    struct stat st; LS_EQ_INT(stat("field-test/entries.bin",&st),0); LS_EQ_INT(st.st_size,21);
}

LS_CASE(recording_source_stays_fixed_until_stop)
{
    reset();
    LS_CHECK(ls_field_source(LS_FIELD_HACKRF));
    LS_CHECK(ls_field_record(true));
    LS_CHECK(ls_field_recording());
    LS_CHECK(!ls_field_source(LS_FIELD_WIFI));
    LS_CHECK(ls_field_source(LS_FIELD_HACKRF));
    LS_CHECK(ls_field_record(false));
    LS_CHECK(!ls_field_recording());
    LS_CHECK(ls_field_source(LS_FIELD_WIFI));
}

LS_CASE(compass_selects_saved_attachment_profiles_and_cancels_mixed_setup_fit)
{
    reset();calibrate(60);
    keyboard_attached=true;ls_shim_time_advance(100000);ls_field_step();ls_field_snapshot(&state);
    LS_CHECK(state.calibration_keyboard);LS_CHECK(!state.calibrated);
    calibrate(120);
    imu_data=(ls_imu_sample_t){.az=1,.mx=85,.my=-80,.mz=100,.mag_valid=true};
    ls_shim_time_advance(100000);ls_field_step();ls_field_snapshot(&state);
    LS_NEAR(state.sample.heading,0,.01);
    keyboard_attached=false;ls_shim_time_advance(100000);ls_field_step();ls_field_snapshot(&state);
    LS_CHECK(!state.calibration_keyboard);LS_CHECK(state.calibrated);LS_NEAR(state.sample.heading,180,.01);
    keyboard_attached=true;ls_field_test_reset("field-test");
    ls_shim_time_advance(100000);ls_field_step();ls_field_snapshot(&state);
    LS_CHECK(state.calibration_keyboard);LS_CHECK(state.calibration_saved);LS_NEAR(state.sample.heading,0,.01);
    ls_field_calibrate(0);ls_field_step();LS_CHECK(ls_field_calibrating());
    keyboard_attached=false;ls_shim_time_advance(100000);ls_field_step();ls_field_snapshot(&state);
    LS_CHECK(!state.calibrating);LS_CHECK(state.calibrated);LS_NEAR(state.sample.heading,180,.01);
}

LS_CASE(spectrum_publishes_only_complete_sweeps_and_exposes_failures)
{
    reset();ls_field_mode(LS_LAB_SPECTRUM);ls_field_direct(true);
    scan_complete=false;ls_shim_time_advance(100000);ls_field_step();ls_field_snapshot(&state);
    LS_CHECK(state.direct);LS_EQ_UINT(state.spectrum_sweeps,0);LS_CHECK(!state.spectrum_us);
    scan_complete=true;ls_shim_time_advance(100000);ls_field_step();ls_field_snapshot(&state);
    LS_EQ_UINT(state.spectrum_sweeps,1);LS_CHECK(state.spectrum_us>0);
    scan_bins=32;ls_shim_time_advance(100000);ls_field_step();ls_field_snapshot(&state);
    LS_EQ_UINT(state.spectrum_errors,1);LS_CHECK(!state.spectrum_us);
    ls_field_direct(false);ls_field_step();LS_CHECK(!held);
}

LS_CASE(csv_health_counts_saved_rows_and_stops_after_failed_write)
{
    reset();ls_field_record(true);ls_shim_time_advance(1100000);ls_field_step();ls_field_snapshot(&state);
    LS_CHECK(state.record_rows>0);LS_EQ_UINT(state.record_errors,0);LS_CHECK(state.record_saved_us>0);
    LS_CHECK(!strcmp(state.record_saved_utc,state.sample.utc));
    LS_EQ_INT(state.record_saved_us,state.sample.time_us);
    ls_field_test_reset("field-test/missing/deeper");ls_field_record(true);ls_shim_time_advance(1100000);ls_field_step();ls_field_snapshot(&state);
    LS_EQ_UINT(state.record_rows,0);LS_EQ_UINT(state.record_errors,1);LS_CHECK(!ls_field_recording());
}

LS_CASE(recorded_timestamp_survives_live_updates_and_rejects_impossible_gps_date)
{
    reset();ls_time_test_set_synced(false);
    gps=(ls_gps_state_t){.alive=true,.fix=true,.lat_deg=43,.lon_deg=-71,.last_sentence_us=1000000,
        .year=2026,.month=9,.day=15,.hour=12,.minute=34,.second=56};
    ls_field_record(true);ls_shim_time_advance(1100000);ls_field_step();ls_field_snapshot(&state);
    LS_CHECK(!strcmp(state.record_saved_utc,"2026-09-15T12:34:56Z"));
    ls_field_record(false);gps.second=58;ls_shim_time_advance(100000);ls_field_step();ls_field_snapshot(&state);
    LS_CHECK(!strcmp(state.record_saved_utc,"2026-09-15T12:34:56Z"));
    gps.month=2;gps.day=30;ls_shim_time_advance(100000);ls_field_step();ls_field_snapshot(&state);
    LS_CHECK(!state.sample.utc[0]);
}

LS_CASE(a_received_payload_is_written_to_packets_csv)
{
    /* samples.csv is a one-second heartbeat of where you were; packets.csv is
       what actually arrived. This is the second one. */
    reset();
    LS_CHECK(ls_field_direct(true)); ls_field_step();
    ls_field_snapshot(&state); LS_CHECK(state.direct);
    LS_CHECK(ls_field_record(true));

    static const uint8_t payload[] = {0xDE, 0xAD, 0xBE, 0xEF};
    memcpy(inbound, payload, sizeof(payload));
    inbound_len = (int)sizeof(payload);
    ls_shim_time_advance(100000); ls_field_step();

    ls_field_snapshot(&state);
    LS_EQ_UINT(state.record_packets, 1);
    LS_EQ_UINT(state.record_errors, 0);

    char text[512] = {0};
    FILE *f = fopen("field-test/packets.csv", "rb");
    LS_CHECK(f != NULL);
    size_t got = fread(text, 1, sizeof(text) - 1, f);
    fclose(f);
    LS_CHECK(got > 0);
    LS_CHECK(strstr(text, "uptime_us,utc,source,hz,rssi,snr,gps_valid,lat,lon,len,hex") != NULL);
    /* The bytes, as hex, and the length that says how many to read. */
    LS_CHECK(strstr(text, "DEADBEEF") != NULL);
    LS_CHECK(strstr(text, ",4,DEADBEEF") != NULL);

    /* A packet arriving with recording off leaves the file alone. */
    LS_CHECK(ls_field_record(false));
    memcpy(inbound, payload, sizeof(payload));
    inbound_len = (int)sizeof(payload);
    ls_shim_time_advance(100000); ls_field_step();
    ls_field_snapshot(&state);
    LS_EQ_UINT(state.record_packets, 1);
}

LS_CASE(gfsk_listens_with_the_fsk_demodulator_and_cannot_transmit)
{
    reset();
    LS_CHECK(ls_field_direct(true)); ls_field_step();
    ls_field_snapshot(&state); LS_CHECK(state.direct);
    LS_EQ_UINT(fsk_begins, 0);

    LS_CHECK(ls_field_mode(LS_LAB_GFSK)); ls_field_step();
    ls_field_snapshot(&state);
    LS_EQ_INT(state.mode, LS_LAB_GFSK);
    LS_EQ_UINT(fsk_begins, 1);
    LS_CHECK(fsk_on);
    /* One BAND choice moves every mode, so the session takes its frequency
       from the LoRa config rather than carrying a second one. */
    LS_EQ_UINT(fsk_cfg.freq_hz, state.config.freq_hz);

    const unsigned before_polls = polls;
    static const uint8_t bytes[] = {0x01, 0x02, 0x03};
    memcpy(fsk_inbound, bytes, sizeof(bytes));
    fsk_inbound_len = (int)sizeof(bytes);
    ls_shim_time_advance(100000); ls_field_step();
    ls_field_snapshot(&state);
    LS_EQ_UINT(state.rx, 1);
    LS_EQ_INT(state.packet_len, 3);
    /* Through the FSK poll, not the LoRa one. */
    LS_EQ_UINT(polls, before_polls);
    /* The FSK demodulator has no reference to measure SNR against. */
    LS_CHECK(isnan(state.detections[0].snr));

    /* Receive only: there is no FSK transmit call to make. */
    const unsigned before_sends = sends;
    ls_field_transmit("nope"); ls_field_step();
    LS_EQ_UINT(sends, before_sends);

    /* Leaving DIRECT ends the session and gives the mesh its radio back. */
    ls_field_direct(false); ls_field_step();
    LS_CHECK(!fsk_on);
    LS_CHECK(fsk_ends >= 1);
    LS_CHECK(!held);
}

LS_CASE(pocsag_pins_the_paging_parameters_and_surfaces_a_page)
{
    reset();
    LS_CHECK(ls_field_direct(true)); ls_field_step();
    LS_CHECK(ls_field_mode(LS_LAB_POCSAG)); ls_field_step();
    LS_EQ_UINT(pocsag_creates, 1);
    /* Not offered to the user: wrong values here look like a quiet channel.
       These are the set the console receiver already works with. */
    LS_EQ_UINT(fsk_cfg.deviation_hz, 4500);
    /* Carson for 1200 baud at +-4.5 kHz is 11.4 kHz, so the narrowest the
       part offers above it. Anything wider is noise the demodulator does not
       need, which costs sensitivity at VHF on a 915 MHz front end. */
    LS_EQ_UINT(fsk_cfg.bandwidth_hz, 11700);
    LS_CHECK(fsk_cfg.bitrate + 2 * fsk_cfg.deviation_hz <= fsk_cfg.bandwidth_hz);
    LS_EQ_UINT(fsk_cfg.sync_word, 0x7cd215d8u);
    LS_EQ_UINT(fsk_cfg.payload_bytes, 64);

    static const uint8_t batch[] = {0xAA, 0xBB};
    memcpy(fsk_inbound, batch, sizeof(batch));
    fsk_inbound_len = (int)sizeof(batch);
    ls_shim_time_advance(100000); ls_field_step();
    ls_field_snapshot(&state);
    LS_EQ_UINT(pocsag_batches, 1);
    LS_EQ_UINT(state.pages, 1);
    /* Newest first, so a screen can list them the way FM does. */
    LS_EQ_UINT(state.page_log_count, 1);
    LS_EQ_UINT(state.page_log[0].address, 1234567);
    LS_CHECK(!strcmp(state.page_log[0].text, "CALL THE OFFICE"));

    /* Back to a LoRa mode and the decoder is handed back too - it is a
       sixteen-entry page ring in PSRAM, not something to leave allocated. */
    LS_CHECK(ls_field_mode(LS_LAB_PACKETS)); ls_field_step();
    LS_EQ_UINT(pocsag_destroys, 1);
    LS_CHECK(!fsk_on);
}
