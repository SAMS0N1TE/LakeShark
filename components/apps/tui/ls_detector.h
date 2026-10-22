#ifndef LS_DETECTOR_H
#define LS_DETECTOR_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
#define LS_DETECTOR_RULES 64
#define LS_DETECTOR_BYTES 32768
#define LS_DETECTOR_HISTORY 24
enum { DET_RID=1, DET_BLE=2, DET_WIFI=4, DET_LORA=8, DET_ADSB=16 };
/* Independent Drone Watch queue, same rules/cooldowns/sinks as DETECTOR. */
#define DET_DRONE_WATCH 32
bool ls_detector_watch_notice(uint64_t id,int rssi,bool calibrated,unsigned band,bool approaching,int64_t now);
/* Aircraft predicates a rule can carry (ls_detector_rule_t.air_mask). */
enum { DET_AIR_SQUAWK=1, DET_AIR_CALL=2, DET_AIR_ICAO=4, DET_AIR_CAT=8, DET_AIR_KM=16, DET_AIR_ALT=32, DET_AIR_EMERG=64 };
#define LS_DETECTOR_AIR_LIST 8
#define LS_DETECTOR_AIR_NO_DISTANCE 0xFFFFu
enum { DET_BANNER=1, DET_VIBRATE=2, DET_SD=4 };
typedef struct {
    char id[32];
    uint32_t cooldown_s, rule_cooldown_s;
    uint8_t sources, categories, sinks, sections, bands;
    int8_t min_rssi_dbm;
    uint32_t min_hits_per_min;
    bool has_min_rssi;
    bool enabled, validated, has_validated;
    /* Aircraft predicates, ANDed; air_mask says which are set (an empty watch
     * list is set and matches nothing). Only rules whose sole source is adsb
     * carry them. */
    uint8_t air_mask, air_squawks, air_calls, air_icaos;
    uint16_t air_cats, air_km;
    int32_t air_alt_below_ft; /* barometric, as the aircraft reports it */
    uint16_t air_squawk[LS_DETECTOR_AIR_LIST];
    char air_call[LS_DETECTOR_AIR_LIST][9];
    uint32_t air_icao_lo[LS_DETECTOR_AIR_LIST], air_icao_hi[LS_DETECTOR_AIR_LIST];
} ls_detector_rule_t;
typedef struct {
    uint64_t entity, rules;
    int64_t at;
    uint32_t sequence, revision, coalesced, dropped, suppressed;
    uint8_t source, category, selected, allowed, delivered, failed;
    int8_t rssi;
    uint16_t band, rate_guess;
    uint32_t hits_per_min;
    bool test, validated;
    bool relative_rssi; /* SDR evidence uses relative dB, not calibrated dBm */
    /* Aircraft events (source adsb). Unknown altitude is INT32_MIN, unknown
     * distance LS_DETECTOR_AIR_NO_DISTANCE; neither position nor distance is
     * ever logged. */
    uint32_t icao;
    uint8_t qualifier, emitter_tc, emitter_ca, emergency;
    uint16_t squawk, distance_x10_km;
    int32_t altitude_ft;
    char callsign[9];
    char reason[96];
    char rule_ids[2177]; /* JSON string list, IDs validated to [A-Za-z0-9_-]. */
} ls_detector_event_t;
typedef struct {
    bool running;
    uint8_t sources, sinks;
    unsigned rules, queued, recent;
    uint32_t revision, observations, dropped, suppressed, matches;
    char error[96], lora[32];
    char adsb[20];            /* receiver another app runs, or "OFF"; DETECTOR never starts one */
} ls_detector_status_t;
void ls_detector_init(void);
bool ls_detector_running(void);
bool ls_detector_pending(void);
void ls_detector_start(bool on);
void ls_detector_request(bool test);
void ls_detector_policy(uint8_t sources, uint8_t sinks);
void ls_detector_status(ls_detector_status_t *out);
/* UI snapshot: never waits on the wireless worker; false preserves the old view. */
bool ls_detector_snapshot(ls_detector_status_t *out,ls_detector_event_t *events,size_t cap,size_t *n);
size_t ls_detector_history(ls_detector_event_t *out, size_t cap);
bool ls_detector_rule(unsigned index, ls_detector_rule_t *out);
/* Nonblocking counterpart for OPTIONS value rendering. */
bool ls_detector_rule_snapshot(unsigned index, ls_detector_rule_t *out);
bool ls_detector_rule_set(unsigned index, const ls_detector_rule_t *rule);
bool ls_detector_parse(const char *json, size_t len);
/* Nonblocking, bounded producer. No file, notification or hardware calls. */
void ls_detector_observe(const uint8_t mac[6], uint8_t type, uint8_t source,
                         uint8_t category, int rssi, bool validated, bool flood, uint8_t section, int64_t now);
void ls_detector_drone(unsigned band, unsigned channel, unsigned rate_guess,
                       int rssi, unsigned hits_per_min, int64_t now);
/* Existing wireless worker calls this, independently of the screen. */
void ls_detector_tick(void);
int ls_detector_command(int argc, char **argv);
const char *ls_detector_source(unsigned source);
/* Platform sinks return accepted queue requests, not physical delivery. */
uint8_t ls_detector_deliver(const ls_detector_event_t *event, uint8_t sinks);
void ls_detector_log_begin(void);
bool ls_detector_log_end(void);
bool ls_detector_write(const ls_detector_event_t *event, uint64_t boot);
#ifdef __cplusplus
}
#endif
#endif
