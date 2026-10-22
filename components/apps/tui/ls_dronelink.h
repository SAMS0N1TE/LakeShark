#ifndef LS_DRONELINK_H
#define LS_DRONELINK_H
#include <stdbool.h>
#include <stdint.h>
/* Called only on the wireless worker; status is an atomic enum snapshot. */
void ls_dronelink_tick(bool enabled, int64_t now);
/* RF worker gives the lease back between sources, retaining scan progress. */
void ls_dronelink_suspend(void);
bool ls_dronelink_pending(void);
/* DRONES on screen: may pause MeshCore for the LoRa radio. */
void ls_dronelink_may_pause_mesh(bool on);
const char *ls_dronelink_status(void);
#endif
