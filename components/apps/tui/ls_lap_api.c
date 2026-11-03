#define LS_LAP_CONTEXT_IMPLEMENTATION
#if defined(__linux__) && !defined(_GNU_SOURCE)
#define _GNU_SOURCE
#endif
/* See ls_lap_api.h. API calls have no effect outside a host callback. */
#include "ls_lap_api.h"
#include "ls_lap_ble_peripheral.h"
#include "../../../app-sdk/include/lap3d.h"
#include "../../../app-sdk/include/lakeshark_ui.h"
#include "ls_glyph.h"
#include "ls_lap_storage.h"
#include "ls_lap_radio.h"
#include "ls_lap_radio_io.h"
#include "ls_lap_sys.h"
#include "ls_lap_system.h"
#include "ls_lap_service.h"
#include "ls_lap_dsp.h"
#include "ls_lap_features.h"
#include "ls_lap_usage.h"
#include "ls_lap_sensors.h"
#include "ls_lap_net.h"
#include "ls_lap_kit.h"
#include "ls_value.h"
#include "apps/adsb/adsb_state.h"
#include "apps/adsb/adsb_demo.h"
#include "esp_timer.h"
#include <dirent.h>
#include <errno.h>
#include <limits.h>
#include <math.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include "ls_cold.h"
#include "ls_textpack.h"
#ifdef _WIN32
#include <windows.h>
#elif defined(__linux__)
#include <pthread.h>
#endif
#ifdef _WIN32
#include <direct.h>
#define MAKE_DIR(p) _mkdir(p)
#else
#define MAKE_DIR(p) mkdir(p, 0777)
#endif
#ifdef ESP_PLATFORM
#include "audio/tone.h"
#include "core/settings.h"
#include "esp_attr.h"
#include "esp_heap_caps.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#define ALLOC(n) heap_caps_malloc((n), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)
#define DEALLOC(p) heap_caps_free(p)
#define RESIZE(p, n) heap_caps_realloc((p), (n), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)
#else
#define EXT_RAM_BSS_ATTR
#define ALLOC(n) malloc(n)
#define DEALLOC(p) free(p)
#define RESIZE(p, n) realloc((p), (n))
#endif
typedef union block block;
union block {
    struct {
        block *next;
        size_t size;
        uint32_t gen;
    } h;
    max_align_t alignment;
};
typedef struct cache cache;
struct cache {
    cache *next;
    char name[49];
    unsigned char *data;
    size_t size, capacity;
    bool dirty, removed;
};
typedef struct {
    cache *file;
    size_t pos;
    char mode;
    int ticket;
} handle;
static int file_ticket;
static handle *file_handle(int ticket);
/* Slot-owned allocator, files, input, callback and log state. Selection is
 * task local; peripheral producers bind their explicit lease owner. */
struct ls_lap_instance {
    unsigned slot;
    struct {
    char id[32], error[96], missing[96], data_root[41];
    uint32_t generation, frame, started;
    bool callback, exit;
    tui_surface *surface;
    tui_rect area;
    bool image_called, image_active;
    tui_rect image_area;
    const uint16_t *image_pixels;
    int image_w, image_h;
    uint32_t image_serial;
    block *blocks;
    uintptr_t image_base;
    size_t image_bytes;
    uintptr_t stack_top;
    char log[8][160];
    atomic_uint log_head, log_tail;
    cache *files;
    handle fd[4];
#ifdef ESP_PLATFORM
    TaskHandle_t owner;
#endif
    } state;
    struct {
    bool enabled, focused, paused, locked;
    unsigned head, count;
    lap_input_state_t state;
    lap_input_event2_t ring[64];
    } input_state;
    lap_input_device_t devices[LAP_INPUT_DEVICES_MAX];
    lap_input_device_state_t device_states[LAP_INPUT_DEVICES_MAX];
    uint32_t topology_frame;
    atomic_size_t allocated;
    atomic_uint invalid_frees;
    atomic_bool flushing;
    void *system;
    ls_lap_service_context_t service_context;
};
EXT_RAM_BSS_ATTR static struct ls_lap_instance instance_pool[LS_LAP_CONTEXTS];
#define default_instance instance_pool[0]
static _Thread_local struct ls_lap_instance *active_instance = &default_instance;
static struct ls_lap_instance *instances[LS_LAP_CONTEXTS] = { &default_instance };
static atomic_uint leases[LS_LAP_LEASE_COUNT];
bool ls_lap_resource_claim_impl(unsigned kind, bool acquire) {
    if (kind>=LS_LAP_LEASE_COUNT) return false;
    unsigned owner=ls_lap_instance_slot()+1, expected=acquire?0:owner;
    return atomic_compare_exchange_strong(&leases[kind],&expected,acquire?owner:0) || (acquire&&expected==owner);
}
ls_lap_instance_t *ls_lap_resource_owner(unsigned kind) {
    unsigned owner=kind<LS_LAP_LEASE_COUNT?atomic_load(&leases[kind]):0;
    return owner?instances[owner-1]:NULL;
}
bool ls_lap_radio_control_allowed(void) {
    unsigned own=ls_lap_instance_slot()+1;
    const unsigned kinds[]={LS_LAP_LEASE_IQ,LS_LAP_LEASE_PACKET,LS_LAP_LEASE_LEGACY_RADIO};
    for(unsigned i=0;i<sizeof(kinds)/sizeof(kinds[0]);i++) {
        unsigned owner=atomic_load(&leases[kinds[i]]); if(owner&&owner!=own) return false;
    }
    return true;
}
unsigned ls_lap_producer_select_impl(unsigned kind) {
    unsigned old=ls_lap_instance_slot(); ls_lap_instance_t *p=ls_lap_resource_owner(kind);
    if(p) ls_lap_instance_bind(p);
    return old;
}
void ls_lap_producer_restore_impl(unsigned slot) { ls_lap_instance_bind(ls_lap_instance_at(slot)); }
#define runtime_state (active_instance->state)
#define runtime_input (active_instance->input_state)
#define input_devices (active_instance->devices)
#define input_states (active_instance->device_states)
#define input25_topology_frame (active_instance->topology_frame)
#define bytes_used (active_instance->allocated)
#define bad_frees (active_instance->invalid_frees)
#define files_busy (active_instance->flushing)
unsigned ls_lap_instance_slot(void) { return active_instance->slot; }
ls_lap_instance_t *ls_lap_instance_at(unsigned slot) { return slot < LS_LAP_CONTEXTS ? instances[slot] : NULL; }
ls_lap_instance_t *ls_lap_instance_bind(ls_lap_instance_t *p) {
    ls_lap_instance_t *old = active_instance;
    active_instance = p ? p : &default_instance;
    return old;
}
ls_lap_instance_t *ls_lap_instance_new(void) {
    for (unsigned i=1; i<LS_LAP_CONTEXTS; i++) if (!instances[i]) {
        struct ls_lap_instance *p = &instance_pool[i];
        memset(p,0,sizeof(*p)); p->slot=i; instances[i]=p; return p;
    }
    return NULL;
}
ls_lap_instance_t *ls_lap_instance_current(void) { return active_instance; }
bool ls_lap_instance_select(ls_lap_instance_t *instance) {
    if (runtime_state.callback || atomic_load(&files_busy)) return false;
    active_instance = instance ? instance : &default_instance;
    return true;
}
bool ls_lap_instance_delete(ls_lap_instance_t *instance) {
    if (!instance || instance == &default_instance || instance == active_instance ||
        instance->state.blocks || instance->state.generation || instance->system) return false;
    for(unsigned i=0;i<LS_LAP_LEASE_COUNT;i++) if(atomic_load(&leases[i])==instance->slot+1) return false;
    instances[instance->slot]=NULL;
    memset(instance,0,sizeof(*instance)); return true;
}
ls_lap_service_context_t *ls_lap_api_services(void) { return &active_instance->service_context; }
void *ls_lap_instance_service_context(unsigned slot) {
    return slot<LS_LAP_CONTEXTS && instances[slot]?&instances[slot]->service_context:NULL;
}
void *ls_lap_instance_extension_at(unsigned slot) { return slot<LS_LAP_CONTEXTS&&instances[slot]?instances[slot]->system:NULL; }
bool ls_lap_instance_service_info(unsigned slot, lap_service_info_t *out) {
    if(slot>=LS_LAP_CONTEXTS || !instances[slot]) return false;
    ls_lap_instance_t *p=instances[slot];
    ls_lap_service_context_t *c=&p->service_context;
    if(!c->state.running) return false;
    *out=(lap_service_info_t){.size=sizeof(*out),.version=1,.shown=c->state.shown,
        .period_ms=c->state.period_ms,.memory_bytes=atomic_load(&p->allocated),
        .memory_limit=LS_LAP_MAX_MEMORY,.tick_budget_us=LAP_SERVICE_BUDGET_US,.stats=c->state.st};
    ls_lap_usage_t usage[]={ls_lap_net_usage(slot),ls_lap_http_usage(slot),ls_lap_udp_usage(slot),ls_lap_mdns_usage(slot),
        ls_lap_ble_usage(slot),ls_lap_sound_usage(slot),ls_lap_radio_usage(slot),ls_lap_io_usage(slot),ls_lap_audio_usage(slot),ls_lap_system_usage(slot),ls_lap_dsp_usage(slot),ls_lap_sys_usage(slot)};
    out->pool_bytes=sizeof(*p); out->pool_limit=2u*1024*1024; out->handle_limit=64;
    for(unsigned i=0;i<sizeof(usage)/sizeof(usage[0]);i++) { out->pool_bytes+=usage[i].bytes; out->handles+=usage[i].handles; }
    for(unsigned i=0;i<4;i++) if(p->state.fd[i].file) out->handles++;
    snprintf(out->id,sizeof(out->id),"%s",c->state.id); snprintf(out->name,sizeof(out->name),"%s",c->state.name);
    return true;
}
void **ls_lap_api_extension(void) { return &active_instance->system; }
static void (*wake_io)(void);
void ls_lap_api_set_wake(void (*wake)(void)) { wake_io = wake; }
#ifndef ESP_PLATFORM
uint32_t (*ls_lap_api_host_clock)(void); /* the bench's clock, when it has one */
#endif
static uint32_t now(void) {
#ifdef ESP_PLATFORM
    return (uint32_t)(esp_timer_get_time() / 1000);
#else
    return ls_lap_api_host_clock ? ls_lap_api_host_clock() : 0;
#endif
}
static bool inside(void) {
    if(ls_lap_dsp_on_worker()) return false;
#ifdef ESP_PLATFORM
    return runtime_state.callback && xTaskGetCurrentTaskHandle() == runtime_state.owner;
#else
    return runtime_state.callback;
#endif
}
bool ls_lap_api_inside(void) { return inside(); }
void ls_lap_api_set_image(const void *base, size_t bytes) {
    runtime_state.image_base = (uintptr_t)base;
    runtime_state.image_bytes = bytes;
}
/* p..p+n lies wholly in one live allocation of this app, or in its loaded
   image (static buffers): somewhere a card job may copy into. */
