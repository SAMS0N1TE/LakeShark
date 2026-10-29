#ifndef LS_DET_SIGNAL_H
#define LS_DET_SIGNAL_H
/* What a decoder tells a detector, without depending on one: REC WATCH,
   CATCH, POCSAG and P25. Producers call ls_seen_signal (tui/ls_seen.c)
   through a weak reference, so a build or bench test without it links and
   the call is skipped. It hands the signal to the native app watching for
   it (SDK 2.2 lap_seen_*). Nonblocking and bounded: a full queue drops the
   signal and counts it. */
#include <stdbool.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
/* Source bits above the wireless ones in tui/ls_seen.h. */
enum { DET_REC=64, DET_CATCH=128, DET_POCSAG=256, DET_P25=512 };
#define DET_SIGNALS (DET_REC|DET_CATCH|DET_POCSAG|DET_P25)
typedef struct {
    uint16_t source;   /* one DET_* signal bit */
    uint8_t  flag;     /* REC/CATCH 1 new pattern; P25 1 emergency */
    int8_t   rssi;     /* dBm, 0 unknown */
    uint32_t a, b;     /* REC/CATCH Hz,pattern; POCSAG capcode,function; P25 talkgroup,unit */
    char     text[64]; /* POCSAG page; REC/CATCH what it was */
} ls_det_signal_t;
void ls_seen_signal(const ls_det_signal_t *s);
#ifdef __cplusplus
}
#endif
#endif
