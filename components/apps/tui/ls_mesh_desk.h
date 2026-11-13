/* MESH on the desk: what a messenger needs to know that the handheld's one
   feed does not. Which conversations there are (the public channel, every
   keyed channel, a node you have written to or that wrote to you, every
   node heard), which messages belong to each, how many arrived since each
   was last looked at, and the nodes in any order. No drawing in here and
   no radio: it is given the message ring and the peer list and answers
   from those, so the bench can check it without a screen.

   "Unread" is since boot: the message ring is in RAM and so is this. */
#ifndef LS_MESH_DESK_H
#define LS_MESH_DESK_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "ls_mesh.h"

#ifdef __cplusplus
extern "C" {
#endif

enum { LS_MC_PUBLIC, LS_MC_CHANNEL, LS_MC_DM };

typedef struct {
    uint8_t kind;                   /* LS_MC_* */
    uint8_t chan;                   /* the slot, for a channel */
    bool heard;                     /* a DM: the node is in the peer list, so it can be addressed */
    uint8_t role;                   /* a DM: LS_MESH_ROLE_* when heard */
    char id[17];                    /* a DM: the node */
    char name[LS_MESH_PEER_NAME];   /* what the list calls it */
    uint16_t total, unread;
    uint32_t last_t;                /* the newest message's time, 0 for none */
} ls_meshd_conv_t;

/* One per conversation that has been looked at: the newest message it held
   then. */
#define LS_MESHD_READ 24
typedef struct { uint32_t key, mark; } ls_meshd_read_one_t;
typedef struct { ls_meshd_read_one_t e[LS_MESHD_READ]; int n; } ls_meshd_read_t;

/* A message's identity for as long as it is in the ring. */
uint32_t ls_meshd_mark(const ls_mesh_msg_t *m);
bool ls_meshd_belongs(const ls_mesh_msg_t *m, const ls_meshd_conv_t *c);
/* The conversation a message is part of, as a key for the read marks. */
uint32_t ls_meshd_key(const ls_meshd_conv_t *c);

/* Channels first (the public one, then each slot with a key), then the
   nodes with messages, newest first, then the rest of the nodes heard.
   Answers how many. `read` may be NULL: then nothing is unread. */
int ls_meshd_convs(ls_meshd_conv_t *out, int max,
                   const ls_mesh_msg_t *msgs, int nmsgs,
                   const ls_mesh_chan_t *chans, int nchans,
                   const ls_mesh_peer_t *peers, int npeers,
                   const ls_meshd_read_t *read);
/* The conversation has been looked at: everything in it now is read. */
void ls_meshd_read_all(ls_meshd_read_t *read, const ls_meshd_conv_t *c,
                       const ls_mesh_msg_t *msgs, int nmsgs);
/* Messages from someone else that arrived after the read mark. */
int ls_meshd_unread(const ls_meshd_read_t *read, const ls_meshd_conv_t *c,
                    const ls_mesh_msg_t *msgs, int nmsgs);

/* "name: text" as it went on the air, split. `who` gets the sender (the
   message's own field when it has one), the answer is the text after it. */
const char *ls_meshd_split(const ls_mesh_msg_t *m, char *who, size_t n);
/* A word for where one of ours got to. */
const char *ls_meshd_state_word(const ls_mesh_msg_t *m);
/* Case-blind: does the sender or the text hold `needle`? An empty needle
   matches everything. */
bool ls_meshd_match(const ls_mesh_msg_t *m, const char *needle);

/* The next line of `text` from `off`, at most `width` cells, broken at a
   space when there is one. Copies it to `line` and answers the offset of
   the line after (text[off] == 0 when done). */
int ls_meshd_wrap(const char *text, int off, int width, char *line, size_t n);

/* The node table's columns. */
enum { LS_MN_NAME, LS_MN_ID, LS_MN_ROLE, LS_MN_HEARD, LS_MN_RSSI, LS_MN_SNR, LS_MN_DIST, LS_MN_ADVERTS,
       LS_MN_FIRST, LS_MN__COUNT };
/* order[0..n) becomes the peers' indices sorted by `col`. dist_m[i] is the
   range to peer i in metres, negative when it or we have no position; a
   node with nothing to sort by goes last whichever way the order runs. */
void ls_meshd_sort(uint8_t *order, const ls_mesh_peer_t *peers, int n, int col, bool desc,
                   const float *dist_m);
const char *ls_meshd_role(uint8_t type);
/* "850 m", "12.4 km"; "-" when negative. */
void ls_meshd_range_text(float metres, char *out, size_t n);
/* HH:MM from an epoch; "+1h02m" style uptime when the clock was never set. */
void ls_meshd_time_text(uint32_t t, char *out, size_t n);

#ifdef __cplusplus
}
#endif

#endif /* LS_MESH_DESK_H */
