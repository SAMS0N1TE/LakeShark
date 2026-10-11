#ifndef LS_AUDIO_VIEW_H
#define LS_AUDIO_VIEW_H
#include "ls_tui_ui.h"
/* One foreground media owner; history is allocated in PSRAM on the P4. */
void ls_audio_view_open(void);
void ls_audio_view_close(void);
void ls_audio_view_reset(void);
uint8_t ls_audio_view_color(int scheme, int level);
void ls_audio_view_draw(tui_surface *sf, tui_rect r, int view, int palette,
                        int span, bool live);

/* LEVEL reads what is heard: the decoded peak after the codec's volume, in
   dBFS of the output, and nothing at all while muted or at volume 0 (the
   codec's register 0 is its own mute). -INFINITY for silence. */
float ls_audio_view_output_db(unsigned peak, int volume, float volume_db,
                              bool muted);

/* The waterfall's reading of the audio. Floor and range set the colour map
   in dB; LOG puts the frequency axis on a log scale from 40 Hz; top_div
   shows 0 .. Nyquist / top_div (1, 2, 4 or 8); hold freezes the picture
   while the audio plays on. Defaults: -100 dB, 80 dB, linear, 1, running. */
typedef struct {
  float floor_db, range_db;
  bool log_axis, hold;
  int top_div;
} ls_audio_wf_opts_t;
void ls_audio_view_waterfall(const ls_audio_wf_opts_t *opts);
/* Samples per waterfall pixel column at SPAN step 0..2. */
unsigned ls_audio_view_hop(int span);
#endif
