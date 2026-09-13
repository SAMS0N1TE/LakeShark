#include "ls_test.h"
#include "ls_field.h"
#include "ls_gps.h"
#include "ls_mesh.h"
#include "esp_timer.h"
#include "radio/radio_health.h"
#include "ls_wireless.h"
#include <sys/stat.h>
#include <stdlib.h>
#include <math.h>

static ls_lora_cfg_t radio;
static bool held, held_ready, scan, receiving, tx_done, fail_config, absent;
static unsigned configurations, sends, polls;
static ls_gps_state_t gps;
static ls_field_state_t state;
static bool imu_busy;
static ls_imu_sample_t imu_data;
static uint32_t mesh_received=42;
bool ls_lora_present(void) { return !absent; }
bool ls_lora_fsk_active(void) { return false; }
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
int ls_lora_scan_pass(float *dbm, int n, bool *done) { for (int i=0;i<n;i++) dbm[i] = -110; *done = true; return n; }
bool ls_lora_send_done(void) { return tx_done; }
esp_err_t ls_lora_send(const uint8_t *data, size_t n) { (void)data; (void)n; sends++; tx_done = false; return ESP_OK; }
uint32_t ls_lora_airtime_ms(int len) { return (uint32_t)len * 10; }
int ls_lora_poll(uint8_t *buf, size_t max, float *rssi, float *snr) { (void)buf; (void)max; polls++; *rssi=-90; *snr=7; return 0; }
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
#ifdef _WIN32
    mkdir("field-test");
#else
    mkdir("field-test", 0775);
#endif
    remove("field-test/entries.bin"); remove("field-test/notes.md"); remove("field-test/samples.csv");
    remove("field-test/compass0.cal"); remove("field-test/compass1.cal");
    held=scan=fail_config=absent=false; held_ready=receiving=tx_done=true; configurations=sends=polls=0;
    imu_busy = false;
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
    FILE *f = fopen("field-test/compass0.cal", "wb"); LS_CHECK(f != NULL);
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
