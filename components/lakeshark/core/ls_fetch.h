/* ls_fetch: the firmware's one downloader (main/ls_fetch.c).

   HTTPS only, with the certificate bundle. A file lands as <dest>.part and is
   renamed over <dest> only after its size, SHA-256 and signature (whichever
   were asked for) pass, so a reader never sees half a file.

     blocking, on your own task (PSRAM stack of 12 KB or more, never the UI):
         ls_fetch_req_t q = { .url = "https://data.terminalbay.com/v1/x.txt",
                              .dest = "/sdcard/lakeshark/x.txt",
                              .bytes = 1234, .sha256 = digest };
         ls_fetch_result_t r = ls_fetch_run(&q, NULL, NULL);

     queued, from anywhere (UI included; the strings are copied):
         int id = ls_fetch_start(&q);          0: queue full
         ls_fetch_info(id, &info);             state, got/total for a bar
         q.done(id, result, ctx) is called on the fetch task when it ends.

   Callbacks run on the fetch task (PSRAM stack): no flash or NVS writes in
   them except through ls_nvs_run / the settings setters.

   Hosts: an update-host URL (ota.terminalbay.com, lakeshark-ota.pages.dev)
   falls back to the other host and then GitHub by itself; any other URL
   tries .fallback when it is given. */
#pragma once
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    LS_FETCH_OK = 0,
    LS_FETCH_QUEUED,        /* info only */
    LS_FETCH_RUNNING,       /* info only */
    LS_FETCH_NO_NET,        /* no Wi-Fi or Ethernet address */
    LS_FETCH_UNREACHABLE,   /* no host answered, or a redirect left https */
    LS_FETCH_HTTP,          /* answered, not 200/206 (see http status) */
    LS_FETCH_SIZE,          /* not the stated size, or over the cap */
    LS_FETCH_HASH,
    LS_FETCH_SIG,
    LS_FETCH_SD,            /* cannot create, write or rename */
    LS_FETCH_TIMEOUT,       /* the job's deadline, or 30 s without a byte */
    LS_FETCH_CANCELLED,
    LS_FETCH_BUSY,          /* a background job that never got a quiet moment */
} ls_fetch_result_t;

/* The body ends with a line sig=<hex DER>: ECDSA P-256 over the SHA-256 of
   every byte before that line, by the update key (as OTA manifests are). */
#define LS_FETCH_SIGNED     1u
/* Keep <dest>.part when the link drops and continue it next time with a
   Range request. Only honoured with .sha256, which is what proves the join. */
#define LS_FETCH_RESUME     2u
/* Queued jobs only: wait (up to timeout_s) for the network to be up. */
#define LS_FETCH_WAIT_NET   4u
/* Queued jobs only: wait while an RF app is live or internal RAM is low, as
   the background update check does. For traffic nobody asked for by hand. */
#define LS_FETCH_BACKGROUND 8u

typedef void (*ls_fetch_progress_fn)(uint32_t got, uint32_t total, void *ctx);
typedef void (*ls_fetch_done_fn)(int id, ls_fetch_result_t result, void *ctx);

typedef struct {
    const char *url;        /* https://... */
    const char *fallback;   /* optional second URL for the same bytes */
    const char *dest;       /* file under /sdcard; folders are made. NULL: buf */
    char       *buf;        /* memory destination, NUL-terminated on success */
    uint32_t    cap;        /* of buf, the NUL included */
    uint32_t    bytes;      /* expected size, 0 when unknown */
    uint32_t    max_bytes;  /* refuse more; 0: LS_FETCH_MAX_DEFAULT */
    const uint8_t *sha256;  /* expected digest (32 bytes), NULL for none */
    const char *token;      /* X-LakeShark-Token; ls_fetch_run only */
    uint16_t    timeout_s;  /* the whole transfer; 0: 120 */
    uint8_t     flags;      /* LS_FETCH_* */
    ls_fetch_progress_fn progress;
    ls_fetch_done_fn     done;      /* queued jobs */
    void       *ctx;
} ls_fetch_req_t;

#define LS_FETCH_MAX_DEFAULT (8u * 1024u * 1024u)

typedef struct {
    int      id;
    ls_fetch_result_t state;    /* QUEUED, RUNNING, or how it ended */
    uint32_t got, total;        /* total 0 while unknown */
    int      http;              /* last HTTP status, <0 transport */
    char     name[32];          /* the destination's file name */
} ls_fetch_info_t;

/* A station or Ethernet address is up right now. Cheap, any task. */
bool ls_fetch_online(void);

/* Do it now on the calling task. *got is the body's length; *http the last
   status (negative: -1 unreachable, -2 left https, -3 deadline). */
ls_fetch_result_t ls_fetch_run(const ls_fetch_req_t *req, uint32_t *got, int *http);

/* Queue it for the fetch task. Returns a job id (> 0), or 0 when the queue
   (LS_FETCH_JOBS) is full. .buf, .sha256 contents and strings are copied
   except .buf itself, which must stay valid until done. */
int  ls_fetch_start(const ls_fetch_req_t *req);
bool ls_fetch_info(int id, ls_fetch_info_t *out);
/* The job on the wire now (false: none); *queued is how many wait behind it. */
bool ls_fetch_active(ls_fetch_info_t *out, unsigned *queued);
/* One job, or every job with id 0. A running one stops within a read. */
void ls_fetch_cancel(int id);

/* True when the file ends in a good sig= line (LS_FETCH_SIGNED's check, for
   a file already on the card). *signed_len: the bytes before that line. */
bool ls_fetch_file_signed(const char *path, uint32_t *signed_len);

/* Puts X.bak back as X when a power cut between the two renames of a
   publish left only the .bak. Run before reading a fetched file (the signed
   check above and every fetch of X do it). False: no X and no X.bak. */
bool ls_fetch_recover(const char *dest);

const char *ls_fetch_result_text(ls_fetch_result_t r);
/* `fetch` console verb: list jobs, queue a URL, cancel. */
void ls_fetch_console_register(void);

#define LS_FETCH_JOBS 6

#ifdef __cplusplus
}
#endif
