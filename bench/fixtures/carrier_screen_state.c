/* Host ownership for screen state normally defined by the app entry point.
 * Carrier backend tests deliberately omit main.c; no drawing is stubbed. */
#if defined(__has_include)
#if __has_include("ui.h")
#include "ui.h"
ui_state ui;
#endif
#endif