static bool owns(const void *p, size_t n) {
    const uintptr_t a = (uintptr_t)p;
    if (!p || a + n < a)
        return false;
    if (runtime_state.image_bytes && a >= runtime_state.image_base && a + n <= runtime_state.image_base + runtime_state.image_bytes)
        return true;
    for (block *b = runtime_state.blocks; b; b = b->h.next) {
        const uintptr_t lo = (uintptr_t)(b + 1);
        if (b->h.gen == runtime_state.generation && a >= lo && a + n <= lo + b->h.size)
            return true;
    }
    return false;
}
bool ls_lap_api_owns(const void *p, size_t n) { return owns(p, n); }
bool ls_lap_api_readable(const void *p, size_t n) {
    if (!inside() || !p || (uintptr_t)p + n < (uintptr_t)p) return false;
    if (owns(p, n)) return true;
    volatile unsigned char marker = 0;
    uintptr_t lo = (uintptr_t)&marker, hi = runtime_state.stack_top;
#ifdef _WIN32
    MEMORY_BASIC_INFORMATION info;
    if (!VirtualQuery((const void *)&marker, &info, sizeof(info))) return false;
    hi = (uintptr_t)info.BaseAddress + info.RegionSize;
#elif defined(__linux__) && !defined(ESP_PLATFORM)
    pthread_attr_t attr; void *base; size_t bytes;
    if (pthread_getattr_np(pthread_self(), &attr)) return false;
    int rc = pthread_attr_getstack(&attr, &base, &bytes);
    pthread_attr_destroy(&attr);
    if (rc) return false;
    hi = (uintptr_t)base + bytes;
#endif
    return (uintptr_t)p >= lo && (uintptr_t)p + n <= hi;
}
bool ls_lap_copy_input(void *out, const void *p, size_t n) {
    if (n > 1024 || !ls_lap_api_readable(p, n)) return false;
    memcpy(out, p, n);
    return true;
}
static void error(const char *text) { snprintf(runtime_state.error, sizeof(runtime_state.error), "%s", text); }
/* Input is UI-task owned; no callback is invoked by producers. */
_Static_assert(sizeof(lap_input_event_t)==22 && sizeof(lap_input_state_t)==80,
               "API 2.3 input ABI must not grow");
_Static_assert((int)LAP_INPUT_CONTACT_UP==(int)LS_IEV_CONTACT_UP &&
               (int)LAP_INPUT_GAMEPAD==(int)LS_IEV_GAMEPAD,
               "public and producer input kinds must match");
