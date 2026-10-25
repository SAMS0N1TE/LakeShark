/* test_lap_api links ls_lap_sys.c without the TUI screen: lap_immersive's host hook needs this. */
#include <stdbool.h>
void ls_tui_screen_set_full(bool full) { (void)full; }
