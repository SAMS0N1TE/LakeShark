#include "ls_lap_net_audio.h"
#include "ls_lap_dsp.h"
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
 *   N:radio=none|sim    the board has no radio (a Nano without an SDR), or the SIM one
 *   N:page=TEXT         a pager decode on the board's timeline (aprs=, ais=, mesh= too)
 *   N:key=C             a key: a letter, or @up @down @left @right @enter @tab
 *   N:keydown=C / N:keyup=C   held key edges; named keys use @left, @enter, etc.
 *   N:dialogtext=TEXT / N:dialogpick=INDEX / N:dialogcancel
 * LAPSIM_HW=wifi,ble,gps,imu,sd,desktop sets the exact hardware inventory.
 * LAPSIM_PROFILE=1 measures just app->draw with a high-resolution host clock,
 * reporting batches of 100 frames. It excludes BMP capture and presentation.
 * Without LAPSIM it is the plain fallback the board's host stub draws. */
#include "ls_lap.h"
#include "ls_lap_api.h"
#include "ls_lap_sys.h"
#include "ls_lap_system.h"
#include "ls_lap_sensors.h"
#include "ls_lap_radio.h"
#include "ls_lap_radio_io.h"
#include "ls_lap_service.h"
#include "ls_lap_net.h"
#include "ls_lap_kit.h"
#include "ls_tui_screen.h"
#include "ls_app.h"
#include "ls_icons.h"
#include "esp_timer.h"
#include "adsb_demo.h"
#include "core/ls_decoded.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <math.h>
#include <time.h>
#ifdef _WIN32
#include <windows.h>
#include <direct.h>
#endif

typedef const lap_app_t *(*lap_main_fn)(void);
extern bool ls_lap_radio_sim_absent;
typedef struct { const char *id; lap_main_fn main; const lap_icon_px_t *icon; } lapsim_app_t;
extern const lapsim_app_t lapsim_apps[];
extern const int lapsim_app_count;
extern uint32_t (*ls_lap_api_host_clock)(void);

static const lap_app_t *resident_apps[LS_LAP_CONTEXTS];
#define app resident_apps[ls_lap_context_index()]
static int frame_no;
#if defined(LAP_API_23)
static ls_input_ev_t scripted_pointer;
static void pointer_event(ls_input_ev_t ev) {
    if (ev.kind == LS_IEV_PTR_DOWN) ev.buttons |= 1u << ev.button;
    if (ev.kind == LS_IEV_PTR_UP) ev.buttons &= ~(1u << ev.button);
    scripted_pointer = ev;
    /* Keep the legacy finger snapshot aligned: sys_frame must not synthesize
       a release immediately after an explicit pointer-down edge. */
    ls_lap_sys_host_t *h = ls_lap_sys_host();
    bool down = (ev.buttons & LAP_BUTTON_LEFT) != 0;
    if (down && !h->down) { h->x0=ev.px; h->y0=ev.py; h->held_ms=0; }
    h->down=down; h->x=ev.px; h->y=ev.py;
    ls_lap_input_event(&ev, 0, 0);
}
#endif
static bool resident_hidden[LS_LAP_CONTEXTS],resident_left[LS_LAP_CONTEXTS];
#define hidden_screen resident_hidden[ls_lap_context_index()]
#define left_screen resident_left[ls_lap_context_index()]
static void shutdown_app(void);
static void hide_app(void);
static void show_app(void);
static double profile_us(void);
static uint32_t clock_ms(void);
static bool measuring_service;
static double service_anchor;
static int64_t service_clock(void) {
    return (int64_t)clock_ms()*1000 + (measuring_service ? (int64_t)(profile_us()-service_anchor) : 0);
}
static void worker_pass(void) {
    ls_lap_instance_t *old=ls_lap_instance_current();
    for(unsigned i=0;i<LS_LAP_CONTEXTS;i++) if(ls_lap_instance_at(i)) {
        ls_lap_instance_bind(ls_lap_instance_at(i));
        ls_lap_api_log_service(); ls_lap_sys_service();
    }
    ls_lap_instance_bind(old);
}
/* Diagnostics wrap the shared firmware validators: readable accepts the
   active callback stack; owns is reserved for retained SD destinations. */
