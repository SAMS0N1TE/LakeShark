/* P25 on the desk: the talkgroup table's rows. The receiver keeps what it
   heard per channel (p25_tg_observed.h); this merges that by talkgroup,
   adds the ones only a list knows (held, locked out, allowed), sorts by a
   column, says what a cell reads and writes a row as CSV. No drawing and
   no receiver in here, so the bench checks it with a snapshot of its own. */
#ifndef LS_P25_DESK_H
#define LS_P25_DESK_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "p25_tg_observed.h"

#ifdef __cplusplus
extern "C" {
#endif

enum { LS_PT_FLAGS, LS_PT_TG, LS_PT_NAME, LS_PT_HITS, LS_PT_LAST, LS_PT_BUSY, LS_PT_CHAN, LS_PT_NAC, LS_PT_VIA, LS_PT__COUNT };
enum { LS_PF_HELD = 1, LS_PF_LOCKED = 2, LS_PF_ALLOW = 4, LS_PF_ENC = 8, LS_PF_NOW = 16 };
#define LS_P25D_ROWS (P25_TG_OBSERVED_MAX + 16)

typedef struct {
    uint16_t tg, nac;
    uint8_t flags, sources;             /* LS_PF_*; P25_TG_SEEN_* */
    bool heard;                         /* in the receiver's store, not only on a list */
    uint32_t hits, recent;              /* since the store began; in the minutes it keeps */
    uint32_t last_ms;                   /* the receiver's clock */
    uint64_t hz;                        /* where it was last heard */
    uint8_t minute[P25_TG_OBSERVED_MINUTES];    /* [0] oldest .. newest */
    char name[24];
} ls_p25d_row_t;

/* One row a talkgroup, however many channels it was heard on. Answers how many. */
int ls_p25d_merge(ls_p25d_row_t *out, int max, const p25_tg_observed_snapshot_t *snap, uint32_t now_ms);
/* The row for `tg`, added empty when it was never heard; -1 when full or tg is 0. */
int ls_p25d_row(ls_p25d_row_t *rows, int *n, int max, uint16_t tg);
/* Ascending unless `desc`; LS_PT_LAST ascending is the most recent first,
   LS_PT_HITS and LS_PT_BUSY the busiest first. A talkgroup never heard goes
   last either way in the columns that need hearing. */
void ls_p25d_sort(uint8_t *order, const ls_p25d_row_t *rows, int n, int col, bool desc);
void ls_p25d_cell(const ls_p25d_row_t *r, int col, uint32_t now_ms, char *out, size_t n);
/* Case-blind: the number or the name holds `needle`; "" matches all. */
bool ls_p25d_match(const ls_p25d_row_t *r, const char *needle);
extern const char LS_P25D_CSV_HEAD[];
int ls_p25d_csv(const ls_p25d_row_t *r, uint32_t now_ms, char *out, size_t n);

#ifdef __cplusplus
}
#endif

#endif /* LS_P25_DESK_H */
