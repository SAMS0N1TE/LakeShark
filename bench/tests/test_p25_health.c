/* LS_TEST_SOURCES: ${APP}/p25/p25_health.c */
/* no hardware proxy is used here. These cases pin the bounded
 * publication, explicit unknown/stale/invalid states and RESET epoch that the
 * LCD consumes; on-air identity and appearance still require later hardware. */

#include "ls_test.h"
#include "p25_health.h"

#include <string.h>

static p25_health_raw_t raw_identity(void)
{
    p25_health_raw_t raw = {0};
    raw.net_valid = true;
    raw.rfss_valid = true;
    raw.net_generation = 1;
    raw.rfss_generation = 1;
    raw.wacn = 0xbee00;
    raw.sysid = 0x123;
    raw.rfss_sysid = 0x123;
    raw.rfss = 4;
    raw.site = 7;
    raw.control_channel = 0x1020;
    raw.control_hz = 851012500ULL;
    raw.neighbor_count = 1;
    raw.neighbor_generation = 1;
    raw.neighbors[0].rfss = 4;
    raw.neighbors[0].site = 8;
    raw.neighbors[0].frequency_hz = 852262500ULL;
    memcpy(raw.effective_demod, "CQPSK", 6);
    raw.acquisition = P25_HEALTH_ACQ_LOCKED;
    raw.buffers_ok = true;
    return raw;
}

LS_CASE(identity_distinguishes_unknown_current_stale_and_invalid)
{
    p25_health_snapshot_t snapshot = {0};
    char text[320];
    p25_health_format_identity(&snapshot, 1, text, sizeof(text));
    LS_CHECK(strstr(text, "SYSTEM unknown") != NULL);
    LS_EQ_INT(p25_health_identity_state(&snapshot, 1),
              P25_HEALTH_ID_UNKNOWN);

    p25_health_init();
    p25_health_raw_t raw = raw_identity();
    p25_health_publish(&raw, 100);
    LS_CHECK(p25_health_read(&snapshot));
    LS_EQ_INT(p25_health_identity_state(&snapshot, 100),
              P25_HEALTH_ID_CURRENT);
    p25_health_format_identity(&snapshot, 100, text, sizeof(text));
    LS_CHECK(strstr(text, "SYSTEM VALID WACN $BEE00 SYS $123") != NULL);
    LS_CHECK(strstr(text, "RFSS 4 SITE 7") != NULL);
    LS_CHECK(strstr(text, "CONTROL 851.0125 MHz") != NULL);
    LS_CHECK(strstr(text, "NEIGHBOR 4/8 852.2625 MHz") != NULL);

    LS_EQ_INT(p25_health_identity_state(
                  &snapshot, 101 + P25_HEALTH_IDENTITY_STALE_MS),
              P25_HEALTH_ID_STALE);
    p25_health_format_identity(
        &snapshot, 101 + P25_HEALTH_IDENTITY_STALE_MS, text, sizeof(text));
    LS_CHECK(strstr(text, "SYSTEM STALE") != NULL);

    raw.rfss_sysid = 0x124;
    raw.rfss_generation++;
    p25_health_publish(&raw, 20000);
    LS_CHECK(p25_health_read(&snapshot));
    LS_EQ_INT(p25_health_identity_state(&snapshot, 20000),
              P25_HEALTH_ID_INVALID);
    p25_health_format_identity(&snapshot, 20000, text, sizeof(text));
    LS_CHECK(strstr(text, "SYSTEM INVALID") != NULL);
}

LS_CASE(unchanged_generation_does_not_refresh_retained_identity)
{
    p25_health_init();
    p25_health_raw_t raw = raw_identity();
    p25_health_snapshot_t snapshot;
    p25_health_publish(&raw, 10);
    p25_health_publish(&raw, 20 + P25_HEALTH_IDENTITY_STALE_MS);
    LS_CHECK(p25_health_read(&snapshot));
    LS_EQ_INT(p25_health_identity_state(
                  &snapshot, 20 + P25_HEALTH_IDENTITY_STALE_MS),
              P25_HEALTH_ID_STALE);

    raw.net_generation++;
    raw.rfss_generation++;
    p25_health_publish(&raw, 30 + P25_HEALTH_IDENTITY_STALE_MS);
    LS_CHECK(p25_health_read(&snapshot));
    LS_EQ_INT(p25_health_identity_state(
                  &snapshot, 30 + P25_HEALTH_IDENTITY_STALE_MS),
              P25_HEALTH_ID_CURRENT);
}

LS_CASE(zero_wire_identity_is_valid_when_decode_says_it_is)
{
    p25_health_init();
    p25_health_raw_t raw = raw_identity();
    raw.wacn = 0;
    raw.sysid = 0;
    raw.rfss_sysid = 0;
    p25_health_publish(&raw, 0);
    p25_health_snapshot_t snapshot;
    char text[128];
    LS_CHECK(p25_health_read(&snapshot));
    LS_EQ_INT(p25_health_identity_state(&snapshot, 0),
              P25_HEALTH_ID_CURRENT);
    p25_health_format_identity(&snapshot, 0, text, sizeof(text));
    LS_CHECK(strstr(text, "WACN $00000 SYS $000") != NULL);
}

