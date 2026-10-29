/* API 2.1 for native apps: the ABI still resolving every 1.3 and 2.0 import,
 * manifests that may claim a service, the shared controls' wrappers (they
 * clip to the app's area, copy what they are handed, refuse memory that is
 * not the app's and never call an app back after it closed), the service
 * tick (grant, period, budget, stop) and lap_emit into the ls_decoded
 * timeline (tagged, rate limited, the 2.1 kinds kept from 2.0 apps).
 *
 *   test_lap_kit <api 1.3 exports> <api 2.0 exports> <app-sdk/exports.txt> <lakeshark_ui.h>
 *
 * The board widgets are the recording stand-ins in shims/lap_kit_widgets.c;
 * lssim draws the real ones. ls_lap.c's lifecycle (hide, show, unload) is
 * test_lap_service's. */
#include "ls_lap_api.h"
#include "ls_lap_kit.h"
#include "ls_lap_radio.h"
#include "ls_lap_service.h"
#include "ls_lap_sys.h"
#include "ls_options.h"
#include "core/ls_decoded.h"
#include "../../app-sdk/include/lakeshark_ui.h"
#include "../shims/lap_kit_widgets.h"
#include "ls_value.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void ls_tui_image(tui_rect r, const uint16_t *p, int w, int h, uint32_t serial) { (void)r; (void)p; (void)w; (void)h; (void)serial; }
void ls_tui_image_ring(tui_rect r, const uint16_t *p, int w, int h, int x0, int y0, int dx, int dy, uint32_t serial) {
    (void)r; (void)p; (void)w; (void)h; (void)x0; (void)y0; (void)dx; (void)dy; (void)serial;
}
bool ls_value_read(const char *path, ls_val_t *out, const char **unit) { (void)path; (void)out; (void)unit; return false; }

static unsigned checks;
#define CHECK(x) do { assert(x); checks++; } while (0)

static int64_t clock_us;
static int64_t host_clock(void) { return clock_us; }
static uint32_t host_ms(void) { return (uint32_t)(clock_us / 1000); }
extern uint32_t (*ls_lap_api_host_clock)(void);
extern uint32_t (*ls_lap_events_host_clock)(void);

static lap_app_t app;
static void enter(uint16_t minor, uint8_t flags) {
    memset(&app, 0, sizeof(app));
    snprintf(app.id, sizeof(app.id), "sentry");
    snprintf(app.name, sizeof(app.name), "SENTRY");
    app.api_major = 2;
    app.api_minor = minor;
    app.flags = flags;
    app.color = LAP_GREEN;
    ls_lap_api_begin(app.id, 1, 0);
    ls_lap_sys_begin();
    ls_lap_radio_begin(&app);
    ls_lap_api_callback(true);
}
static void leave(void) {
    ls_lap_api_callback(false);
    ls_lap_sys_end();
    for (int i = 0; i < 4; i++) ls_lap_radio_service();
    ls_lap_api_cleanup();
}

/* ---- the ABI ------------------------------------------------------------------ */

static int resolve_file(const char *path, int *missing) {
    FILE *f = fopen(path, "r");
    assert(f);
    char name[96];
    int n = 0;
    while (fscanf(f, "%95s", name) == 1) {
        n++;
        if (!ls_lap_resolve(name)) { printf("unresolved: %s\n", name); (*missing)++; }
    }
    fclose(f);
    return n;
}
static void abi(const char *api13, const char *api20, const char *now, const char *ui_h) {
    int missing = 0;
    CHECK(resolve_file(api13, &missing) == 216 && !missing);    /* every 1.3 import */
    CHECK(resolve_file(api20, &missing) == 239 && !missing);    /* every 2.0 import */
    CHECK(resolve_file(now, &missing) >= 275 && !missing);
    size_t n = 0;
    const ls_lap_export_t *e = ls_lap_exports(&n);
    for (size_t i = 1; i < n; i++) CHECK(strcmp(e[i - 1].name, e[i].name) < 0);
    /* Every call lakeshark_ui.h declares is exported. */
    FILE *f = fopen(ui_h, "r");
    assert(f);
    char line[256];
    int declared = 0;
    while (fgets(line, sizeof(line), f)) {
        char *p = strstr(line, "lap_");
        if (!p || line[0] == ' ' || line[0] == '/' || line[0] == '#' || !strchr(p, '(')) continue;
        char name[64];
        if (sscanf(p, "%63[a-z0-9_]", name) == 1 && p[strlen(name)] == '(' && !strstr(line, "typedef")) {
            if (!ls_lap_resolve(name)) printf("ui not exported: %s\n", name);
            CHECK(ls_lap_resolve(name));
            declared++;
        }
    }
    fclose(f);
    CHECK(declared == 29);                                         /* every call in the header */
    CHECK(ls_lap_resolve("lap_service_start") && ls_lap_resolve("lap_event_source") && ls_lap_resolve("lap_emit_dropped"));
}

