/* RADIOS lists a native app's background service; this target has no app host. */
#include "ls_lap_service.h"
int ls_lap_services(ls_lap_service_info_t *out, int max) { (void)out; (void)max; return 0; }
bool ls_lap_service_request_stop(const char *id) { (void)id; return false; }
