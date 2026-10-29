/* API 2.1 background services through the real loader (ls_lap.c), with the
 * same deterministic ELF/task/heap adapters as test_lap_host: a service keeps
 * the app loaded when its screen closes and its tick keeps running; the tile
 * shows it again without a reload; STOP, a slow tick, a bad tick, lap_exit
 * from a tick and opening another app each end it, and every one of those
 * unloads the app and frees every PSRAM and ELF buffer it had. */
#define LS_LAP_DATA_ROOT "lap-svc-data"
#include "../../components/apps/tui/ls_lap.c"
#include "ls_anim.h"
#include "ls_value.h"
#include <assert.h>
#include <direct.h>
#include <stdlib.h>
#include <unistd.h>
static size_t outstanding;
static unsigned enters, leaves, ticks, stops, relocations;
static int64_t clock_us = 1000000, tick_cost = 50;
static bool exit_in_tick, start_in_enter = true;
static char crumb[32], tick_crumb[32];
static const ls_tui_screen_t *screens[64];
static int screen_count, current;
static const lap_app_t *descriptor;
static unsigned checks;
#define CHECK(x) do { assert(x); checks++; } while (0)

int ls_icon_art(int slot, const char art[LS_ICON_ROWS][LS_ICON_COLS + 1], int fallback) { (void)slot; (void)art; return fallback; }
int ls_icon_px(int slot, const uint8_t px[LS_ICON_ROWS * 3][LS_ICON_COLS * 2], int fallback) { (void)slot; (void)px; return fallback; }
const char *ls_tui_radio_claimed(void) { return NULL; }
void ls_tui_radio_want(const char *id) { (void)id; }
bool ls_tui_screen_is_bare(void) { return false; }
void ls_tui_screen_set_full(bool on) { (void)on; }
void ls_lap_ui_draw(tui_surface *sf, tui_rect area, const ls_lap_info_t *app, const ls_lap_status_t *st,
                    const char *error, bool crashed) {
    (void)sf; (void)area; (void)app; (void)st; (void)error; (void)crashed;
}
ls_lap_ui_act_t ls_lap_ui_touch(int col, int row) { (void)col; (void)row; return LS_LAP_UI_NONE; }
ls_lap_ui_act_t ls_lap_ui_key(ls_tk_t k, char ch, const ls_lap_status_t *st) { (void)k; (void)ch; (void)st; return LS_LAP_UI_NONE; }

/* ---- the app ---------------------------------------------------------------- */

static void on_tick(void) {
    snprintf(tick_crumb, sizeof(tick_crumb), "%s", crumb);
    ticks++;
    clock_us += tick_cost;
    if (exit_in_tick) lap_exit();
}
static void on_stopped(void) { stops++; }
static void on_enter(void) {
    enters++;
    if (!start_in_enter) return;
    lap_service_t *svc = ls_lap_malloc(sizeof(*svc));
    *svc = (lap_service_t){on_tick, on_stopped, 100};
    assert(lap_service_start(svc) == LAP_SVC_OK);
}
static void on_leave(void) { leaves++; }
static void on_draw(void) { lap_put_str(0, 0, "svc", 7); }
static bool on_key(lap_key_t k, char ch) { (void)k; (void)ch; return true; }
static bool on_touch(int c, int r) { (void)c; (void)r; return true; }
static const lap_app_t *entry_fn(void) { return descriptor; }

/* ---- the board ---------------------------------------------------------------- */

