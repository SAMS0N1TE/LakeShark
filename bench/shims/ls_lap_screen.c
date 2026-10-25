/* lssim's APP screen. With LAPSIM=<id> it runs that example app, compiled
 * for the host (bench/CMakeLists.txt renames each app's lap_main), through
 * the real app API and the real renderer, so its frames can be looked at
 * before a board runs it:
 *
 *   LAPSIM=dice LSSIM_FRAMES=out lssim lap -A 40 -f 60
 *
 * LAPSIM_SCRIPT drives the sensors by frame, ';' between steps:
 *   N:tilt=AX,AY[,AZ]   the board's up vector from frame N on
 *   N:down=X,Y          a finger at app pixel X,Y (held until up)
 *   N:up                lift it
 *   N:swipe=U|D|L|R     a finished swipe
 *   N:mesh              the mesh is up, transmit armed, three nodes heard
 *   N:rx=K              K more packets heard
 *   N:ble=K             K Bluetooth devices about
 *   N:hour=H            the clock reads H:00 (default noon)
 *   N:pet=NAME          MESHLING preview: egg, hatchling, sprout, relay, swarm,
 *                      scanner, nexus, drifter, relic; or happy, hungry, sleep,
 *                      chomp, dex, room, save (host only, real branch inputs)
 * LAPSIM_PROFILE=1 measures just app->draw with a high-resolution host clock,
 * reporting batches of 100 frames. It excludes BMP capture and presentation.
 * Without LAPSIM it is the plain fallback the board's host stub draws. */
#include "ls_lap.h"
#include "ls_lap_api.h"
#include "ls_lap_sys.h"
#include "ls_tui_screen.h"
#include "ls_app.h"
#include "ls_icons.h"
#include "esp_timer.h"
#include "adsb_demo.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <math.h>
#include <time.h>
#ifdef _WIN32
#include <windows.h>
#endif

typedef const lap_app_t *(*lap_main_fn)(void);
typedef struct { const char *id; lap_main_fn main; const lap_icon_px_t *icon; } lapsim_app_t;
extern const lapsim_app_t lapsim_apps[];
extern const int lapsim_app_count;
extern uint32_t (*ls_lap_api_host_clock)(void);

static const lap_app_t *app;
static int frame_no;
extern void meshling_preview(const char *command);
extern void carrier_preview(const char *command);

static double profile_us(void)
{
#ifdef _WIN32
    LARGE_INTEGER count, freq;
    QueryPerformanceCounter(&count);
    QueryPerformanceFrequency(&freq);
    return (double)count.QuadPart * 1000000.0 / (double)freq.QuadPart;
#else
    struct timespec ts;
    timespec_get(&ts, TIME_UTC);
    return ts.tv_sec * 1000000.0 + ts.tv_nsec / 1000.0;
#endif
}

static uint32_t clock_ms(void) { return (uint32_t)(esp_timer_get_time() / 1000); }

