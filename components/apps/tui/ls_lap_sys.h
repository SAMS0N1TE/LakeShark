/* The board behind API 1.1: motion, touch, sound, position, mesh, Wi-Fi,
 * Bluetooth and the SD card for native apps. The lap_* calls themselves run
 * on the screen task inside an app callback; anything that can block (the
 * mesh, the card) is done by the lap_io worker in ls_lap_sys_service and
 * handed over through copies, so a frame never waits on a radio or a card. */
#ifndef LS_LAP_SYS_H
#define LS_LAP_SYS_H
#include "ls_lap_manifest.h"
#include "ls_tui.h"
#ifdef __cplusplus
extern "C" {
#endif
/* Screen task. begin when the app is entered, frame before each draw with
 * the app's cell area, end when it closes (restores the screen, brightness,
 * rotation and scans; abandons queued jobs). */
void ls_lap_sys_begin(void);
void ls_lap_sys_frame(tui_rect area);
void ls_lap_sys_end(void);
bool ls_lap_sys_immersive(void);      /* the app took the whole screen, close button too */
/* lap_io worker: mesh refresh and sends, card jobs. True when it did work
 * and wants to be called again soon. */
bool ls_lap_sys_service(void);
void ls_lap_sys_set_wake(void (*wake)(void));

#if !defined(ESP_PLATFORM) || defined(LS_LAP_HOST_TEST)
/* The bench's stand-in for the hardware: what the sensors say. */
typedef struct {
    bool motion;
    lap_motion_t m;
    bool down;
    int x, y, x0, y0;
    uint32_t held_ms;
    int swipe;
    bool gps;
    lap_gps_t g;
    lap_time_t t;
    bool battery;
    lap_battery_t b;
    int mesh_nodes;
    lap_mesh_node_t nodes[12];
    lap_mesh_self_t self;
    int wifi_count;
    lap_wifi_ap_t aps[16];
    int ble_count;
    lap_ble_dev_t ble[24];
    int tones;                  /* notes queued so far */
    int last_tone_hz;
} ls_lap_sys_host_t;
ls_lap_sys_host_t *ls_lap_sys_host(void);
/* A finger at grid pixel gx, gy (cells cw x ch px), for the simulator's
   terminal mouse: made relative to the open app's area. */
void ls_lap_sys_host_finger(int gx, int gy, int cw, int ch, bool down);
#endif
#ifdef __cplusplus
}
#endif
#endif