void *heap_caps_malloc(size_t n, int caps) {
    assert(caps & (MALLOC_CAP_SPIRAM | MALLOC_CAP_INTERNAL));
    void *p = malloc(n ? n : 1);
    if (p) outstanding++;
    return p;
}
void *heap_caps_calloc(size_t n, size_t size, int caps) {
    void *p = heap_caps_malloc(n * size, caps);
    if (p) memset(p, 0, n * size);
    return p;
}
void *heap_caps_realloc(void *p, size_t n, int caps) { return p ? realloc(p, n) : heap_caps_malloc(n, caps); }
void heap_caps_free(void *p) {
    if (p) { assert(outstanding); outstanding--; free(p); }
}
bool esp_ptr_external_ram(const void *p) { return p != NULL; }
bool esp_ptr_in_dram(const void *p) { return p != NULL; }
unsigned uxTaskPriorityGet(TaskHandle_t task) { (void)task; return 2; }
int xPortGetCoreID(void) { return 0; }
void vTaskSuspend(TaskHandle_t task) { (void)task; }
void vTaskDelete(TaskHandle_t task) { (void)task; }
int64_t esp_timer_get_time(void) { return clock_us; }
uint32_t esp_random(void) { return 42; }
TaskHandle_t xTaskGetCurrentTaskHandle(void) { return (void *)1; }
unsigned uxTaskGetStackHighWaterMark(TaskHandle_t task) { (void)task; return 4096; }
unsigned ulTaskNotifyTake(int clear, unsigned wait) { (void)clear; (void)wait; return 1; }
void xTaskNotifyGive(TaskHandle_t task) { assert(task); }
TaskHandle_t xTaskCreateStaticPinnedToCore(void (*fn)(void *), const char *name, unsigned stack, void *arg,
                                           unsigned priority, StackType_t *mem, StaticTask_t *tcb, int core) {
    (void)stack; (void)priority; (void)mem; (void)tcb; (void)core;
    if (!strcmp(name, "lap_reloc")) fn(arg);
    return (void *)2;
}
void ls_trail_native_callback(const char *id) { snprintf(crumb, sizeof(crumb), "%s", id ? id : ""); }
const char *ls_trail_last_native(void) { return NULL; }
void ls_lap_console_expire(void) {}
void elf_set_symbol_resolver(uintptr_t (*fn)(const char *)) { (void)fn; }
int esp_elf_init(esp_elf_t *elf) { memset(elf, 0, sizeof(*elf)); return 0; }
int esp_elf_relocate(esp_elf_t *elf, const uint8_t *bytes) {
    (void)bytes;
    relocations++;
    elf->psegment = heap_caps_malloc(32768, 3);
    lap_app_t *app = (lap_app_t *)(elf->psegment + 256);
    *app = s.apps[s.active].manifest;
    app->enter = on_enter; app->leave = on_leave; app->draw = on_draw; app->key = on_key; app->touch = on_touch;
    descriptor = app;
    elf->entry = (void *)(uintptr_t)entry_fn;
    return 0;
}
void esp_elf_deinit(esp_elf_t *elf) { heap_caps_free(elf->psegment); memset(elf, 0, sizeof(*elf)); }
void ls_anim_start(int i, const char *n, uint8_t h) { (void)i; (void)n; (void)h; }
int ls_tui_screen_register(const ls_tui_screen_t *sc) {
    for (int i = 0; i < screen_count; i++) if (screens[i] == sc) return i;
    screens[screen_count] = sc;
    return screen_count++;
}
int ls_tui_screen_current(void) { return current; }
void ls_tui_screen_set_tab_count(int n) { (void)n; }
void ls_tui_screen_show(int index) {
    if (current == index) return;
    if (screens[current]->leave) screens[current]->leave();
    current = index;
    if (screens[current]->enter) screens[current]->enter();
}
void ls_tui_image(tui_rect r, const uint16_t *p, int w, int h, uint32_t serial) { (void)r; (void)p; (void)w; (void)h; (void)serial; }
void ls_tui_image_ring(tui_rect r, const uint16_t *p, int w, int h, int x0, int y0, int dx, int dy, uint32_t serial) {
    (void)r; (void)p; (void)w; (void)h; (void)x0; (void)y0; (void)dx; (void)dy; (void)serial;
}
bool ls_value_read(const char *path, ls_val_t *v, const char **unit) { (void)path; (void)v; (void)unit; return false; }
bool snd_notification_start(int n) { (void)n; return true; }
static void home_draw(tui_surface *sf, tui_rect area) { (void)sf; (void)area; }
static const ls_tui_screen_t home_screen = {.name = "HOME", .draw = home_draw};
static const ls_app_doc_t home_doc = {.purpose = "test"};

