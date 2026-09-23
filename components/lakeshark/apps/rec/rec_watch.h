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
/* Appended, never inserted: the value is stored in every archived event
   and in the file on the card, so an existing archive would rename its own
   captures if these moved.

   REC_SOURCE_COUNT is here so that the next one after this cannot be added
   by editing an enum alone - anything sized per source says so. */
typedef enum {
    REC_SOURCE_RTL,
    REC_SOURCE_CC1101,
    /* The base-board SX1262. Unlike the other two it demodulates rather than
       timing edges, so its captures carry the modulation that heard them -
       see the fsk fields on rec_watch_event_t. It replays FSK; CC1101 replays OOK. */
    REC_SOURCE_SX1262,
    REC_SOURCE_COUNT
} rec_source_t;

/* One place that names a source. Every call site used to spell this as a
   ternary between two names, which silently reports a third source as the
   one it is not.

   Inline because the screens are built in the bench against a stubbed
   rec_watch: a link dependency for a switch statement would make every stub
   grow its own copy, and a copy can disagree. */
static inline const char *rec_source_name(rec_source_t source)
{
    switch (source) {
    case REC_SOURCE_RTL:    return "RTL";
    case REC_SOURCE_CC1101: return "CC1101";
    case REC_SOURCE_SX1262: return "SX1262";
    default:                return "?";
    }
}

/* Replay follows the capture source: CC1101 OOK or SX1262 FSK. */
static inline bool rec_source_can_replay(rec_source_t source)
{
    return source == REC_SOURCE_SX1262 || source == REC_SOURCE_CC1101;
}
#include "subghz_pwm.h"
#include "subghz_nrz.h"

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
    /* How the capture was modulated. Zero for a source that times edges
       rather than demodulating, which is what every pre-v4 archive reads
       back as after migration.

       In the event and not in a table beside it, on purpose: a capture and
       the settings that heard it have to be evicted, rotated, pinned and
       restored as one thing. A parallel structure is a second truth that
       drifts, and the way it fails is a replay on the wrong channel, which
       looks exactly like a replay that did nothing. */
    uint32_t bitrate, deviation_hz, sync_word;
    uint16_t preamble_bits, bandwidth_khz;
} rec_watch_event_t;

/* The pre-v4 event, kept so a card written by an older build still loads.
   Never written - rec_watch_store always writes the current layout, so an
   archive migrates the first time it is saved. */
typedef struct {
    uint32_t id, frequency, count, first_boot, last_boot;
    uint64_t first_ms, last_ms, order;
    uint32_t span_us;
    uint16_t edges;
    uint8_t pinned, end_reason;
    int32_t peak;
    uint8_t source, reserved;
} rec_watch_event_v3_t;
typedef struct { rec_watch_event_v3_t event; int32_t pulse[REC_WATCH_EDGES]; } rec_watch_record_v3_t;
typedef struct {
    uint64_t archive_id, sequence;
    uint32_t next_id, rejected;
    rec_watch_record_v3_t record[REC_WATCH_SLOTS];
} rec_watch_catalog_v3_t;
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

/* How a demodulating source was modulated when it heard something. Passed
   alongside the capture rather than set afterwards, so a record is never
   briefly on the card describing settings it was not heard with. */
typedef struct {
    uint32_t bitrate, deviation_hz, sync_word;
    uint16_t preamble_bits, bandwidth_khz;
} rec_fsk_mod_t;

/* As rec_watch_observe_from, and additionally stores `mod` with the capture.
   NULL means a source that times edges and has no modulation to record - the
   two entry points above are that case. */
int rec_watch_observe_mod(rec_watch_catalog_t *c, rec_source_t source, uint32_t frequency,
    const int32_t *pulse, int edges, uint32_t boot, uint64_t ms,
    int peak, int reason, const rec_fsk_mod_t *mod, bool *novel);
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
    /* The same frames read for any length and, at 24 bits, for what the
       payload means. Kept beside decoded[] rather than replacing it: the
       catalogue's ranking and de-duplication key on the 24-bit result. */
    subghz_pwm_t pwm[REC_WATCH_SLOTS];
    /* The other family: run-length OOK, which the PWM reader cannot see
       because it sends no long preamble space. */
    subghz_nrz_t nrz[REC_WATCH_SLOTS];
} rec_watch_status_t;
bool rec_watch_start(void);
bool rec_watch_enable(bool on);
bool rec_watch_enabled(void);
void rec_watch_snapshot(rec_watch_status_t *out);
/* The copy rec_watch_snapshot makes while it holds the lock, on its own so it
   can be tested: the runtime that calls it needs FreeRTOS and never reaches
   the bench. Copies the scalars and the first `count` entries of the three
   per-slot arrays, which is all publish() ever populates - the slots past it
   hold whatever a longer catalog left behind, in the source as much as in the
   destination. Returns the bytes moved, which is the point of doing it this
   way: the whole struct is 4.7 KB and it is being moved with interrupts off
   once per drawn frame. */