extern const char *ls_lap_sim_call;
bool __real_ls_lap_api_owns(const void *, size_t);
bool __wrap_ls_lap_api_owns(const void *p, size_t n) {
    bool ok = __real_ls_lap_api_owns(p, n);
    if (!ok) fprintf(stderr, "simulator contract: %s EINVAL: pointer is outside app image/tracked allocation (stack storage is not owned)\n", ls_lap_sim_call);
    return ok;
}
bool __real_ls_lap_api_readable(const void *, size_t);
bool __wrap_ls_lap_api_readable(const void *p, size_t n) {
    bool ok = __real_ls_lap_api_readable(p, n);
    if (!ok) fprintf(stderr, "simulator contract: %s EINVAL: synchronous pointer outside app image/tracked allocation/active stack\n", ls_lap_sim_call);
    return ok;
}
static bool set_app_image(void) {
#ifdef _WIN32
    HMODULE mod = NULL;
    if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           (LPCSTR)app, &mod)) return false;
    const IMAGE_DOS_HEADER *dos = (const IMAGE_DOS_HEADER *)mod;
    const IMAGE_NT_HEADERS *nt = (const IMAGE_NT_HEADERS *)((const char *)mod + dos->e_lfanew);
    ls_lap_api_set_image(mod, nt->OptionalHeader.SizeOfImage);
    if (!getenv("LAPSIM_DLL")) fprintf(stderr, "simulator limit: app image: statically linked apps share the executable image; untracked heap and retained stack pointers remain excluded\n");
    return true;
#else
    extern char __executable_start[], _end[];
    ls_lap_api_set_image(__executable_start, (size_t)(_end - __executable_start));
    return true;
#endif
}
/* Called by the normal UI polling path even after the APP screen closes. */
static void poll_selected(void) {
    if (!app) return;
    worker_pass();
    service_anchor = profile_us(); measuring_service = true;
    int r = ls_lap_service_poll();
    measuring_service = false;
    if (r < 0 || ls_lap_service_take_stop()) {
        void (*stopped)(void) = ls_lap_service_off(ls_lap_service_why());
        if (hidden_screen && stopped) { ls_lap_call_begin(); stopped(); ls_lap_call_end(); }
        if (hidden_screen) shutdown_app();
    }
}

void __wrap_ls_lap_poll(void) {
    ls_lap_instance_t *old=ls_lap_instance_current(); static unsigned first;
    for(unsigned n=0;n<LS_LAP_CONTEXTS;n++) {
        unsigned i=(first+n)%LS_LAP_CONTEXTS;
        if(ls_lap_instance_at(i)) { ls_lap_instance_bind(ls_lap_instance_at(i)); poll_selected(); }
    }
    first=(first+1)%LS_LAP_CONTEXTS; ls_lap_instance_bind(old);
}
static void shutdown_all(void) {
    for(unsigned i=0;i<LS_LAP_CONTEXTS;i++) if(ls_lap_instance_at(i)) {
        ls_lap_instance_bind(ls_lap_instance_at(i)); shutdown_app();
    }
}


#if defined(_WIN32) && defined(LSSIM_DYNAMIC_APPS)
/* One app per host process. Keep its DLL loaded until process exit: service
   callbacks and renderer-owned pointers can outlive the APP screen. */
static const lap_app_t *dynamic_app(void)
{
    static const lap_app_t *loaded;
    wchar_t wide[32768];
    DWORD n = GetEnvironmentVariableW(L"LAPSIM_DLL", wide, 32768);
    if (!n) return NULL;
    if (loaded) return loaded;
    if (n >= 32768) { fprintf(stderr, "App DLL path is too long\n"); exit(2); }
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
    HMODULE dll = LoadLibraryExW(wide, NULL, LOAD_WITH_ALTERED_SEARCH_PATH);
    lap_main_fn main = dll ? (lap_main_fn)(uintptr_t)GetProcAddress(dll, "lap_main") : NULL;
    if (!main) {
        fprintf(stderr, "Cannot load app DLL (Windows error %lu)\n", GetLastError());
        exit(2);
    }
    loaded = main();
    if (!loaded || loaded->magic != LAP_MAGIC) {
        fprintf(stderr, "Invalid app DLL\n");
        exit(2);
    }
    return loaded;
}
#else
static const lap_app_t *dynamic_app(void) { return NULL; }
#endif

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