static void manifests(void) {
    lap_app_t m = {.magic = LAP_MAGIC, .api_major = 2, .api_minor = 1};
    snprintf(m.id, sizeof(m.id), "svc"); snprintf(m.name, sizeof(m.name), "SVC");
    char why[96];
    CHECK(ls_lap_fields_valid(&m, why, sizeof(why)));                 /* a 2.1 app loads */
    m.flags = LAP_CAP_SERVICE | LAP_CAP_RADIO_RX;
    CHECK(ls_lap_fields_valid(&m, why, sizeof(why)));
    m.api_minor = 0;                                                  /* 2.0 cannot claim a service */
    CHECK(!ls_lap_fields_valid(&m, why, sizeof(why)));
    m.flags = LAP_CAP_RADIO_RX;
    CHECK(ls_lap_fields_valid(&m, why, sizeof(why)));                 /* and 2.0 still loads */
    m.api_minor = 2; m.flags = LAP_CAP_RADIO_RX | LAP_CAP_SERVICE;
    CHECK(ls_lap_fields_valid(&m, why, sizeof(why)));                 /* a 2.2 app loads */
    m.api_minor = 3; m.flags = 0;
    CHECK(!ls_lap_fields_valid(&m, why, sizeof(why)));                /* 2.3 is not this firmware */
    m.api_minor = 1; m.flags = 16;
    CHECK(!ls_lap_fields_valid(&m, why, sizeof(why)));                /* no such capability */
}

/* ---- the controls ----------------------------------------------------------- */

static tui_cell back[40 * 30], front[40 * 30];
static tui_surface sf;
static const tui_rect AREA = {5, 4, 20, 16};
static void draw_begin(void) {
    tui_surface_setup(&sf, back, front, 40, 30);
    tui_frame_begin(&sf);
    ls_lap_api_draw_begin(&sf, AREA);
}
static bool inside_area(tui_rect r) {
    return r.w > 0 && r.h > 0 && r.x >= AREA.x && r.y >= AREA.y && r.x + r.w <= AREA.x + AREA.w &&
           r.y + r.h <= AREA.y + AREA.h;
}
static bool outside_untouched(void) {
    for (int y = 0; y < 30; y++)
        for (int x = 0; x < 40; x++)
            if (!tui_rect_contains(AREA, x, y) && back[y * 40 + x].ch != ' ' && back[y * 40 + x].ch != 0) return false;
    return true;
}

static int picked = -1, sets, n_gets;
static double last_set;
static void on_pick(int i) { CHECK(ls_lap_api_inside()); picked = i; }
static double opt_get(int id) { n_gets++; return id == 2 ? 433.92 : 1; }
static void opt_set(int id, double v) { (void)id; sets++; last_set = v; }

