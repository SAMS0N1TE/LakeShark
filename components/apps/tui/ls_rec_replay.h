#ifndef LS_REC_REPLAY_H
#define LS_REC_REPLAY_H
#include "subghz_file.h"
#ifdef __cplusplus
extern "C" {
#endif
/* Copies a bounded preview; FILES retains ownership of its parsing buffers.
   Loads RECORD without starting transmission. */
bool ls_scr_rec_replay_file(const char *path,const subghz_file_t *file,const int32_t *edges);
#ifdef __cplusplus
}
#endif
#endif
