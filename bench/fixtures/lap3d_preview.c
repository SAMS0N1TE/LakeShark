/* Host-only engine preview, compiled into lssim and never into firmware. */
#include "../tools/l3bench_scene.h"
static l3_scene *preview;
static void enter(void) { preview = NULL; }
static void draw(void) {
    if (!preview) {
        preview = l3_create(lap_cols(), lap_rows());
        if (preview)
            l3bench_setup(preview, 2);
    }
    if (!preview)
        return;
    if (!l3_resize(preview, lap_cols(), lap_rows()))
        return;
    l3bench_frame(preview, 2, 0, NULL);
    l3_show(preview, 0, 0);
}
static void leave(void) {
    l3_destroy(preview);
    preview = NULL;
}
const lap_icon_px_t lap_icon_px_lap3d = {0};
static const lap_app_t app = {.magic = LAP_MAGIC,
                              .api_major = LAP_API_MAJOR,
                              .api_minor = LAP_API_MINOR,
                              .id = "lap3d",
                              .name = "LAP3D",
                              .version = "1.0",
                              .enter = enter,
                              .draw = draw,
                              .leave = leave};
const lap_app_t *lap_main_lap3d(void) { return &app; }
