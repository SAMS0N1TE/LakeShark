/* Native app TCP service. Only the IO worker owns OS socket descriptors. */
#ifndef LS_LAP_NET_H
#define LS_LAP_NET_H
#include <stdbool.h>
void ls_lap_net_begin(bool allowed);
void ls_lap_net_end(void);
bool ls_lap_net_service(void);
void ls_lap_net_set_wake(void (*wake)(void));
#endif
