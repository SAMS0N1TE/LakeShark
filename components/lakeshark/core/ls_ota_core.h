/* What an update server says, and what the board makes of it. Text and
   numbers only: the network, the signature check and the flash are in
   main/ls_ota.c, so all of this runs on the bench.

   A manifest is lines of key=value under a magic first line, and ends with
   the signature over every byte before it:

       LAKESHARK-OTA 1
       board=T-Display-P4
       channel=stable
       version=2.9.0
       build=2.9.0-g0123456789ab
       size=3538752
       sha256=<64 hex>
       url=2.9.0/lakeshark.bin
       notes=one line for the screen
       sig=<hex DER ECDSA P-256 over SHA-256>

   channel= is the name of the manifest file without ".txt" (.../tdp4/stable.txt
   is "stable"). It sits before sig= like every other field, so the signature
   covers it, and a board refuses a manifest whose channel is not its own.

   Unknown keys are allowed and covered by the signature, so a later field
   does not break an older board. */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Where an update is, as the ota.stage value publishes it for the screen. */
typedef enum {
    LS_OTA_ST_IDLE = 0,     /* nothing asked yet this boot */
    LS_OTA_ST_WIFI,         /* joining the saved network */
    LS_OTA_ST_CHECKING,     /* reading the manifest */
    LS_OTA_ST_CURRENT,      /* nothing newer */
    LS_OTA_ST_AVAILABLE,    /* a newer build, waiting for ENTER */
    LS_OTA_ST_DOWNLOADING,
    LS_OTA_ST_VERIFYING,
    LS_OTA_ST_RESTARTING,
    LS_OTA_ST_FAILED,
    LS_OTA_ST_UPDATED,      /* this boot is a new build, on trial then kept */
} ls_ota_stage_t;

#define LS_OTA_MAGIC    "LAKESHARK-OTA 1"
#define LS_OTA_MAX_TEXT 4096
#define LS_OTA_MAX_SIZE (16u * 1024u * 1024u)

typedef struct {
    char     board[24];
    char     version[24];
    char     build[48];
    uint32_t size;
    uint8_t  sha256[32];
    char     url[192];      /* https, resolved against the manifest's own URL */
    char     notes[160];
    char     channel[24];   /* empty when the manifest has none */
    size_t   signed_len;    /* the bytes the signature covers */
    uint8_t  sig[80];       /* DER */
    size_t   sig_len;
} ls_ota_manifest_t;

typedef enum {
    LS_OTA_OK = 0,
    LS_OTA_ERR_MAGIC,       /* not an update manifest */
    LS_OTA_ERR_LINE,        /* a line that is not key=value, or too long */
    LS_OTA_ERR_MISSING,     /* a required key is absent */
    LS_OTA_ERR_VALUE,       /* a value is malformed, too long or repeated */
    LS_OTA_ERR_URL,         /* the image is not on https */
    LS_OTA_ERR_SIG,         /* no signature, or text after it */
} ls_ota_err_t;

ls_ota_err_t ls_ota_parse(const char *text, size_t len, const char *manifest_url,
                          ls_ota_manifest_t *out);
const char  *ls_ota_err_text(ls_ota_err_t e);

/* The channel a manifest URL asks for: its file name less any query and
   ".txt". False when there is none. */
bool ls_ota_channel_of(const char *manifest_url, char *out, size_t cap);

/* True when the manifest names exactly the channel its URL asks for. A
   manifest with no channel, or another one, is not the board's to install. */
bool ls_ota_channel_ok(const ls_ota_manifest_t *m, const char *manifest_url);

/* Release order of two version strings: "2.8.3", "2.8.3-g6fb3ac015e1a",
   "2.9.0-rc2-g0123-dirty". Only X.Y.Z and an -rcN count, and an rc sorts
   below its release. Negative, zero or positive, like strcmp. */
int ls_ota_version_cmp(const char *a, const char *b);

typedef enum {
    LS_OTA_CURRENT,         /* this exact build */
    LS_OTA_NEWER,           /* a later release: offered */
    LS_OTA_OLDER,           /* an earlier release: never offered */
    LS_OTA_OTHER_BUILD,     /* the same release, another build */
} ls_ota_verdict_t;

ls_ota_verdict_t ls_ota_judge(const char *running_build, const ls_ota_manifest_t *m);

/* A signature is good when any trusted key accepts it. `verify` asks one key
   by index; the check is here so the bench can run it. No keys, no match. */
