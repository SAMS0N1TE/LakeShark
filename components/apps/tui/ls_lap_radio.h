/* API 2.0 for native apps: radio sessions, receiver audio, decoded events,
 * actions and notices. What the lap_* calls in lakeshark_app.h stand on.
 *
 * Three tasks touch it, and each has its own side:
 *  - the screen task runs the app's callbacks: the lap_* calls only read
 *    snapshots, copy out of queues and leave requests in a mailbox;
 *  - the lap_io worker (ls_lap_radio_service) acquires, tunes and releases
 *    radios through a backend, and turns I/Q snapshots into spectrum lines;
 *  - a backend's capture context (the SDR capture task, the LR2021 owner
 *    task) pushes I/Q with ls_lap_rx_push. It never runs app code and never
 *    waits: a full queue drops the newest samples and counts them.
 * Everything big (the 256 KB I/Q queue, the spectrum, the audio rings, the
 * event ring) is PSRAM, allocated while a session or subscription is open. */
#ifndef LS_LAP_RADIO_H
#define LS_LAP_RADIO_H
#include "ls_lap_manifest.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

#define LS_LAP_RX_SLOTS      32u
#define LS_LAP_RX_SLOT_PAIRS 2048u

/* Screen task. begin with the app's descriptor when it is entered (NULL:
   nothing granted, as ls_lap_sys_begin does first); the app's declared
   flags are its grants, because the board asked before loading it. end when
   it closes: every session, tap and subscription is closed, and the radio is
   given back by the worker. */
void ls_lap_radio_begin(const lap_app_t *app);
void ls_lap_radio_end(void);
uint32_t ls_lap_granted(void);           /* LAP_CAP_* this launch has */
/* lap_io worker. True while it has work and wants calling again soon. */
bool ls_lap_radio_service(void);
void ls_lap_radio_set_wake(void (*wake)(void));
/* Console (`app rx`): the radio list and the session, one line each. */
void ls_lap_radio_describe(void (*line)(const char *text));

/* ---- backends ----------------------------------------------------------- */

typedef struct {
    uint64_t center_hz;
    uint32_t rate_hz;
    int gain_tenths_db;                  /* LAP_GAIN_AUTO when automatic */
} ls_lap_rx_actual_t;

/* Every call is made on the lap_io worker, one at a time. Negative returns
   are LAP_RX_E*; open may return LAP_RX_EAGAIN to be called again on a later
   pass (the LR2021 waits for MeshCore to let go). close returns only when
   the capture context has stopped pushing. */
typedef struct {
    int (*list)(lap_radio_info_t *out, int max);
    int (*open)(int radio, const lap_rx_config_t *cfg, ls_lap_rx_actual_t *actual);
    int (*tune)(uint64_t hz, ls_lap_rx_actual_t *actual);
    int (*gain)(int tenths_db, ls_lap_rx_actual_t *actual);
    int (*rate)(uint32_t rate_hz, ls_lap_rx_actual_t *actual);
    int (*poll)(ls_lap_rx_actual_t *actual);   /* <0 when the stream has died */
    void (*close)(void);
} ls_lap_rx_backend_t;
/* The board's (ls_lap_radio_hw.c) or the bench's simulator (ls_lap_radio_sim.c). */
const ls_lap_rx_backend_t *ls_lap_rx_default_backend(void);
/* Tests replace it; NULL goes back to the default. Not while a session is open. */
void ls_lap_radio_set_backend(const ls_lap_rx_backend_t *backend);

/* Capture context only (one producer). iq is interleaved int16 pairs. */
void ls_lap_rx_push(const int16_t *iq, size_t pairs);
/* The next block starts fresh with these flags (LAP_IQ_*); a partly filled
   block is handed over first. center_hz/rate_hz of 0 keep the current ones.
   From the capture context, or from the worker while capture is stopped. */
void ls_lap_rx_mark(uint64_t center_hz, uint32_t rate_hz, uint32_t flags);

/* ---- audio (ls_lap_audio.c) ---------------------------------------------- */

void ls_lap_audio_begin(void);
void ls_lap_audio_end(void);
bool ls_lap_audio_service(void);          /* worker: drains app PCM to the speaker */
/* Live radio audio as it reaches audio_out, from the receiver's own task. */
void ls_lap_audio_tap(const int16_t *pcm, int n);

/* ---- events, actions, notices (ls_lap_events.c) --------------------------- */

void ls_lap_events_begin(const lap_app_t *app);
void ls_lap_events_end(void);
bool ls_lap_events_service(void);         /* worker: mesh polling */

#if !defined(ESP_PLATFORM) || defined(LS_LAP_HOST_TEST)
/* The bench: what the simulator and the tests can see and set. */
typedef struct {
    int notices, alerts;                  /* posted so far */
    char last_title[24], last_body[128];
    int last_level;
    int played;                           /* app PCM samples that reached the "speaker" */
    bool muted, live;
} ls_lap_hub_host_t;
ls_lap_hub_host_t *ls_lap_hub_host(void);
/* The bench's stand-in for ls_action_call; NULL answers LAP_ACT_UNAVAILABLE. */
extern int (*ls_lap_host_action)(const char *name, const char *args, char *out, size_t cap,
                                 uint32_t granted);
#endif
#ifdef __cplusplus
}
#endif
#endif