static void script(int frame)
{
    const char *s = getenv("LAPSIM_SCRIPT");
    ls_lap_sys_host_t *h = ls_lap_sys_host();
    while (s && *s) {
        int n = atoi(s);
        const char *c = strchr(s, ':');
        const char *end = strchr(s, ';');
        if (!c) break;
        c++;
        if (n == frame) {
            float ax = 0, ay = 0, az = 1;
            int x, y;
            char d;
            if (sscanf(c, "tilt=%f,%f,%f", &ax, &ay, &az) >= 2) {
                h->motion = true;
                const float len = sqrtf(ax * ax + ay * ay + az * az);
                h->m.ax = ax / len; h->m.ay = ay / len; h->m.az = az / len;
                h->m.pitch = asinf(h->m.ay) * 57.29578f;
                h->m.roll = asinf(h->m.ax) * 57.29578f;
                h->m.heading = h->m.true_heading = -1;
            } else if (sscanf(c, "down=%d,%d", &x, &y) == 2) {
                if (!h->down) { h->x0 = x; h->y0 = y; h->held_ms = 0; }
                h->down = true; h->x = x; h->y = y;
            } else if (!strncmp(c, "up", 2)) {
                h->down = false;
            } else if (!strncmp(c, "key=", 4) && app->key) {   /* key=q a letter; key=@up @down @left @right @enter */
                lap_key_t k = LAP_KEY_CHAR;
                if (!strncmp(c + 4, "@up", 3)) k = LAP_KEY_UP;
                else if (!strncmp(c + 4, "@down", 5)) k = LAP_KEY_DOWN;
                else if (!strncmp(c + 4, "@left", 5)) k = LAP_KEY_LEFT;
                else if (!strncmp(c + 4, "@right", 6)) k = LAP_KEY_RIGHT;
                else if (!strncmp(c + 4, "@enter", 6)) k = LAP_KEY_ENTER;
                ls_lap_api_callback(true);
                app->key(k, k == LAP_KEY_CHAR ? c[4] : 0);
                ls_lap_api_callback(false);
            } else if (!strncmp(c, "mesh", 4)) {
                static const char *const NAMES[] = { "RIDGE RPT", "KD9XYZ", "BASECAMP" };
                h->self = (lap_mesh_self_t){ .running = true, .tx_armed = true, .rx_packets = 10 };
                snprintf(h->self.name, sizeof(h->self.name), "LAKESHARK");
                h->mesh_nodes = 3;
                for (int i = 0; i < 3; i++) {
                    h->nodes[i] = (lap_mesh_node_t){ .rssi = -60.0f - 15 * i, .snr = 8, .heard_s = 30u * i, .type = 1 };
                    snprintf(h->nodes[i].id, sizeof(h->nodes[i].id), "%016X", 0xA1B2C3D4u + i);
                    snprintf(h->nodes[i].name, sizeof(h->nodes[i].name), "%s", NAMES[i]);
                }
            } else if (sscanf(c, "rx=%d", &x) == 1) {
                h->self.rx_packets += (uint32_t)x;
            } else if (sscanf(c, "ble=%d", &x) == 1) {
                h->ble_count = x > 24 ? 24 : x;
                for (int i = 0; i < h->ble_count; i++) h->ble[i] = (lap_ble_dev_t){ .addr = { 1, 2, 3, 4, 5, (uint8_t)i }, .rssi = -70, .age_ms = 1000 };
            } else if (sscanf(c, "hour=%d", &x) == 1) {
                h->t.hour = x;
            } else if (!strncmp(c, "pet=", 4) && !strcmp(app->id, "meshling")) {
                char command[24];
                if (sscanf(c + 4, "%23[^;]", command) == 1) {
                    ls_lap_api_callback(true);
                    meshling_preview(command);
                    ls_lap_api_callback(false);
                }
            } else if (!strncmp(c, "carrier=", 8) && !strcmp(app->id, "carrier")) {
                char command[24];
                if (sscanf(c + 8, "%23[^;]", command) == 1) {
                    ls_lap_api_callback(true);
                    carrier_preview(command);
                    ls_lap_api_callback(false);
                }
            } else if (sscanf(c, "swipe=%c", &d) == 1) {
                h->swipe = d == 'U' ? LAP_SWIPE_UP : d == 'D' ? LAP_SWIPE_DOWN : d == 'L' ? LAP_SWIPE_LEFT : LAP_SWIPE_RIGHT;
            }
        }
        s = end ? end + 1 : NULL;
    }
    if (h->down) h->held_ms += 40;
}

/* With LAPSIM set, the USER tab's APP tile is that app: its name, line,
   colour and colour icon, as the board's directory shows it. */