/* Live Studio input, in displayed screen axes: +x drags right, +y down.
   Update the same host sample lap_motion reads for linked and DLL apps.
   The simulator clock makes release/shake work headlessly too. */
static struct {
    float x, y, from_x, from_y;
    uint32_t released, shaken;
    bool active, easing, shaking;
} motion;

static void motion_sample(void)
{
    if (!motion.active) return;  /* preserve legacy LAPSIM_SCRIPT fixtures */
    const uint32_t now = clock_ms();
    if (motion.easing) {
        float t = (now - motion.released) / 300.0f;
        if (t >= 1) { t = 1; motion.easing = false; }
        const float k = (1 - t) * (1 - t) * (1 - t);
        motion.x = motion.from_x * k; motion.y = motion.from_y * k;
    }
    const float angle = hypotf(motion.x, motion.y);
    const float rad = angle * 0.0174532925f;
    const float k = angle > 0 ? sinf(rad) / angle : 0;
    lap_motion_t m = { .ax = -motion.x * k, .ay = motion.y * k, .az = cosf(rad),
        .heading = -1, .true_heading = -1, .temp_c = 25 };
    m.roll = asinf(m.ax) * 57.29578f;
    m.pitch = asinf(m.ay) * 57.29578f;
    if (motion.shaking) {
        const uint32_t elapsed = now - motion.shaken;
        if (elapsed >= 240) motion.shaking = false;
        else {
            /* A hand shake jolts the board side to side as well as up and down,
               so apps that read only the tilt (ax/ay) feel it too. */
            const float s = (elapsed / 40 % 2) ? -1.0f : 1.0f;
            m.az += 1.4f * s; m.gz = 320 * s;
            m.ax += 0.8f * s; m.ay += 0.5f * ((elapsed / 60 % 2) ? -1.0f : 1.0f);
        }
    }
    ls_lap_sys_host()->motion = true;
    ls_lap_sys_host()->m = m;
}