/* The relocated text: every callback the board may call, tick and stopped too. */
static bool host_text = true;
static void step(void) {
    io_service();
    if (!s.work_error[0] && s.elf.psegment && host_text) {
        uintptr_t p[] = {(uintptr_t)on_enter, (uintptr_t)on_leave, (uintptr_t)on_draw, (uintptr_t)on_key,
                         (uintptr_t)on_touch, (uintptr_t)on_tick, (uintptr_t)on_stopped};
        uintptr_t lo = p[0], hi = p[0];
        for (int i = 1; i < 7; i++) { if (p[i] < lo) lo = p[i]; if (p[i] > hi) hi = p[i]; }
        bool have = false;
        for (unsigned i = 0; i < s.nexec; i++) have |= s.exec[i].start == lo && s.exec[i].end == hi + 1;
        if (!have && s.nexec < 16) {
            s.exec[s.nexec].start = lo;
            s.exec[s.nexec++].end = hi + 1;
        }
    }
    ls_lap_poll();
}
static void frames(int n) {
    for (int i = 0; i < n; i++) { clock_us += 100000; step(); }
}
static bool loaded(void) { return s.elf.psegment != NULL; }
static void open_app(void) {
    ls_app_open(1);
    step();
    CHECK(s.status.state == LS_LAP_RUNNING);
}
static void go_home(void) { ls_tui_screen_show(0); }
static void unloaded(void) {
    step(); step();
    CHECK(!loaded() && !outstanding && !ls_lap_api_bytes() && !crumb[0] && !ls_lap_service_on());
}

/* The probe as a 2.1 service app: LAP_CAP_SERVICE in its descriptor. */
static void make_service_app(const char *path) {
    FILE *in = fopen(path, "r+b");
    assert(in);
    unsigned char header[52];
    assert(fread(header, 1, sizeof(header), in) == sizeof(header));
    uint32_t section_offset; uint16_t sections;
    memcpy(&section_offset, header + 32, 4); memcpy(&sections, header + 48, 2);
    bool changed = false;
    for (unsigned i = 0; i < sections && !changed; ++i) {
        uint32_t section[10];
        assert(!fseek(in, section_offset + i * 40, SEEK_SET));
        assert(fread(section, 1, sizeof(section), in) == sizeof(section));
        if (section[5] != LS_LAP_DESCRIPTOR_BYTES) continue;
        uint32_t magic; uint16_t api[2];
        assert(!fseek(in, section[4], SEEK_SET));
        assert(fread(&magic, 1, 4, in) == 4 && fread(api, 2, 2, in) == 2);
        if (magic != LAP_MAGIC) continue;
        assert(api[0] == 2 && api[1] == 1);                    /* the SDK builds 2.1 now */
        assert(!fseek(in, section[4] + 130, SEEK_SET));
        assert(fputc(LAP_CAP_SERVICE, in) != EOF);
        changed = true;
    }
    assert(changed && !fclose(in));
}

