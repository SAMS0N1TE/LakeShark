/* ls_hub: the board's side of hub.terminalbay.com (main/ls_hub.c, ls_report.c, ls_locker.c, ls_chat.c).
   Bug reports, the file locker and anonymous chat. Plain C, no screens: an app calls these and draws
   the result. Everything slow runs on one hub task (PSRAM stack); the calls below only queue work and
   return, so the UI never waits on the network. Results come back through ls_hub_status() and, for
   chat, the message ring.

   Identity: a random 8-byte device id and 16-byte secret made on first use and kept in NVS ("lshub").
   There are no accounts. The server derives the chat handle (adjective-animal-4hex) from the id.

   REPORT   ls_report_send(note, with_gps)   redacted bundle (version, board, uptime, heap, reset
            ls_report_preview(buf,cap)       report, crash notes, error log, boot log tail) up to the hub.
            ls_report_offer_pending()        true after a crash that has not been offered yet;
            ls_report_offer_done()           call when the user answered (sent or not now).
   LOCKER   ls_locker_pair()                 asks for a 6-digit code (ls_locker_code), good 10 minutes
            ls_locker_list()                 refresh ls_locker_file(i)
            ls_locker_upload(path)           a file under /sdcard, <= 25 MB
            ls_locker_pull()                 every file the website/desktop put there -> /sdcard/inbox
            ls_locker_unpair()               revoke the website and desktop sessions
            ls_locker_get(name)/delete(name) one file -> /sdcard/inbox, or gone from the locker
   CHAT     ls_chat_join(chan)               "global", "us", "z6-17-26"; ls_chat_join_here() picks the
            ls_chat_send(text)               country and area from the position; ring of the newest 48.
            ls_chat_focus(true|false)        true while a chat screen is open: polls every 3 s, else 60 s.
            ls_chat_watch(slot, chan)        while focused, the newest id of up to 3 other channels (unread).
   Console: `report`, `locker`, `chat` (each prints its usage). */
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define LS_HUB_URL "https://hub.terminalbay.com"

typedef enum { LS_HUB_IDLE = 0, LS_HUB_BUSY, LS_HUB_OK, LS_HUB_FAIL } ls_hub_state_t;

typedef struct {
    ls_hub_state_t state;
    char     what[12];      /* "report", "pair", "list", "upload", "pull", "unpair", "chat" */
    char     text[72];      /* "sent r/0mw..", "no Wi-Fi", "locker full (200 MB)" ... */
    uint32_t done, total;   /* bytes, while a transfer runs */
    uint32_t seq;           /* goes up when state changes */
} ls_hub_status_t;

void ls_hub_status(ls_hub_status_t *out);
bool ls_hub_online(void);   /* Wi-Fi up with an address */
/* Device id as 16 hex characters (for a "your id" line, never the secret). */
bool ls_hub_device_id(char out[17]);
void ls_hub_console_register(void);

/* ---- report ---- */
bool   ls_report_send(const char *note, bool with_gps);
size_t ls_report_preview(char *out, size_t cap, bool with_gps);   /* the bundle as sent (boot log cut short) */
bool   ls_report_offer_pending(void);
void   ls_report_offer_done(void);

/* ---- locker ---- */
#define LS_LOCKER_LIST_MAX 24
typedef struct {
    char     name[64];
    uint32_t size;
    uint32_t age_s;
    bool     from_web;      /* put there by the website or the desktop */
} ls_locker_file_t;

bool ls_locker_pair(void);
bool ls_locker_code(char out[8], uint32_t *left_s);   /* false when none is live */
bool ls_locker_list(void);
int  ls_locker_count(void);
bool ls_locker_file(int i, ls_locker_file_t *out);
bool ls_locker_upload(const char *sd_path);
bool ls_locker_pull(void);
bool ls_locker_unpair(void);
bool ls_locker_delete(const char *name);
bool ls_locker_get(const char *name);     /* one file to /sdcard/inbox */
int  ls_locker_ready(void);               /* 1 locker made, 0 never paired (the hub said 401), -1 not asked yet */

/* ---- chat ---- */
#define LS_CHAT_RING 48
#define LS_CHAT_TEXT 281
typedef struct {
    uint32_t id, ts;
    char     handle[24];
    char     text[LS_CHAT_TEXT];
} ls_chat_msg_t;

bool ls_chat_join(const char *chan);
bool ls_chat_join_here(bool area);     /* country channel, or the z6 area when area */
void ls_chat_leave(void);
bool ls_chat_send(const char *text);   /* queued; the server filters links and bad words */
void ls_chat_focus(bool on);
const char *ls_chat_channel(void);     /* "" when not joined */
const char *ls_chat_handle(void);      /* your anonymous handle once the server told us */
uint32_t ls_chat_seq(void);            /* goes up when the ring changes */
int  ls_chat_count(void);
bool ls_chat_get(int i, ls_chat_msg_t *out);   /* 0 = oldest held; copy, retry-safe */
/* Unread counts for channels other than the open one: while focused the hub task asks each watched
   channel for its newest id every 12 s (ids count up by one a message). "" stops watching a slot. */
#define LS_CHAT_WATCH 3
void ls_chat_watch(int slot, const char *chan);
uint32_t ls_chat_last_id(int slot);    /* 0 until asked */
/* LISTEN: after the chat screen closes keep polling the room once a minute (true), or leave it (false).
   Saved; off until the user turns it on. Nothing joins a room at boot. */
bool ls_chat_background(void);
void ls_chat_set_background(bool on);
/* The country ("us", "" when unknown) and area ("z6-17-26") channel names for the position; false without one. */
bool ls_chat_here_names(char cc[4], char area[12]);
/* "z6-<x>-<y>" for a position (the tile at zoom 6). */
void ls_chat_area_name(double lat, double lon, char out[12]);

#ifdef __cplusplus
}
#endif
