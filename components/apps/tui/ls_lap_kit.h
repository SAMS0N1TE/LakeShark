/* API 2.1 for native apps: the board's own controls (lakeshark_ui.h).
 *
 * Thin wrappers over the widgets the board's screens draw with: ls_btn_*
 * keys, ls_options OPTIONS lists, ls_picker, ls_numpad, the FM frequency
 * pad, the shared radio glass (ls_radio_panel), ls_meter_ascii,
 * tui_sparkline, braille history (ls_braille.h) and the waterfall
 * (ls_waterfall). Each wrapper copies what the app hands it (strings,
 * tables, values) into firmware storage, clips to the app's area, and calls
 * the app back only through the loader's guard (ls_lap_service.h).
 * Screen task only. */
#ifndef LS_LAP_KIT_H
#define LS_LAP_KIT_H
#include <stdbool.h>
#ifdef __cplusplus
extern "C" {
#endif
/* The app is entered: forget everything from the last one. */
void ls_lap_kit_begin(void);
/* Its screen closes (or it is unloaded): close whatever list or keypad it
   opened, give the waterfall back, free the OPTIONS tables, and never call
   its callbacks again. */
void ls_lap_kit_end(void);
#ifdef __cplusplus
}
#endif
#endif
