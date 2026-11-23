/* WARDRIVE REPLAY: a saved drive played back through the session, so LIVE,
   3D, LIST and MAP show it the way they showed it on the road.

   The drive is read back from what STOP wrote in /sdcard/wardrive: the GPX
   track gives the route and its clock, the WiGLE CSV gives every network
   where it was heard, the GPX waypoints the cells and drones. Its events are
   fed to the session (wd_core.h) in order as a clock of its own runs, at
   1x to 64x, paused, or jumped to a point (a jump back starts the session
   again and feeds up to there at once). UI task only. */
#ifndef WD_REPLAY_H
#define WD_REPLAY_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define WD_REPLAY_NAME 28
#define WD_REPLAY_LIST 24

typedef struct {
    bool on, paused, done;
    bool loading, full;         /* still reading the files; the drive was longer than one replay holds */
    int read_pct;
    int speed;                  /* 1, 4, 16 or 64 */
    uint32_t pos_s, len_s;      /* where the playback is, and how long the drive was */
    unsigned points, rows, marks;
    char name[WD_REPLAY_NAME];
} wd_replay_info_t;

/* The drives on the card, newest first: stem names and a line about each. */
int  wd_replay_list(char name[][WD_REPLAY_NAME], char detail[][40], int max);
/* Reads the drive `stem` and starts playing it; false with a reason. */
bool wd_replay_open(const char *stem, uint32_t real_ms, char *why, size_t n);
/* Moves the playback clock and feeds what it reached. Once a frame. */
void wd_replay_tick(uint32_t real_ms);
/* The session's clock while a replay is on (the screen's "now"). */
uint32_t wd_replay_now(uint32_t real_ms);
bool wd_replay_on(void);            /* reading or playing */
/* Why the last open stopped while reading ("" none). */
const char *wd_replay_error(void);
void wd_replay_close(void);
void wd_replay_info(wd_replay_info_t *out);
void wd_replay_pause(uint32_t real_ms);
void wd_replay_speed(int dir, uint32_t real_ms);
/* To a fraction of the drive (0 start, 1 end). */
void wd_replay_seek(float frac, uint32_t real_ms);

/* Every place the drive's log heard one network, up to the playback's
   time: its WiGLE rows (and a cell's or drone's mark). How many. */
int wd_replay_sightings(const uint8_t addr[6], uint8_t kind,
                        void (*cb)(double lat, double lon, int rssi, void *ctx), void *ctx);

/* The whole drive's route, start to end, whatever the playback has reached
   (each point with its seconds from the start). How many points. */
int wd_replay_track(void (*cb)(double lat, double lon, uint32_t s, void *ctx), void *ctx);

/* Text parsing, for the bench. */
long wd_replay_utc(const char *s);           /* "YYYY-MM-DD HH:MM:SS" or ISO with T/Z; -1 bad */
int  wd_replay_csv_fields(char *line, char *field[], int max);
unsigned wd_replay_auth(const char *caps);

#ifdef __cplusplus
}
#endif
#endif