#if !defined(ESP_PLATFORM) || defined(LS_LAP_HOST_TEST)
extern bool ls_tui_locked(void) __attribute__((weak));
#endif
static void input_push2(lap_input_event2_t e) {
    e.event.size = sizeof(e); e.event.version = 2;
    if (runtime_input.count == 64) {
        runtime_input.head = (runtime_input.head + 1) % 64; runtime_input.count--; runtime_input.state.dropped++;
    }
    runtime_input.ring[(runtime_input.head + runtime_input.count++) % 64] = e;
}
static void input_push(lap_input_event_t e) {
    input_push2((lap_input_event2_t){.event=e});
}
#include "ls_lap_input25.inc"
static unsigned input_key(unsigned key, char ch) {
    unsigned c = (unsigned char)ch;
    if (c >= 'A' && c <= 'Z') c += 'a' - 'A';
    return key == LAP_KEY_CHAR ? LAP_KEY_CHAR(c) : key;
}
bool ls_lap_input_active(void) { return runtime_input.enabled; }
void ls_lap_input_enable(bool on) {
    if (on == runtime_input.enabled) return;
    if (on) {
        runtime_input.enabled = true;
        runtime_input.state.size = sizeof(runtime_input.state); runtime_input.state.version = 1;
        ls_lap_lifecycle(LAP_INPUT_FOCUS_GAINED, 0, 0);
        ls_lap_lifecycle(LAP_INPUT_RESUME, 0, 0);
    } else {
        ls_lap_lifecycle(LAP_INPUT_FOCUS_LOST, 0, 0);
        ls_lap_lifecycle(LAP_INPUT_PAUSE, 0, 0);
        runtime_input.enabled = false;
    }
}
void ls_lap_lifecycle(unsigned kind, int w, int h) {
    if (kind == LAP_INPUT_FOCUS_LOST || kind == LAP_INPUT_PAUSE) ls_lap_peripheral_release_inputs();
    ls_lap_system_input(kind, w, h);
    if (!runtime_input.enabled || kind < LAP_INPUT_FOCUS_LOST || kind > LAP_INPUT_RESIZE) return;
    if ((kind == LAP_INPUT_FOCUS_LOST && !runtime_input.focused) ||
        (kind == LAP_INPUT_FOCUS_GAINED && runtime_input.focused) ||
        (kind == LAP_INPUT_PAUSE && runtime_input.paused) ||
        (kind == LAP_INPUT_RESUME && !runtime_input.paused)) return;
    if (kind == LAP_INPUT_FOCUS_LOST || kind == LAP_INPUT_PAUSE) {
        memset(runtime_input.state.keys, 0, sizeof(runtime_input.state.keys));
        runtime_input.state.buttons = runtime_input.state.mods = 0;
        runtime_input.focused = false;
        input25_clear();
    }
    if (kind == LAP_INPUT_FOCUS_GAINED) runtime_input.focused = true;
    if (kind == LAP_INPUT_PAUSE) runtime_input.paused = true;
    if (kind == LAP_INPUT_RESUME) runtime_input.paused = false;
    input_push((lap_input_event_t){.kind = kind, .width = w, .height = h});
}
void ls_lap_input_event(const ls_input_ev_t *ev, int ox, int oy) {
    if (!ev || !runtime_input.enabled || ev->kind > LS_IEV_CONTACT_UP) return;
    if (ev->kind > LS_IEV_RELEASE_ALL && ev->kind <= LS_IEV_RESIZE) {
        ls_lap_lifecycle(ev->kind, ev->px, ev->py); return;
    }
    if ((runtime_input.paused || ls_lap_dialog_active()) && ev->kind != LS_IEV_RELEASE_ALL) return;
    if (ev->kind == LS_IEV_RELEASE_ALL) {
        input25_clear();
        memset(runtime_input.state.keys, 0, sizeof(runtime_input.state.keys));
        runtime_input.state.buttons = runtime_input.state.mods = 0;
    } else if (!runtime_input.focused) ls_lap_lifecycle(LAP_INPUT_FOCUS_GAINED, 0, 0);
    if (input25_event(ev, ox, oy)) return;
    unsigned key = input_key(ev->key, ev->ch);
    if (key < 512 && (ev->kind == LS_IEV_KEY_DOWN || ev->kind == LS_IEV_KEY_UP)) {
        uint32_t mask = 1u << (key % 32);
        if (ev->kind == LS_IEV_KEY_DOWN) runtime_input.state.keys[key / 32] |= mask;
        else runtime_input.state.keys[key / 32] &= ~mask;
    }
    if (ev->kind >= LS_IEV_PTR_MOVE && ev->kind <= LS_IEV_WHEEL) {
        runtime_input.state.x = ev->px - ox; runtime_input.state.y = ev->py - oy;
        runtime_input.state.buttons = ev->buttons;
        if (ev->kind == LS_IEV_PTR_DOWN && ev->button < 8) runtime_input.state.buttons |= 1u << ev->button;
        if (ev->kind == LS_IEV_PTR_UP && ev->button < 8) runtime_input.state.buttons &= ~(1u << ev->button);
    }
    if(ev->kind>=LS_IEV_PTR_MOVE && ev->kind<=LS_IEV_WHEEL) {
        runtime_input.state.buttons=0;
        for(unsigned i=0;i<LAP_INPUT_DEVICES_MAX;i++)if(input_states[i].connected)
            runtime_input.state.buttons|=input_states[i].input.buttons;
    }
    runtime_input.state.mods = ev->kind == LS_IEV_RELEASE_ALL ? 0 : ev->mods;
    if(ev->kind<=LS_IEV_KEY_UP) {
        memset(runtime_input.state.keys,0,sizeof(runtime_input.state.keys));
        for(unsigned i=0;i<LAP_INPUT_DEVICES_MAX;i++)if(input_states[i].connected)
            for(unsigned k=0;k<LAP_INPUT_KEY_WORDS;k++)runtime_input.state.keys[k]|=input_states[i].input.keys[k];
    }
    uint32_t device=ev->device ? ev->device : 1u+ev->source;
    int slot=input25_slot(device,false);
    input_push2((lap_input_event2_t){.device=device,
        .classes=slot>=0?input_devices[slot].classes:ev->classes,
        .transport=slot>=0?input_devices[slot].transport:ev->transport,
        .event={.kind = ev->kind, .mods = runtime_input.state.mods,
        .button = ev->button, .buttons = runtime_input.state.buttons, .key = key,
        .x = runtime_input.state.x, .y = runtime_input.state.y, .wheel = ev->wheel,
        .clicks = ev->clicks, .ch = ev->ch}});
    if (ev->kind == LS_IEV_RELEASE_ALL) ls_lap_lifecycle(LAP_INPUT_FOCUS_LOST, 0, 0);
}
void ls_lap_input_board(const ls_input_ev_t *ev) {
#if defined(ESP_PLATFORM) && !defined(LS_LAP_HOST_TEST)
    if (ls_tui_locked()) {
#else
    if (ls_tui_locked && ls_tui_locked()) {
#endif
        runtime_input.locked = true;
        ls_lap_lifecycle(LAP_INPUT_FOCUS_LOST, 0, 0);
        ls_lap_lifecycle(LAP_INPUT_PAUSE, 0, 0);
        return;
    }
    if (runtime_input.locked) {
        runtime_input.locked = false;
        ls_lap_lifecycle(LAP_INPUT_FOCUS_GAINED, 0, 0);
        ls_lap_lifecycle(LAP_INPUT_RESUME, 0, 0);
    }
    int cw = 0, ch = 0; ls_tui_geometry(NULL, NULL, &cw, &ch);
    int ox = runtime_state.area.x * cw, oy = runtime_state.area.y * ch;
    if (((ev->kind >= LS_IEV_PTR_MOVE && ev->kind <= LS_IEV_WHEEL && !runtime_input.state.buttons) || ev->kind == LS_IEV_CONTACT_DOWN) && runtime_state.area.w) {
        int x = ev->px - ox, y = ev->py - oy;
        if (x < 0 || y < 0 || x >= runtime_state.area.w * cw || y >= runtime_state.area.h * ch) return;
    }
    ls_lap_input_event(ev, ox, oy);
}
bool lap_key_down(unsigned key) {
    return inside() && key < 512 && (runtime_input.state.keys[key / 32] & (1u << (key % 32)));
}
int lap_input_state(lap_input_state_t *out) {
    if (!inside() || !ls_lap_api_readable(out, sizeof(*out))) return LAP_RX_EINVAL;
    *out = runtime_input.state; return 0;
}
int lap_input_next(lap_input_event_t *out) {
    if (!inside() || !ls_lap_api_readable(out, sizeof(*out))) return LAP_RX_EINVAL;
    while (runtime_input.count && runtime_input.ring[runtime_input.head].event.kind > LAP_INPUT_RESIZE) {
        runtime_input.head = (runtime_input.head + 1) % 64; runtime_input.count--;
    }
    if (!runtime_input.count) return 0;
    *out = runtime_input.ring[runtime_input.head].event; out->size=sizeof(*out); out->version=1;
    runtime_input.head = (runtime_input.head + 1) % 64; runtime_input.count--;
    return 1;
}
void ls_lap_input_touch(int x, int y, bool down) {
    if (!runtime_input.enabled || !runtime_input.focused) return;
    int local=input25_slot(1u+LS_ISRC_LOCAL,false);
    uint8_t buttons=local>=0?input_states[local].input.buttons:0;
    bool was = (buttons & LAP_BUTTON_LEFT) != 0;
    /* A legacy finger must not release or duplicate another device's drag. */
    if(down && !was && (runtime_input.state.buttons & LAP_BUTTON_LEFT))return;
    if (down == was && x == runtime_input.state.x && y == runtime_input.state.y) return;
    if (!down && !was) return;
    ls_input_ev_t ev = {.kind = down == was ? LS_IEV_PTR_MOVE : down ? LS_IEV_PTR_DOWN : LS_IEV_PTR_UP,
        .source=LS_ISRC_LOCAL, .button = LS_IBTN_LEFT, .buttons = (buttons & ~1u) | (down ? 1 : 0),
        .px = x, .py = y, .clicks = 1};
    ls_lap_input_event(&ev, 0, 0);
}
/* Return true if a legacy producer needs a release after its key callback. */
bool ls_lap_input_key(unsigned key, char ch) {
    unsigned id = input_key(key, ch);
    if (id >= 512 || (runtime_input.state.keys[id / 32] & (1u << (id % 32)))) return false;
    ls_input_ev_t ev = {.kind = LS_IEV_KEY_DOWN, .key = key, .ch = ch, .clicks = 1};
    ls_lap_input_event(&ev, 0, 0); return true;
}
void ls_lap_input_key_up(unsigned key, char ch) {
    ls_input_ev_t ev = {.kind = LS_IEV_KEY_UP, .key = key, .ch = ch};
    ls_lap_input_event(&ev, 0, 0);
}
uint32_t lap_api_version(void) { return (2u << 16) | LS_LAP_API2_MINOR; }
int lap_caps(lap_caps_t *out, size_t size) {
    if (!inside() || size < sizeof(*out) || !ls_lap_api_readable(out, sizeof(*out))) return LAP_RX_EINVAL;
    lap_caps_t c = {.size = sizeof(c), .version = 1, .hardware = ls_lap_sys_caps(), .granted = ls_lap_granted()};
    if (!ls_lap_net_allowed()) c.granted &= ~LAP_CAP_NETWORK;
    ls_lap_radio_caps(&c);
    c.hardware |= ls_lap_io_caps() | ls_lap_input_hardware() | ls_lap_peripheral_caps();
    if (LAP_WORKER_DSP) c.hardware |= LAP_HW_WORKER_DSP;
    if (LAP_MDNS && (c.hardware & LAP_HW_WIFI)) c.hardware |= LAP_HW_MDNS;
    int cols = 0, rows = 0, cw = 0, ch = 0;
    ls_tui_geometry(&cols, &rows, &cw, &ch);
    c.width_px = cols * cw; c.height_px = rows * ch;
    c.screen_class = c.width_px >= 800 ? LAP_SCREEN_LARGE : c.width_px >= 400 ? LAP_SCREEN_MEDIUM : LAP_SCREEN_SMALL;
    *out = c; return 0;
}

void ls_lap_api_begin(const char *id, uint32_t gen, uint32_t opened_ms) {
    const uintptr_t image_base = runtime_state.image_base;
    const size_t image_bytes = runtime_state.image_bytes;
    memset(&runtime_state, 0, sizeof(runtime_state));
    runtime_state.image_base = image_base;
    runtime_state.image_bytes = image_bytes;
    snprintf(runtime_state.id, sizeof(runtime_state.id), "%s", id);
    memset(&runtime_input, 0, sizeof(runtime_input));
    memset(input_devices,0,sizeof(input_devices));
    memset(input_states,0,sizeof(input_states));
    input25_topology_frame=UINT32_MAX;
    runtime_input.paused = true;
    runtime_state.generation = gen;
    runtime_state.started = opened_ms;
    ls_lap_system_begin(id, gen);
    atomic_store(&files_busy, false);
    atomic_store(&bytes_used, 0);
    atomic_store(&bad_frees, 0);
    atomic_init(&runtime_state.log_head, 0);
    atomic_init(&runtime_state.log_tail, 0);
}
void ls_lap_api_callback(bool on) {
    if (on) runtime_state.stack_top = (uintptr_t)&on;
#ifdef ESP_PLATFORM
    runtime_state.owner = on ? xTaskGetCurrentTaskHandle() : NULL;
#endif
    runtime_state.callback = on;
}
void ls_lap_api_draw_begin(tui_surface *sf, tui_rect area) {
    if (runtime_state.area.w != area.w || runtime_state.area.h != area.h) {
        int cw = 0, ch = 0; ls_tui_geometry(NULL, NULL, &cw, &ch);
        ls_lap_lifecycle(LAP_INPUT_RESIZE, area.w * cw, area.h * ch);
    }
    runtime_state.surface = sf;
    runtime_state.area = tui_rect_intersect(area, tui_surface_rect(sf));
    runtime_state.frame++;
    runtime_state.image_called = false;
}
void ls_lap_api_draw_end(void) {
    if (runtime_state.surface && runtime_state.image_active && !runtime_state.image_called) {
        ls_tui_image(tui_rect_make(0, 0, 0, 0), NULL, 0, 0, 0);
        runtime_state.image_active = false;
    }
    runtime_state.surface = NULL;
}
bool ls_lap_api_exit_requested(void) { return runtime_state.exit; }
size_t ls_lap_api_bytes(void) { return bytes_used; }
unsigned ls_lap_api_bad_frees(void) { return bad_frees; }
const char *ls_lap_api_error(void) { return runtime_state.error; }
void *ls_lap_malloc(size_t n) {
    if(ls_lap_dsp_on_worker()) return NULL;
    if (!runtime_state.generation || !n)
        return NULL;
    if (n > LS_LAP_MAX_MEMORY || n + sizeof(block) > LS_LAP_MAX_MEMORY - bytes_used) {
        error(LS_TEXT(t_ls_lap_api_app_memory_cap_is, "app memory cap is 4 MB"));
        return NULL;
    }
    block *b = ALLOC(sizeof(block) + n);
    if (!b) {
        error(LS_TEXT(t_ls_lap_api_psram_allocation_refused, "PSRAM allocation refused"));
        return NULL;
    }
    b->h.next = runtime_state.blocks;
    b->h.size = n;
    b->h.gen = runtime_state.generation;
    runtime_state.blocks = b;
    bytes_used += n + sizeof(block);
    return b + 1;
}
void *ls_lap_calloc(size_t n, size_t size) {
    if (size && n > SIZE_MAX / size) {
        error("allocation overflow");
        return NULL;
    }
    void *p = ls_lap_malloc(n * size);
    if (p)
        memset(p, 0, n * size);
    return p;
}
static block **find(void *p) {
    block **b = &runtime_state.blocks;
    while (*b && ((void *)(*b + 1) != p || (*b)->h.gen != runtime_state.generation))
        b = &(*b)->h.next;
    return b;
}
void ls_lap_free(void *p) {
    if(ls_lap_dsp_on_worker()) return;
    if (!p)
        return;
    block **prev = find(p), *b = *prev;
    if (!b) {
        bad_frees++;
        return;
    }
    *prev = b->h.next;
    bytes_used -= b->h.size + sizeof(block);
    DEALLOC(b);
}
void *ls_lap_realloc(void *p, size_t n) {
    if(ls_lap_dsp_on_worker()) return NULL;
    if (!p)
        return ls_lap_malloc(n);
    block *b = *find(p);
    if (!b) {
        bad_frees++;
        return NULL;
    }
    if (!n) {
        ls_lap_free(p);
        return NULL;
    }
    if (n > LS_LAP_MAX_MEMORY ||
        n + sizeof(block) > LS_LAP_MAX_MEMORY - (bytes_used - b->h.size - sizeof(block))) {
        error(LS_TEXT(t_ls_lap_api_app_memory_cap_is, "app memory cap is 4 MB"));
        return NULL;
    }
    block **prev = find(p);
    size_t old = b->h.size;
    block *q = RESIZE(b, n + sizeof(block));
    if (!q) {
        error(LS_TEXT(t_ls_lap_api_psram_allocation_refused, "PSRAM allocation refused"));
        return NULL;
    }
    q->h.size = n;
    *prev = q;
    bytes_used = bytes_used - old + n;
    return q + 1;
}
void ls_lap_api_cleanup(void) {
    ls_lap_audio_reap();
    ls_lap_system_end();
    ls_lap_api_draw_end();
    runtime_state.callback = false;
    while (runtime_state.blocks)
        ls_lap_free(runtime_state.blocks + 1);
    runtime_state.files = NULL;
    memset(runtime_state.fd, 0, sizeof(runtime_state.fd));
    ls_lap_input_enable(false);
    ls_lap_dialog_forget();
    runtime_state.generation = 0;
    memset(&active_instance->service_context,0,sizeof(active_instance->service_context));
}
static bool draw(void) { return inside() && runtime_state.surface; }
bool ls_lap_api_canvas(tui_surface **sf, tui_rect *area) {
    if (!draw())
        return false;
    *sf = runtime_state.surface;
    *area = runtime_state.area;
    return true;
}
static int16_t cell(int16_t c) {
    return ((c >= 32 && c <= 126) || (c >= 0x80 && c <= 0x9f) || (c >= 0xc0 && c <= 0xff) ||
            (c >= 0x2800 && c <= 0x28ff))
               ? c
               : '?';
}
int lap_cols(void) { return draw() ? runtime_state.area.w : 0; }
int lap_rows(void) { return draw() ? runtime_state.area.h : 0; }
void lap_put_cell(int x, int y, int16_t c, uint8_t attr) {
    if (!draw() || x < 0 || y < 0 || x >= runtime_state.area.w || y >= runtime_state.area.h)
        return;
    int ax = x + runtime_state.area.x, ay = y + runtime_state.area.y;
    runtime_state.surface->back[(size_t)ay * runtime_state.surface->w + ax] = (tui_cell){cell(c), attr};
}
void lap_clear(uint8_t attr) {
    if (draw())
        tui_fill(runtime_state.surface, runtime_state.area, ' ', attr);
}
void lap_put_str(int x, int y, const char *str, uint8_t attr) {
    if (!draw() || !str || y < 0 || y >= runtime_state.area.h || x >= runtime_state.area.w || x < -4096)
        return;
    tui_put_str(runtime_state.surface, runtime_state.area, runtime_state.area.x + x, runtime_state.area.y + y, str, attr);
}
void lap_fill(int x, int y, int w, int h, int16_t c, uint8_t a) {
    if (!draw() || w <= 0 || h <= 0)
        return;
    int64_t x1 = (int64_t)x + w, y1 = (int64_t)y + h;
    int l = x < 0 ? 0 : x, t = y < 0 ? 0 : y, r = x1 > runtime_state.area.w ? runtime_state.area.w : (int)x1,
        b = y1 > runtime_state.area.h ? runtime_state.area.h : (int)y1;
    for (int row = t; row < b; row++)
        for (int col = l; col < r; col++)
            lap_put_cell(col, row, c, a);
}
void lap_hline(int x, int y, int len, int16_t c, uint8_t a) { lap_fill(x, y, len, 1, c, a); }
void lap_vline(int x, int y, int len, int16_t c, uint8_t a) { lap_fill(x, y, 1, len, c, a); }
void lap_box(int x, int y, int w, int h, const char *title, uint8_t a) {
    if (!draw() || w < 2 || h < 2 || (int64_t)x + w > INT_MAX || (int64_t)y + h > INT_MAX)
        return;
    /* tui_box has no caller clip; use it only when wholly inside the app. */
    if (x >= 0 && y >= 0 && w <= runtime_state.area.w - x && h <= runtime_state.area.h - y) {
        tui_box(runtime_state.surface, tui_rect_make(runtime_state.area.x + x, runtime_state.area.y + y, w, h), title, a);
        return;
    }
    lap_hline(x, y, w, '-', a);
    lap_hline(x, y + h - 1, w, '-', a);
    lap_vline(x, y, h, '|', a);
    lap_vline(x + w - 1, y, h, '|', a);
    lap_put_cell(x, y, '+', a);
    lap_put_cell(x + w - 1, y, '+', a);
    lap_put_cell(x, y + h - 1, '+', a);
    lap_put_cell(x + w - 1, y + h - 1, '+', a);
}
bool lap_big_char(int x, int y, char c, uint8_t a) {
    if (!draw() || !ls_glyph_has(c))
        return false;
    /* Render into a tiny local surface so even a partially visible glyph clips. */
    tui_cell cells[15], front[15];
    tui_surface sf;
    tui_surface_setup(&sf, cells, front, 3, 5);
    tui_frame_begin(&sf);
    ls_glyph_draw(&sf, tui_rect_make(0, 0, 3, 5), c, a);
    for (int row = 0; row < 5; row++)
        for (int col = 0; col < 3; col++)
            if ((int64_t)x + col <= INT_MAX && (int64_t)y + row <= INT_MAX)
                lap_put_cell(x + col, y + row, (uint8_t)cells[row * 3 + col].ch,
                             cells[row * 3 + col].attr);
    return true;
}
void lap_image(int x, int y, int w, int h, const uint16_t *p, int pw, int ph, uint32_t serial) {
    if (!draw() || !p || w <= 0 || h <= 0 || pw <= 0 || ph <= 0 ||
        (size_t)pw > LS_LAP_MAX_MEMORY / 2 / (size_t)ph)
        return;
    int64_t right = (int64_t)x + w, bottom = (int64_t)y + h;
    int l = x < 0 ? 0 : x, t = y < 0 ? 0 : y, r = right > runtime_state.area.w ? runtime_state.area.w : (int)right,
        b = bottom > runtime_state.area.h ? runtime_state.area.h : (int)bottom;
    if (r <= l || b <= t)
        return;
    /* Existing image API scales the full source into the clipped cell rectangle. */
    tui_rect rct = tui_rect_make(runtime_state.area.x + l, runtime_state.area.y + t, r - l, b - t);
    bool changed = !runtime_state.image_active || runtime_state.image_pixels != p || runtime_state.image_w != pw || runtime_state.image_h != ph ||
                   runtime_state.image_serial != serial || memcmp(&runtime_state.image_area, &rct, sizeof(rct));
    /* Stationary ring mode uses the shared image renderer without allocating its comparison copy.
     */
    ls_tui_image_ring(rct, p, pw, ph, 0, 0, changed ? -1 : 0, changed ? -1 : 0, serial);
    runtime_state.image_called = runtime_state.image_active = true;
    runtime_state.image_area = rct;
    runtime_state.image_pixels = p;
    runtime_state.image_w = pw;
    runtime_state.image_h = ph;
    runtime_state.image_serial = serial;
    tui_fill(runtime_state.surface, rct, LS_TUI_IMAGE_CELL, TUI_DEFAULT_ATTR);
}
/* COMPASS's packing (ls_dots_blit): each cell keeps its two commonest
   colours, the commonest as ink, ties to the brighter index. */
void lap_dots(int x, int y, int w, int h, const uint8_t *dots) {
    if (!draw() || !dots || w <= 0 || h <= 0 || w > 4096 || h > 4096)
        return;
    const int dw = w * 2;
    for (int r = 0; r < h; r++) {
        if (y + r < 0 || y + r >= runtime_state.area.h)
            continue;
        const uint8_t *row0 = dots + (size_t)(3 * r) * dw;
        for (int c = 0; c < w; c++) {
            if (x + c < 0 || x + c >= runtime_state.area.w)
                continue;
            uint8_t col[6];
            for (int k = 0; k < 6; k++) {
                const uint8_t d = row0[(size_t)(k / 2) * dw + 2 * c + (k & 1)];
                col[k] = d == LAP_DOT_CLEAR ? 0 : d & 15;
            }
            int fg = col[0], bg = 0, bits = 0x3F;
            if (col[1] != fg || col[2] != fg || col[3] != fg || col[4] != fg || col[5] != fg) {
                /* A count per colour; ties go to the brighter ink, and the
                   paper to the first colour met with the most dots. */
                uint8_t count[16] = {0};
                for (int k = 0; k < 6; k++)
                    count[col[k]]++;
                for (int k = 1; k < 6; k++)
                    if (count[col[k]] > count[fg] || (count[col[k]] == count[fg] && col[k] > fg))
                        fg = col[k];
                int paper = -1;
                for (int k = 0; k < 6; k++)
                    if (col[k] != fg && (paper < 0 || count[col[k]] > count[paper]))
                        paper = col[k];
                bg = paper >= 0 ? paper : 0;
                bits = 0;
                for (int k = 0; k < 6; k++)
                    if (col[k] == fg)
                        bits |= 1 << k;
            }
            lap_put_cell(x + c, y + r, LAP_SEXT(bits), LAP_ATTR(fg, bg));
        }
    }
}
#if defined(ESP_PLATFORM) && !defined(LS_LAP_HOST_TEST)
#include "ls_theme.h"
void lap_put_glass(int x, int y, const char *str, uint8_t attr) {
    if (!draw() || !str || y < 0 || y >= runtime_state.area.h)
        return;
    for (int i = 0; str[i] && x + i < runtime_state.area.w; i++)
        if (x + i >= 0)
            ls_tui_put_glass(runtime_state.surface, runtime_state.area, runtime_state.area.x + x + i, runtime_state.area.y + y,
                             cell((unsigned char)str[i]), attr);
}
void lap_cell_px(int *w, int *h) {
    int cols, rows, cw = 10, ch = 17;
    if (inside())
        ls_tui_geometry(&cols, &rows, &cw, &ch);
    if (w)
        *w = cw;
    if (h)
        *h = ch;
}
uint16_t lap_palette(int c) {
    const ls_tui_theme_t *t = ls_tui_theme_effective(ls_tui_get_theme(), ls_tui_daylight());
    return t && inside() ? t->palette[c & 15] : 0;
}
#else
void lap_put_glass(int x, int y, const char *str, uint8_t attr) { lap_put_str(x, y, str, attr); }
void lap_cell_px(int *w, int *h) {
    if (w)
        *w = 10;
    if (h)
        *h = 17;
}
uint16_t lap_palette(int c) {
    static const uint16_t P[16] = {0x0000, 0xB249, 0x4C6B, 0xB447, 0x3B74, 0x8AD4, 0x3C73, 0xAD97,
                                   0x3A09, 0xFB4D, 0x6F0F, 0xFE4A, 0x6DBF, 0xDC5F, 0x5F1C, 0xF7BF};
    return inside() ? P[c & 15] : 0;
}
#endif
uint32_t lap_millis(void) { return inside() ? now() - runtime_state.started : 0; }
uint32_t lap_frame(void) { return inside() ? runtime_state.frame : 0; }

/* Receiver has no reader lock. Copy each record before deriving public data;
 * scratch stays in PSRAM and is used only on the serialized app callback task. */
int lap_aircraft(lap_aircraft_t *out, int max) {
    EXT_RAM_BSS_ATTR static lap_aircraft_t sorted[ADSB_MAX_TRACKED], tmp;
    EXT_RAM_BSS_ATTR static adsb_aircraft_t copy;
    lap_gps_t gps;
    if (!inside() || !out || max <= 0) return 0;
    const bool fix = lap_gps(&gps) && gps.fix;
    const int64_t us = esp_timer_get_time();
    int n = 0;
    for (int i = 0; i < ADSB_MAX_TRACKED; i++) {
        const adsb_aircraft_t *src = adsb_state_get(i);
        if (!src) continue;
        copy = *src;
        if (!copy.active || !copy.icao) continue;
        memset(&tmp, 0, sizeof(tmp));
        tmp.icao = copy.icao & 0xFFFFFF;
        memcpy(tmp.callsign, copy.callsign, 8);
        tmp.has_pos = copy.pos_valid;
        tmp.has_alt = copy.altitude_valid;
        tmp.has_speed = copy.velocity_valid;
        tmp.has_track = copy.heading_valid && copy.heading_is_track;
        tmp.has_vrate = copy.vert_rate_valid;
        tmp.lat = copy.lat; tmp.lon = copy.lon;
        tmp.alt_ft = copy.altitude;
        tmp.speed_kt = (int16_t)copy.velocity;
        tmp.track_deg = (int16_t)copy.heading;
        tmp.vrate_fpm = (int16_t)copy.vert_rate;
        tmp.squawk = (uint16_t)adsb_state_squawk(i);
        tmp.emergency = (uint8_t)adsb_state_emergency(i, NULL);
        if (copy.emitter_tc >= 1 && copy.emitter_tc <= 4 && copy.emitter_ca > 0 && copy.emitter_ca <= 7) {
            tmp.category[0] = 'A' + 4 - copy.emitter_tc;
            tmp.category[1] = '0' + copy.emitter_ca;
        }
        int64_t age = us > copy.last_seen_us ? (us - copy.last_seen_us) / 1000 : 0;
        tmp.age_ms = age > UINT32_MAX ? UINT32_MAX : (uint32_t)age;
        tmp.range_km = -1;
        if (fix && tmp.has_pos) {
            const float rad = 0.01745329252f;
            const float a = (float)gps.lat * rad, b = copy.lat * rad;
            const float dl = ((float)copy.lon - (float)gps.lon) * rad;
            const float sn = sinf((b - a) * 0.5f), se = sinf(dl * 0.5f);
            const float h = fminf(1, fmaxf(0, sn * sn + cosf(a) * cosf(b) * se * se));
            tmp.range_km = 12742.0f * atan2f(sqrtf(h), sqrtf(1 - h));
            tmp.bearing_deg = fmodf(atan2f(sinf(dl) * cosf(b),
                cosf(a) * sinf(b) - sinf(a) * cosf(b) * cosf(dl)) * 57.29578f + 360, 360);
        }
        int j = n;
        const float key = tmp.range_km < 0 ? INFINITY : tmp.range_km;
        while (j && (sorted[j-1].range_km < 0 ? INFINITY : sorted[j-1].range_km) > key) {
            sorted[j] = sorted[j-1]; j--;
        }
        sorted[j] = tmp; n++;
    }
    if (n > max) n = max;
    memcpy(out, sorted, (size_t)n * sizeof(*out));
    return n;
}

int lap_aircraft_track(uint32_t icao, float *lat, float *lon, int32_t *alt_ft, int max) {
    EXT_RAM_BSS_ATTR static adsb_trail_pt_t trail[ADSB_TRAIL_N];
    if (!inside() || !lat || !lon || !alt_ft || max <= 0 || icao > 0xFFFFFF) return 0;
    for (int i = 0; i < ADSB_MAX_TRACKED; i++) {
        const adsb_aircraft_t *a = adsb_state_get(i);
        if (!a || !a->active || a->icao != icao) continue;
        int n = adsb_state_trail(i, trail, max > ADSB_TRAIL_N ? ADSB_TRAIL_N : max);
        for (int j = 0; j < n; j++) {
            lat[j] = trail[j].lat; lon[j] = trail[j].lon; alt_ft[j] = trail[j].altitude;
        }
        return n;
    }
    return 0;
}

void lap_aircraft_demo(bool on) {
    if (inside()) adsb_demo_set(on ? 8 : 0);
}
uint32_t lap_random(void) {
#ifdef ESP_PLATFORM
    return inside() ? esp_random() : 0;
#else
    static uint32_t x = 2463534242u; /* the bench: repeatable */
    if (!inside())
        return 0;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    return x;
#endif
}
void lap_exit(void) {
    if (inside())
        runtime_state.exit = true;
}
LS_COLD void lap_log(const char *fmt, ...) {
    if (!inside() || !fmt)
        return;
    unsigned head = atomic_load_explicit(&runtime_state.log_head, memory_order_relaxed);
    unsigned tail = atomic_load_explicit(&runtime_state.log_tail, memory_order_acquire);
    if (head - tail >= 8) {
        error("app log queue full");
        return;
    }
    char *line = runtime_state.log[head % 8];
    va_list args;
    va_start(args, fmt);
    vsnprintf(line, 160, fmt, args);
    va_end(args);
    for (char *p = line; *p; p++)
        if ((unsigned char)*p < 32 || *p == 127)
            *p = ' ';
    atomic_store_explicit(&runtime_state.log_head, head + 1, memory_order_release);
    if (wake_io)
        wake_io();
}
void ls_lap_api_log_service(void) {
    unsigned tail = atomic_load_explicit(&runtime_state.log_tail, memory_order_relaxed);
    unsigned head = atomic_load_explicit(&runtime_state.log_head, memory_order_acquire);
    while (tail != head) {
        printf("[app %s] %s\n", runtime_state.id, runtime_state.log[tail % 8]);
        atomic_store_explicit(&runtime_state.log_tail, ++tail, memory_order_release);
    }
}

bool lap_value_text(const char *path, char *out, size_t cap) {
    ls_val_t v;
    const char *unit;
    if (!inside() || !path || !out || !cap || !ls_value_read(path, &v, &unit))
        return false;
    switch (v.kind) {
    case LS_VAL_TEXT:
        snprintf(out, cap, "%s", v.s ? v.s : "");
        break;
    case LS_VAL_FLOAT:
        snprintf(out, cap, "%.6g", (double)v.f);
        break;
    case LS_VAL_BOOL:
    case LS_VAL_INT:
        snprintf(out, cap, "%ld", v.i);
        break;
    default:
        return false;
    }
    return true;
}
bool lap_value_float(const char *path, float *out) {
    ls_val_t v;
    const char *unit;
    if (!inside() || !path || !out || !ls_value_read(path, &v, &unit))
        return false;
    if (v.kind == LS_VAL_FLOAT)
        *out = v.f;
    else if (v.kind == LS_VAL_INT || v.kind == LS_VAL_BOOL)
        *out = (float)v.i;
    else
        return false;
    return true;
}
LS_COLD void lap_sound(int which) {
#ifdef ESP_PLATFORM
    /* Existing notification worker honours board volume/mute. */
    if (!inside())
        return;
    switch (which) {
    case LAP_SOUND_KEY:
        snd_notification_start(SETTINGS_NOTIFY_RADIOKEY);
        break;
    case LAP_SOUND_ROGER:
        snd_notification_start(SETTINGS_NOTIFY_ROGER);
        break;
    case LAP_SOUND_CHIRP:
        snd_notification_start(SETTINGS_NOTIFY_CHIRP);
        break;
    case LAP_SOUND_ERROR:
        snd_notification_start(SETTINGS_NOTIFY_ALERT);
        break;
    default:
        break;
    }
#else
    (void)which;
#endif
}
static bool filename(const char *name) { return ls_lap_plain_name(name, 48); }

static cache *lookup(const char *name) {
    for (cache *f = runtime_state.files; f; f = f->next)
        if (ls_lap_name_equal(name, f->name))
            return f;
    return NULL;
}
/* Worker receives only firmware-owned copied names. */
bool ls_lap_api_data_path(char *out, size_t cap, const char *name) {
    if (!runtime_state.data_root[0] || !ls_lap_plain_name(name,48)) return false;
    int n=snprintf(out,cap,"%s/%s/%s",runtime_state.data_root,runtime_state.id,name);
    return n>0 && (size_t)n<cap;
}
static void path(char out[128], const char *name) {
    snprintf(out, 128, "%.40s/%.31s/%.48s", runtime_state.data_root, runtime_state.id, name ? name : "");
}
LS_COLD bool ls_lap_api_files_load(const char *root) {
    if (!root || strlen(root) > 40) {
        error(LS_TEXT(t_ls_lap_api_app_data_root_too, "app data root too long"));
        return false;
    }
    snprintf(runtime_state.data_root, sizeof(runtime_state.data_root), "%s", root);
    char dir[128];
    path(dir, NULL);
    if (MAKE_DIR(root) && errno != EEXIST) {
        error(LS_TEXT(t_ls_lap_api_cannot_create_apps_data, "cannot create apps_data"));
        return false;
    }
    if (MAKE_DIR(dir) && errno != EEXIST) {
        error(LS_TEXT(t_ls_lap_api_cannot_create_app_data, "cannot create app data folder"));
        return false;
    }
    if (!ls_lap_recover_dir(dir)) {
        error(LS_TEXT(t_ls_lap_api_app_data_recovery_failed, "app data recovery failed"));
        return false;
    }
    DIR *d = opendir(dir);
    if (!d) {
        error(LS_TEXT(t_ls_lap_api_cannot_read_app_data, "cannot read app data folder"));
        return false;
    }
    struct dirent *e;
    bool ok = true;
    while (ok && (e = readdir(d))) {
        if (!filename(e->d_name))
            continue;
        char full[128];
        path(full, e->d_name);
        struct stat st;
        if (stat(full, &st) || !S_ISREG(st.st_mode))
            continue;
        cache *c = ls_lap_calloc(1, sizeof(cache));
        if (!c) {
            ok = false;
            break;
        }
        memcpy(c->name, e->d_name, strlen(e->d_name) + 1);
        c->next = runtime_state.files;
        runtime_state.files = c;
        if (st.st_size < 0 || (uint64_t)st.st_size > LS_LAP_MAX_MEMORY) {
            error(LS_TEXT(t_ls_lap_api_app_data_exceeds_4, "app data exceeds 4 MB cache"));
            ok = false;
            break;
        }
        c->size = c->capacity = (size_t)st.st_size;
        c->data = c->size ? ls_lap_malloc(c->size) : NULL;
        if (c->size && !c->data) {
            ok = false;
            break;
        }
        FILE *f = fopen(full, "rb");
        ok = f && fread(c->data, 1, c->size, f) == c->size;
        if (f)
            fclose(f);
        if (!ok)
            error(LS_TEXT(t_ls_lap_api_cannot_cache_app_data, "cannot cache app data"));
    }
    closedir(d);
    return ok;
}
LS_COLD bool ls_lap_api_files_flush(void) {
    bool ok = true;
    for (cache *c = runtime_state.files; c; c = c->next)
        if (c->dirty) {
            char full[128];
            path(full, c->name);
            if (c->removed) {
                if (remove(full) && errno != ENOENT) ok = false;
                else c->dirty = false;
            } else {
                /* Stage in the app's folder before replacing the previous file. */
                char temp[144];
                snprintf(temp, sizeof(temp), "%s/%s/.lap-write", runtime_state.data_root, runtime_state.id);
                FILE *f = fopen(temp, "wb");
                bool wrote = f && fwrite(c->data, 1, c->size, f) == c->size;
                if (f && fclose(f))
                    wrote = false;
                if (wrote)
                    wrote = ls_lap_replace(temp, full);
                if (!wrote) {
                    remove(temp);
                    ok = false;
                } else c->dirty = false;
            }
        }
    if (!ok)
        error(LS_TEXT(t_ls_lap_api_app_data_flush_failed, "app data flush failed"));
    return ok;
}
LS_COLD int lap_file_open(const char *name, const char *mode) {
    if (!inside() || atomic_load(&files_busy) || !filename(name) || !mode || !mode[0] || mode[1] || !strchr("rwa", mode[0]))
        return -1;
    int fd = 0;
    while (fd < 4 && runtime_state.fd[fd].file)
        fd++;
    if (fd == 4)
        return -1;
    cache *c = lookup(name);
    if ((!c || c->removed) && *mode == 'r')
        return -1;
    if (!c) {
        c = ls_lap_calloc(1, sizeof(cache));
        if (!c)
            return -1;
        snprintf(c->name, sizeof(c->name), "%s", name);
        c->next = runtime_state.files;
        runtime_state.files = c;
    }
    if (*mode == 'w' || c->removed) {
        c->size = 0;
        c->dirty = true;
    }
    if (*mode == 'a')
        c->dirty = true;
    c->removed = false;
    if(file_ticket==INT_MAX) return -1;
    runtime_state.fd[fd] = (handle){c, *mode == 'a' ? c->size : 0, *mode,++file_ticket};
    return file_ticket;
}
static handle *file_handle(int ticket) {
    if(!inside()||ticket<=0) return NULL;
    for(int i=0;i<4;i++) if(runtime_state.fd[i].file&&runtime_state.fd[i].ticket==ticket) return &runtime_state.fd[i];
    return NULL;
}
LS_COLD int lap_file_read(int fd, void *buf, size_t n) {
    handle *h=file_handle(fd);
    if (!h || !buf || h->mode != 'r')
        return -1;
    if (n > INT_MAX)
        n = INT_MAX;
    size_t avail = h->pos < h->file->size ? h->file->size - h->pos : 0;
    if (n > avail)
        n = avail;
    if (n)
        memcpy(buf, h->file->data + h->pos, n);
    h->pos += n;
    return (int)n;
}
LS_COLD int lap_file_write(int fd, const void *buf, size_t n) {
    handle *h=file_handle(fd);
    if (!h || atomic_load(&files_busy) || !buf || h->mode == 'r' ||
        n > INT_MAX)
        return -1;
    cache *c = h->file;
    if (h->mode == 'a')
        h->pos = c->size;
    if (n > LS_LAP_MAX_MEMORY || h->pos > LS_LAP_MAX_MEMORY - n) {
        error(LS_TEXT(t_ls_lap_api_app_memory_cap_is, "app memory cap is 4 MB"));
        return -1;
    }
    size_t end = h->pos + n;
    if (end > c->capacity) {
        unsigned char *p = ls_lap_realloc(c->data, end);
        if (!p)
            return -1;
        c->data = p;
        c->capacity = end;
    }
    if (n)
        memcpy(c->data + h->pos, buf, n);
    h->pos = end;
    if (end > c->size)
        c->size = end;
    c->dirty = true;
    return (int)n;
}
void lap_file_close(int fd) {
    handle *h=file_handle(fd); if(h) memset(h,0,sizeof(*h));
}
LS_COLD bool lap_file_remove(const char *name) {
    if (!inside() || atomic_load(&files_busy) || !filename(name))
        return false;
    cache *c = lookup(name);
    if (!c || c->removed)
        return false;
    for (int i = 0; i < 4; i++)
        if (runtime_state.fd[i].file == c)
            return false;
    c->removed = true;
    c->dirty = true;
    return true;
}
LS_COLD long lap_file_size(const char *name) {
    if (!inside() || !filename(name))
        return -1;
    cache *c = lookup(name);
    return c && !c->removed ? (long)c->size : -1;
}

/* A flush freezes cached bytes while the IO worker reads them. */
bool ls_lap_api_flush_acquire(void) {
    bool expected = false;
    return inside() && runtime_state.data_root[0] && atomic_compare_exchange_strong(&files_busy, &expected, true);
}
void ls_lap_api_flush_release(void) { atomic_store(&files_busy, false); }
static uint32_t save_hash(const unsigned char *p, size_t n) {
    uint32_t h = 2166136261u;
    while (n--) h = (h ^ *p++) * 16777619u;
    return h;
}
static void save_u32(unsigned char *p, uint32_t v) {
    for (int i = 0; i < 4; i++) p[i] = (unsigned char)(v >> (8 * i));
}
static uint32_t save_get32(const unsigned char *p) {
    return p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}
static bool save_name(char out[49], const char *prefix, const char *slot) {
    if (!inside() || !slot) return false;
    char name[33]; size_t i;
    for (i = 0; i < sizeof(name); i++) {
        if (!ls_lap_api_readable(slot + i, 1)) return false;
        name[i] = slot[i]; if (!name[i]) break;
    }
    if (i == sizeof(name) || !ls_lap_plain_name(name, 32)) return false;
    snprintf(out, 49, "%s%s", prefix, name); return true;
}
static int save_write(const char *prefix, const char *slot, const void *data, size_t len, uint32_t schema) {
    char name[49];
    if (!save_name(name, prefix, slot) || len > 65536 || (len && !ls_lap_api_readable(data, len))) return LAP_SD_EINVAL;
    if (atomic_load(&files_busy)) return LAP_SD_EBUSY;
    cache *c = lookup(name);
    for (int i = 0; i < 4; i++) if (c && runtime_state.fd[i].file == c) return LAP_SD_EBUSY;
    unsigned char *payload = ls_lap_malloc(len + 20);
    if (!payload) return LAP_SD_EBUSY;
    memcpy(payload, "LSAV", 4); save_u32(payload + 4, 1);
    save_u32(payload + 8, schema); save_u32(payload + 12, len);
    if (len) memcpy(payload + 20, data, len);
    save_u32(payload + 16, save_hash(payload + 20, len));
    if (!c) {
        c = ls_lap_calloc(1, sizeof(*c));
        if (!c) { ls_lap_free(payload); return LAP_SD_EBUSY; }
        strcpy(c->name, name); c->next = runtime_state.files; runtime_state.files = c;
    }
    ls_lap_free(c->data); c->data = payload; c->size = c->capacity = len + 20;
    c->dirty = true; c->removed = false;
    return lap_file_flush();
}
static int save_read(const char *prefix, const char *slot, void *data, size_t cap, uint32_t *schema) {
    char name[49];
    if (!save_name(name, prefix, slot) || (schema && !ls_lap_api_readable(schema, sizeof(*schema)))) return LAP_SD_EINVAL;
    cache *c = lookup(name);
    if (!c || c->removed) return LAP_SD_ENOENT;
    if (c->size < 20 || memcmp(c->data, "LSAV", 4) || save_get32(c->data + 4) != 1) return LAP_SD_EIO;
    uint32_t n = save_get32(c->data + 12);
    if (n > 65536 || n != c->size - 20 || save_get32(c->data + 16) != save_hash(c->data + 20, n)) return LAP_SD_EIO;
    if (cap < n || (n && !ls_lap_api_readable(data, n))) return LAP_SD_EINVAL;
    if (n) memcpy(data, c->data + 20, n);
    if (schema) *schema = save_get32(c->data + 8);
    return (int)n;
}
int lap_save_write(const char *slot, const void *data, size_t len, uint32_t schema) { return save_write("save_", slot, data, len, schema); }
int lap_save_read(const char *slot, void *data, size_t cap, uint32_t *schema) { return save_read("save_", slot, data, cap, schema); }
int lap_save(const char *slot, const void *data, size_t len) { return lap_save_write(slot, data, len, 1); }
int lap_load(const char *slot, void *data, size_t cap) { return lap_save_read(slot, data, cap, NULL); }
int lap_settings_set(const char *key, const void *data, size_t len) { return len <= 256 ? save_write("pref_", key, data, len, 1) : LAP_SD_EINVAL; }
int lap_settings_get(const char *key, void *data, size_t cap) { return save_read("pref_", key, data, cap, NULL); }

#if defined(ESP_PLATFORM) && !defined(LS_LAP_HOST_TEST)
/* Compiler helpers in the board libgcc; the SDK imports aliases to avoid non-PIC libgcc.a. */
extern double __adddf3(double, double);
extern double __subdf3(double, double);
extern double __muldf3(double, double);
extern double __divdf3(double, double);
extern double __extendsfdf2(float);
extern float __truncdfsf2(double);
extern int __fixdfsi(double);
extern unsigned __fixunsdfsi(double);
extern long long __fixdfdi(double);
extern unsigned long long __fixunsdfdi(double);
extern double __floatsidf(int);
extern double __floatunsidf(unsigned);
extern double __floatdidf(long long);
extern double __floatundidf(unsigned long long);
extern int __eqdf2(double, double);
extern int __nedf2(double, double);
extern int __ltdf2(double, double);
extern int __ledf2(double, double);
extern int __gtdf2(double, double);
extern int __gedf2(double, double);
extern int __unorddf2(double, double);
extern long long __divdi3(long long, long long);
extern unsigned long long __udivdi3(unsigned long long, unsigned long long);
extern long long __moddi3(long long, long long);
extern unsigned long long __umoddi3(unsigned long long, unsigned long long);

#else
static void host_helper(void) {}
#define __adddf3 host_helper
#define __subdf3 host_helper
#define __muldf3 host_helper
#define __divdf3 host_helper
#define __extendsfdf2 host_helper
#define __truncdfsf2 host_helper
#define __fixdfsi host_helper
#define __fixunsdfsi host_helper
#define __fixdfdi host_helper
#define __fixunsdfdi host_helper
#define __floatsidf host_helper
#define __floatunsidf host_helper
#define __floatdidf host_helper
#define __floatundidf host_helper
#define __eqdf2 host_helper
#define __nedf2 host_helper
#define __ltdf2 host_helper
#define __ledf2 host_helper
#define __gtdf2 host_helper
#define __gedf2 host_helper
#define __unorddf2 host_helper
#define __divdi3 host_helper
#define __udivdi3 host_helper
#define __moddi3 host_helper
#define __umoddi3 host_helper
#endif

#define EXPORT(name, fn) {name, (uintptr_t)(fn)}
/* Area blocks are mergeable; names are unique and checked against the public header and SDK exports.txt. */
static const ls_lap_export_t export_table[] = {
#include "ls_lap_exports.inc"
};
#undef EXPORT
/* Area blocks are sorted once before lookup, into a copy in PSRAM: the table
   itself stays in flash rather than 3.6 KB of internal RAM. Publication is
   synchronized. */
static LS_LAP_POOL ls_lap_export_t exports[sizeof(export_table) / sizeof(export_table[0])];
static int export_compare(const void *a, const void *b) {
    return strcmp(((const ls_lap_export_t *)a)->name, ((const ls_lap_export_t *)b)->name);
}
const ls_lap_export_t *ls_lap_exports(size_t *count) {
    static atomic_bool sorted;
    static atomic_flag sorting = ATOMIC_FLAG_INIT;
    if (!atomic_load_explicit(&sorted, memory_order_acquire)) {
        while (atomic_flag_test_and_set_explicit(&sorting, memory_order_acquire)) {}
        if (!atomic_load_explicit(&sorted, memory_order_relaxed)) {
            memcpy(exports, export_table, sizeof(exports));
            qsort(exports, sizeof(exports) / sizeof(exports[0]), sizeof(exports[0]), export_compare);
            atomic_store_explicit(&sorted, true, memory_order_release);
        }
        atomic_flag_clear_explicit(&sorting, memory_order_release);
    }
    if (count)
        *count = sizeof(exports) / sizeof(exports[0]);
    return exports;
}
uintptr_t ls_lap_resolve(const char *name) {
    if (!name)
        return 0;
    size_t n;
    const ls_lap_export_t *table = ls_lap_exports(&n);
    size_t lo = 0, hi = n;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        int c = strcmp(name, table[mid].name);
        if (!c)
            return table[mid].address;
        if (c < 0)
            hi = mid;
        else
            lo = mid + 1;
    }
    if (!runtime_state.missing[0])
        snprintf(runtime_state.missing, sizeof(runtime_state.missing), "needs %.88s", name);
    return 0;
}
const char *ls_lap_missing_symbol(void) { return runtime_state.missing; }
