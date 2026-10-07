/* One route shared by MAP and COMPASS, owned by the UI task. */
#ifndef LS_ROUTE_LIVE_H
#define LS_ROUTE_LIVE_H
#include "ls_route.h"
#include "ls_options.h"
#ifdef __cplusplus
extern "C" {
#endif
const ls_route_t *ls_route_live(void);
/* Opens only the requested SD GPX or the recorder's current/last ring. */
bool ls_route_live_load(const char *path, bool backtrack);
void ls_route_live_poll(void);
void ls_route_live_clear(void);
/* Shared constant context lets parent menu tables live in flash. */
extern const ls_opt_ctx_t LS_ROUTE_OPTIONS;
const ls_opt_ctx_t *ls_route_options(void);
void ls_route_guidance(char *out, size_t cap);
#ifdef __cplusplus
}
#endif
#endif
