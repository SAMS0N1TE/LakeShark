#ifndef INCLUDED_AMBE_H
#define INCLUDED_AMBE_H
#include "mbelib.h"
#ifdef __cplusplus
extern "C" {
#endif
int ls_p2_mbe_dequantizeAmbe2250Parms (ls_p2_mbe_parms* cur_mp, ls_p2_mbe_parms* prev_mp, ls_p2_mbe_errs* errs, const int *b);
int ls_p2_mbe_dequantizeAmbe2400Parms (ls_p2_mbe_parms* cur_mp, ls_p2_mbe_parms* prev_mp, ls_p2_mbe_errs* errs, const int *b);
int ls_p2_mbe_dequantizeAmbeTone(ls_p2_mbe_tone* tone, ls_p2_mbe_errs* errs, const int *u);
#ifdef __cplusplus
}
#endif
#endif /* INCLUDED_AMBE_H */
