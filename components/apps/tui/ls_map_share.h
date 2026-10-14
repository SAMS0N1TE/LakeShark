/* Public, readable MeshCore map exchange. UI-task owned; no automatic TX. */
#ifndef LS_MAP_SHARE_H
#define LS_MAP_SHARE_H
#include "ls_map_marks.h"
/* Examples (ASCII, coordinates always at five decimals):
   WPT Camp [LS1 1234abcd 1/1] 43.12345,-71.54321
   RTE Trail [LS1 1234abcd 1/3] 43.12345,-71.54321;...
   MeshCore.h: 184-byte packet, 16-byte cipher blocks. The channel sender
   prefix is at most 18 bytes: 5 + 18 + 144 + 1 + 15 <= 184.
   DM's 5 + 144 + 2-byte MAC + 15 also fits. No opaque delta encoding. */
#define LS_SHARE_TEXT 144
#define LS_SHARE_PART_POINTS 4
#define LS_SHARE_PARTS (LS_SKETCH_PTS / LS_SHARE_PART_POINTS)
#define LS_SHARE_TIMEOUT 180u
#define LS_SHARE_SLOTS 4
#define LS_SHARE_RECENT 24

typedef struct {
    bool line;
    uint32_t id;
    char name[LS_MARK_NAME];
    int n;
    double lat[LS_SKETCH_PTS],lon[LS_SKETCH_PTS];
} ls_share_item_t;
typedef struct {
    ls_share_item_t item;
    unsigned part,total;
} ls_share_part_t;
typedef struct {
    ls_share_item_t item;
    char source[48];
    uint32_t started,mask;
    unsigned total;
    uint8_t count[LS_SHARE_PARTS];
    bool used;
} ls_share_slot_t;
typedef struct {
    char source[48];
    uint32_t id,stamp;
    bool used;
} ls_share_seen_t;
typedef struct {
    ls_share_slot_t slots[LS_SHARE_SLOTS];
    ls_share_seen_t recent[LS_SHARE_RECENT];
    unsigned next_recent;
} ls_share_rx_t;

/* One-based parts; 0 on invalid input, never truncates a message. */
int ls_share_parts(const ls_share_item_t *item);
bool ls_share_encode(const ls_share_item_t *item,unsigned part,char *out,size_t cap);
bool ls_share_decode(const char *text,ls_share_part_t *out);
void ls_share_expire(ls_share_rx_t *rx,uint32_t now);
/* -1 invalid/conflicting/full, 0 pending or duplicate, 1 complete.
   Key source by DM peer id or channel slot + advertised name (not authenticated).
   The caller owns rx/out and should put both in PSRAM. */
int ls_share_receive(ls_share_rx_t *rx,const char *source,const char *text,
                     uint32_t now,ls_share_item_t *out);
/* Import only into received RAM state; never writes SD or transmits. */
int ls_share_import(const ls_share_item_t *item,const char *sender);
#endif
