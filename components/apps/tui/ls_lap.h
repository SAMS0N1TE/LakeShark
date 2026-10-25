/* Native SD app host. One shared screen, one resident ELF, no layout policy.
 * Callbacks run ONLY on the existing 6144-byte TUI task stack (never an ISR).
 * ls_lap_stack_unused() reports the minimum sampled FreeRTOS headroom in bytes.
 * Budget apps to <=1024 bytes of callback-local stack until measured on board;
 * firmware/router/libc share the remainder. No TUI stack increase is made.
 * Load, relocation, signature checking and data flush use the lap_io worker.
 */
#ifndef LS_LAP_H
#define LS_LAP_H
#include "ls_lap_manifest.h"
#ifdef __cplusplus
extern "C" {
#endif
#define LS_LAP_MAX_APPS 24
typedef enum { LS_LAP_IDLE, LS_LAP_LOADING, LS_LAP_RUNNING, LS_LAP_FAILED } ls_lap_state_t;
typedef struct {
    ls_lap_state_t state;
    const char *id;
    const char *reason;
    bool needs_confirmation, is_signed, signature_checked;
} ls_lap_status_t;
typedef struct {
    lap_app_t manifest;
    char path[192];
    size_t size;
    bool is_signed, needs_confirmation, installed;
    bool signature_present,
        signature_checked; /* pending until open/probe; never label pending as trusted */
    uint8_t sha256[32];
    bool has_icon_px;                       /* LAP_ICON_PX: colour dots, 0xFF clear */
    uint8_t icon_px[LAP_PX_H][LAP_PX_W];
} ls_lap_info_t;
int ls_lap_scan_file(const char *path); /* boot scan, no execution */
/* Worker/console only: stream SHA256 + verify signature without executing code. */
bool ls_lap_inspect_file(const char *path, ls_lap_info_t *out, char *why, size_t cap);
const char *ls_lap_last_error(void);
const ls_lap_info_t *ls_lap_at(int index);
const ls_lap_info_t *ls_lap_by_id(const char *id);
const char (*ls_lap_icon_art(const char *id))[LAP_ICON_W + 1];
const ls_lap_status_t *ls_lap_status(void);
const char *ls_lap_last_crash(void);
/* Explicit UI confirmation; the UI supplies its own tap/ask screen. */
bool ls_lap_confirm(const char *id);
bool ls_lap_is_open(const char *id);
unsigned ls_lap_stack_unused(void); /* zero means not measured yet */
void ls_lap_poll(void);             /* TUI task, before router draw, also after leaving */
void ls_lap_console_register(void);
bool ls_lap_start_worker(void); /* backend worker, PSRAM stack; no app code */
/* Console worker transaction fence. Acquiring fails while the target native app is open. NULL
 * targets all apps. publish is applied by poll on the TUI task; disk changes survive a stopped UI.
 */
bool ls_lap_catalog_busy(void);
bool ls_lap_maintenance_begin(const char *id);
void ls_lap_maintenance_end(void);
bool ls_lap_publish(const ls_lap_info_t *info);
void ls_lap_console_expire(void); /* lap_io worker only */
#ifdef __cplusplus
}
#endif
#endif
