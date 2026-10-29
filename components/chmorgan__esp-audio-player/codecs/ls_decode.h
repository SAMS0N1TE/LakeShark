#ifndef LS_DECODE_H
#define LS_DECODE_H
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#ifdef __cplusplus
extern "C" {
#endif
typedef struct ls_decode ls_decode;
/* All decoder allocations and scratch are PSRAM. The caller keeps ownership
   of FILE and closes it only after ls_decode_close. Never called by the UI. */
ls_decode *ls_decode_open(FILE *file);
void ls_decode_close(ls_decode *d);
unsigned ls_decode_rate(const ls_decode *d);
unsigned ls_decode_channels(const ls_decode *d);
uint64_t ls_decode_frames(const ls_decode *d);
uint64_t ls_decode_position(const ls_decode *d);
const char *ls_decode_kind(const ls_decode *d);
/* Signed 16-bit interleaved source channels. No whole-file PCM allocation. */
size_t ls_decode_read(ls_decode *d,int16_t *pcm,size_t frames);
bool ls_decode_seek(ls_decode *d,uint64_t frame);
bool ls_decode_failed(const ls_decode *d);
#ifdef __cplusplus
}
#endif
#endif