static void controls(void) {
    enter(1, LAP_CAP_RADIO_RX);
    draw_begin();
    /* Keys: copied, translated and clipped. */
    lap_btn_t *keys = ls_lap_malloc(sizeof(lap_btn_t) * 3);
    char *words = ls_lap_malloc(32);
    strcpy(words, "RADIO");
    keys[0] = (lap_btn_t){words, NULL, 'r', false, false, false};
    keys[1] = (lap_btn_t){words, NULL, 'h', true, false, false};
    keys[2] = (lap_btn_t){words, NULL, 'o', false, true, false};
    lap_kit_seen.calls = 0;
    lap_ui_keys(0, 13, 40, 3, keys, 3, -1);                           /* wider than the app */
    CHECK(lap_kit_seen.calls == 1 && !strcmp(lap_kit_seen.what, "keys") && inside_area(lap_kit_seen.last));
    CHECK(lap_kit_seen.last.x == 5 && lap_kit_seen.last.y == 17 && lap_kit_seen.last.w == 20 && lap_kit_seen.last.h == 3);
    CHECK(!strcmp(lap_kit_seen.label[0], "RADIO") && lap_kit_seen.label_ptr[0] != words);   /* a copy */
    CHECK(lap_ui_key_hit(1, 14) == 0 && lap_ui_key_hit(19, 15) == 2 && lap_ui_key_hit(1, 2) == -1);
    lap_kit_seen.calls = 0;
    lap_ui_keys(0, 0, 10, 3, (const lap_btn_t *)&checks, 1, -1);       /* not the app's memory */
    lap_ui_keys(0, 0, 10, 3, keys, LAP_KEYS_MAX + 1, -1);              /* too many */
    lap_ui_keys(30, 30, 10, 3, keys, 1, -1);                           /* wholly outside */
    lap_ui_keys(-5000, 0, 10, 3, keys, 1, -1);
    CHECK(lap_kit_seen.calls == 0);
    keys[0].label = "STATIC";                                          /* a label that is not the app's */
    lap_ui_keys(0, 0, 10, 3, keys, 1, -1);
    CHECK(lap_kit_seen.calls == 1 && !lap_kit_seen.label[0][0]);       /* drawn blank, never read */
    keys[0].label = words;

    /* Glass, meters, the frequency pad: rects held to the area. */
    lap_kit_seen.calls = 0;
    lap_ui_bezel(-3, -3, 100, 100, NULL);
    CHECK(lap_kit_seen.calls == 1 && inside_area(lap_kit_seen.last));
    lap_ui_meter(10, 2, 40, 0.5f, LAP_GREEN);
    CHECK(inside_area(lap_kit_seen.last) && lap_kit_seen.last.w == 10);
    lap_ui_bargraph(0, 15, 20, 9, 0.3f, -1);
    CHECK(inside_area(lap_kit_seen.last) && lap_kit_seen.last.h == 1);
    CHECK(lap_ui_frequency(0, 2, 20, 8, 5000000000ull, NULL) == 7 && lap_kit_seen.hz == 0xFFFFFFFFul);
    lap_ui_freq_pad(0, 8, 20, 6, words, false);
    CHECK(lap_ui_freq_hit(3, 8) == '3' && lap_ui_freq_hit(0, 0) == 0);
    lap_ui_vfo(0, 0, 4, 20, 3, words, words, 0.5f, LAP_CYAN);
    float *frac = ls_lap_malloc(sizeof(float));
    CHECK(lap_ui_vfo_hit(0, 0, 5, frac) == LAP_UI_PART_DOWN && lap_ui_vfo_hit(0, 19, 5, frac) == LAP_UI_PART_UP);
    CHECK(lap_ui_vfo_hit(0, 3, 5, frac) == LAP_UI_PART_BAR && *frac == 0.0f);
    CHECK(lap_ui_vfo_hit(0, 3, 5, (float *)&checks) == LAP_UI_PART_BAR);   /* answer kept from foreign memory */
    CHECK(lap_ui_vfo_hit(9, 3, 5, frac) == -1);
    int *lv = ls_lap_malloc(sizeof(int) * 8);
    for (int i = 0; i < 8; i++) lv[i] = i;
    lap_kit_seen.calls = 0;
    lap_ui_sparkline(0, 0, 8, 2, lv, 8, 7, -1);
    lap_ui_sparkline(0, 0, 8, 2, lv, 9, 7, -1);                         /* runs past the app's block */
    CHECK(lap_kit_seen.calls == 1 && lap_kit_seen.n == 8);

    /* Braille history: drawn by the wrapper itself, inside the area only. */
    float *hist = ls_lap_malloc(sizeof(float) * 64);
    for (int i = 0; i < 64; i++) hist[i] = (float)i;
    lap_ui_history(-4, 10, 40, 10, hist, 64, 0, 63, LAP_CYAN);
    CHECK(outside_untouched());
    /* The strip is rows 14..19, 24 dots high. The newest pair (62, 63 of 63)
       fills its column; the oldest shown (24, 25) stands 9 and 10 dots, so
       its third cell up holds one left dot and two right ones. */
    CHECK(back[19 * 40 + 24].ch == 0x28FF && back[14 * 40 + 24].ch == 0x28FF);
    CHECK(back[19 * 40 + 5].ch == 0x28FF && back[17 * 40 + 5].ch == 0x2800 + 0xE0 && back[16 * 40 + 5].ch == ' ');

    /* The waterfall: normalised, claimed while shown, refused when hidden. */
    float *db = ls_lap_malloc(sizeof(float) * 4);
    lap_wf_line_t *line = ls_lap_malloc(sizeof(*line));
    db[0] = -120; db[1] = -100; db[2] = -60; db[3] = 0;
    *line = (lap_wf_line_t){433920000, 1024000, -100, -60, false};
    CHECK(lap_wf_push(db, 4, line) && lap_kit_seen.wf_bins == 4 && lap_kit_seen.wf_first == 0 && lap_kit_seen.wf_last == 1);
    CHECK(!lap_wf_push(db, 5, line) && !lap_wf_push(db, 4, (lap_wf_line_t *)&checks));
    lap_wf_draw(0, 0, 20, 8, LAP_WF_FULL);
    CHECK(!strcmp(lap_kit_seen.what, "wf") && inside_area(lap_kit_seen.last));
    ls_lap_service_set_shown(false);
    CHECK(!lap_wf_push(db, 4, line));                                  /* a closed screen never takes it */
    ls_lap_service_set_shown(true);
    CHECK(lap_wf_touch(2, 3) && lap_kit_seen.last.x == 7 && lap_kit_seen.last.y == 7);

    /* The picker calls back inside the app's context, and never after it closed. */
    CHECK(lap_picker_open(words, on_pick) && lap_picker_add(words, NULL) && lap_kit_seen.picker_rows == 1);
    CHECK(!lap_picker_add("NOT MINE", NULL));
    ls_lap_api_callback(false);                                        /* the owner taps, from the router */
    lap_kit_pick(0);
    ls_lap_api_callback(true);
    CHECK(picked == 0);
    CHECK(lap_picker_open(words, on_pick));
    ls_lap_kit_end();                                                  /* the app closes with it up */
    CHECK(lap_kit_seen.picker_closes >= 1);
    picked = -1;
    lap_kit_pick(1);
    CHECK(picked == -1);
    ls_lap_kit_begin();

    /* OPTIONS: copied into the board's table, callbacks through the guard. */
    lap_opt_t *rows = ls_lap_malloc(sizeof(lap_opt_t) * 4);
    lap_opt_list_t *root = ls_lap_malloc(sizeof(*root)), *sub = ls_lap_malloc(sizeof(*sub));
    const char **names = ls_lap_malloc(sizeof(char *) * 2);
    char *hold = ls_lap_malloc(16), *off = ls_lap_malloc(8), *on = ls_lap_malloc(8), *title = ls_lap_malloc(16);
    strcpy(hold, "PEAK HOLD"); strcpy(off, "OFF"); strcpy(on, "ON"); strcpy(title, "SENTRY");
    names[0] = off; names[1] = on;
    rows[0] = (lap_opt_t){.label = hold, .kind = LAP_OPT_TOGGLE, .id = 1, .names = names};
    rows[1] = (lap_opt_t){.label = hold, .kind = LAP_OPT_NUMBER, .id = 2, .lo = 24, .hi = 1766};
    rows[2] = (lap_opt_t){.label = hold, .kind = LAP_OPT_MENU, .sub = sub};
    rows[3] = (lap_opt_t){.label = hold, .kind = LAP_OPT_DISPLAY};
    *sub = (lap_opt_list_t){title, rows, 2, NULL, NULL};                /* inherits get and set */
    *root = (lap_opt_list_t){title, rows, 4, opt_get, opt_set};
    CHECK(lap_options_open(root) && lap_kit_seen.opt && lap_kit_seen.opt->n == 4);
    const ls_opt_ctx_t *c = lap_kit_seen.opt;
    CHECK(c->opt[0].label != hold && !strcmp(c->opt[0].label, "PEAK HOLD") && c->opt[0].names[1] != on);
    CHECK(c->opt[2].kind == LS_OPT_MENU && c->opt[2].sub && c->opt[2].sub->n == 2);
    CHECK(c->opt[3].kind == LS_OPT_MENU && !strcmp(c->opt[3].sub->name, "WATERFALL"));
    ls_lap_api_callback(false);                                        /* ls_options asks from the router */
    CHECK(c->opt[1].num(&c->opt[1]) == 433.92 && c->opt[0].get(&c->opt[0]) == 1);
    c->opt[2].sub->opt[1].set_num(&c->opt[2].sub->opt[1], 162.4);
    ls_lap_api_callback(true);
    CHECK(sets == 1 && last_set == 162.4);
    rows[0].label = "NOT MINE";
    CHECK(!lap_options_open(root));                                    /* refused whole, nothing half-built */
    rows[0].label = hold;
    rows[2].sub = root;                                                /* a loop: bounded, refused */
    CHECK(!lap_options_open(root));
    rows[2].sub = sub;
    CHECK(lap_options_open(root));
    c = lap_kit_seen.opt;
    const ls_opt_t row = c->opt[1];                                    /* what ls_options holds on to */
    const int asked = n_gets;
    leave();                                                           /* closing frees the table... */
    CHECK(lap_kit_seen.opt_closes >= 1);
    CHECK(row.num(&row) == 0 && n_gets == asked);                        /* ...and the app is never called again */

    enter(1, LAP_CAP_RADIO_RX);                                        /* ...and the next app starts clean */
    CHECK(!lap_ui_overlay());
    leave();
    /* Outside draw nothing draws; outside a callback nothing opens. */
    enter(1, 0);
    lap_kit_seen.calls = 0;
    lap_ui_bezel(0, 0, 10, 10, NULL);
    CHECK(lap_kit_seen.calls == 0);
    ls_lap_api_callback(false);
    CHECK(!lap_picker_open(NULL, on_pick) && lap_ui_key_hit(0, 0) == -1);
    ls_lap_api_callback(true);
    leave();
}