int main(int argc, char **argv) {
    assert(argc == 2);
    FILE *in = fopen(argv[1], "rb"), *out = fopen("lap-svc-probe.lap", "wb");
    assert(in && out);
    int c;
    while ((c = fgetc(in)) != EOF) assert(fputc(c, out) != EOF);
    fclose(in); fclose(out);
    make_service_app("lap-svc-probe.lap");
    _mkdir("lap-svc-data");
    ls_app_t home = {.id = "home", .name = "HOME", .screen = &home_screen, .doc = &home_doc};
    assert(ls_app_register(&home) == 0);
    assert(ls_lap_scan_file("lap-svc-probe.lap") == 1 && !outstanding);
    CHECK(s.apps[0].manifest.flags == LAP_CAP_SERVICE);
    /* Asked once per install: SERVICE is a remembered grant. */
    ls_app_open(1);
    CHECK(s.status.state == LS_LAP_FAILED && s.status.needs_confirmation);
    CHECK(ls_lap_confirm("probe") && ls_lap_grant_has(&s.apps[0], LAP_CAP_SERVICE));
    ls_lap_poll();
    step();
    CHECK(s.status.state == LS_LAP_RUNNING && enters == 1 && ls_lap_service_on());

    /* 1. The screen closes: leave() runs, the app stays, the tick goes on. */
    go_home();
    CHECK(leaves == 1 && loaded() && s.entered && ls_lap_service_on() && !ls_lap_service_shown());
    frames(1);
    CHECK(loaded());                                           /* not cleaned up behind its back */
    const unsigned t0 = ticks;
    frames(5);
    CHECK(ticks >= t0 + 4 && !strcmp(tick_crumb, "probe"));   /* named in the crash trail while it runs */
    CHECK(!crumb[0]);
    ls_lap_service_info_t info;
    CHECK(ls_lap_services(&info, 1) == 1 && !info.shown && !strcmp(info.id, "probe"));
    CHECK(ls_lap_hub_host()->notices >= 1 && strstr(ls_lap_hub_host()->last_body, "STOP in RADIOS"));
    CHECK(!ls_lap_maintenance_begin("probe"));                 /* cannot be removed while it runs */

    /* 2. The tile again: shown with what it kept, no second load. */
    const unsigned relocs = relocations;
    ls_app_open(1);
    step();
    CHECK(enters == 2 && relocations == relocs && ls_lap_service_shown() && s.status.state == LS_LAP_RUNNING);
    tui_cell back[400], front[400];
    tui_surface sf;
    tui_surface_setup(&sf, back, front, 20, 20);
    draw(&sf, tui_rect_make(0, 0, 20, 20));
    CHECK(back[0].ch == 's');

    /* 3. STOP with the screen open: the service ends, the app stays open. */
    CHECK(ls_lap_service_request_stop("probe"));
    step();
    CHECK(!ls_lap_service_on() && loaded() && s.status.state == LS_LAP_RUNNING && stops == 0);
    CHECK(strstr(ls_lap_hub_host()->last_body, "STOP pressed"));
    go_home();                                                  /* no service: an ordinary close */
    unloaded();
    CHECK(leaves == 2);

    /* 4. STOP from RADIOS with the screen closed: stopped(), then unloaded. */
    open_app();
    go_home();
    CHECK(loaded());
    CHECK(ls_lap_service_request_stop(NULL));
    step();
    CHECK(stops == 1);
    unloaded();

    /* 5. Too slow while closed: stopped with a notice, unloaded. */
    open_app();
    go_home();
    tick_cost = 10 * LAP_SERVICE_BUDGET_US + 1;
    const int notices = ls_lap_hub_host()->notices;
    frames(2);
    CHECK(!ls_lap_service_on() && ls_lap_hub_host()->notices == notices + 1 &&
          strstr(ls_lap_hub_host()->last_body, "too slow") && stops == 2);
    unloaded();
    tick_cost = 50;

    /* 6. lap_exit from a tick while closed: unloaded quietly, HOME stays put. */
    open_app();
    go_home();
    exit_in_tick = true;
    const int quiet = ls_lap_hub_host()->notices;
    frames(2);
    CHECK(current == 0 && ls_lap_hub_host()->notices == quiet && stops == 3);
    unloaded();
    exit_in_tick = false;

    /* 7. A bad tick pointer (not the app's code): ended, unloaded. */
    open_app();
    go_home();
    host_text = false;
    s.nexec = 0;                                              /* the text no longer covers the tick */
    frames(2);
    CHECK(!ls_lap_service_on() && stops == 3);                /* nor stopped(): not app code either */
    unloaded();
    host_text = true;

    /* 8. Another native app opened while it runs: the service stops first. */
    open_app();
    go_home();
    CHECK(ls_lap_maintenance_begin("other"));
    ls_lap_info_t other = s.apps[0];
    snprintf(other.manifest.id, sizeof(other.manifest.id), "other");
    snprintf(other.manifest.name, sizeof(other.manifest.name), "OTHER");
    CHECK(ls_lap_publish(&other));
    ls_lap_poll();
    ls_app_open(2);
    CHECK(!ls_lap_service_on() && strstr(ls_lap_hub_host()->last_body, "OTHER opened") && stops == 4);
    step(); step(); step();
    CHECK(s.active == 1);                                       /* the other app's turn */
    go_home();
    step(); step();
    CHECK(!loaded() && !outstanding);

    /* 9. An app that never starts a service closes as before. */
    start_in_enter = false;
    open_app();
    go_home();
    unloaded();

    remove("lap-svc-probe.lap");
    rmdir("lap-svc-data/probe");
    rmdir("lap-svc-data/other");
    rmdir("lap-svc-data");
    printf("test_lap_service: %u checks passed; every PSRAM and ELF buffer freed\n", checks);
    return 0;
}