LS_CASE(signal_format_is_honest_and_bounded)
{
    p25_health_snapshot_t s = {0};
    memcpy(s.effective_demod, "C4FM", 5);
    s.acquisition = P25_HEALTH_ACQ_HUNT;
    s.nid_valid = 12;
    s.nid_invalid = 3;
    s.tsbk_valid = 9;
    s.tsbk_invalid = 2;
    s.rf_level_permille = 375;
    s.usb_read_errors = 4;
    s.usb_dropped_bytes = 8192;
    s.ring_fill = 1024;
    s.ring_size = 8192;
    s.audio_drops = 5;
    s.audio_underruns = 6;
    s.buffers_ok = true;
    s.unhandled[0x12] = 2;
    s.unhandled_total = 2;
    s.unhandled_distinct = 1;
    s.phase2_grants = 1;
    s.phase2_talkgroup = 42;
    s.phase2_slot = 1;
    s.phase2_slots_per_carrier = 2;

    char text[768];
    p25_health_format_signal(&s, text, sizeof(text));
    LS_CHECK(strstr(text, "NID valid 12 invalid 3") != NULL);
    LS_CHECK(strstr(text, "TSBK valid 9 invalid 2") != NULL);
    LS_CHECK(strstr(text, "DEMOD C4FM HUNT") != NULL);
    LS_CHECK(strstr(text, "timing n/a") != NULL);
    LS_CHECK(strstr(text, "carrier n/a") != NULL);
    LS_CHECK(strstr(text, "RF normalized 37.5%") != NULL);
    LS_CHECK(strstr(text, "USB errors 4 dropped 8192 B") != NULL);
    LS_CHECK(strstr(text, "AUDIO drop 5 underrun 6") != NULL);
    LS_CHECK(strstr(text, "CPU busy unavailable") != NULL);
    LS_CHECK(strstr(text, "OP UNKNOWN 2 in 1: $12=2") != NULL);
    LS_CHECK(strstr(text, "PHASE 2 voice UNSUPPORTED") != NULL);
    LS_CHECK(strstr(text, "BER") == NULL);

    char tiny[17];
    memset(tiny, 'X', sizeof(tiny));
    p25_health_format_signal(&s, tiny, sizeof(tiny));
    LS_EQ_INT(tiny[sizeof(tiny) - 1], 0);
}

LS_CASE(reset_starts_counter_epoch_but_retains_live_state)
{
    p25_health_init();
    p25_health_raw_t raw = raw_identity();
    raw.nid_valid = 100;
    raw.nid_invalid = 7;
    raw.tsbk_valid = 50;
    raw.tsbk_invalid = 4;
    raw.unhandled[0x2a] = 3;
    raw.reacquires = 2;
    raw.usb_read_errors = 8;
    raw.usb_dropped_bytes = 4096;
    raw.audio_drops = 9;
    raw.audio_underruns = 10;
    raw.phase2_grants = 4;
    raw.rf_level_permille = 500;
    raw.cpu_valid = true;
    raw.cpu_core0_pct = 31;
    raw.cpu_core1_pct = 42;
    p25_health_publish(&raw, 100);
    p25_health_reset_counters();

    p25_health_snapshot_t s;
    LS_CHECK(p25_health_read(&s));
    LS_EQ_UINT(s.nid_valid, 0);
    LS_EQ_UINT(s.tsbk_valid, 0);
    LS_EQ_UINT(s.unhandled_total, 0);
    LS_EQ_UINT(s.usb_dropped_bytes, 0);
    LS_EQ_UINT(s.audio_underruns, 0);
    LS_EQ_UINT(s.phase2_grants, 0);
    LS_CHECK(s.net_valid);
    LS_EQ_UINT(s.wacn, 0xbee00);
    LS_EQ_UINT(s.rf_level_permille, 500);
    LS_CHECK(s.cpu_valid);

    /* The first full post-reset publication establishes the decoder-owned
     * TSBK/opcode epoch; otherwise pre-reset values could reappear. */
    p25_health_publish(&raw, 200);
    raw.nid_valid += 2;
    raw.tsbk_invalid += 1;
    raw.unhandled[0x2a] += 4;
    raw.usb_dropped_bytes += 512;
    raw.audio_underruns += 3;
    p25_health_publish(&raw, 201);
    LS_CHECK(p25_health_read(&s));
    LS_EQ_UINT(s.nid_valid, 2);
    LS_EQ_UINT(s.tsbk_invalid, 1);
    LS_EQ_UINT(s.unhandled_total, 4);
    LS_EQ_UINT(s.usb_dropped_bytes, 512);
    LS_EQ_UINT(s.audio_underruns, 3);
}
