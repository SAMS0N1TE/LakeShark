/* Private to ls_hub.c / ls_report.c / ls_locker.c / ls_chat.c. */
#pragma once
#include "ls_hub.h"

typedef enum { HOP_NONE = 0, HOP_REPORT, HOP_PAIR, HOP_LIST, HOP_UP, HOP_PULL, HOP_UNPAIR, HOP_CHAT_SEND, HOP_CHAT_POLL, HOP_DEL, HOP_GET } hub_op_t;

typedef struct {
    const char *method, *path;
    const char *note;               /* X-Note */
    const char *ctype;              /* request Content-Type */
    const void *body;               /* memory body, or */
    uint32_t    blen;               /* its length (or the file's size with upfile) */
    const char *upfile;
    char       *out;                /* response body to memory (NUL-terminated), or */
    uint32_t    ocap;
    const char *outfile;            /* response body to a file, via .part */
} hub_req_t;

/* On the hub task only. HTTP status, or -1 no network, -2 unreachable, -3 card or memory. */
int  hub_call(const hub_req_t *q, uint32_t *got);
bool hub_json_str(const char *j, const char *key, char *out, size_t n);
void hub_set(ls_hub_state_t st, const char *what, const char *fmt, ...) __attribute__((format(printf, 3, 4)));
void hub_progress(uint32_t done, uint32_t total);
bool hub_submit(hub_op_t op, const char *a, bool flag);
bool hub_online(void);
/* True on ids and offers: the saved report sequence (see ls_report_offer_pending). */
uint32_t hub_rep_seq(void);
void     hub_rep_seq_set(uint32_t v);

void report_run(const char *note, bool gps);
void locker_run(hub_op_t op, const char *arg);
void chat_run(hub_op_t op, const char *arg);
/* From the hub task: how long to wait for work, ms; 0 = forever. */
uint32_t chat_poll_wait_ms(void);
void report_console(int argc, char **argv);
void locker_console(int argc, char **argv);
void chat_console(int argc, char **argv);
bool hub_wait(uint32_t timeout_ms);   /* console: block until the queued job ends */
void ls_hub_wake(void);            /* chat: re-time the poll */
