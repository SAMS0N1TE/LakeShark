/* NFM voice as an RX HEAD plugin (ls_rx_plugin.h): the listening chain FM
   ran inline on both the SDR and the LR2021 paths.

   From the stream's front it takes the discriminator (DISC) for the noise
   squelch and the CTCSS/DCS tone gate, then the 16 kHz audio (PCM16) of the
   same block, gated, optionally capped, to the head's audio sink. It is the
   one AUDIO plugin of its head and is fed before any decoder beside it.

   Settings come from the bound FM state (squelch_tenths, tone_required) and
   the readouts go back there (squelch_open, noise, tone_detected), which is
   what the FM screen draws. */
#ifndef RX_VOICE_H
#define RX_VOICE_H

#include "ls_rx_plugin.h"
#include "fm_state.h"

#ifdef __cplusplus
extern "C" {
#endif

extern const ls_rx_plugin_t rx_voice;

typedef struct {
    int16_t peak_cap;     /* clip audio to +-this; 0 none                    */
    bool    pad_closed;   /* write silence while shut, keeping the sink fed */
} rx_voice_cfg_t;

void rx_voice_bind(void *st, fm_state_t *out, const rx_voice_cfg_t *cfg);

#ifdef __cplusplus
}
#endif
#endif
