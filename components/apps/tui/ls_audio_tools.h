#ifndef LS_AUDIO_TOOLS_H
#define LS_AUDIO_TOOLS_H
#include "ls_audio_analysis.h"
#ifdef __cplusplus
extern "C" {
#endif
typedef struct ls_audio_tools ls_audio_tools;
typedef enum {
  LS_TOOL_DTMF,
  LS_TOOL_CTCSS,
  LS_TOOL_DCS,
  LS_TOOL_CW,
  LS_TOOL_APRS
} ls_tool_kind;
typedef struct {
  uint32_t time_ms;
  ls_tool_kind kind;
  char text[176];
} ls_tool_event;
typedef struct {
  float wpm;
  char subtone[24], digits[32];
  unsigned events;
} ls_tool_status;
ls_audio_tools *ls_audio_tools_new(void);
void ls_audio_tools_free(ls_audio_tools *t);
/* Consumer-owned reset/feed. Any file, microphone or routed radio PCM. */
void ls_audio_tools_reset(ls_audio_tools *t);
void ls_audio_tools_feed(ls_audio_tools *t, const int16_t *pcm, size_t frames,
                         unsigned channels, unsigned rate);
unsigned ls_audio_tools_pull(ls_audio_tools *t, const ls_audio_analysis *a,
                             unsigned max_frames);
/* Try-lock snapshot: drawing never waits for the background decoder. */
bool ls_audio_tools_snapshot(ls_audio_tools *t, ls_tool_status *status,
                             ls_tool_event *events, unsigned capacity,
                             unsigned *count);
size_t ls_audio_tools_bytes(void);
typedef enum { LS_GEN_SINE, LS_GEN_SQUARE, LS_GEN_SWEEP } ls_gen_kind;
typedef struct ls_audio_generator ls_audio_generator;
ls_audio_generator *ls_audio_generator_new(void);
void ls_audio_generator_free(ls_audio_generator *g);
/* Deliberately modest levels: -60..-12 dBFS, default -24 dBFS. */
bool ls_audio_generator_set(ls_audio_generator *g, ls_gen_kind kind,
                            unsigned start_hz, unsigned end_hz,
                            unsigned duration_ms, int dbfs);
void ls_audio_generator_render(ls_audio_generator *g, int16_t *pcm,
                               size_t frames, unsigned channels, unsigned rate);
#ifdef __cplusplus
}
#endif
#endif