typedef bool (*ls_ota_key_fn)(size_t key, void *ctx);
bool ls_ota_verify_any(size_t nkeys, ls_ota_key_fn verify, void *ctx);

/* `ref` against `base`: absolute, rooted at base's host, or beside it. */
bool ls_ota_resolve(const char *base, const char *ref, char *out, size_t cap);

/* The two hosts that serve the update project, each a fallback for the other. */
#define LS_OTA_HOST_PRIMARY  "ota.terminalbay.com"
#define LS_OTA_HOST_FALLBACK "lakeshark-ota.pages.dev"

/* `url` with its host swapped for the other known one, path and query kept.
   False, with `out` untouched, for a host that is not one of the two, a URL
   that is not https, or an `out` too small. */
bool ls_ota_other_host(const char *url, char *out, size_t cap);

/* ------------------------------------------------- the background check

   One quiet look at the manifest, so the board can say an update exists. It
   never installs and never downloads the image. All of the timing is here,
   in uptime seconds, so the bench can run it; main/ls_ota.c supplies the
   clock and what is busy.

   The first check comes 2 to 5 minutes after WiFi first connects (the seed
   spreads boards out), then one a day. A check that is due while the board is
   busy is put off and asked again in ten minutes. The setting turns it all
   off: no timer, no traffic. */

#define LS_OTA_BG_FIRST_MIN_S   120
#define LS_OTA_BG_FIRST_SPAN_S  180             /* so 120 to 300 inclusive */
#define LS_OTA_BG_EVERY_S       (24 * 3600)
#define LS_OTA_BG_RETRY_S       600

/* Below either of these a background check is put off: a TLS session is
   internal RAM, and on this board the DMA pool is what the radios' SPI
   transfers, AES and SD writes live in. Measured on the LR2021 board at idle
   (c06f861a, after a successful manual check): internal free 24779 bytes
   (minimum ever 15875), DMA free 9667. */
#define LS_OTA_BG_MIN_INTERNAL  (18u * 1024u)
#define LS_OTA_BG_MIN_DMA       (4u * 1024u)

typedef struct {
    bool     enabled;       /* Settings > DEVICE > Check for updates: Daily */
    int64_t  wifi_up_s;     /* uptime WiFi first connected this boot, < 0 never */
    int64_t  last_run_s;    /* uptime the last check began, < 0 none this boot */
    int64_t  last_defer_s;  /* uptime a due check was last put off, < 0 none */
    uint32_t seed;          /* random per boot: the first check's jitter */
} ls_ota_bg_t;

/* Why a due check cannot run now. */
#define LS_OTA_BG_BUSY_JOB   1u     /* a check or an install is under way */
#define LS_OTA_BG_BUSY_RF    2u     /* an RF app is streaming or receiving */
#define LS_OTA_BG_LOW_HEAP   4u     /* internal or DMA heap under the threshold */
#define LS_OTA_BG_NO_WIFI    8u     /* the network is not up right now */

typedef enum {
    LS_OTA_BG_WAIT,         /* not due, or switched off */
    LS_OTA_BG_RUN,          /* due and clear: start it */
    LS_OTA_BG_DEFER,        /* due but something is busy: note it, retry later */
} ls_ota_bg_act_t;

/* Seconds after WiFi connects for the first check, 120 to 300, from `seed`. */
int64_t ls_ota_bg_jitter_s(uint32_t seed);

/* The uptime second the next background check is due, or -1 when none will
   be: switched off, or WiFi has not connected yet this boot. */
int64_t ls_ota_bg_next_s(const ls_ota_bg_t *s);

/* Which of the LS_OTA_BG_* reasons stop a check right now. 0 means none. */
unsigned ls_ota_bg_blockers(bool job_busy, bool rf_busy, bool wifi_up,
                            size_t internal_free, size_t dma_free);

/* What to do at `now_s`. */
ls_ota_bg_act_t ls_ota_bg_act(const ls_ota_bg_t *s, int64_t now_s, unsigned blockers);

/* ------------------------------------------------------------ the notice

   A newer verified build is known: HOME shows a dot, and once per build a
   footer line says where to find it. The last build announced is kept in NVS
   so a reboot does not say it again. */

/* True when `newer` (the build a check found, "" for none) has not been
   announced yet. */
bool ls_ota_notice_new(const char *newer, const char *announced);

/* "2.9.0 available: SYSTEM > UPDATE". False when `version` is empty or the
   buffer is too small. */
bool ls_ota_toast_text(const char *version, char *out, size_t cap);

#ifdef __cplusplus
}
#endif