/* Exact comma-separated hardware inventory; radio inventory is independent. */
static void hardware(const char *spec) {
    ls_lap_sys_host_t *h = ls_lap_sys_host();
    h->hardware = 0; h->motion = h->gps = false;
    static const struct { const char *name; uint32_t flag; } bits[] = {
        {"wifi", LAP_HW_WIFI}, {"ble", LAP_HW_BLE}, {"gps", LAP_HW_GPS},
        {"imu", LAP_HW_IMU}, {"sd", LAP_HW_SD}, {"speaker", LAP_HW_SPEAKER},
        {"mic", LAP_HW_MIC}, {"desktop", LAP_HW_DESKTOP},
        {"hidble", LAP_HW_HID_BLE}, {"gamepadble", LAP_HW_GAMEPAD_BLE},
        {"gamepadusb", LAP_HW_GAMEPAD_USB}, {"multitouch", LAP_HW_MULTITOUCH}};
    for (const char *p = spec; p && *p;) {
        const char *end = strchr(p, ','); size_t n = end ? (size_t)(end-p) : strlen(p);
        for (unsigned i=0; i<sizeof(bits)/sizeof(bits[0]); i++)
            if (strlen(bits[i].name)==n && !strncmp(p,bits[i].name,n)) h->hardware |= bits[i].flag;
        p = end ? end+1 : NULL;
    }
    h->motion = (h->hardware & LAP_HW_IMU) != 0;
    h->gps = (h->hardware & LAP_HW_GPS) != 0;
}
static lap_key_t script_key(const char *s) {
    static const struct { const char *name; lap_key_t key; } keys[] = {
        {"@up",LAP_KEY_UP},{"@down",LAP_KEY_DOWN},{"@left",LAP_KEY_LEFT},
        {"@right",LAP_KEY_RIGHT},{"@enter",LAP_KEY_ENTER},{"@tab",LAP_KEY_TAB},
        {"@esc",LAP_KEY_BACK},{"@backspace",LAP_KEY_BACKSPACE}};
    for (unsigned i=0;i<sizeof(keys)/sizeof(keys[0]);i++) if (!strcmp(s,keys[i].name)) return keys[i].key;
    return LAP_KEY_CHAR;
}
/* One LAPSIM_SCRIPT step, the part after "N:". */
static void step(const char *c)
{
    if (!app) return;
    if(ls_lap_dsp_script(c)) return;
    if(!strcmp(c,"services")) {
        ls_lap_service_info_t rows[LS_LAP_CONTEXTS]; int n=ls_lap_services(rows,LS_LAP_CONTEXTS);
        fprintf(stderr,"services count=%d\n",n);
        for(int i=0;i<n;i++) fprintf(stderr,"service id=%s shown=%d ticks=%lu\n",rows[i].id,rows[i].shown,(unsigned long)rows[i].stats.ticks);
        return;
    }
    if(!strncmp(c,"service_stop=",13)) { ls_lap_service_request_stop(c+13); return; }
    if (ls_lap_system_script(c)) return;
    if (ls_lap_sensors_script(c)) return;
    if (ls_lap_rx24_sim_step(c)) return;
    if (ls_lap_io_sim_command(c)) return;
    if (!strncmp(c,"caps=",5)) { hardware(c+5); return; }
    if (!strncmp(c,"dialogtext=",11)) { ls_lap_dialog_answer(c+11,0,false); return; }
    if (!strncmp(c,"dialogpick=",11)) { ls_lap_dialog_answer(NULL,atoi(c+11),false); return; }
    if (!strcmp(c,"dialogcancel")) { ls_lap_dialog_answer(NULL,0,true); return; }
#if defined(LAP_API_23)
    /* API 2.3 script input uses the same producer channel as the device. */
    int a, b, buttons = 0, button = 0;
    ls_input_ev_t ev = {.clicks = 1};
    if (sscanf(c, "focus=%d", &a) == 1) {
        if (!a) { scripted_pointer.buttons=0; ls_lap_sys_host()->down=false; }
        ls_lap_lifecycle(a ? LAP_INPUT_FOCUS_GAINED : LAP_INPUT_FOCUS_LOST, 0, 0); return;
    }
    if (sscanf(c, "resize=%d,%d", &a, &b) == 2) {
        ls_lap_lifecycle(LAP_INPUT_RESIZE, a, b); return;
    }
    if (!strncmp(c, "keydown=", 8) || !strncmp(c, "keyup=", 6)) {
        bool down = c[3] == 'd';
        ev.kind = down ? LS_IEV_KEY_DOWN : LS_IEV_KEY_UP;
        const char *text = c + (down ? 8 : 6);
        ev.key = script_key(text); ev.ch = ev.key == LAP_KEY_CHAR ? *text : 0;
        ls_lap_input_event(&ev, 0, 0); return;
    }
    if (sscanf(c, "pointer=%d,%d,%d", &a, &b, &buttons) >= 2) {
        ev.kind = LS_IEV_PTR_MOVE; ev.px = a; ev.py = b; ev.buttons = buttons;
        pointer_event(ev); return;
    }
    buttons=scripted_pointer.buttons;
    if (sscanf(c, "buttondown=%d,%d,%d,%d", &a, &b, &button, &buttons) >= 3 ||
        sscanf(c, "buttonup=%d,%d,%d,%d", &a, &b, &button, &buttons) >= 3) {
        ev.kind = c[6] == 'd' ? LS_IEV_PTR_DOWN : LS_IEV_PTR_UP;
        if (button < 0 || button > 2) return;
        ev.px = a; ev.py = b; ev.button = button; ev.buttons = buttons;
        pointer_event(ev); return;
    }
    if (sscanf(c, "wheel=%d", &a) == 1) {
        ev=scripted_pointer; ev.kind = LS_IEV_WHEEL; ev.wheel = a;
        pointer_event(ev); return;
    }
#endif
    if (!strcmp(c, "hide")) { hide_app(); return; }
    if (!strcmp(c, "show")) { show_app(); return; }
    if (!strcmp(c, "stop") || !strcmp(c, "switch")) {
        void (*stopped)(void) = ls_lap_service_off("simulator stop/app switch");
        if (hidden_screen && stopped) { ls_lap_call_begin(); stopped(); ls_lap_call_end(); }
        shutdown_app(); return;
    }
    if (!strcmp(c, "revoke")) {
        /* Device revocation ends affected sessions/services before unload. */
        shutdown_app(); return;
    }
    ls_lap_sys_host_t *h = ls_lap_sys_host();
    {
        {
            float ax = 0, ay = 0, az = 1;
            int x, y;
            char d;
            if (sscanf(c, "motion=%f,%f", &ax, &ay) == 2 && isfinite(ax) && isfinite(ay)) {
                const float n = hypotf(ax, ay), k = n > 45 ? 45 / n : 1;
                motion.x = ax * k; motion.y = ay * k;
                motion.active = true; motion.easing = false;
                motion_sample();
            } else if (!strcmp(c, "release")) {
                motion_sample();
                motion.from_x = motion.x; motion.from_y = motion.y;
                motion.released = clock_ms(); motion.easing = true;
            } else if (!strcmp(c, "level")) {
                memset(&motion, 0, sizeof(motion)); motion.active = true;
                motion_sample();
            } else if (!strcmp(c, "shake")) {
                motion.active = true; motion.shaking = true; motion.shaken = clock_ms();
                motion_sample();
            } else if (sscanf(c, "tilt=%f,%f,%f", &ax, &ay, &az) >= 2) {
                memset(&motion, 0, sizeof(motion));
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
            } else if (!strncmp(c, "key=", 4)) {   /* key=q a letter; key=@up @down @left @right @enter @tab */
                lap_key_t k = LAP_KEY_CHAR;
                if (!strncmp(c + 4, "@tab", 4)) k = LAP_KEY_TAB;
                else if (!strncmp(c + 4, "@up", 3)) k = LAP_KEY_UP;
                else if (!strncmp(c + 4, "@down", 5)) k = LAP_KEY_DOWN;
                else if (!strncmp(c + 4, "@left", 5)) k = LAP_KEY_LEFT;
                else if (!strncmp(c + 4, "@right", 6)) k = LAP_KEY_RIGHT;
                else if (!strncmp(c + 4, "@enter", 6)) k = LAP_KEY_ENTER;
                ls_lap_api_callback(true);
#if defined(LAP_API_23)
                const bool pulse = ls_lap_input_key(k, k == LAP_KEY_CHAR ? c[4] : 0);
#endif
                if (app->key) app->key(k, k == LAP_KEY_CHAR ? c[4] : 0);
#if defined(LAP_API_23)
                if (pulse) ls_lap_input_key_up(k, k == LAP_KEY_CHAR ? c[4] : 0);
#endif
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
            } else if (!strncmp(c, "radio=", 6)) {
                ls_lap_radio_sim_absent = !strncmp(c + 6, "none", 4);
            } else if (!strncmp(c, "page=", 5) || !strncmp(c, "aprs=", 5) || !strncmp(c, "ais=", 4) ||
                       !strncmp(c, "mesh=", 5)) {
                ls_decoded_t e;
                memset(&e, 0, sizeof(e));
                const char *eq = strchr(c, '=');
                e.kind = c[0] == 'p' ? LS_DECODED_PAGE : c[1] == 'p' ? LS_DECODED_APRS : c[1] == 'i' ? LS_DECODED_AIS : LS_DECODED_MESH;
                e.at_us = esp_timer_get_time();
                e.address = 1234567;
                snprintf(e.from, sizeof(e.from), "%s", c[0] == 'p' ? "POCSAG" : c[1] == 'p' ? "N0CALL-9" : c[1] == 'i' ? "SEA SPRITE" : "RIDGE RPT");
                snprintf(e.text, sizeof(e.text), "%s", eq + 1);
                ls_decoded_publish(&e);
            } else if (sscanf(c, "swipe=%c", &d) == 1) {
                h->swipe = d == 'U' ? LAP_SWIPE_UP : d == 'D' ? LAP_SWIPE_DOWN : d == 'L' ? LAP_SWIPE_LEFT : LAP_SWIPE_RIGHT;
            }
        }
    }
}

