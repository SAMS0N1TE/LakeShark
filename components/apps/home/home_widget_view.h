#pragma once
#include "home_widget_pref.h"
#include "shell/ls_hub.h"
#include <time.h>
#ifdef __cplusplus
extern "C" {
#endif
typedef struct {
    char title[32];
    char value[48];
    char detail[96];
} home_widget_view_t;
void home_widget_present(home_widget_id_t id, const ls_hub_state_t *hub,
                         bool clock_valid, time_t now, home_widget_view_t *out);
#ifdef __cplusplus
}
#endif
