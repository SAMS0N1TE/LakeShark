/* The LSX link as a service on this board: one engine (ls_lsx.h) on the
   LoRa chip, a task that steps it, the HOME BASE sync on top (ls_lsx_sync.h),
   settings in NVS, and the 'lsx' console command.

   The radio is borrowed the way an app's background service borrows it
   (ls_mesh_radio_hold_svc): below every radio app, the MESH screen and a
   MeshCore send, which take it back at once; MeshCore restores its own
   settings when it has the chip again. A field unit holds it only for a
   probe (about 0.4 s every 8 s) and for a sync; the home base holds it
   while nothing else wants it.

   Nothing starts at boot unless 'lsx auto home|field' was set. */
#ifndef LS_LSX_SVC_H
#define LS_LSX_SVC_H

#include "ls_lsx.h"
#include "ls_lsx_sync.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Start as LSX_ROLE_HOME or LSX_ROLE_FIELD (stopping any other role).
   false with ls_lsx_svc_why() saying why: no key, no LoRa chip, no memory. */
bool ls_lsx_svc_start(int role);
void ls_lsx_svc_stop(void);
int  ls_lsx_svc_role(void);          /* LSX_ROLE_OFF when stopped */
const char *ls_lsx_svc_why(void);
/* A copy of the link's and the sync's state; either may be NULL. */
void ls_lsx_svc_status(ls_lsx_status_t *link, ls_lsx_sync_status_t *sync);
int  ls_lsx_svc_units(ls_lsx_unit_t *out, int max);
/* Field: probe now and ask for a sync even if the home is up to date. */
void ls_lsx_svc_sync_now(void);
bool ls_lsx_svc_has_key(void);
/* This board's link id and name. */
uint32_t ls_lsx_svc_id(void);
const char *ls_lsx_svc_name(void);

/* PARTY: the engine as LSX_ROLE_PARTY with the party's own key (not saved),
   holding the radio on the discovery channel like a home base, answering no
   probes, sending and hearing broadcast datagrams. Each datagram heard goes
   to `rx` on the link's task (keep it short, no NVS) with the frame's RSSI
   and SNR (127: none). Stops any other role; ls_lsx_svc_stop() ends it.
   false with ls_lsx_svc_why() saying why. */
typedef void (*ls_lsx_party_rx_t)(void *ctx, uint32_t src, uint8_t port, const uint8_t *data, int len,
                                  int8_t rssi, int8_t snr);
bool ls_lsx_svc_party(const uint8_t key[16], ls_lsx_party_rx_t rx, void *ctx);
/* A broadcast datagram while the PARTY role runs: 0 queued, -1 not (not
   running, queue full, longer than LSX_BODY_MAX - 1). */
int  ls_lsx_svc_dgram(uint8_t port, const uint8_t *data, int len);
/* LSX_CAP_* of this board's LoRa chip, 0 when there is none. */
uint32_t ls_lsx_svc_caps(void);

/* Where the atlas records come from and go. WARDRIVE registers its store
   when its module loads (a T-Display); NULL unregisters. The desk build
   binds wd_atlas itself (ls_lsx_default_store). The store's calls run on
   the link's task with no wardrive lock held. */
void ls_lsx_set_store(const ls_lsx_store_t *store);
/* Weak NULL here; the desk build's ls_desk_home.c returns wd_atlas. */
const ls_lsx_store_t *ls_lsx_default_store(void);

/* A field unit's HOME line, for WARDRIVE's screen. */
typedef struct {
    bool on;                /* the field role is running           */
    bool in_range;          /* the home was heard in the last 2 min */
    bool syncing;           /* a session is open                   */
    int  progress_pct;      /* -1 when not syncing                 */
    uint32_t last_sync_ago_s;   /* UINT32_MAX never                */
    char home[LSX_NAME];
    int8_t rssi;
    const char *mode;       /* "FLRC975", "GFSK200", "LORA"...     */
    uint32_t recs_up, recs_down;    /* the last or current sync    */
} ls_lsx_home_status_t;
void ls_lsx_home_status(ls_lsx_home_status_t *out);

/* Registers 'lsx'; starts the saved role if 'lsx auto' set one. */
void ls_lsx_console_register(void);
/* The command itself, for a console that dispatches by hand. */
int  ls_lsx_command(int argc, char **argv);

#ifdef __cplusplus
}
#endif
#endif
