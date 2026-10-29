#ifndef LS_AUDIO_LAYERS_H
#define LS_AUDIO_LAYERS_H
#include "ls_audio_analysis.h"
#include "ls_audio_overview.h"
#include "ls_audio_tools.h"
#include "ls_tui.h"
typedef struct ls_audio_layers ls_audio_layers;
enum {
  LS_LAYER_WAVE,
  LS_LAYER_SPECTRO,
  LS_LAYER_MELODIC,
  LS_LAYER_SPECTRUM,
  LS_LAYER_SCOPE,
  LS_LAYER_TOOLS,
  LS_LAYERS
};
ls_audio_layers *ls_audio_layers_new(void);
void ls_audio_layers_free(ls_audio_layers *p);
/* The screen supplies source PCM consumers; drawing never opens files. */
void ls_audio_layers_draw(ls_audio_layers *p, tui_surface *sf, tui_rect r,
                          int layer, ls_audio_analysis *a, ls_audio_overview *o,
                          ls_audio_tools *tools, uint32_t position_ms,
                          bool hold);
bool ls_audio_layers_touch(ls_audio_layers *p, int x, int y);
void ls_audio_layers_zoom(ls_audio_layers *p);
uint32_t ls_audio_layers_time(const ls_audio_layers *p);
float ls_audio_layers_frequency(const ls_audio_layers *p);
#endif
