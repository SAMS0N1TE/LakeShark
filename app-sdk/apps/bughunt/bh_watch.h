/* BUGHUNT's tag watch: what was the BLE WATCH app. Every Bluetooth advert
   the app hears goes into one table (bh_tags.c) that lasts as long as the
   app is open, through sweeps and hunts: it names the tracker tags, follows
   their rotating addresses, raises the "a tag is following you" alert, keeps
   the card log, and draws the TAGS tab (list, the pick's detail, INFO).
   main.c owns the Bluetooth sources, the tabs, the key bar and HUNT.
   Receive only. */
#ifndef BH_WATCH_H
#define BH_WATCH_H
#include "lakeshark_app.h"
#include "bh_tags.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum { BHW_TAGS, BHW_ALL, BHW_INFO, BHW_VIEWS };
#define BHW_LOG_DIR "/sdcard/bughunt"
#define BHW_OLD_DIR "/sdcard/ble_watch"     /* where BLE WATCH kept its logs */

bool bhw_open(void);                    /* false: no memory */
void bhw_close(void);                   /* writes what the log still holds */
/* Settings: the alert after this long present, possible trackers alert too,
   the card log. bhw_config is applied at once (the log opens or closes). */
void bhw_config(uint32_t follow_ms, bool possible, bool log);
void bhw_get_config(uint32_t *follow_ms, bool *possible, bool *log);
/* The Bluetooth source, for the header and INFO: 1 advert capture, 2 scan,
   0 none (why says so); ended: a replayed capture ran out. */
void bhw_source(int kind, const char *why, bool ended);
/* One advert. ts_us is the radio's time of it (0: none, now): replayed
   captures run the watch's clock forward with it. */
void bhw_advert(const uint8_t addr[6], uint8_t type, uint8_t pdu, int rssi, const uint8_t *ad, size_t n, uint64_t ts_us);
/* Once a frame: GPS, rotations, the alert, the log, the list. Returns the
   index of a tag whose following alert fired just now, or -1. */
int bhw_poll(void);
void bhw_alert_text(int i, char *out, size_t cap);
bool bhw_note(char *out, size_t cap);   /* a message for the title line, once */
void bhw_clear(void);
uint32_t bhw_now(void);                 /* the watch's clock (ms) */

/* Reading the table. */
bw_t *bhw_table(void);
bw_dev_t *bhw_dev(uint32_t uid);        /* follows rotations to the newest address */
uint32_t bhw_uid_of(const uint8_t addr[6], uint8_t type);   /* 0 not in the table */
int bhw_counts(int *trackers, int *alerts, int *all);
uint8_t bhw_colour(const bw_dev_t *d);
const char *bhw_distance_word(float q, bool heard);

/* The TAGS tab. */
int bhw_view(void);
void bhw_view_set(int v);
const char *bhw_view_name(int v);
uint32_t bhw_pick(void);                /* 0 none */
void bhw_pick_set(uint32_t uid);
uint32_t bhw_step(uint32_t uid, int by);    /* the tag before/after in the list */
bool bhw_follow(bool listing);          /* the finger on the list; true while it drags */
void bhw_draw(int x, int y, int w, int h, bool wide);
void bhw_draw_head(int x, int y, int w, int h);     /* radar and the tally */
void bhw_draw_detail(int x, int y, int w, int h);   /* the pick */
/* -100 to -30 dBm over the last `cells`*2 buckets, `rows` high. */
void bhw_graph(int x, int y, int cells, int rows, const bw_dev_t *d);
/* 0 not taken, 1 taken, 2 HUNT the pick */
int bhw_key(lap_key_t k, char ch);
int bhw_touch(int col, int row);
void bhw_follow_step(int by);           /* the alert's minutes, a step up or down */
void bhw_possible_toggle(void);
void bhw_log_toggle(void);
bool bhw_log_on(void);
#endif
