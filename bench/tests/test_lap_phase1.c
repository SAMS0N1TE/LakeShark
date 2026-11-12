#include <errno.h>
/* API 2.3 runtime contracts; no windows or hardware. */
#include "ls_lap_api.h"
#include "ls_lap_kit.h"
#include "ls_lap_radio.h"
#include "ls_lap_service.h"
#include "ls_lap_sys.h"
#include "ls_keyboard.h"
#include "ls_picker.h"
#include "../shims/lap_kit_widgets.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <sys/stat.h>
#ifdef _WIN32
#include <direct.h>
#include <windows.h>
#else
#include <unistd.h>
#define _mkdir(p) mkdir(p, 0777)
#define _rmdir(p) rmdir(p)
#endif

static bool locked;
bool ls_tui_locked(void) { return locked; }
static bool keyboard_on;
static ls_keyboard_done_t keyboard_done;
static char keyboard_initial[769], keyboard_title[96];
void ls_keyboard_open(const char *t, const char *v, int max, ls_keyboard_done_t done) {
    assert(max >= 1 && max <= 768);
    snprintf(keyboard_initial, sizeof(keyboard_initial), "%s", v);
    snprintf(keyboard_title, sizeof(keyboard_title), "%s", t);
    keyboard_on = true; keyboard_done = done;
}
void ls_keyboard_open_lower(const char *t, const char *v, int max, ls_keyboard_done_t done) { ls_keyboard_open(t, v, max, done); }
void ls_keyboard_close(void) { keyboard_on = false; keyboard_done = NULL; }
bool ls_keyboard_active(void) { return keyboard_on; }
void ls_picker_note(const char *text) { assert(text && *text); }
static void type_done(const char *text) {
    ls_keyboard_done_t done = keyboard_done;
    keyboard_on = false; keyboard_done = NULL;
    if (done) done(text);
}
static void begin(const char *id, uint32_t gen) {
    ls_lap_api_begin(id, gen, 0); ls_lap_sys_begin();
    ls_lap_api_callback(true);
}
static void end(void) {
    ls_lap_sys_end(); ls_lap_api_callback(false); ls_lap_api_cleanup();
}
static void empty_tick(void) {}
static void stack_inputs(void) {
    begin("stack", 8);
    lap_app_t app = {.api_major = 2, .api_minor = 3, .flags = LAP_CAP_RADIO_RX | LAP_CAP_SERVICE};
    ls_lap_radio_begin(&app);
    lap_service_t svc = {.tick = empty_tick, .period_ms = 100};
    assert(!ls_lap_api_owns(&svc, sizeof(svc)) && lap_service_start(&svc) == 0);
    lap_decoded_t decoded = {.kind = LAP_EV_TEXT};
    strcpy(decoded.text, "stack input");
    assert(!ls_lap_api_owns(&decoded, sizeof(decoded)) && lap_emit(&decoded) == 0);
    assert(lap_service_start((const lap_service_t *)1) == LAP_SVC_EINVAL);
    assert(lap_emit((const lap_decoded_t *)1) == LAP_RX_EINVAL);
    end();
}
static int no_radios(lap_radio_info_t *out, int max) { (void)out; (void)max; return 0; }
static const ls_lap_rx_backend_t ABSENT = {.list = no_radios};
static void discovery(void) {
    begin("caps", 1);
    lap_caps_t stack_caps, *c = &stack_caps;
    assert(lap_caps((lap_caps_t *)1, sizeof(*c)) == LAP_RX_EINVAL);
    assert(lap_api_version() == ((LAP_API_MAJOR << 16) | LAP_API_MINOR));
    assert(lap_api_version() == 0x20006);
    ls_lap_sys_host()->hardware = LAP_HW_WIFI | LAP_HW_SD | LAP_HW_DESKTOP;
    assert(lap_caps(c, sizeof(*c) - 1) == LAP_RX_EINVAL);
    assert(lap_caps(c, sizeof(*c)) == 0 && c->size == sizeof(*c) && c->version == 1);
    assert((c->hardware & 255u) == (LAP_HW_WIFI | LAP_HW_SD | LAP_HW_DESKTOP) && c->granted == 0);
    lap_app_t app = {.api_major = 2, .api_minor = 3, .flags = LAP_CAP_RADIO_TX | LAP_CAP_SERVICE};
    ls_lap_radio_begin(&app);
    assert(lap_caps(c, sizeof(*c)) == 0 && c->granted == app.flags);
    ls_lap_radio_service();
    assert(lap_caps(c, sizeof(*c)) == 0 && c->radio_present && (c->radio_kinds & (1u << LAP_RADIO_KIND_SIM)));
    ls_lap_radio_set_backend(&ABSENT); ls_lap_radio_service();
    assert(lap_caps(c, sizeof(*c)) == 0 && !c->radio_present && !c->radio_tx && !c->radio_count);
    ls_lap_radio_set_backend(NULL);
    ls_lap_sys_host()->hardware = 0;
    end();
}
static void inputs(void) {
    begin("input", 2);
    lap_input_state_t stack_state, *s = &stack_state;
    lap_input_event_t stack_event, *e = &stack_event;
    while (lap_input_next(e) > 0) {}
    ls_input_ev_t in = {.kind = LS_IEV_KEY_DOWN, .key = LAP_KEY_LEFT, .clicks = 1};
    ls_lap_input_event(&in, 0, 0);
    in.key = LAP_KEY_CHAR; in.ch = 'W'; ls_lap_input_event(&in, 0, 0);
    assert(lap_key_down(LAP_KEY_LEFT) && lap_key_down(LAP_KEY_CHAR('w')));
    in = (ls_input_ev_t){.kind = LS_IEV_PTR_DOWN, .button = LS_IBTN_LEFT, .buttons = 1, .px = 71, .py = 82};
    ls_lap_input_event(&in, 10, 20);
    in.button = LS_IBTN_RIGHT; in.buttons = 5; ls_lap_input_event(&in, 10, 20);
    in.kind = LS_IEV_PTR_UP; in.button = LS_IBTN_LEFT; in.buttons = 4; ls_lap_input_event(&in, 10, 20);
    in.kind = LS_IEV_PTR_MOVE; in.px = 91; ls_lap_input_event(&in, 10, 20);
    in.kind = LS_IEV_WHEEL; in.wheel = -3; ls_lap_input_event(&in, 10, 20);
    assert(lap_input_state(s) == 0 && s->buttons == 4 && s->x == 81 && s->y == 62);
    int count = 0;
    while (lap_input_next(e) == 1) { count++; if (e->kind == LAP_INPUT_WHEEL) assert(e->wheel == -3 && e->buttons == 4); }
    assert(count == 7);
    in = (ls_input_ev_t){.kind = LS_IEV_KEY_UP, .key = LAP_KEY_LEFT}; ls_lap_input_event(&in, 0, 0);
    assert(!lap_key_down(LAP_KEY_LEFT) && lap_key_down(LAP_KEY_CHAR('w')));
    ls_lap_lifecycle(LAP_INPUT_FOCUS_LOST, 0, 0);
    assert(!lap_key_down(LAP_KEY_CHAR('w')));
    assert(lap_input_state(s) == 0 && !s->buttons);
    ls_lap_lifecycle(LAP_INPUT_PAUSE, 0, 0); ls_lap_lifecycle(LAP_INPUT_RESUME, 0, 0);
    ls_lap_lifecycle(LAP_INPUT_FOCUS_GAINED, 0, 0); ls_lap_lifecycle(LAP_INPUT_RESIZE, 800, 600);
    for (int i = 0; i < 80; i++) ls_lap_input_event(&in, 0, 0);
    assert(lap_input_state(s) == 0 && s->dropped >= 16);
    count = 0; while (lap_input_next(e) == 1) count++; assert(count == 64);
    ls_lap_sys_host_finger(100, 80, 10, 10, true); ls_lap_sys_frame((tui_rect){0,0,20,20});
    lap_touch_t touch; assert(lap_touch(&touch) && touch.down);
    assert(lap_input_next(e) == 1 && e->kind == LAP_INPUT_POINTER_DOWN);
    ls_lap_sys_host_finger(140, 80, 10, 10, false); ls_lap_sys_frame((tui_rect){0,0,20,20});
    assert(lap_swipe() != 0);
    /* Board/terminal grid pixels become app-client pixels too. */
    tui_surface surface = {.w = 40, .h = 30};
    ls_lap_api_draw_begin(&surface, (tui_rect){5,4,20,16});
    in = (ls_input_ev_t){.kind = LS_IEV_PTR_MOVE, .px = 83, .py = 110};
    ls_lap_input_board(&in);
    assert(lap_input_state(s) == 0 && s->x == 33 && s->y == 42);
    ls_lap_api_draw_end();
    locked = true; in = (ls_input_ev_t){.kind = LS_IEV_KEY_DOWN, .key = LAP_KEY_LEFT};
    ls_lap_input_board(&in); assert(!lap_key_down(LAP_KEY_LEFT));
    locked = false; ls_lap_input_board(&in); assert(lap_key_down(LAP_KEY_LEFT));
    end();
}
static void dialogs(void) {
    begin("dialog", 3);
    char out[769]; int stack_value, *value = &stack_value;
    char title[] = "Name", initial[] = "Ada";
    int h = lap_text_input(title, initial, 20, LAP_TEXT_LOWER); assert(h > 0);
    strcpy(initial, "bad"); assert(!strcmp(keyboard_initial, "Ada") && !strcmp(keyboard_title, "Name"));
    assert(lap_dialog_poll(h, out, 769, NULL) == 0);
    assert(lap_confirm("Busy?") == LAP_RX_EBUSY);
    type_done("Grace");
    assert(lap_dialog_poll(h, out, 2, NULL) == LAP_RX_EINVAL);
    assert(lap_dialog_poll(h, out, 769, NULL) == 1 && !strcmp(out, "Grace"));
    assert(lap_dialog_poll(h, out, 769, NULL) == LAP_RX_EINVAL);
    h = lap_confirm("Continue?"); assert(h > 0); lap_kit_pick(1);
    assert(lap_dialog_poll(h, NULL, 0, value) == 1 && *value == 1);
    const char *items[] = {"Alpha", "Beta"}; h = lap_choose(items, 2); assert(h > 0);
    lap_kit_pick(0); assert(lap_dialog_poll(h, NULL, 0, value) == 1 && *value == 0);
    h = lap_text_input("Cancel", "", 20, 0); ls_keyboard_close();
    assert(lap_dialog_poll(h, NULL, 0, NULL) == 2);
    h = lap_confirm("Cancel?"); lap_dialog_cancel(h); assert(lap_dialog_poll(h, NULL, 0, NULL) == 2);
    h = lap_text_input("Hidden", "", 20, 0); assert(h > 0);
    ls_lap_sys_hide();
    assert(!keyboard_on && lap_dialog_poll(h, NULL, 0, NULL) == 2);
    ls_lap_input_enable(true); ls_lap_kit_begin();
    h = lap_text_input("Unload", "", 20, 0); assert(h > 0);
    end(); assert(!keyboard_on);
    begin("next", 4); assert(lap_dialog_poll(h, NULL, 0, NULL) == LAP_RX_EINVAL); end();
}
static void complete(int job) {
    assert(job >= 0 && lap_sd_poll(job) == LAP_SD_PENDING);
    ls_lap_sys_service(); assert(lap_sd_poll(job) == 0);
}
static void saves(void) {
    _mkdir("p1-data"); begin("save-test", 5);
    assert(ls_lap_api_files_load("p1-data"));
    char data[128]; uint32_t stack_schema, *schema = &stack_schema;
    assert(lap_sd_read("lap-sd-io/x", 0, data, sizeof(data)) == LAP_SD_EINVAL);
    assert(lap_sd_list("lap-sd-io", (lap_sd_entry_t *)data, 1) == LAP_SD_EINVAL);
    int j = lap_save_write("progress", "checkpoint", 11, 7); assert(j >= 0);
    assert(lap_save("other", "x", 1) == LAP_SD_EBUSY);
    assert(lap_file_open("other", "w") < 0);
    complete(j);
    assert(lap_save_read("progress", data, 128, schema) == 11 && *schema == 7 && !memcmp(data, "checkpoint", 11));
    /* Failed staging preserves the previous on-card save and allows retry. */
    assert(_mkdir("p1-data/save-test/.lap-write") == 0);
    j = lap_save_write("progress", "checkpoint", 11, 8); assert(j >= 0);
    ls_lap_sys_service(); assert(lap_sd_poll(j) == LAP_SD_EIO);
    FILE *previous = fopen("p1-data/save-test/save_progress", "rb");
    unsigned char envelope[20]; assert(previous && fread(envelope, 1, 20, previous) == 20); fclose(previous);
    assert(envelope[8] == 7); /* old schema survived */
    /* The failed save's cleanup removes its staging path; remove() takes an
       empty directory with it on Linux and on the card's FAT, not on
       Windows. Either way the blocker is gone afterwards. */
    assert(_rmdir("p1-data/save-test/.lap-write") == 0 || errno == ENOENT);
    complete(lap_file_flush());
    complete(lap_settings_set("volume", "42", 3));
    assert(lap_settings_get("volume", data, 128) == 3 && !strcmp(data, "42"));
    assert(lap_settings_set("huge", data, 257) == LAP_SD_EINVAL);
    assert(lap_save("../escape", "x", 1) == LAP_SD_EINVAL);
    int fd = lap_file_open("ordinary", "w"); assert(fd >= 0 && lap_file_write(fd, "a", 1) == 1);
    complete(lap_file_flush()); assert(lap_file_write(fd, "b", 1) == 1); /* flush keeps handles */
    lap_file_close(fd); complete(lap_file_flush());
    end();
    /* Simulate interrupted replacement: old target was moved to backup. */
    assert(rename("p1-data/save-test/save_progress", "p1-data/save-test/.lap-old-save_progress") == 0);
    begin("save-test", 6); assert(ls_lap_api_files_load("p1-data"));
    schema = &stack_schema;
    assert(lap_save_read("progress", data, 128, schema) == 11 && *schema == 8);
    assert(lap_settings_get("volume", data, 128) == 3 && !strcmp(data, "42"));
    /* Corrupt envelope/checksum is an error, never handed to app as a save. */
    fd = lap_file_open("save_progress", "w"); assert(lap_file_write(fd, "bad", 3) == 3); lap_file_close(fd);
    assert(lap_load("progress", data, 128) == LAP_SD_EIO);
    assert(lap_load("missing", data, 128) == LAP_SD_ENOENT);
    end();
    begin("different-app", 7); assert(ls_lap_api_files_load("p1-data"));
    assert(lap_load("progress", data, 128) == LAP_SD_ENOENT); end();
    remove("p1-data/save-test/save_progress"); remove("p1-data/save-test/pref_volume"); remove("p1-data/save-test/ordinary");
    _rmdir("p1-data/save-test"); _rmdir("p1-data/different-app"); _rmdir("p1-data");
}
int main(void) {
    /* Model this test module's globals/literals as the loaded app image. */
#ifdef _WIN32
    unsigned char *base = (unsigned char *)GetModuleHandleA(NULL);
    IMAGE_NT_HEADERS *nt = (IMAGE_NT_HEADERS *)(base + ((IMAGE_DOS_HEADER *)base)->e_lfanew);
    ls_lap_api_set_image(base, nt->OptionalHeader.SizeOfImage);
#else
    extern char __executable_start, _end;
    ls_lap_api_set_image(&__executable_start, (uintptr_t)&_end - (uintptr_t)&__executable_start);
#endif
    stack_inputs(); discovery(); inputs(); dialogs(); saves();
    puts("test_lap_phase1: discovery, input, dialogs, durable data passed");
    return 0;
}
