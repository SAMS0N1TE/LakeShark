#ifndef LS_FIELD_H
#define LS_FIELD_H

#include <stdbool.h>
#include <stdint.h>
#include "ls_lora.h"
#include "ls_imu.h"

#ifdef __cplusplus
extern "C" {
#endif

#define LS_FIELD_BINS 64
#define LS_JOURNAL_CAP 48
#define LS_JOURNAL_TEXT 768

typedef enum { LS_FIELD_NONE, LS_FIELD_MESH, LS_FIELD_LORA, LS_FIELD_RTL,
               LS_FIELD_CC1101, LS_FIELD_NRF24, LS_FIELD_NFC, LS_FIELD_WIFI, LS_FIELD_BLE, LS_FIELD_SOURCES } ls_field_source_t;
typedef enum { LS_LAB_PACKETS, LS_LAB_SPECTRUM, LS_LAB_BEARING } ls_lab_mode_t;

typedef struct {
    int64_t time_us;
    char utc[24];
    bool gps_valid, imu_valid, radio_valid, signal_valid;
    double lat, lon;
    float alt_m, heading, rssi, snr;
    ls_imu_sample_t imu;
    uint32_t frequency, packets;
    ls_field_source_t source;
} ls_field_sample_t;

typedef struct {
    uint32_t id;
    char title[48];
    char text[LS_JOURNAL_TEXT];
    ls_field_sample_t sample;
    bool saved;
} ls_journal_entry_t;

#define LS_FIELD_DETECTIONS 8
typedef struct {
    int64_t observed_us;
    uint32_t frequency;
    float rssi, snr, heading;
    bool mesh;
} ls_field_detection_t;
typedef struct { char name[20], id[17]; float rssi; } ls_field_peer_t;

typedef struct {
    bool ready, requested, direct, busy, transmitting, recording;
    ls_lab_mode_t mode;
    ls_lora_cfg_t config;
    float trace[LS_FIELD_BINS], spectrum[LS_FIELD_BINS], bearing[36];
    uint16_t bearing_count[36];
    uint32_t rx, bad, tx, sequence, drops;
    uint8_t packet[64];
    int packet_len, journal_count;
    ls_field_sample_t sample;
    ls_field_detection_t detections[LS_FIELD_DETECTIONS];
    uint8_t detection_count, peer_count;
    ls_field_peer_t peers[3];
    char status[80], storage[80];
    bool calibrating, calibrated, calibration_saved;
    uint8_t calibration_faces;
    uint16_t calibration_samples;
    uint8_t calibration_step, calibration_hold;
    bool calibration_aligned, calibration_failed;
    char compass_status[80];
} ls_field_state_t;

/* UI calls only enqueue work or copy bounded snapshots. */
bool ls_field_start(void);
void ls_field_snapshot(ls_field_state_t *out);
bool ls_field_direct(bool enabled);
bool ls_field_owned(void);
bool ls_field_configure(const ls_lora_cfg_t *cfg);
bool ls_field_mode(ls_lab_mode_t mode);
/* 0 starts the guide, 1 retries the completed fit, 2 cancels. */
bool ls_field_calibrate(int action);
bool ls_field_calibrating(void);
bool ls_field_clear_plot(void);
bool ls_field_transmit(const char *text);
bool ls_field_source(ls_field_source_t source);
const char *ls_field_source_name(ls_field_source_t source);
bool ls_field_record(bool enabled);
void ls_field_watch(bool enabled);
bool ls_field_note(uint32_t id, const char *title, const char *text);
bool ls_field_mark_lora(void);
/* Saved radio metadata with sensors at mark time, not at capture time. */
bool ls_field_mark_radio(const char *title, const char *text,
                         ls_field_source_t source, uint32_t frequency, uint32_t count);
bool ls_field_entry(int newest_index, ls_journal_entry_t *out);

/* A driver registers a snapshot provider after its hardware probe succeeds.
   Providers run on the field worker; they must not retune or consume packets. */
typedef bool (*ls_field_provider_t)(ls_field_sample_t *out);
bool ls_field_provider(ls_field_source_t source, ls_field_provider_t provider);

#ifdef LS_FIELD_TEST
void ls_field_step(void);
void ls_field_test_reset(const char *directory);
#endif

#ifdef __cplusplus
}
#endif
#endif
