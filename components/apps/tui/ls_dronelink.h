#ifndef LS_DRONELINK_H
#define LS_DRONELINK_H
#include <stdbool.h>
#include <stdint.h>
/* Called only on the wireless worker; status is an atomic enum snapshot. */
void ls_dronelink_tick(bool enabled, int64_t now);
bool ls_dronelink_pending(void);
const char *ls_dronelink_status(void);
#endif
