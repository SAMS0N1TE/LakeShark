/* FM's data decoders as RX HEAD plugins (ls_rx_plugin.h). Each wraps the
   decoder FM already had and keeps its store:
     rx_flex   FLEX pagers, four timing loops (1600/3200 2-level, 3200/6400
               4-level) on the discriminator; pages into the FM page ring;
     rx_aprs   APRS 1200 baud AFSK on the 16 kHz audio; aprs_store;
     rx_same   NOAA weather SAME bursts on the 16 kHz audio; same_store;
     rx_ais    AIS, both channels, straight from the SDR's 256 kHz I/Q;
               ais_store;
     rx_acars  ACARS 2400 baud MSK on the AM envelope; acars_app_state.
   The FM state's flex_* counters follow rx_flex as the screen reads them. */
#ifndef RX_FM_DATA_H
#define RX_FM_DATA_H

#include "ls_rx_plugin.h"
#include "same.h"

#ifdef __cplusplus
extern "C" {
#endif

extern const ls_rx_plugin_t rx_flex, rx_aprs, rx_same, rx_ais, rx_acars;

/* Where rx_same's alerts go from its next attach: same_store_receive unless
   the owner names another (FM's weather speaker, fm_wx_on_same, stores the
   alert the same way and opens the speaker for it). NULL restores it. */
void rx_same_sink(same_callback_t fn, void *user);

#ifdef __cplusplus
}
#endif
#endif
