/* Channelised scanner for the FM app's AM side: airband (25 kHz and
   8.33 kHz plans, 121.5 MHz guard watch) and CB (40 channels, AM, USB or
   LSB), plus the seven NOAA weather channels for FM's SAME mode.

   One tune sees about 176 kHz, so instead of retuning per channel the way
   scan_engine.c does for stored NFM memories, every window's channels are
   measured at once from one averaged FFT: 760 airband channels in about a
   hundred hops. A carrier standing far enough above the window's noise floor
   (and not just the mirror image of a strong one on the other side of the
   LO) is a hit; the receiver parks on it until it has been quiet for the hang
   time, sampling the guard channel every few seconds while it does.

   Pure logic, no RTOS and no radio: the receive task asks where to tune,
   feeds one dB spectrum per block, and does what the returned action says
   (fm_air.c). Host tested in bench/tests/test_air_scan.c. */
#ifndef AIR_SCAN_H
#define AIR_SCAN_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    AIR_DEMOD_AM = 0, AIR_DEMOD_USB, AIR_DEMOD_LSB, AIR_DEMOD_NFM,
} air_demod_t;

typedef enum {
    AIR_PRESET_OFF = 0,
    AIR_PRESET_AIR25,     /* 118.000-136.975 MHz, 25 kHz, guard 121.5       */
    AIR_PRESET_AIR833,    /* 118.000-136.992 MHz, 8.33 kHz, guard 121.5     */
    AIR_PRESET_CB_AM,     /* CB 1-40 AM                                     */
    AIR_PRESET_CB_USB,    /* CB 1-40 USB (the HF chain's hf_demod)          */
    AIR_PRESET_CB_LSB,
    AIR_PRESET_WX,        /* NOAA weather 1-7, NFM                          */
    AIR_PRESET_COUNT
} air_preset_id_t;

typedef struct {
    const char *name;        /* "AIR 25k"                                    */
    const char *band;        /* "AIRBAND", "CB", "WEATHER"                   */
    uint8_t  demod;          /* air_demod_t                                  */
    uint32_t lo_hz, step_hz; /* a raster: lo + i * step (8.33k: 25000/3)     */
    uint16_t raster_n;
    const uint32_t *list;    /* or a list, in channel-number order           */
    uint16_t list_n;
    uint32_t guard_hz;       /* 0 for none                                   */
    uint16_t bw_hz;          /* +- this around a channel is its signal       */
} air_preset_t;

const air_preset_t *air_preset(int id);
/* Channel `i` of a preset in Hz, and its number as the band names it
   (CB 1..40, WX 1..7, airband 0 = by frequency). */
uint32_t air_channel_hz(const air_preset_t *p, int i);
int air_channel_count(const air_preset_t *p);
int air_channel_number(const air_preset_t *p, int i);
/* The channel nearest `hz` within half a spacing, -1 for none. */
int air_channel_find(const air_preset_t *p, uint32_t hz);

#define AIR_MAX_CH      2304
#define AIR_MAX_WIN     128
#define AIR_HITS        24
#define AIR_SPAN_HZ     176000u
/* Listening parks the LO a quarter of the sample rate above the channel
   and the receive task rotates it back down by fs/4: the dongle's DC spike
   lands far outside the AM filter. */
#define AIR_LISTEN_OFS  64000u

typedef struct {
    uint32_t hz;
    uint16_t channel;        /* index into the preset                        */
    uint16_t count;          /* times it opened                              */
    int64_t  first_us, last_us;
    uint32_t talk_ms;        /* carrier time while listened to               */
    int8_t   peak_db;        /* above the floor                              */
    bool     guard;
} air_hit_t;

typedef enum {
    AIR_PH_IDLE = 0, AIR_PH_HOP, AIR_PH_GUARD, AIR_PH_LISTEN,
} air_phase_t;

typedef struct {
    uint32_t tune_hz;        /* nonzero: retune the LO here before the next  */
    bool     audio;          /* listening with the carrier up: play it       */
    bool     listen;         /* parked on listen_hz (LO = it + AIR_LISTEN_OFS) */
    uint32_t listen_hz;
} air_action_t;

typedef struct {
    const air_preset_t *p;
    int      preset;
    int      nch;
    float    thresh_db;      /* above the window floor                       */
    int      settle;         /* blocks dropped after a retune                */
    int      hang_ms;
    int      guard_every;    /* hops between guard samples while scanning    */
    int      guard_ms;       /* between guard samples while listening        */
    bool     guard_on;
    /* Windows: centre and the channel range each one measures. */
    int      nwin;
    uint32_t win_hz[AIR_MAX_WIN];
    uint16_t win_first[AIR_MAX_WIN], win_n[AIR_MAX_WIN];
    uint16_t order[64];      /* a list preset's channels in frequency order  */
    int      guard_ch;       /* channel index of the guard, -1 none          */
    /* Where it is. */
    air_phase_t phase;
    int      win;
    int      drop;           /* blocks still to drop                         */
    int      hops;           /* since the last guard sample                  */
    uint32_t lo_hz;          /* the LO as last asked                         */
    int      listen_ch;
    bool     held;           /* manual hold: stays whatever the carrier does */
    bool     carrier;
    int64_t  carrier_us;     /* last time the listened carrier was up        */
    int64_t  listen_since_us, guard_due_us, talk_mark_us;
    air_phase_t resume;      /* what a guard sample returns to               */
    uint32_t sweeps;
    int64_t  sweep_t0_us;
    uint32_t sweep_ms;
    /* What the band looks like: dB above floor per channel, 0..63. */
    uint8_t  level[AIR_MAX_CH];
    uint8_t  lockout[(AIR_MAX_CH + 7) / 8];
    air_hit_t hits[AIR_HITS];
    int      nhits;
    uint32_t images_rejected;
} air_scan_t;

/* Start a preset (or stop with AIR_PRESET_OFF). The first action is a tune. */
air_action_t air_scan_start(air_scan_t *s, int preset, int64_t now_us);
void air_scan_stop(air_scan_t *s);
bool air_scan_active(const air_scan_t *s);
/* One block's spectrum: n bins of dB, fftshifted (bin 0 is -rate/2), taken
   at the LO the last action asked for. NULL while the block could not be
   measured: it still counts toward the settle. */
air_action_t air_scan_block(air_scan_t *s, const float *db, int n, uint32_t rate,
                            int64_t now_us);

/* The person: resume scanning now; lock the listened channel out and
   resume; hold here; listen to channel `ch` and hold. */
air_action_t air_scan_skip(air_scan_t *s, int64_t now_us);
air_action_t air_scan_lockout(air_scan_t *s, int64_t now_us);
void air_scan_hold(air_scan_t *s, bool hold);
air_action_t air_scan_listen(air_scan_t *s, int ch, int64_t now_us);
void air_scan_clear_lockouts(air_scan_t *s);
bool air_scan_locked(const air_scan_t *s, int ch);
int  air_scan_lockout_count(const air_scan_t *s);

#ifdef __cplusplus
}
#endif

#endif
