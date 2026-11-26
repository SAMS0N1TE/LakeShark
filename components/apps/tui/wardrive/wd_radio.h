/* WARDRIVE's extra radios: the LoRa chip (SX1262 or LR2021) and a USB SDR
   (RTL-SDR or HackRF), run as jobs beside the C6's Wi-Fi/BLE scan: TPMS and
   other sub-GHz devices (WD_RF), aircraft (WD_AIR), LoRa nodes (WD_LORA).
   A scheduler decides which job each radio runs, from the region, what the
   radios are, what is being found and what the user pinned.

   The scheduler and the decoder-to-sighting mapping live here and are pure.
   What touches a radio is the port (wd_rport_*), written by the feed: the
   board's in wd_feed_board.c, the simulator's in lssim_wardrive.c, a fake in
   the bench test. Only the feed calls wd_radio_start/stop/poll; the RADIO
   page reads wd_radio_view and sets pins. */
#ifndef WD_RADIO_H
#define WD_RADIO_H

#include "wd_core.h"
#include "../experiments/lr433_dec.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Whether the board can hear this kind at all with what is plugged in. */
bool wd_radio_has(int kind);
void wd_radio_start(const bool src[WD_KINDS]);
void wd_radio_stop(void);
/* What the radio jobs heard since the last call. Runs the scheduler. Takes
   wd_lock for a moment to read the fix and to publish state. */
int  wd_radio_poll(wd_obs_t *out, int cap, uint32_t now);

/* ---- jobs, radios, regions ---- */

/* TPMS: tyre sensors (315 MHz in the Americas north of Panama, 433.92
   elsewhere). ISM: weather stations, remotes, meters. AIR: ADS-B 1090.
   MESH: Meshtastic, MeshCore and LoRaWAN nodes. */
enum { WD_JOB_TPMS, WD_JOB_ISM, WD_JOB_AIR, WD_JOB_MESH, WD_JOBS };
/* The radios jobs run on. */
enum { WD_RJ_LORA, WD_RJ_SDR, WD_RJ_N };
/* NA: 315 TPMS, 915 ISM/LoRa. EU: 433.92 TPMS, 868 ISM/LoRa. AU: 433.92
   TPMS, 915-928 LoRa (Australia, New Zealand, South America). */
enum { WD_REGION_NA, WD_REGION_EU, WD_REGION_AU, WD_REGIONS };
/* A radio's state: not fitted; fitted, no job wanted; asking for it; running
   a job; another app has it. */
enum { WD_RS_ABSENT, WD_RS_OFF, WD_RS_WAIT, WD_RS_RUN, WD_RS_YIELD };
/* LoRa flavours (wd_obs_t.sub for WD_LORA). */
enum { WD_MESH_MESHTASTIC = 1, WD_MESH_MESHCORE, WD_MESH_LORAWAN };

const char *wd_radio_job_name(int job);     /* "TPMS", "ISM", "ADS-B", "MESH" */
const char *wd_radio_job_what(int job);     /* "tyre sensors", ... */
const char *wd_radio_region_name(int region);
/* Where a job listens in a region, kHz: the first of its frequencies, and
   all of them (at most 4; returns how many). */
uint32_t wd_radio_job_khz(int job, int region);
int wd_radio_job_freqs(int job, int region, uint32_t khz[4]);
/* The region a place is in. */
int wd_radio_region_of(double lat, double lon);

/* ---- the port: what the feed provides ---- */

typedef struct {
    bool present;
    char name[12];          /* "LR2021", "SX1262", "RTL-SDR", "HackRF" */
    uint8_t can;            /* 1 << WD_JOB_* it can run now */
    char busy[28];          /* "" free, else who has it ("ADS-B", "MESH screen") */
    bool waiting;           /* asked for, not granted yet */
    uint32_t freq_khz;      /* where it is listening, 0 not */
} wd_rport_t;

void wd_rport_probe(int r, wd_rport_t *out);
/* Run `job` (< 0: none) in `region`. The port opens it when it can and steps
   through the job's frequencies itself. False and why when it never can. */
bool wd_rport_run(int r, int job, int region, char *why, size_t n);
/* What radio r's job heard since the last call. */
int  wd_rport_poll(int r, wd_obs_t *out, int cap, uint32_t now);
/* What other apps' receivers and the mesh tables heard: zero radio claim. */
int  wd_rport_passive(wd_obs_t *out, int cap, uint32_t now);
void wd_rport_start(void);
void wd_rport_stop(void);