/* ---- the service tick -------------------------------------------------------- */

static int ticks, stopped_calls;
static int64_t tick_cost;
static void tick(void) { CHECK(ls_lap_api_inside()); ticks++; clock_us += tick_cost; }
static void stopped(void) { stopped_calls++; }
static void stop_in_tick(void) { ticks++; lap_service_stop(); }
static bool not_code(uintptr_t fn) { return fn != (uintptr_t)stopped; }
static const ls_lap_guard_t picky = {not_code, NULL, NULL};

static void service(void) {
    ls_lap_service_host_clock = host_clock;
    clock_us = 1000000;
    enter(1, LAP_CAP_RADIO_RX);                                        /* no SERVICE in the manifest */
    lap_service_t *svc = ls_lap_malloc(sizeof(*svc));
    *svc = (lap_service_t){tick, stopped, 200};
    CHECK(lap_service_start(svc) == LAP_SVC_EACCES && !lap_service_running());
    leave();
    enter(0, LAP_CAP_RADIO_RX);                                        /* a 2.0 app cannot have one */
    svc = ls_lap_malloc(sizeof(*svc));
    *svc = (lap_service_t){tick, stopped, 200};
    CHECK(lap_service_start(svc) == LAP_SVC_EACCES);
    leave();

    enter(1, LAP_CAP_SERVICE);
    svc = ls_lap_malloc(sizeof(*svc));
    *svc = (lap_service_t){NULL, NULL, 200};
    CHECK(lap_service_start(svc) == LAP_SVC_EINVAL);                   /* a tick is required */
    CHECK(lap_service_start((const lap_service_t *)&checks) == LAP_SVC_EINVAL);
    ls_lap_set_guard(&picky);
    *svc = (lap_service_t){tick, stopped, 200};
    CHECK(lap_service_start(svc) == LAP_SVC_EINVAL);                   /* a callback that is not app code */
    ls_lap_set_guard(NULL);
    *svc = (lap_service_t){tick, stopped, 1};                          /* clamped to 50 ms */
    CHECK(lap_service_start(svc) == LAP_SVC_OK && lap_service_running());
    ls_lap_api_callback(false);
    ticks = 0;
    tick_cost = 100;
    CHECK(ls_lap_service_poll() == 0);                                 /* not due yet */
    clock_us += 50000;
    CHECK(ls_lap_service_poll() == 1 && ticks == 1);
    CHECK(ls_lap_service_poll() == 0);
    for (int i = 0; i < 10; i++) { clock_us += 50000; ls_lap_service_poll(); }
    CHECK(ticks == 11);
    ls_lap_service_info_t info;
    CHECK(ls_lap_services(&info, 1) == 1 && !strcmp(info.id, "sentry") && info.period_ms == 50 &&
          info.stats.ticks == 11 && info.stats.last_us == 100);
    /* Over budget twice, under once: the count starts again. */
    tick_cost = LAP_SERVICE_BUDGET_US + 1;
    clock_us += 50000; CHECK(ls_lap_service_poll() == 1);
    clock_us += 50000; CHECK(ls_lap_service_poll() == 1);
    tick_cost = 10;
    clock_us += 50000; CHECK(ls_lap_service_poll() == 1);
    /* Three in a row stop it, and say why. */
    tick_cost = LAP_SERVICE_BUDGET_US + 1;
    clock_us += 50000; CHECK(ls_lap_service_poll() == 1);
    clock_us += 50000; CHECK(ls_lap_service_poll() == 1);
    clock_us += 50000; CHECK(ls_lap_service_poll() == -1 && strstr(ls_lap_service_why(), "too slow"));
    void (*cb)(void) = ls_lap_service_off(ls_lap_service_why());
    CHECK(cb == stopped && !ls_lap_service_on() && ls_lap_services(&info, 1) == 0);
    ls_lap_api_callback(true);
    /* One tick ten times over stops it at once. */
    CHECK(lap_service_start(svc) == LAP_SVC_OK);
    ls_lap_api_callback(false);
    tick_cost = 10 * LAP_SERVICE_BUDGET_US + 1;
    clock_us += 50000;
    CHECK(ls_lap_service_poll() == -1);
    ls_lap_service_off(NULL);
    /* STOP from RADIOS or the console: a flag the screen task takes once. */
    ls_lap_api_callback(true);
    tick_cost = 10;
    CHECK(lap_service_start(svc) == LAP_SVC_OK);
    ls_lap_api_callback(false);
    CHECK(!ls_lap_service_request_stop("other") && ls_lap_service_request_stop("sentry"));
    CHECK(ls_lap_service_take_stop() && !ls_lap_service_take_stop());
    ls_lap_service_off("STOP pressed");
    /* Stopping itself from a tick with the screen closed: the loader unloads it. */
    ls_lap_api_callback(true);
    *svc = (lap_service_t){stop_in_tick, NULL, 100};
    CHECK(lap_service_start(svc) == LAP_SVC_OK);
    ls_lap_api_callback(false);
    ls_lap_service_set_shown(false);
    clock_us += 100000;
    CHECK(ls_lap_service_poll() == -1 && !ls_lap_service_why()[0]);    /* its own wish: no notice */
    ls_lap_service_off(NULL);
    ls_lap_service_set_shown(true);
    ls_lap_api_callback(true);
    /* Inside a tick lap_screen_open says whether anyone is looking. */
    CHECK(lap_screen_open());
    leave();
    CHECK(!ls_lap_service_on());                                       /* closing the app ends it */
    ls_lap_service_host_clock = NULL;
}