static void script(int frame)
{
    const char *s = getenv("LAPSIM_SCRIPT");
    while (s && *s) {
        const int n = atoi(s);
        const char *c = strchr(s, ':');
        const char *end = strchr(s, ';');
        if (!c) break;
        if (n == frame) {
            char one[1024];
            const size_t len = end && end > c ? (size_t)(end - c - 1) : strlen(c + 1);
            snprintf(one, sizeof(one), "%.*s", (int)(len < sizeof(one) ? len : sizeof(one) - 1), c + 1);
            ls_lap_av_script(one);
            step(one);
        }
        s = end ? end + 1 : NULL;
    }
    ls_lap_sys_host_t *h = ls_lap_sys_host();
    if (h->down) h->held_ms += 40;
}

/* lssim --feed (LakeShark Studio's sensor buttons): one step now, in the
   LAPSIM_SCRIPT words without the frame number, e.g. "tilt=0.3,-0.1". */
void ls_lap_sim_step(const char *spec)
{
    if (!app || !spec) return;
    step(spec);
}

/* With LAPSIM set, the USER tab's APP tile is that app: its name, line,
   colour and colour icon, as the board's directory shows it. */
void ls_lap_sim_tile(ls_app_t *tile)
{
    const lap_app_t *dyn = dynamic_app();
    if (dyn) {
        tile->name = dyn->name;
        tile->sub = dyn->sub;
        tile->hue = dyn->color;
        return;
    }
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

static void start_current(const char *want,bool dynamic)
{
    if (app && hidden_screen) { show_app(); return; }
    app = dynamic ? dynamic_app() : NULL;
    for (int i = 0; !app && want && i < lapsim_app_count; i++)
        if (!strcmp(want, lapsim_apps[i].id)) app = lapsim_apps[i].main();
    if (!app) return;
    ls_lap_api_host_clock = clock_ms;
    ls_lap_service_host_clock = service_clock;
    ls_lap_radio_sim_absent = getenv("LAPSIM_RADIO") && !strcmp(getenv("LAPSIM_RADIO"), "none");
    ls_lap_sys_host()->motion = true;
    ls_lap_sys_host()->m = (lap_motion_t){ .az = 1, .heading = -1, .true_heading = -1 };
    ls_lap_sys_host()->t = (lap_time_t){ 2026, 10, 23, 12, 0, 0, 5, 1792756800, true };
    ls_lap_api_begin(app->id, 1, clock_ms());
#ifdef _WIN32
    _mkdir("sdcard");
#else
    mkdir("sdcard", 0777);
#endif
    if (!ls_lap_api_files_load("sdcard/apps_data")) { fprintf(stderr, "simulator contract: file cache load failed\n"); exit(2); }
    hardware(getenv("LAPSIM_HW") ? getenv("LAPSIM_HW") : "wifi,ble,imu,sd,desktop");
    ls_lap_sys_begin();
    /* A headless consent answer represents the trusted loader prompt. Denial
       cancels the launch; it never starts an app with undeclared grants. */
    const char *consent = getenv("LAPSIM_CONSENT");
    if (app->flags && consent && !strcmp(consent, "deny")) {
        fprintf(stderr, "simulator consent: launch denied for %s\n", app->id);
        ls_lap_sys_end(); ls_lap_api_cleanup(); app = NULL; return;
    }
    if (app->flags) fprintf(stderr, "simulator limit: consent: scripted loader answer; device prompts, signature trust and persistent grants are not simulated\n");
    if (app->flags) fprintf(stderr, "simulator consent: %s declared=0x%02x approved (headless loader answer)\n", app->id, app->flags);
    ls_lap_net_begin((app->flags & LAP_CAP_NETWORK) != 0);
    ls_lap_radio_begin(app);
    ls_lap_radio_service(); /* inventory is ready before enter/caps */
    if (!set_app_image()) { fprintf(stderr, "simulator contract: cannot identify app image\n"); exit(2); }
    static bool registered;
    if (!registered) { atexit(shutdown_all); registered=true; }
    hidden_screen = left_screen = false;
#if defined(LAP_API_23)
    memset(&scripted_pointer, 0, sizeof(scripted_pointer));
#endif
    fprintf(stderr, "simulator limit: worker scheduling: deterministic frame/poll passes, not concurrent device tasks\n");
    frame_no = 0;
    memset(&motion, 0, sizeof(motion));
    if (app->enter) { ls_lap_api_callback(true); app->enter(); ls_lap_api_callback(false); }
    /* ADS-B apps exercise the real receiver's synthetic producer on the host. */
    if (!strcmp(app->radio, LAP_RADIO_ADSB)) {
        ls_lap_api_callback(true); lap_aircraft_demo(true); ls_lap_api_callback(false);
    }
}

static void enter(void) {
    static bool services_started;
    if(!services_started) {
        services_started=true;
        const char *spec=getenv("LAPSIM_SERVICES");
        while(spec&&*spec) {
            const char *end=strchr(spec,','); size_t n=end?(size_t)(end-spec):strlen(spec);
            if(n&&n<32) {
                char id[32]; memcpy(id,spec,n); id[n]=0;
                ls_lap_instance_t *p=ls_lap_instance_new();
                if(!p) break;
                ls_lap_instance_bind(p); start_current(id,false); hide_app();
            }
            spec=end?end+1:NULL;
        }
    }
    ls_lap_instance_bind(ls_lap_instance_at(0)); start_current(getenv("LAPSIM"),true);
}

static void hide_app(void) {
    if (!app || hidden_screen) return;
    if (app->leave) { ls_lap_call_begin(); app->leave(); ls_lap_call_end(); }
    left_screen = true;
    if (!ls_lap_service_on() || ls_lap_api_exit_requested()) { shutdown_app(); return; }
    hidden_screen = true;
#if defined(LAP_API_23)
    scripted_pointer.buttons=0; ls_lap_sys_host()->down=false;
#endif
    ls_lap_service_set_shown(false);
    ls_lap_sys_hide();
}
static void show_app(void) {
    if (!app || !hidden_screen) return;
    hidden_screen = left_screen = false;
    ls_lap_service_set_shown(true);
    ls_lap_kit_begin();
#if defined(LAP_API_23)
    ls_lap_input_enable(true);
#endif
    if (app->enter) { ls_lap_call_begin(); app->enter(); ls_lap_call_end(); }
}
static void shutdown_app(void) {
    if (!app) return;
    if (!left_screen && app->leave) { ls_lap_call_begin(); app->leave(); ls_lap_call_end(); }
    ls_lap_sys_end();
    /* End cancels jobs/closes sessions. The worker releases staged buffers
       and sockets before cached files flush and generation allocations free. */
    for (int i=0; i<8; i++) worker_pass();
    if (!ls_lap_api_files_flush()) fprintf(stderr, "simulator contract: file flush failed: %s\n", ls_lap_api_error());
    ls_lap_api_cleanup();
    app = NULL; hidden_screen = left_screen = false;
}
static void leave(void) { hide_app(); }

static void draw(tui_surface *sf, tui_rect area)
{
    if (!app) {
        tui_put_str(sf, area, area.x, area.y, ls_lap_status()->reason, TUI_DEFAULT_ATTR);
        return;
    }
    script(frame_no++);
    if (!app) return;
    worker_pass();
    __wrap_ls_lap_poll();
    if (!app || hidden_screen) return;
    motion_sample();
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
    worker_pass();
    if (ls_lap_api_exit_requested()) shutdown_app();
}

static bool key(ls_tk_t k, char ch)
{
    if (!app || k < LS_TK_CHAR || k > LS_TK_MUTE) return false;
    ls_lap_api_callback(true);
#if defined(LAP_API_23)
    const bool pulse = ls_lap_input_key(k, ch);
#endif
    bool used = app->key && app->key((lap_key_t)k, ch);
#if defined(LAP_API_23)
    if (pulse) ls_lap_input_key_up(k, ch);
#endif
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