void ls_lap_sim_tile(ls_app_t *tile)
{
    const char *want = getenv("LAPSIM");
    for (int i = 0; want && i < lapsim_app_count; i++) {
        if (strcmp(want, lapsim_apps[i].id)) continue;
        const lap_app_t *a = lapsim_apps[i].main();
        tile->name = a->name;
        tile->sub = a->sub;
        tile->hue = a->color;
        if (lapsim_apps[i].icon) {
            static uint8_t px[LAP_PX_H][LAP_PX_W];
            for (int y = 0; y < LAP_PX_H; y++)
                for (int x = 0; x < LAP_PX_W; x++) {
                    const char c = lapsim_apps[i].icon->rows[y][x];
                    px[y][x] = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : 0xFF;
                }
            tile->icon = ls_icon_px(0, px, tile->icon);
        }
        return;
    }
}

static void enter(void)
{
    const char *want = getenv("LAPSIM");
    app = NULL;
    for (int i = 0; want && i < lapsim_app_count; i++)
        if (!strcmp(want, lapsim_apps[i].id)) app = lapsim_apps[i].main();
    if (!app) return;
    ls_lap_api_host_clock = clock_ms;
    ls_lap_sys_host()->motion = true;
    ls_lap_sys_host()->m = (lap_motion_t){ .az = 1, .heading = -1, .true_heading = -1 };
    ls_lap_sys_host()->t = (lap_time_t){ 2026, 10, 23, 12, 0, 0, 5, 1792756800, true };
    ls_lap_api_begin(app->id, 1, clock_ms());
    ls_lap_api_files_load("lapsim-data");
    ls_lap_sys_begin();
    frame_no = 0;
    if (app->enter) { ls_lap_api_callback(true); app->enter(); ls_lap_api_callback(false); }
    /* ADS-B apps exercise the real receiver's synthetic producer on the host. */
    if (!strcmp(app->radio, LAP_RADIO_ADSB)) {
        ls_lap_api_callback(true); lap_aircraft_demo(true); ls_lap_api_callback(false);
    }
}

static void leave(void)
{
    if (!app) return;
    if (app->leave) { ls_lap_api_callback(true); app->leave(); ls_lap_api_callback(false); }
    ls_lap_sys_end();
    ls_lap_api_cleanup();
    app = NULL;
}

static void draw(tui_surface *sf, tui_rect area)
{
    if (!app) {
        tui_put_str(sf, area, area.x, area.y, ls_lap_status()->reason, TUI_DEFAULT_ATTR);
        return;
    }
    script(frame_no++);
    static int64_t demo_last;
    int64_t demo_now = esp_timer_get_time();
    if (adsb_demo_count() && (demo_now - demo_last >= 1000000 || demo_now < demo_last)) {
        adsb_demo_tick(); demo_last = demo_now;
    }
    ls_lap_sys_frame(area);
    ls_lap_api_draw_begin(sf, area);
    ls_lap_api_callback(true);
    const bool profiling = getenv("LAPSIM_PROFILE") != NULL;
    const double start = profiling ? profile_us() : 0;
    if (app->draw) app->draw();
    if (profiling) {
        static double total, peak;
        static int samples;
        const double us = profile_us() - start;
        total += us;
        if (us > peak) peak = us;
        if (++samples == 100) {
            printf("lapsim draw: mean %.1f us, max %.1f us (100 frames)\n", total / samples, peak);
            samples = 0; total = peak = 0;
        }
    }
    ls_lap_api_callback(false);
    ls_lap_api_draw_end();
}

static bool key(ls_tk_t k, char ch)
{
    if (!app || !app->key || k < LS_TK_CHAR || k > LS_TK_MIC_HOLD) return false;
    ls_lap_api_callback(true);
    bool used = app->key((lap_key_t)k, ch);
    ls_lap_api_callback(false);
    return used;
}

static tui_rect last_area;
static bool touch(int col, int row)
{
    if (!app || !app->touch) return false;
    ls_lap_api_callback(true);
    bool used = app->touch(col - last_area.x, row - last_area.y);
    ls_lap_api_callback(false);
    return used;
}

static void draw_tracked(tui_surface *sf, tui_rect area) { last_area = area; draw(sf, area); }

const ls_tui_screen_t ls_lap_screen = {.name = "APP", .hint = "ESC  HOME", .enter = enter, .leave = leave,
                                       .draw = draw_tracked, .key = key, .touch = touch};
