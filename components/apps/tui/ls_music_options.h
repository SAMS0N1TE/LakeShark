#ifndef LS_MUSIC_OPTIONS_H
#define LS_MUSIC_OPTIONS_H
#include "ls_audio_layers.h"
#include "ls_options.h"
extern const ls_opt_ctx_t ls_music_analysis_options;
extern const ls_opt_ctx_t ls_music_playback_options;
extern const ls_opt_ctx_t ls_music_generator_options;
void ls_music_options_cursor(ls_audio_layers *p);
#endif
