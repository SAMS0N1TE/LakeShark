/* Native app execution context, clipped drawing and generation-owned PSRAM. */
#ifndef LS_LAP_API_H
#define LS_LAP_API_H
#include "ls_lap_manifest.h"
#include "ls_tui.h"
typedef struct {
    const char *name;
    uintptr_t address;
} ls_lap_export_t;
const ls_lap_export_t *ls_lap_exports(size_t *count);
uintptr_t ls_lap_resolve(const char *name);
const char *ls_lap_missing_symbol(void);
void ls_lap_api_begin(const char *id, uint32_t generation, uint32_t opened_ms);
void ls_lap_api_cleanup(void);
void ls_lap_api_draw_begin(tui_surface *sf, tui_rect area);
void ls_lap_api_draw_end(void);
void ls_lap_api_callback(bool on);
void ls_lap_api_set_wake(void (*wake)(void));
void ls_lap_api_log_service(void); /* worker only; bounded queue, no console IO on TUI */
bool ls_lap_api_exit_requested(void);
size_t ls_lap_api_bytes(void);
unsigned ls_lap_api_bad_frees(void);
const char *ls_lap_api_error(void);
bool ls_lap_api_inside(void); /* on the screen task, inside an app callback */
/* The loaded image, for ls_lap_api_owns; kept across begin/cleanup. */
void ls_lap_api_set_image(const void *base, size_t bytes);
bool ls_lap_api_owns(const void *p, size_t n);
void *ls_lap_malloc(size_t n);
void *ls_lap_calloc(size_t n, size_t size);
void *ls_lap_realloc(void *p, size_t n);
void ls_lap_free(void *p);
/* Worker only. Files are cached in tracked PSRAM before enter; changes flush
 * after leave. No SD access from app callbacks. Data cache shares the 4 MB cap. */
bool ls_lap_api_files_load(const char *root);
bool ls_lap_api_files_flush(void);
#endif
