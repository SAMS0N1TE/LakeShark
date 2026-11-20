/* STORE: the app catalog on terminalbay.com, and signed SD apps installed,
   updated and removed from it (ls_store.c; the screen is scr_store.c).

   Catalogs: https://terminalbay.com/apps/catalog.json (LakeShark's own) and
   https://apps.terminalbay.com/catalog.json (community), the format
   app-sdk/catalog.py writes. A file is "<host>/<file>". The first catalog
   wins an id both list. Each fetch is kept on the card, so the list still
   shows with no network.

   Installs go through ls_fetch (size + SHA-256 from the catalog) and then the
   lap loader's own checks (signature by a store key, manifest, preflight)
   before the file replaces anything: ls_lap_install_file(). Unsigned files
   are refused. All jobs run on one worker task (PSRAM stack). */
#ifndef LS_STORE_H
#define LS_STORE_H
#include <stdbool.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

/* Where apps are installed. A sibling that adds an internal app area
   (/spiffs/apps) sets this per build, or turns it into a choice per app. */
#ifndef LS_STORE_DIR
#define LS_STORE_DIR "/sdcard/apps"
#endif

#define LS_STORE_MAX 64

typedef enum {
    LS_ST_NEW = 0,      /* not installed */
    LS_ST_INSTALLED,    /* this version (or newer) is on the card */
    LS_ST_UPDATE,       /* the catalog has a newer one */
    LS_ST_NEEDS_FW,     /* min_firmware is newer than this firmware */
    LS_ST_BUILTIN,      /* the id is a built-in app's */
} ls_store_state_t;

typedef struct {
    char id[32], name[16], sub[32], ver[16], author[24];
    char minfw[16], cat[12], license[16], publisher[24];
    char desc[200];
    char credits[112];      /* third_party: "name (licence); ..." */
    char file[80];
    char art[6][11];        /* the app's own tile picture */
    uint8_t sha[32];
    uint32_t size;
    uint8_t group;          /* LS_APP_GROUP_*: declared, else by caps/category */
    uint8_t caps;           /* LAP_CAP_* bits; 0xFF when the catalog does not say */
    uint8_t src;            /* 0 LakeShark, 1 community */
    uint8_t color, wip;
} ls_store_app_t;

typedef enum { LS_SJ_IDLE = 0, LS_SJ_FETCH, LS_SJ_DOWNLOAD, LS_SJ_VERIFY, LS_SJ_REMOVE } ls_store_phase_t;

typedef struct {
    int n;                  /* apps in the table */
    int updates;            /* of them, LS_ST_UPDATE */
    bool loaded;            /* a catalog (live or kept) is in the table */
    bool live;              /* ...and it came over the network this boot */
    bool online;            /* the network is up now */
    bool busy;              /* the worker has a job */
    ls_store_phase_t phase;
    char job_id[32];        /* the app being worked on */
    uint32_t got, total;    /* download bytes, for the bar */
    int queued;             /* jobs waiting behind it */
    bool msg_bad;
    char msg[96];           /* the last result, in words */
} ls_store_status_t;

/* Any task. The table is copied in under a lock; call ls_store_lock()
   around reads that must not see a refresh land mid-way. */
void ls_store_lock(void);
void ls_store_unlock(void);
int  ls_store_count(void);
const ls_store_app_t *ls_store_at(int i);
int  ls_store_find(const char *id);
void ls_store_status(ls_store_status_t *out);

/* State of entry i, and what is on the card (ver "" when not installed). */
ls_store_state_t ls_store_state(int i, char *have, int cap);
/* This firmware's version, as min_firmware is compared against it. */
const char *ls_store_fw(void);
/* <0, 0, >0 like strcmp, numbers dot by dot ("2.10" > "2.9"). */
int  ls_store_ver_cmp(const char *a, const char *b);
/* "NET RX TX BG MIC HID", or "not listed" when caps is unknown. */
void ls_store_caps_text(uint8_t caps, char *out, int cap);

/* Queue work; false when the queue is full or the app is unknown. */
bool ls_store_refresh(void);
bool ls_store_install(const char *id);  /* install or update */
bool ls_store_remove(const char *id);
int  ls_store_update_all(void);         /* how many were queued */
/* The first call loads the kept catalog and asks for a fresh one. */
void ls_store_open(void);
/* STORE on screen at the next frame, at that app's card once the catalog
   has it (the whole list when it does not). Any task; the "store.open"
   action, so an SD app can send people to another app. False when this
   build has no STORE screen. (scr_store.c) */
bool ls_store_focus(const char *id);

void ls_store_console_register(void);

#ifndef ESP_PLATFORM
/* Bench: show a job under way, to look at the screen. */
void ls_store_sim_job(const char *id, uint32_t got, uint32_t total);
#endif

#ifdef __cplusplus
}
#endif
#endif
