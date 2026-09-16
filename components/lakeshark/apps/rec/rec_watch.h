#ifndef REC_WATCH_H
#define REC_WATCH_H
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#ifdef __cplusplus
extern "C" {
#endif
#define REC_WATCH_SLOTS 16
#define REC_WATCH_EDGES 4096
#define REC_WATCH_RESERVE (16u * 1024u * 1024u)
typedef enum { REC_SOURCE_RTL, REC_SOURCE_CC1101 } rec_source_t;
typedef struct { uint32_t value; uint16_t repeats, unit_us; } rec_ook24_t;
int rec_watch_filter_pulses(int32_t *pulse,int edges,uint32_t min_us);
bool rec_decode_ook24(const int32_t *pulse, int edges, rec_ook24_t *out);
rec_source_t rec_watch_source(void);
bool rec_watch_select_source(rec_source_t source);
typedef struct {
    uint32_t id, frequency, count, first_boot, last_boot;
    uint64_t first_ms, last_ms, order;
    uint32_t span_us;
    uint16_t edges;
    uint8_t pinned, end_reason;
    int32_t peak;
    uint8_t source, reserved;
} rec_watch_event_t;
typedef struct {
    rec_watch_event_t event;
    int32_t pulse[REC_WATCH_EDGES];
} rec_watch_record_t;
typedef struct {
    uint64_t archive_id;
    uint64_t sequence;
    uint32_t next_id, rejected;
    rec_watch_record_t record[REC_WATCH_SLOTS];
} rec_watch_catalog_t;
/* Similar pulse timing is a pattern match, never a device identity. */
int rec_watch_observe(rec_watch_catalog_t *c, uint32_t frequency,
    const int32_t *pulse, int edges, uint32_t boot, uint64_t ms,
    int peak, int reason, bool *novel);
int rec_watch_observe_from(rec_watch_catalog_t *c, rec_source_t source, uint32_t frequency,
    const int32_t *pulse, int edges, uint32_t boot, uint64_t ms,
    int peak, int reason, bool *novel);
bool rec_watch_pin(rec_watch_catalog_t *c, uint32_t id, bool pin);
uint32_t rec_watch_crc(const void *data, size_t len);
int rec_watch_receiver_want(int visible_mode, int rec_mode, bool enabled);
int rec_watch_receiver_want_source(int visible_mode, int rec_mode, bool enabled, rec_source_t source);
bool rec_watch_store(const char *dir, const rec_watch_catalog_t *c, uint64_t free_bytes);
/* c must remain immutable throughout the call. pump may process live captures,
   but must not call storage/export recursively (one shared SD sector buffer). */
bool rec_watch_store_pumped(const char *dir, const rec_watch_catalog_t *c,
    uint64_t free_bytes, void (*pump)(void *), void *context);

bool rec_watch_restore(const char *dir, rec_watch_catalog_t *c);
/* Manual export only. Existing output is never overwritten. */
bool rec_watch_export(const char *dir, const rec_watch_catalog_t *c, uint32_t id,
                      uint64_t free_bytes, char *result, size_t result_size);

typedef struct {
    bool ready, enabled, alerts, exporting;
    bool saved, pending_save, save_failed;
    uint32_t boot_id;
    uint32_t received, dropped, alert_sent, alert_failed, alert_suppressed;
    int count;
    char storage[64], peer[17];
    char export_status[112];
    rec_watch_event_t event[REC_WATCH_SLOTS];
    int32_t preview[REC_WATCH_SLOTS][48];
    rec_ook24_t decoded[REC_WATCH_SLOTS];
} rec_watch_status_t;
bool rec_watch_start(void);
bool rec_watch_enable(bool on);
bool rec_watch_enabled(void);
void rec_watch_snapshot(rec_watch_status_t *out);
void rec_watch_filter_status(rec_watch_status_t *out,rec_source_t source);
void rec_watch_submit(uint32_t frequency, const int32_t *pulse, int edges,
                      int peak, int reason);
void rec_watch_submit_from(rec_source_t source, uint32_t frequency,
                           const int32_t *pulse, int edges, int peak, int reason);
bool rec_watch_request_pin(uint32_t id, bool pin);
bool rec_watch_request_export(uint32_t id);
bool rec_watch_alert_target(const char *peer);
/* Integration hook: true means accepted for sending, not acknowledged. */
bool rec_watch_notify(const char *peer, const char *text);
#ifdef __cplusplus
}
#endif
#endif
