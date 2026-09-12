/* What feeds the waterfall. */

#ifndef LS_WF_SOURCE_H
#define LS_WF_SOURCE_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    LS_WF_SRC_AUTO = 0,   /* whichever receiver is actually running     */
    LS_WF_SRC_P25,
    LS_WF_SRC_FM,
    /* The LoRa radio as a swept spectrum analyser. Not the dongle:
       this one is soldered to the board and works with nothing plugged in.
       See ls_lora.h for what a swept measurement can and cannot show. */
    LS_WF_SRC_LORA,
    LS_WF_SRC__COUNT
} ls_wf_src_t;

void        ls_wf_source_select(ls_wf_src_t src);
ls_wf_src_t ls_wf_source_get(void);

/* The short name of a source, for a chip or a label. Never NULL. */
const char *ls_wf_source_label(ls_wf_src_t src);

const char *ls_wf_source_blocked(ls_wf_src_t src);

/* Start whatever this source needs, and select it. */

bool ls_wf_source_start(ls_wf_src_t src);

/* WHERE to point the radio, which is a different question per radio. */

int         ls_wf_preset_count(ls_wf_src_t src);

/* Valid until the next call - the picker copies what it is given. */
const char *ls_wf_preset_label(ls_wf_src_t src, int i);
const char *ls_wf_preset_detail(ls_wf_src_t src, int i);

/* Point the radio at preset `i`. False when there is no such preset. */
bool        ls_wf_preset_apply(ls_wf_src_t src, int i);
void        ls_wf_fm_sweep(bool on);

/* What the radio is pointed at now, for a button face. Never NULL. */
const char *ls_wf_preset_current(ls_wf_src_t src);

/* Why the list is empty, or NULL when it is not. */
const char *ls_wf_preset_none(ls_wf_src_t src);

/* The band a LoRa sweep covers. Defaults to the 902-928 MHz ISM
   band this board's mesh lives in; the console can move it anywhere the part
   tunes, which is 150 to 960 MHz. */
void ls_wf_source_lora_band(uint32_t min_hz, uint32_t max_hz);
void ls_wf_source_lora_band_get(uint32_t *min_hz, uint32_t *max_hz);

/* Name of the source actually feeding right now, which under AUTO is not
   necessarily the one that was selected. "none" when nothing is. */
const char *ls_wf_source_name(void);
const char *ls_wf_source_progress(void);

/* Enable the feed if needed, read one frame, push it. Call once per frame
   from a screen that is drawing a waterfall. */
void ls_wf_source_pump(void);

/* Turn the feed off. A spectrum nobody is looking at costs FFTs and PSRAM
   bandwidth on the path already competing for it, so leaving is not
   optional. */
void ls_wf_source_release(void);

#ifdef __cplusplus
}
#endif

#endif /* LS_WF_SOURCE_H */
