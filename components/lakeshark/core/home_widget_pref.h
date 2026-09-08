#pragma once
#include <stdbool.h>
#ifdef __cplusplus
extern "C" {
#endif

/* Stable persisted IDs. Append future supported widgets; never repurpose IDs. */
typedef enum {
    HOME_WIDGET_RECEIVER = 0,
    HOME_WIDGET_SYSTEM = 1,
    HOME_WIDGET_CLOCK = 2,
    HOME_WIDGET_COUNT
} home_widget_id_t;

void home_widget_pref_init(bool read_ok, int stored);
home_widget_id_t home_widget_pref_get(void);
bool home_widget_pref_set(int id, bool (*persist)(int id));

#ifdef __cplusplus
}
#endif
