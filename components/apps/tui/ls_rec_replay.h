#ifndef LS_REC_REPLAY_H
#define LS_REC_REPLAY_H
#include "subghz_file.h"
#include "ls_tui_screen.h"
#include "radio_choice.h"
#ifdef __cplusplus
extern "C" {
#endif
/* Copies a bounded preview; FILES retains ownership of its parsing buffers.
   Loads RECORD without starting transmission. */
bool ls_scr_rec_replay_file(const char *path,const subghz_file_t *file,const int32_t *edges);

/* The same player hosted inside another screen. Load does not switch
   screens. Key and touch return 0 when not handled, 1 when handled and 2
   when the operator asked for another file - the host decides what that
   means. */
bool ls_rec_player_load(const char *path,const subghz_file_t *file,const int32_t *edges);
void ls_rec_player_draw(tui_surface *sf,tui_rect area);
int  ls_rec_player_key(ls_tk_t k,char c);
int  ls_rec_player_touch(int x,int y);
bool ls_rec_player_busy(void);

/* SUB-GHZ's capture source, chosen from REC's RADIO list: the RTL-SDR, the
   CC1101 or the LoRa chip, each back on its own frequency. False when WATCH
   is running or the receiver would not start. */
bool ls_scr_subghz_choose_source(ls_rsel_radio_t radio);

/* REC's level meter and ARM button, hosted the same way. */
void ls_rec_tools_enter(void);
void ls_rec_tools_draw(tui_surface *sf,tui_rect area);
bool ls_rec_tools_key(ls_tk_t k,char c);
bool ls_rec_tools_touch(int x,int y);
#ifdef __cplusplus
}
#endif
#endif