/* ---- lap_emit into ls_decoded ----------------------------------------------- */

static void emit(void) {
    ls_lap_api_host_clock = host_ms;
    ls_lap_events_host_clock = host_ms;
    clock_us = 5000000;
    enter(0, LAP_CAP_RADIO_RX);                                        /* 2.0: still reserved */
    lap_decoded_t *d = ls_lap_malloc(sizeof(*d));
    memset(d, 0, sizeof(*d));
    d->kind = LAP_EV_TEXT;
    CHECK(lap_emit(d) == LAP_RX_EUNSUPPORTED);
    CHECK(lap_event_subscribe(0xFFFFFFFFu) == 0);
    leave();

    const int board = ls_decoded_open(LS_DECODED_KINDS);               /* the radio head's list, say */
    CHECK(board >= 0);
    enter(1, 0);                                                       /* 2.1 without RADIO_RX */
    d = ls_lap_malloc(sizeof(*d));
    memset(d, 0, sizeof(*d));
    d->kind = LAP_EV_TEXT;
    CHECK(lap_emit(d) == LAP_RX_EACCES);
    leave();

    enter(1, LAP_CAP_RADIO_RX);
    lap_event_t *ev = ls_lap_malloc(sizeof(*ev));
    char *src = ls_lap_malloc(32);
    CHECK(lap_event_subscribe(LAP_EV_TEXT | LAP_EV_PAGE) == 0);
    d = ls_lap_malloc(sizeof(*d));
    memset(d, 0, sizeof(*d));
    d->kind = LAP_EV_TEXT | LAP_EV_PAGE;
    CHECK(lap_emit(d) == LAP_RX_EINVAL);                               /* one kind a decode */
    d->kind = 1024;
    CHECK(lap_emit(d) == LAP_RX_EINVAL);                               /* no such kind */
    CHECK(lap_emit((const lap_decoded_t *)&checks) == LAP_RX_EINVAL);
    d->kind = LAP_EV_TEXT;
    memset(d->from, 'F', sizeof(d->from));                             /* unterminated: cut, not overrun */
    snprintf(d->text, sizeof(d->text), "carrier up 433.920");
    CHECK(lap_emit(d) == 0);
    ls_decoded_t got;
    CHECK(ls_decoded_next(board, &got, NULL) && got.kind == LS_DECODED_TEXT && !strcmp(got.source, "sentry") &&
          strlen(got.from) == sizeof(got.from) - 1 && !strcmp(got.text, "carrier up 433.920") && got.at_us == 5000000);
    CHECK(lap_event_next(ev) == 1 && ev->kind == 0 && ev->kind16 == LAP_EV_TEXT && !strcmp(ev->text, "carrier up 433.920"));
    CHECK(lap_event_source(src, 32) == 6 && !strcmp(src, "sentry"));
    ls_decoded_t page = {.kind = LS_DECODED_PAGE, .address = 77};      /* the board's own: no source */
    ls_decoded_publish(&page);
    CHECK(lap_event_next(ev) == 1 && ev->kind == LAP_EV_PAGE && ev->kind16 == LAP_EV_PAGE);
    CHECK(lap_event_source(src, 32) == 0 && !src[0]);
    /* Bursts of 8, then 4 a second. */
    int ok = 0;
    for (int i = 0; i < 20; i++) ok += lap_emit(d) == 0;
    CHECK(ok == 7 && lap_emit_dropped() == 13);                        /* one went above */
    clock_us += 250000;
    CHECK(lap_emit(d) == 0 && lap_emit(d) == LAP_RX_EAGAIN);
    clock_us += 10000000;
    ok = 0;
    for (int i = 0; i < 20; i++) ok += lap_emit(d) == 0;
    CHECK(ok == 8);                                                    /* refilled to the burst, no more */
    leave();
    /* A 2.0 reader is never handed a kind it cannot hold. */
    enter(0, LAP_CAP_RADIO_RX);
    ev = ls_lap_malloc(sizeof(*ev));
    CHECK(lap_event_subscribe(0xFFFFFFFFu) == 0);
    ls_decoded_t text = {.kind = LS_DECODED_TEXT};
    ls_decoded_publish(&text);
    ls_decoded_publish(&page);
    CHECK(lap_event_next(ev) == 1 && ev->kind == LAP_EV_PAGE && lap_event_next(ev) == 0);
    leave();
    ls_decoded_close(board);
    ls_lap_api_host_clock = NULL;
    ls_lap_events_host_clock = NULL;
}

int main(int argc, char **argv) {
    if (argc < 5) { fprintf(stderr, "usage: test_lap_kit <api13> <api20> <exports.txt> <lakeshark_ui.h>\n"); return 2; }
    abi(argv[1], argv[2], argv[3], argv[4]);
    manifests();
    controls();
    service();
    emit();
    printf("test_lap_kit: %u checks passed\n", checks);
    return 0;
}
