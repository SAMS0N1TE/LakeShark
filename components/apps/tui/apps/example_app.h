/* Optional native app teaching the shared screen and touch contracts. */
#ifndef LS_EXAMPLE_APP_H
#define LS_EXAMPLE_APP_H
#ifdef ESP_PLATFORM
#include "sdkconfig.h"
#endif
#include "../ls_app.h"
#include "../ls_icons.h"
#ifdef __cplusplus
extern "C" {
#endif
#if defined(CONFIG_LS_EXAMPLE_APP) && CONFIG_LS_EXAMPLE_APP
extern const ls_tui_screen_t ls_scr_example;
extern const ls_app_doc_t ls_doc_example;
#define LS_EXAMPLE_APP_ENTRY \
    { "example", "EXAMPLE", "tap counter", LS_ICON_CHIP, TUI_CYAN, \
      LS_APP_EXTRA, &ls_scr_example, NULL, &ls_doc_example }
#endif
#ifdef __cplusplus
}
#endif
#endif
