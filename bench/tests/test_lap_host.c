/* Exercise the real ESP loader host with deterministic ELF/task/heap adapters. */
#define LS_LAP_DATA_ROOT "lap-host-data"
#include "../../components/apps/tui/ls_lap.c"
#include "ls_anim.h"
#include "ls_value.h"
#include <assert.h>
#include <direct.h>
#include <stdlib.h>
#include <unistd.h>
static size_t outstanding;
static bool fail_alloc, fail_relocate, external = true, bad_descriptor, exit_entry, exit_enter,
                                       exit_draw;
static unsigned enters, leaves, draws, keys, taps;
static bool expected_network;
static int touch_col, touch_row;
static char crumb[32];
static const ls_tui_screen_t *screens[64];
static int screen_count, current;
static const lap_app_t *descriptor;
/* Board/UI adapters: the loader's real lifecycle is exercised below. */
int ls_icon_art(int slot, const char art[LS_ICON_ROWS][LS_ICON_COLS + 1], int fallback) { (void)slot; (void)art; return fallback; }
int ls_icon_px(int slot, const uint8_t px[LS_ICON_ROWS * 3][LS_ICON_COLS * 2], int fallback) { (void)slot; (void)px; return fallback; }
const char *ls_tui_radio_claimed(void) { return NULL; }
void ls_tui_radio_want(const char *id) { (void)id; }
bool ls_tui_screen_is_bare(void) { return false; }
void ls_tui_screen_set_full(bool on) { (void)on; }
void ls_lap_ui_draw(tui_surface *sf, tui_rect area, const ls_lap_info_t *app,
                   const ls_lap_status_t *st, const char *error, bool crashed) {
    (void)sf; (void)area; (void)app; (void)st; (void)error; (void)crashed;
}
ls_lap_ui_act_t ls_lap_ui_touch(int col, int row) { (void)col; (void)row; return LS_LAP_UI_NONE; }
ls_lap_ui_act_t ls_lap_ui_key(ls_tk_t k, char ch, const ls_lap_status_t *st) {
    (void)ch;
    return st && st->needs_confirmation && k == LS_TK_ENTER ? LS_LAP_UI_OPEN : LS_LAP_UI_NONE;
}
static void on_enter(void) {
    assert(!strcmp(crumb, "probe"));
    enters++;
    assert(lap_millis() == 0);
    assert(ls_lap_malloc(127));
    lap_net_status_t *network = ls_lap_malloc(sizeof(*network));
    assert(lap_net_status(network) == 0 && network->allowed == expected_network);
    ls_lap_free(network);
    if (exit_enter)
        lap_exit();
}
static void on_leave(void) {
    assert(!strcmp(crumb, "probe"));
    leaves++;
}
static void on_draw(void) {
    assert(!strcmp(crumb, "probe"));
    draws++;
    lap_put_str(0, 0, "probe", 7);
    if (exit_draw)
        lap_exit();
}
static bool on_key(lap_key_t k, char ch) {
    assert(!strcmp(crumb, "probe"));
    assert(k == LAP_KEY_BACK);
    assert(!ch);
    keys++;
    return true;
}
static bool on_touch(int c, int r) {
    assert(!strcmp(crumb, "probe"));
    taps++;
    touch_col = c;
    touch_row = r;
    return true;
}
static const lap_app_t *entry_fn(void) {
    assert(!strcmp(crumb, "probe"));
    if (exit_entry)
        lap_exit();
    return bad_descriptor ? (const lap_app_t *)(uintptr_t)1 : descriptor;
}
void *heap_caps_malloc(size_t n, int caps) {
    assert(caps & (MALLOC_CAP_SPIRAM | MALLOC_CAP_INTERNAL));
    if (fail_alloc) {
        fail_alloc = false;
        return NULL;
    }
    void *p = malloc(n);
    if (p)
        outstanding++;
    return p;
}
void *heap_caps_calloc(size_t n, size_t size, int caps) {
    void *p = heap_caps_malloc(n * size, caps);
    if (p)
        memset(p, 0, n * size);
    return p;
}
void *heap_caps_realloc(void *p, size_t n, int caps) {
    if (!p)
        return heap_caps_malloc(n, caps);
    if (fail_alloc) {
        fail_alloc = false;
        return NULL;
    }
    return realloc(p, n);
}
void heap_caps_free(void *p) {
    if (p) {
        assert(outstanding);
        outstanding--;
        free(p);
    }
}
bool esp_ptr_external_ram(const void *p) { return p && external; }
bool esp_ptr_in_dram(const void *p) { return p != NULL; }
unsigned uxTaskPriorityGet(TaskHandle_t task) { (void)task; return 2; }
int xPortGetCoreID(void) { return 0; }
void vTaskSuspend(TaskHandle_t task) { (void)task; }
void vTaskDelete(TaskHandle_t task) { (void)task; }
int64_t esp_timer_get_time(void) { return 1000000; }
uint32_t esp_random(void) { return 42; }
TaskHandle_t xTaskGetCurrentTaskHandle(void) { return (void *)1; }
unsigned uxTaskGetStackHighWaterMark(TaskHandle_t task) {
    (void)task;
    return 4096;
}
unsigned ulTaskNotifyTake(int clear, unsigned wait) {
    (void)clear;
    (void)wait;
    return 1;
}
void xTaskNotifyGive(TaskHandle_t task) { assert(task); }
TaskHandle_t xTaskCreateStaticPinnedToCore(void (*fn)(void *), const char *name, unsigned stack,
                                           void *arg, unsigned priority, StackType_t *mem,
                                           StaticTask_t *tcb, int core) {
    (void)fn;
    (void)name;
    (void)arg;
    (void)priority;
    (void)mem;
    (void)tcb;
    (void)core;
    if (!strcmp(name, "lap_reloc")) { assert(stack == 2048 / sizeof(StackType_t)); fn(arg); }
    else assert(stack == 6144);
    return (void *)2;
}
void ls_trail_native_callback(const char *id) {
    snprintf(crumb, sizeof(crumb), "%s", id ? id : "");
}
const char *ls_trail_last_native(void) { return NULL; }
void ls_lap_console_expire(void) {}
void elf_set_symbol_resolver(uintptr_t (*fn)(const char *)) { assert(fn == ls_lap_resolve); }
int esp_elf_init(esp_elf_t *elf) {
    memset(elf, 0, sizeof(*elf));
    return 0;
}
int esp_elf_relocate(esp_elf_t *elf, const uint8_t *bytes) {
    (void)bytes;
    elf->psegment = heap_caps_malloc(32768, 3);
    if (!elf->psegment)
        return -1;
    if (fail_relocate) {
        ls_lap_resolve("forbidden_symbol");
        return -1;
    }
    lap_app_t *app = (lap_app_t *)(elf->psegment + 256);
    *app = s.apps[s.active].manifest;
    app->enter = on_enter;
    app->leave = on_leave;
    app->draw = on_draw;
    app->key = on_key;
    app->touch = on_touch;
    descriptor = app;
    elf->entry = (void *)(uintptr_t)entry_fn;
    return 0;
}
void esp_elf_deinit(esp_elf_t *elf) {
    heap_caps_free(elf->psegment);
    memset(elf, 0, sizeof(*elf));
}
void ls_anim_start(int i, const char *n, uint8_t h) {
    (void)i;
    (void)n;
    (void)h;
}
int ls_tui_screen_register(const ls_tui_screen_t *sc) {
    for (int i = 0; i < screen_count; i++)
        if (screens[i] == sc)
            return i;
    assert(screen_count < 64);
    screens[screen_count] = sc;
    return screen_count++;
}
int ls_tui_screen_current(void) { return current; }
void ls_tui_screen_set_tab_count(int n) { (void)n; }
void ls_tui_screen_show(int index) {
    if (current == index)
        return;
    if (screens[current]->leave)
        screens[current]->leave();
    current = index;
    if (screens[current]->enter)
        screens[current]->enter();
}
void ls_tui_image(tui_rect r, const uint16_t *p, int w, int h, uint32_t serial) {
    (void)r;
    (void)p;
    (void)w;
    (void)h;
    (void)serial;
}
void ls_tui_image_ring(tui_rect r, const uint16_t *p, int w, int h, int x0, int y0, int dx, int dy,
                       uint32_t serial) {
    (void)x0;
    (void)y0;
    (void)dx;
    (void)dy;
    ls_tui_image(r, p, w, h, serial);
}
bool ls_value_read(const char *path, ls_val_t *v, const char **unit) {
    (void)path;
    (void)v;
    (void)unit;
    return false;
}
bool snd_notification_start(int n) {
    (void)n;
    return true;
}
static void home_draw(tui_surface *sf, tui_rect area) {
    (void)sf;
    (void)area;
}
static const ls_tui_screen_t home_screen = {.name = "HOME", .draw = home_draw};
static const ls_app_doc_t home_doc = {.purpose = "test"};
static void step(void) {
    io_service();
    if (!s.work_error[0] && s.elf.psegment) {
        /* Model the relocated callback text with the host's test callback addresses. */
        uintptr_t ptrs[] = {(uintptr_t)on_enter, (uintptr_t)on_leave, (uintptr_t)on_draw,
                            (uintptr_t)on_key, (uintptr_t)on_touch};
        uintptr_t lo = ptrs[0], hi = ptrs[0];
        for (int i = 1; i < 5; i++) {
            if (ptrs[i] < lo)
                lo = ptrs[i];
            if (ptrs[i] > hi)
                hi = ptrs[i];
        }
        s.exec[s.nexec].start = lo;
        s.exec[s.nexec++].end = hi + 1;
    }
    ls_lap_poll();
}
static void open_app(void) {
    assert(current == 0);
    assert(ls_lap_confirm("probe")); /* normal lifecycle cases assume OPEN was approved */
    ls_app_open(1);
    assert(s.status.state == LS_LAP_LOADING);
}
static void close_app(void) {
    ls_tui_screen_show(0);
    step();
    step();
    assert(!outstanding && !ls_lap_api_bytes() && !s.elf.psegment && !crumb[0]);
}
int main(int argc, char **argv) {
    assert(argc == 2);
    FILE *in = fopen(argv[1], "rb");
    assert(in);
    FILE *out = fopen("lap-host-probe.lap", "wb");
    assert(out);
    int c;
    while ((c = fgetc(in)) != EOF)
        assert(fputc(c, out) != EOF);
    fclose(in);
    fclose(out);
    _mkdir("lap-host-data");
    ls_app_t home = {.id = "home", .name = "HOME", .screen = &home_screen, .doc = &home_doc};
    assert(ls_app_register(&home) == 0);
    assert(ls_lap_scan_file("lap-host-probe.lap") == 1 && !outstanding);
    assert(!s.apps[0].signature_checked && !s.apps[0].is_signed);
    unsigned cases = 0;
    ls_lap_info_t probe;
    char why[96];
    assert(ls_lap_inspect_file("lap-host-probe.lap", &probe, why, sizeof(why)) &&
           probe.signature_checked && !probe.is_signed && !outstanding);
    out = fopen("lap-host-signed.lap", "wb");
    in = fopen("lap-host-probe.lap", "rb");
    assert(out && in);
    while ((c = fgetc(in)) != EOF)
        assert(fputc(c, out) != EOF);
    fclose(in);
    const unsigned char bad_sig[] = {0x30, 0, 0, 0, 0, 0, 0, 0, 8, 0, 'L', 'S', 'G', '1'};
    assert(fwrite(bad_sig, 1, sizeof(bad_sig), out) == sizeof(bad_sig));
    fclose(out);
    assert(read_info("lap-host-signed.lap", &probe, false, why, sizeof(why)) &&
           probe.signature_present && !probe.signature_checked && !probe.is_signed);
    assert(!ls_lap_inspect_file("lap-host-signed.lap", &probe, why, sizeof(why)) &&
           strstr(why, "signature") && !outstanding);
    remove("lap-host-signed.lap");
    cases++;
    open_app();
    step();
    assert(s.status.state == LS_LAP_RUNNING && enters == 1 && !crumb[0]);
    tui_cell back[400], front[400];
    tui_surface sf;
    tui_surface_setup(&sf, back, front, 20, 20);
    draw(&sf, tui_rect_make(3, 4, 10, 10));
    assert(draws == 1 && key(LS_TK_ESC, 0) && keys == 1 && touch(5, 7) && taps == 1 &&
           touch_col == 2 && touch_row == 3);
    close_app();
    assert(leaves == 1);
    cases++;
    fail_alloc = true;
    open_app();
    step();
    assert(s.status.state == LS_LAP_FAILED && !outstanding);
    close_app();
    cases++;
    fail_relocate = true;
    open_app();
    step();
    assert(s.status.state == LS_LAP_FAILED && !outstanding &&
           strstr(s.error, "needs forbidden_symbol"));
    close_app();
    fail_relocate = false;
    cases++;
    external = false;
    open_app();
    step();
    assert(s.status.state == LS_LAP_FAILED && !outstanding);
    close_app();
    external = true;
    cases++;
    bad_descriptor = true;
    open_app();
    step();
    assert(s.status.state == LS_LAP_FAILED);
    step();
    assert(s.status.state == LS_LAP_FAILED && !outstanding);
    close_app();
    bad_descriptor = false;
    cases++;
    open_app();
    step();
    assert(!ls_lap_maintenance_begin("probe"));
    assert(ls_lap_maintenance_begin("other"));
    ls_lap_info_t other = s.apps[0];
    snprintf(other.manifest.id, sizeof(other.manifest.id), "other");
    assert(ls_lap_publish(&other));
    ls_lap_poll();
    assert(ls_lap_by_id("other") && !strcmp(ls_app_current()->id, "probe") && s.entered);
    assert(ls_lap_maintenance_begin("other"));
    other.installed = false;
    assert(ls_lap_publish(&other));
    ls_lap_poll();
    assert(!ls_app_by_id("other") && !strcmp(ls_app_current()->id, "probe"));
    close_app();
    cases++;
    unsigned old_enters = enters;
    open_app();
    ls_tui_screen_show(0);
    step();
    step();
    assert(enters == old_enters && !outstanding);
    cases++;
    exit_entry = true;
    open_app();
    step();
    assert(current == 0 && enters == old_enters);
    step();
    assert(!outstanding);
    exit_entry = false;
    cases++;
    exit_enter = true;
    open_app();
    step();
    assert(current == 0);
    step();
    assert(!outstanding);
    exit_enter = false;
    cases++;
    exit_draw = true;
    open_app();
    step();
    draw(&sf, tui_rect_make(3, 4, 10, 10));
    assert(current == 0);
    step();
    assert(!outstanding);
    exit_draw = false;
    cases++;
    s.apps[0].needs_confirmation = true;
    ls_app_open(1);
    assert(s.status.state == LS_LAP_FAILED && s.status.needs_confirmation && !outstanding);
    assert(ls_lap_confirm("probe"));
    ls_lap_poll();
    step();
    assert(s.status.state == LS_LAP_RUNNING);
    close_app();
    cases++;
    assert(ls_lap_maintenance_begin(NULL));
    ls_app_open(1);
    assert(s.status.state == LS_LAP_FAILED);
    ls_tui_screen_show(0);
    ls_lap_maintenance_end();
    cases++;
    open_app();
    step();
    ls_tui_screen_show(0);
    io_service();
    assert(!outstanding && atomic_load(&operation) == IO_CLEANED && ls_lap_maintenance_begin(NULL));
    ls_lap_poll();
    assert(ls_lap_catalog_busy());
    ls_lap_maintenance_end();
    cases++;
    assert(ls_lap_stack_unused() == 4096);
    assert(!outstanding);
    /* Reinspect a real network-capability ELF, then prove no enter callback
       runs until approval and every relaunch asks again (even a signed tile). */
    in = fopen("lap-host-probe.lap", "r+b"); assert(in);
    unsigned char header[52]; assert(fread(header, 1, sizeof(header), in) == sizeof(header));
    uint32_t section_offset; uint16_t sections;
    memcpy(&section_offset, header + 32, 4); memcpy(&sections, header + 48, 2);
    bool changed = false;
    for (unsigned i = 0; i < sections; ++i) {
        uint32_t section[10];
        assert(!fseek(in, section_offset + i * 40, SEEK_SET));
        assert(fread(section, 1, sizeof(section), in) == sizeof(section));
        if (section[5] != LS_LAP_DESCRIPTOR_BYTES) continue;
        uint32_t magic; assert(!fseek(in, section[4], SEEK_SET)); assert(fread(&magic, 1, 4, in) == 4);
        if (magic != LAP_MAGIC) continue;
        assert(!fseek(in, section[4] + 130, SEEK_SET)); assert(fputc(LAP_CAP_NETWORK, in) != EOF);
        changed = true; break;
    }
    assert(changed && !fclose(in));
    assert(ls_lap_inspect_file("lap-host-probe.lap", &probe, why, sizeof(why)));
    register_info(&probe);
    expected_network = true;
    for (unsigned round = 0; round < 2; ++round) {
        unsigned before_enter = enters;
        s.apps[0].needs_confirmation = false;
        if (round) s.apps[0].is_signed = s.apps[0].signature_present = true;
        ls_app_open(1);
        assert(s.status.state == LS_LAP_FAILED && s.status.needs_confirmation && enters == before_enter);
        assert(!outstanding);
        assert(ls_lap_confirm("probe")); ls_lap_poll(); step();
        assert(s.status.state == LS_LAP_RUNNING && enters == before_enter + 1);
        close_app();
        ls_lap_api_callback(true);
        assert(lap_tcp_connect("localhost", 80, 1000) == LAP_NET_EACCES);
        ls_lap_api_callback(false);
    }
    cases += 2;
    remove("lap-host-probe.lap");
    rmdir("lap-host-data/probe");
    rmdir("lap-host-data");
    printf("test_lap_host: %u lifecycle/failure cases passed; all PSRAM and ELF buffers freed\n",
           cases);
    return 0;
}
