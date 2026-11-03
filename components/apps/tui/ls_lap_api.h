/* Native app execution context, clipped drawing and generation-owned PSRAM. */
#ifndef LS_LAP_API_H
#define LS_LAP_API_H
#include "ls_lap_manifest.h"
#include "ls_tui.h"
#include "ls_input_ev.h"
#include "ls_lap_context.h"
typedef struct {
    const char *name;
    uintptr_t address;
} ls_lap_export_t;
const ls_lap_export_t *ls_lap_exports(size_t *count);
uintptr_t ls_lap_resolve(const char *name);
const char *ls_lap_missing_symbol(void);
/* Selection is task local. Resource tables are bounded and owned by a slot. */
typedef struct ls_lap_instance ls_lap_instance_t;
ls_lap_instance_t *ls_lap_instance_new(void);
ls_lap_instance_t *ls_lap_instance_current(void);
bool ls_lap_instance_select(ls_lap_instance_t *instance);
bool ls_lap_instance_delete(ls_lap_instance_t *instance);
ls_lap_instance_t *ls_lap_instance_at(unsigned slot);
/* Producer/worker binding: no callbacks or app pointers are enabled by this. */
ls_lap_instance_t *ls_lap_instance_bind(ls_lap_instance_t *instance);
ls_lap_instance_t *ls_lap_resource_owner(unsigned kind);
bool ls_lap_radio_control_allowed(void);
bool ls_lap_instance_service_info(unsigned slot, lap_service_info_t *out);
void *ls_lap_instance_service_context(unsigned slot);
void *ls_lap_instance_extension_at(unsigned slot);
void **ls_lap_api_extension(void);
void ls_lap_api_begin(const char *id, uint32_t generation, uint32_t opened_ms);
void ls_lap_api_cleanup(void);
void ls_lap_input_enable(bool on);
bool ls_lap_input_active(void);
void ls_lap_input_board(const ls_input_ev_t *ev);
void ls_lap_input_touch(int x, int y, bool down);
bool ls_lap_input_key(unsigned key, char ch);
void ls_lap_input_key_up(unsigned key, char ch);
void ls_lap_input_event(const ls_input_ev_t *ev, int origin_x, int origin_y);
void ls_lap_lifecycle(unsigned kind, int width_px, int height_px);
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
/* Inside draw(): the surface and the app's area (absolute cells), for the shared controls. */
bool ls_lap_api_canvas(tui_surface **sf, tui_rect *area);
/* The loaded image, for ls_lap_api_owns; kept across begin/cleanup. */
void ls_lap_api_set_image(const void *base, size_t bytes);
bool ls_lap_api_owns(const void *p, size_t n);
/* Synchronous input/output: image/allocation or the active callback's stack.
 * Never use for a pointer retained after return; those require api_owns. */
bool ls_lap_api_readable(const void *p, size_t n);
bool ls_lap_copy_input(void *out, const void *p, size_t n);
void *ls_lap_malloc(size_t n);
void *ls_lap_calloc(size_t n, size_t size);
void *ls_lap_realloc(void *p, size_t n);
void ls_lap_free(void *p);
/* Worker only. Files are cached in tracked PSRAM before enter; changes flush
 * after leave. No SD access from app callbacks. Data cache shares the 4 MB cap. */
bool ls_lap_api_files_load(const char *root);
bool ls_lap_api_files_flush(void);
bool ls_lap_api_flush_acquire(void);
void ls_lap_api_flush_release(void);
#endif
