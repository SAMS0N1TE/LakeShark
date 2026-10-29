/* Broadcast and listening extras inside the FM app: WFM stereo and RDS,
   the airband and CB channel scanner, SSTV pictures and the NOAA weather
   channels. Their options (one packed, persisted word) and the snapshots the
   receive task publishes for the screen.

   Pure C: the receive task writes the snapshots, the UI task reads them
   through a sequence lock, so neither waits on the other and a reader never
   sees half a card. Built into lssim as well, which seeds the snapshots. */
#ifndef FM_BCAST_H
#define FM_BCAST_H

#include <stdbool.h>
#include <stdint.h>

#include "rds.h"
#include "air_scan.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---- options ----------------------------------------------------------- */

enum {
    FM_BC_RDS_OFF    = 1u << 0,   /* skip the RDS demodulator (saves CPU)   */
    FM_BC_MONO       = 1u << 1,   /* force mono on a stereo station        */
    FM_BC_PTY_EU     = 1u << 2,   /* European RDS PTY names, not RBDS       */
    FM_BC_CLOCK_OFF  = 1u << 3,   /* never set the clock from RDS CT        */
    FM_BC_GUARD_OFF  = 1u << 6,   /* airband: no 121.5 guard watch          */
    FM_BC_SSTV       = 1u << 7,   /* SSTV decoder on LISTEN / AM audio      */
};
#define FM_BC_WX_SHIFT 8          /* bits 8-9: SAME-mode speaker, fm_wx.h   */
#define FM_BC_WX_MASK  (3u << FM_BC_WX_SHIFT)

uint32_t fm_bcast_options(void);
bool fm_bcast_opt(uint32_t bit);
void fm_bcast_set_opt(uint32_t bit, bool on);
/* The weather speaker: 0 off, 1 on, 2 alerts only. */
int fm_bcast_wx_audio(void);
void fm_bcast_set_wx_audio(int mode);
/* Load from settings (boot, FM init). */
void fm_bcast_load(void);

/* ---- WFM --------------------------------------------------------------- */

typedef struct {
    uint32_t pubs;           /* moves on every publish (set by publish)    */
    bool  running;           /* WFM mode is demodulating                   */
    bool  pilot, stereo;
    float pilot_level;       /* pilot injection, full deviation = 1        */
    float lvl_l, lvl_r;      /* audio level per channel, 0..1              */
    float rds_level;
    rds_info_t rds;
} fm_wfm_status_t;

void fm_wfm_publish(const fm_wfm_status_t *s);
void fm_wfm_status(fm_wfm_status_t *out);

/* The last FM_WFM_HIST publishes, oldest first: each channel's level over
   a 40 dB scale (0..255), what RDS delivered (0 nothing, 1 clean, 2 mixed,
   3 bad) and whether the decoder was in mono. Returns how many. A history
   column is written per publish, so it stands still when the decoder does. */
#define FM_WFM_HIST 128
int fm_wfm_history(uint8_t *l, uint8_t *r, uint8_t *q, bool *mono, int max);
/* 0..255 for a level on that scale (full deviation = 1). */
uint8_t fm_wfm_level_byte(float level);
/* Moves on every publish whose card changed. */
uint32_t fm_wfm_seq(void);

/* ---- AIR / CB / WX channel scanner ----------------------------------------- */

typedef struct {
    uint32_t pubs;
    int64_t  now_us;          /* when published: ages in the hit list        */
    int      preset;          /* air_preset_id_t, 0 when off                 */
    int      phase;           /* air_phase_t                                 */
    uint32_t lo_hz, listen_hz, win_lo_hz, win_hi_hz;
    int      listen_ch, nch, win, nwin;
    bool     carrier, held, guard_on, guard_now;
    bool     demod_as_am;     /* USB/LSB preset playing through AM: no SSB state */
    float    thresh_db;
    uint32_t sweeps, sweep_ms, images;
    int      lockouts;
    int      nhits;
    air_hit_t hits[AIR_HITS];
} fm_air_status_t;

void fm_air_publish(const fm_air_status_t *s, const uint8_t *levels, int n);
void fm_air_status(fm_air_status_t *out);
/* dB above the floor per channel, 0..63, as last measured. */
int fm_air_levels(uint8_t *out, int max);

/* The screen asks; the receive task takes. */
enum {
    FM_AIR_REQ_PRESET = 1,    /* arg: air_preset_id_t, 0 stops              */
    FM_AIR_REQ_SKIP,
    FM_AIR_REQ_LOCKOUT,
    FM_AIR_REQ_HOLD,          /* arg: 0 release, 1 hold                     */
    FM_AIR_REQ_LISTEN,        /* arg: channel index                         */
    FM_AIR_REQ_CLEAR_LOCKOUTS,
    FM_AIR_REQ_THRESH,        /* arg: dB above the floor                    */
    FM_AIR_REQ_WX_FIND,       /* SAME mode: tune the strongest WX channel   */
};
void fm_air_request(int req, int arg);
bool fm_air_take(int *req, int *arg);

/* ---- SSTV ----------------------------------------------------------------- */

typedef struct {
    uint32_t pubs;
    bool     on;              /* the decoder is listening to this mode's audio */
    bool     receiving;       /* a picture is coming in                        */
    char     mode[12];        /* "MARTIN 1"; the last picture's when idle      */
    int      w, h, rows;
    float    slant_ppm, tone_hz, level;
    int      syncs_found, syncs_missed;
    uint32_t images;
    bool     saving;
    char     saved[48];       /* the last picture's file, "" before any        */
} fm_sstv_status_t;

void fm_sstv_publish(const fm_sstv_status_t *s);
void fm_sstv_status(fm_sstv_status_t *out);
/* The picture as RGB565, w x h; rows past status.rows are not in yet. The
   receive task owns the memory and swaps it only between pictures. */
void fm_sstv_set_image(const uint16_t *pix, int w, int h);
const uint16_t *fm_sstv_image(int *w, int *h);

/* ---- NOAA weather ------------------------------------------------------------ */

typedef struct {
    uint32_t pubs;
    int      channel;         /* WX channel index (0 is WX1), -1 off-plan      */
    int      audio;           /* FM_WX_AUDIO_*                                 */
    bool     alert_open;      /* a SAME alert has the speaker                  */
    bool     finding, found_none;
    uint8_t  level[7];        /* dB over the floor from the last FIND          */
} fm_wx_status_t;

void fm_wx_publish(const fm_wx_status_t *s);
void fm_wx_status(fm_wx_status_t *out);

/* Wall clock offered by RDS CT: true once two consecutive CT groups agree a
   minute apart. The receive task applies it when no better source has. */
typedef struct {
    int64_t  last_ct;
    int64_t  last_uptime_us;
    int      agree;
} fm_ct_trust_t;
bool fm_ct_trust(fm_ct_trust_t *t, int64_t ct_unix, int64_t uptime_us);

#ifdef __cplusplus
}
#endif

#endif