size_t rec_watch_status_copy(rec_watch_status_t *dst,
                             const rec_watch_status_t *src, int count);
void rec_watch_filter_status(rec_watch_status_t *out,rec_source_t source);
void rec_watch_submit(uint32_t frequency, const int32_t *pulse, int edges,
                      int peak, int reason);
void rec_watch_submit_from(rec_source_t source, uint32_t frequency,
                           const int32_t *pulse, int edges, int peak, int reason);
void rec_watch_submit_mod(rec_source_t source, uint32_t frequency,
                          const int32_t *pulse, int edges, int peak, int reason,
                          const rec_fsk_mod_t *mod);

/* What a demodulating source listens with. Set only while the watch is
   stopped - changing it under a running session would file captures against
   settings that did not hear them. */
/* Why the last WATCH refused to start, or "" if it did. */
const char *rec_watch_error(void);

void rec_watch_fsk_get(rec_fsk_mod_t *out);
bool rec_watch_fsk_set(const rec_fsk_mod_t *in);
bool rec_watch_request_pin(uint32_t id, bool pin);
bool rec_watch_request_export(uint32_t id);

/* Put a capture back on air at `dbm`. Only a source that can transmit and a
   capture that recorded how it was modulated - anything else is a frame with
   no settings to send it under. Queued like a pin or an export; the result
   lands in the status line. */
bool rec_watch_request_replay(uint32_t id, int dbm);

/* FIND THE SIGNAL, rather than requiring it to be described first.

   A receiver that must be told the frequency, rate, deviation and sync word
   before it will hear anything is only useful to somebody who already knows
   the answer. This is the part that can be measured: sweep a range, keep the
   loudest reading each bin saw, and report it.

   `seconds` matters because a transmitter that only speaks when a button is
   pressed is absent from any single pass. The sweep repeats for that long and
   each bin keeps its peak, so a burst anywhere in the window is caught. */
#define REC_SCAN_BINS 32
/* `dbm` is the level after ballistics, `hold` the decaying peak.

   A raw reading per pass is unreadable: thermal noise moves several dB pass
   to pass, so every column dances and nothing stands out. Rising instantly
   and falling slowly is what a spectrum analyser does and why one is legible
   - a burst still jumps the moment it appears, but the display settles
   between bursts instead of boiling. The hold line is what catches a 60 ms
   press that landed between two looks at that bin. */
typedef struct {
    uint32_t hz;
    float dbm, hold;
    /* Only filled for a recorded detection, not for a live display bin.
       "Heard once, an hour ago" and "heard forty times, still going" are
       completely different findings and the list used to show them
       identically. */
    uint32_t seen;
    int64_t  first_us, last_us;
} rec_scan_bin_t;

/* `bins` is the resolution, and it is also the capture probability.

   A swept receiver is in one place at a time. Each bin costs about 8.4 ms on
   this board - mostly filter settling - so 32 bins is a 270 ms pass, and a
   60 ms burst only overlaps the moment the sweep is sitting on its bin about
   a quarter of the time. That is not a sensitivity problem and no amount of
   gain fixes it: it is the duty cycle.

   Fewer bins is the lever. Eight bins is a 67 ms pass, which a 60 ms burst
   can hardly miss - at the cost of knowing the frequency only to an eighth
   of the span. So: many bins to find something, few to watch it. */
bool rec_watch_request_scan(uint32_t min_hz, uint32_t max_hz, uint32_t seconds,
                            int bins);
bool rec_watch_scan_busy(void);

/* How far through, 0..100, and what it is doing. A sweep takes tens of
   seconds; without these the screen is indistinguishable from a hung one,
   which is exactly how it was first reported. */
int rec_watch_scan_progress(void);
const char *rec_watch_scan_stage(void);

/* The sweep as it stands right now, in frequency order, for drawing.

   A scan that runs to a deadline and then reports is a job. A scan that
   keeps sweeping and shows what it can hear is an instrument, and the
   difference matters when the thing you are looking for only speaks when
   somebody presses a button - you want to watch the band, press, and see it
   appear. This is the live row; rec_watch_scan_result is the accumulated
   list of things that actually crossed the threshold. */
int   rec_watch_scan_live(rec_scan_bin_t *out, int max);
float rec_watch_scan_live_floor(void);
int   rec_watch_scan_hits(void);
void  rec_watch_scan_stop(void);

