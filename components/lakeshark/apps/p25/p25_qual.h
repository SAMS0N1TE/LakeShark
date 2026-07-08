#ifndef P25_QUAL_H
#define P25_QUAL_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint32_t nac;
    float    dur_s;
    int      bch_ok;
    int      bch_fail;
    int      ok_pct;
    int      vox;
    uint32_t under;
    uint32_t drop;
    int      dec_ms;
} p25_qual_rec_t;

void p25_qual_push(const p25_qual_rec_t *r);
int  p25_qual_count(void);
bool p25_qual_get(int i, p25_qual_rec_t *out);
void p25_qual_clear(void);
void p25_qual_aggregate(int *calls, int *ok, int *fail, int *ok_pct,
                        int *vox, uint32_t *under, uint32_t *drop);

#ifdef __cplusplus
}
#endif

#endif