/* ---- the scheduler ---- */

#define WD_TRACE        60      /* activity buckets kept a radio */
#define WD_TRACE_MS     10000u  /* ...each ten seconds: the last ten minutes */
#define WD_PLAN         8
#define WD_LAST         6       /* the newest finds kept a radio */

typedef struct {
    bool can;
    uint16_t hits, runs;    /* finds this session; dwells started */
    uint8_t dry;            /* dwells in a row that found nothing */
    float rate;             /* finds a minute while it runs, smoothed */
    float score;            /* the scheduler's last word on it */
    uint32_t last_hit_ms, last_run_ms;  /* 0 never */
} wd_job_view_t;

typedef struct { int8_t job; uint32_t at_s, for_s; } wd_plan_step_t;

typedef struct {
    bool present;
    uint8_t state;                  /* WD_RS_* */
    int8_t job, pin;                /* -1 none */
    char name[12], why[32];
    uint32_t freq_khz, since_ms, until_ms;
    uint16_t hits;
    wd_job_view_t jobs[WD_JOBS];
    uint8_t trace[WD_TRACE];        /* finds a bucket, oldest first */
    int8_t trace_job[WD_TRACE];     /* the job in each bucket, -1 none */
    wd_plan_step_t plan[WD_PLAN];
    int plan_n;
    char last[WD_LAST][24];         /* the newest finds by name, newest first; "" none */
    int8_t last_job[WD_LAST];
} wd_radio_view_t;

enum { WD_MOVE_STILL, WD_MOVE_SLOW, WD_MOVE_DRIVE, WD_MOVE_UNKNOWN };

typedef struct {
    bool on;                        /* a session runs the radio jobs */
    uint8_t region, motion;
    bool region_fix;                /* the region came from a fix */
    float speed;                    /* m/s, < 0 unknown */
    uint32_t now, passive;          /* finds other apps' receivers made */
    wd_radio_view_t r[WD_RJ_N];
} wd_radio_state_t;

/* A copy of everything the page shows. Takes wd_lock itself. */
void wd_radio_view(wd_radio_state_t *out);
/* SWAP: pin radio r to a job for the drive (< 0 unpins). Takes wd_lock. */
void wd_radio_pin(int r, int job);
int  wd_radio_pinned(int r);

/* ---- the pure parts, for the feeds and the bench ---- */

typedef struct {
    int region;
    float speed;                    /* m/s, < 0 unknown */
    uint32_t now;
} wd_rctx_t;

/* One scheduler pass: dwell ends, choices, port runs. */
void wd_radio_tick(const wd_rctx_t *c);
/* Credit what was heard to the jobs that heard it. */
void wd_radio_take(const wd_obs_t *o, int n, uint32_t now);
/* The score the scheduler gives job j on radio r now (< -100: cannot). */
float wd_radio_score(int r, int j, const wd_rctx_t *c);
/* How long a dwell of job j lasts, given what it has found so far. */
uint32_t wd_radio_dwell_ms(int r, int j, int motion);
int wd_radio_motion(float speed);
/* The job an observation counts for, -1 none. */
int wd_radio_job_of(const wd_obs_t *o);
void wd_radio_reset(void);

/* Decoders to sightings. radio: WD_RADIO_*. */
bool wd_radio_obs_ism(wd_obs_t *o, const lr433_msg_t *m, uint32_t hz, float rssi_dbm, int radio);
typedef struct {
    uint32_t icao;
    uint8_t qual, cat;              /* address qualifier; emitter tc << 4 | ca */
    char callsign[9];
    bool pos;
    float lat, lon;
    int alt_ft;                     /* INT32_MIN unknown */
} wd_air_t;
void wd_radio_obs_air(wd_obs_t *o, const wd_air_t *a, int radio);
/* A LoRa packet heard with a flavour's settings: header and, for a MeshCore
   advert, the node's name and place. False when it is not one. */
bool wd_radio_obs_lora(wd_obs_t *o, const uint8_t *p, int n, int flavour, float rssi_dbm,
                       uint32_t hz, int radio);

#ifdef __cplusplus
}
#endif
#endif
