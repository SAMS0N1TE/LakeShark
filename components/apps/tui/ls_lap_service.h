/* API 2.1 for native apps: background services, and how firmware code calls
 * back into an app (the shared controls' callbacks, a service's tick).
 *
 * A service is the loaded app's own tick, run by ls_lap_poll on the screen
 * task every period whether the app's screen is open or not. ls_lap.c owns
 * the lifecycle: it keeps the app loaded when the screen closes with a
 * service running, shows it again when the tile is opened, and unloads it
 * when the service ends. This file keeps the service's state, runs the
 * tick inside the guard and measures it against the budget; it never loads,
 * frees or draws anything. */
#ifndef LS_LAP_SERVICE_H
#define LS_LAP_SERVICE_H
#include "ls_lap_manifest.h"
#include <stdbool.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

/* ---- calling into the app ------------------------------------------------ */

/* The loader's checks around a call into app code: code_ok says a function
   pointer lies in the app's executable segments; begin/end mark the call
   (crash trail, API context). Without a guard (the bench) any non-NULL
   pointer is accepted and begin/end only open the API context. */
typedef struct {
    bool (*code_ok)(uintptr_t fn);
    void (*begin)(void);
    void (*end)(void);
} ls_lap_guard_t;
void ls_lap_set_guard(const ls_lap_guard_t *guard);
bool ls_lap_code_ok(const void *fn);
/* Nest-aware: inside an app callback already, these do nothing, so a
   control that calls back while the app is calling it stays in one context. */
void ls_lap_call_begin(void);
void ls_lap_call_end(void);

/* ---- the service ---------------------------------------------------------- */

#define LS_LAP_SVC_MIN_MS 50u
#define LS_LAP_SVC_MAX_MS 10000u
#define LS_LAP_SVC_STRIKES 3

/* Screen task. begin when the app is entered (its flags say whether it may
   run one), end when it is unloaded. */
void ls_lap_service_begin(const lap_app_t *app);
void ls_lap_service_end(void);
bool ls_lap_service_on(void);
/* The app's screen is showing (ls_lap.c sets it on show and hide). */
void ls_lap_service_set_shown(bool shown);
bool ls_lap_service_shown(void);
/* Screen task, every frame: runs the tick when it is due. 0 nothing to do,
   1 ticked, -1 the service must end now (too slow, or it asked to stop
   while its screen was closed); ls_lap_service_why says why. */
int ls_lap_service_poll(void);
const char *ls_lap_service_why(void);
/* Ends the service and answers its `stopped` callback (NULL when none or the
   app never set one); the caller calls it inside the guard if it unloads. */
void (*ls_lap_service_off(const char *why))(void);
/* Any task: ask the screen task to stop the service of app `id` (NULL any).
   ls_lap_service_take_stop answers true once per request. */
bool ls_lap_service_request_stop(const char *id);
bool ls_lap_service_take_stop(void);
/* A banner in the app's colour: "<NAME> SERVICE" and the words. */
void ls_lap_service_notice(const char *body);

typedef struct {
    bool running, shown;
    char id[32], name[16];
    uint8_t hue;
    uint32_t period_ms;
    lap_service_stats_t stats;
} ls_lap_service_info_t;
/* What runs now, for RADIOS and the console: 0 or 1 (LAP_SERVICES_MAX). */
int ls_lap_services(ls_lap_service_info_t *out, int max);

#if !defined(ESP_PLATFORM) || defined(LS_LAP_HOST_TEST)
/* The bench's clock for tick timing, in microseconds; NULL uses the API's. */
extern int64_t (*ls_lap_service_host_clock)(void);
#endif
#ifdef __cplusplus
}
#endif
#endif