/* WHAT HAPPENS WHEN IT FINDS SOMETHING, so a sweep can be left running.
 *
 * A sweep measures energy; it does not demodulate, because the radio cannot
 * do both at once. So CATCH is a hand-off: on the first clear hit it stops
 * sweeping, tunes to that frequency and starts the receiver, which is the
 * only way to get from "something is transmitting there" to a capture.
 *
 * That hand-off costs the sweep. Everything found after it is found by the
 * receiver, not by looking around - which is the right trade once you know
 * where to look, and the wrong one while you are still hunting. */
typedef enum {
    REC_SCAN_ON_HIT_NOTHING = 0,  /* just add it to the list           */
    REC_SCAN_ON_HIT_BUZZ,         /* and buzz, and DM if alerts are on */
    REC_SCAN_ON_HIT_CATCH         /* and tune to it and start the receiver */
} rec_scan_on_hit_t;

void rec_watch_scan_on_hit(rec_scan_on_hit_t mode);
rec_scan_on_hit_t rec_watch_scan_on_hit_get(void);
/* The frequency of the most recent detection, or 0. */
uint32_t rec_watch_scan_last_hit(void);
/* How many times, this sweep, any bin rose through the threshold. Counts
   bursts, where rec_watch_scan_hits counts frequencies. */
int rec_watch_scan_events(void);
/* Stop the running sweep, tune to hz and start WATCH on it - the same
   hand-off as ON DETECT CATCH, on request. False when no sweep is running. */
bool rec_watch_scan_catch(uint32_t hz);
/* Send a .sub file once on the SX1262, on the storage worker. Only files
   that carry FSK settings can be sent; the result lands in
   rec_watch_last_result(). False when the worker is busy or WATCH is on. */
bool rec_watch_request_replay_file(const char *path, int dbm);
/* The last export, replay or sweep result, for a screen that is not SUB-GHZ. */
const char *rec_watch_last_result(void);
/* Atomic replay-only busy/result snapshot; includes queued work. */
bool rec_watch_replay_status(char *out, size_t len);

/* How far above the floor a bin has to sit before it counts as something
   rather than as noise. The default; the working value is adjustable,
   because the right number depends on the room and on what is being looked
   for - and the operator watching the display can see where it should sit
   far better than a constant can. */
#define REC_SCAN_DETECT_DB 12.0f
#define REC_SCAN_DETECT_MIN 3.0f
#define REC_SCAN_DETECT_MAX 45.0f

float rec_watch_scan_threshold(void);
void  rec_watch_scan_set_threshold(float db);
/* Moves the line without saving it - for a drag, which saves once on release. */
void  rec_watch_scan_preview_threshold(float db);

/* The last completed sweep, strongest first. Returns how many were written. */
int  rec_watch_scan_result(rec_scan_bin_t *out, int max);

/* The noise floor that sweep saw, so a reading can be judged against it
   rather than against an absolute number that means nothing indoors. */
float rec_watch_scan_floor(void);

/* WORK OUT HOW TO LISTEN, instead of being told.
 *
 * Three different kinds of answer, and they are not equally trustworthy:
 *
 *   deviation  MEASURED. A 2-FSK signal is two tones; a fine sweep across
 *              the carrier shows their separation, which is twice the
 *              deviation. No demodulation needed, so this works before
 *              anything else is known.
 *   bit rate   SEARCHED. Tried against a ladder of standard rates; the right
 *              one is the one where repeated captures of the same frame come
 *              out identical and the wrong ones produce noise that differs
 *              every time.
 *   sync word  INFERRED. Read out of those captures - the bytes after the
 *              preamble run. A radio cannot discover a sync word, because
 *              matching one is the only thing it does with it.
 *
 * `confidence` is how many of the captures at the winning rate agreed, as a
 * percentage. Anything below about half is a guess and says so.
 */
typedef struct {
    uint32_t bitrate, deviation_hz, sync_word;
    uint16_t preamble_bits;
    uint8_t  confidence, agreed, tried;
    char     note[56];
    bool     valid;
} rec_learn_t;

bool rec_watch_request_learn(uint32_t seconds);
bool rec_watch_learn_busy(void);
bool rec_watch_learn_result(rec_learn_t *out);
/* Copy what it worked out into the receive settings. */
bool rec_watch_learn_apply(void);
bool rec_watch_alert_target(const char *peer);
/* Integration hook: true means accepted for sending, not acknowledged. */
bool rec_watch_notify(const char *peer, const char *text);
#ifdef __cplusplus
}
#endif
#endif
