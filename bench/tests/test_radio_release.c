/* Nothing keeps the LoRa chip from the app on screen.

   Reported on the LR2021 board (2.21.3): SUB-GHZ WATCH on, HOME, then P25
   read "LR2021 IN USE ELSEWHERE" and the mesh stayed paused - WATCH's worker
   ("subghz_io") still held the chip as a foreground holder. Here every
   activity SUB-GHZ (and REC, which opens it) can start on the LoRa chip is
   started, the screen is left for each other app that wants the chip, and
   that app must have it within LEAVE_US; then, with that app gone to HOME,
   the mesh must have it back. A WATCH must also come back on its own.

   The runtime is the production one (rec_watch_runtime.c) and so is the
   lease (ls_mesh.cpp, through radio_release/lease.cpp). The other apps ask
   for the chip the way their code does: P25, FM and ADS-B on the LR2021 as
   background holders (app_p25.c, app_fm.c, app_adsb.c), LORA LABS, FALLS,
   MESHTASTIC and EXPERIMENTS as foreground holders, MESH by asking the
   chip back for MeshCore (scr_mesh.c). One thread: each app is a task id,
   and every wait in the runtime gives the others a turn. */
#include "ls_test.h"
#include <setjmp.h>
#include <stdint.h>
#include <sys/stat.h>
#ifdef _WIN32
#define mkdir(path,mode) mkdir(path)
#endif
uint32_t esp_random(void);
#define vTaskDelay rr_delay
#define xTaskGetCurrentTaskHandle rr_current_task
#define xTaskCreateStaticPinnedToCore rr_create_task
#define vTaskDelete rr_delete_task
#define vTaskSuspend rr_suspend_task
#define eTaskGetState rr_task_state
#define xQueueReceive rr_queue_receive
#include "../../components/lakeshark/apps/rec/rec_watch_runtime.c"
#undef xQueueReceive
BaseType_t xQueueReceive(QueueHandle_t queue, void *item, TickType_t ticks);

/* Not in the runtime before the fix; the case runs either way. */
extern bool rec_watch_lent(void) __attribute__((weak));
extern void rec_watch_screen_left(void) __attribute__((weak));

bool ls_mesh_radio_hold_bg(bool on);
bool ls_mesh_radio_wanted(void);
extern void rr_lease_reset(void);
extern bool rr_mesh_paused(void);
extern void rr_mesh_sent(void);
extern void rec_watch_screen_front(bool on) __attribute__((weak));
extern int  rr_lease_of(uintptr_t task);
extern void rr_lease_describe(char *out, size_t len);
extern void ls_shim_time_advance(int64_t);
extern void ls_mesh_radio_front(bool on);
extern void ls_mesh_radio_bg_allow(bool on);

enum { T_UI = 1, T_WORKER = 2, T_POLLER = 3, T_APP = 10 };
static uintptr_t rr_task = T_UI;
TaskHandle_t rr_current_task(void) { return (TaskHandle_t)rr_task; }

/* ---- the chip and the card, as far as the runtime asks ---- */
static ls_lora_chip_t chip = LS_LORA_CHIP_LR20XX;
static bool lr_fsk;
static int lr_streams;
static uint32_t frequency = 433920000u;
ls_lora_chip_t ls_lora_chip(void) { return chip; }
bool lr20xx_stream_source2_active(void) { return false; }
esp_err_t lr_wf_start_consumer(uint32_t hz, lr_wf_consumer_t consume, void *ctx)
{ (void)hz; (void)consume; (void)ctx; lr_streams++; return ESP_OK; }
void lr_wf_stop(void) {}
void lr_wf_get_stats(lr_wf_stats *out) { memset(out, 0, sizeof(*out)); }
void lr_wf_stop_stats(lr_wf_stats *out) { memset(out, 0, sizeof(*out)); }
uint32_t rec_get_freq(void) { return frequency; }
bool rec_set_freq(uint32_t hz) { if (s_status.enabled) return false; frequency = hz; return true; }
uint32_t ls_lora_caps(void) { return LS_LORA_CAP_RX_WIDE | LS_LORA_CAP_BAND_1G5_2G5 | LS_LORA_CAP_FSK_DETECT; }
esp_err_t ls_lora_scan_begin(uint32_t lo, uint32_t hi) { (void)lo; (void)hi; return ESP_OK; }
esp_err_t ls_lora_scan_end(void) { return ESP_OK; }
int ls_lora_scan_pass(float *row, int n, bool *done)
{ for (int i = 0; i < n; i++) row[i] = -110; *done = true; return n; }
uint32_t ls_lora_scan_bin_hz(uint32_t lo, uint32_t hi, int n, int i)
{ return lo + (uint64_t)(hi - lo) * i / (n - 1); }
uint32_t ls_lora_fsk_bw_snap(uint32_t hz) { return hz; }
esp_err_t ls_lora_fsk_begin(const ls_fsk_cfg_t *cfg) { (void)cfg; return ESP_OK; }
esp_err_t ls_lora_fsk_end(void) { return ESP_OK; }
int ls_lora_fsk_poll(uint8_t *b, size_t n, float *r) { memset(b, 0, n); (void)r; return 0; }
esp_err_t ls_lora_iq_send_rate_reason(const uint8_t *d, size_t n, uint32_t hz, int dbm,
                                      uint32_t rate, const char **why)
{ (void)d; (void)n; (void)hz; (void)dbm; (void)rate; *why = NULL; return ESP_OK; }
esp_err_t esp_vfs_fat_info(const char *path, uint64_t *total, uint64_t *free_bytes)
{ (void)path; *total = 1u << 30; *free_bytes = 1u << 29; return ESP_OK; }
bool ls_haptic_play(ls_haptic_effect_t effect) { (void)effect; return true; }
int settings_get_subghz_gate_db(void) { return 12; }
void settings_set_subghz_gate_db(int db) { (void)db; }
int settings_get_subghz_on_hit(void) { return 0; }
void settings_set_subghz_on_hit(int mode) { (void)mode; }
bool settings_get_subghz_lr_fsk(void) { return lr_fsk; }
void settings_set_subghz_lr_fsk(bool fsk) { (void)fsk; }
void settings_get_subghz_fsk(uint32_t *rate, uint32_t *dev, uint32_t *sync, int *pre, int *bw)
{ *rate = 2400; *dev = 10000; *sync = 0; *pre = 32; *bw = 59; }
void settings_set_subghz_fsk(uint32_t rate, uint32_t dev, uint32_t sync, int pre, int bw)
{ (void)rate; (void)dev; (void)sync; (void)pre; (void)bw; }
ls_rsel_radio_t ls_rsel_saved(ls_rsel_job_t job) { (void)job; return LS_RSEL_LORA; }
bool ls_mixrf_start(void) { return true; }
bool ls_mixrf_capture(bool on, uint32_t hz) { (void)on; (void)hz; return true; }
void ls_mixrf_snapshot(ls_mixrf_status_t *out) { memset(out, 0, sizeof(*out)); }
uint32_t esp_random(void) { return 1; }
/* Decoding, storage and sending: nothing this suite looks at. */
bool subghz_pwm_decode(const int32_t *p, int n, subghz_pwm_t *out) { (void)p; (void)n; (void)out; return false; }
bool subghz_nrz_decode(const int32_t *p, int n, subghz_nrz_t *out) { (void)p; (void)n; (void)out; return false; }
void ls_decoded_publish(const ls_decoded_t *d) { (void)d; }
esp_err_t ls_lora_ook_send(const uint8_t *c, size_t n, uint32_t bps) { (void)c; (void)n; (void)bps; return ESP_OK; }
esp_err_t ls_lora_fsk_send(const uint8_t *d, size_t n) { (void)d; (void)n; return ESP_OK; }
size_t rec_ook_packet(rec_ook_packet_t *st, const int32_t *e, size_t n, uint8_t *out, size_t cap, uint32_t *gap)
{ (void)st; (void)e; (void)n; (void)out; (void)cap; (void)gap; return 0; }
bool rec_watch_restore(const char *dir, rec_watch_catalog_t *c) { (void)dir; (void)c; return false; }
bool rec_watch_store_pumped(const char *dir, const rec_watch_catalog_t *c, uint64_t free_bytes,
                            void (*pump)(void *), void *context)
{ (void)dir; (void)c; (void)free_bytes; (void)pump; (void)context; return true; }
bool rec_watch_export(const char *dir, const rec_watch_catalog_t *c, uint32_t id, uint64_t free_bytes,
                      char *result, size_t len)
{ (void)dir; (void)c; (void)id; (void)free_bytes; snprintf(result, len, "-"); return true; }
const char *rec_dir(void) { return "/sdcard/lakeshark/rec"; }
int rec_pick_unique_name(const char *dir, const char *base, const char *ext, char *out, size_t len)
{ (void)dir; (void)ext; snprintf(out, len, "%s", base); return 0; }
bool ls_mixrf_replay(uint32_t hz, const int32_t *p, size_t n, int dbm) { (void)hz; (void)p; (void)n; (void)dbm; return false; }
bool ls_mixrf_replay_fsk(uint32_t hz, const int32_t *p, size_t n, int dbm, const subghz_cc_fsk_t *mod)
{ (void)hz; (void)p; (void)n; (void)dbm; (void)mod; return false; }

/* ---- the other apps ---- */
typedef enum { K_BG, K_FG, K_FRONT, K_HOME } kind_t;
typedef struct { const char *name; kind_t kind; bool radio_screen; } other_t;
static const other_t APPS[] = {
    { "P25 on the LR2021",  K_BG,    true  },
    { "FM on the LR2021",   K_BG,    true  },
    { "ADSB on the LR2021", K_BG,    true  },
    { "LORA LABS",          K_FG,    false },
    { "FALLS on the LR2021",K_FG,    false },
    { "MESHTASTIC",         K_FG,    false },
    { "EXPERIMENTS",        K_FG,    false },
    { "MESH",               K_FRONT, false },
    { "HOME",               K_HOME,  false },
};
#define N_APPS (int)(sizeof(APPS) / sizeof(APPS[0]))

static const other_t *app;          /* the one on screen after SUB-GHZ, or NULL */
static bool app_asked, app_has, app_gone;
static int64_t app_has_at;

/* One turn of the app on screen, as its own task. */
static void app_turn(void)
{
    if (!app || app_gone) return;
    const uintptr_t was = rr_task;
    rr_task = T_APP;
    switch (app->kind) {
    case K_BG: app_has = ls_mesh_radio_hold_bg(true); break;   /* P25.lora_wait while false */
    case K_FG:
        if (!app_asked) ls_mesh_radio_hold(true);
        app_has = ls_mesh_radio_held();
        break;
    case K_FRONT:
        if (!app_asked) ls_mesh_radio_front(true);
        app_has = !rr_mesh_paused();
        break;
    case K_HOME: app_has = !rr_mesh_paused(); break;
    }
    app_asked = true;
    if (app_has && !app_has_at) app_has_at = esp_timer_get_time();
    rr_task = was;
}
/* The app on screen goes to HOME. */
static void app_leave(void)
{
    const uintptr_t was = rr_task;
    rr_task = T_APP;
    if (app->kind == K_BG) ls_mesh_radio_hold_bg(false);
    if (app->kind == K_FG) ls_mesh_radio_hold(false);
    if (app->kind == K_FRONT) ls_mesh_radio_front(false);
    rr_task = was;
    app_gone = true;
    ls_mesh_radio_bg_allow(false);
}

/* ---- SUB-GHZ ---- */
typedef enum { A_WATCH, A_SWEEP, A_LEARN, A_CATCH, A_IQ } act_t;
typedef struct { const char *name; act_t act; ls_lora_chip_t chip; bool lr_fsk; } activity_t;
static const activity_t ACTS[] = {
    { "WATCH (LR2021 OOK stream)",  A_WATCH, LS_LORA_CHIP_LR20XX, false },
    { "WATCH (LR2021 FSK stream)",  A_WATCH, LS_LORA_CHIP_LR20XX, true  },
    { "WATCH (SX1262 FSK packets)", A_WATCH, LS_LORA_CHIP_SX126X, false },
    { "SCAN sweep",                 A_SWEEP, LS_LORA_CHIP_LR20XX, false },
    { "LEARN",                      A_LEARN, LS_LORA_CHIP_LR20XX, false },
    { "CATCH (sweep, then WATCH)",  A_CATCH, LS_LORA_CHIP_LR20XX, false },
    { "I/Q record",                 A_IQ,    LS_LORA_CHIP_LR20XX, false },
};
#define N_ACTS (int)(sizeof(ACTS) / sizeof(ACTS[0]))
static const activity_t *act;
static bool left, caught;
static int64_t left_at;

static int scan_passes;
static bool sweep_seen(void) { return scan_passes > 2; }
static bool watch_live(void) { return s_fsk_run || s_lr_run || s_lrf_run; }
static bool running(void)
{
    if (!rr_lease_of(T_WORKER)) return false;
    switch (act->act) {
    case A_WATCH: case A_CATCH: return watch_live();
    case A_SWEEP: return s_scan_busy && sweep_seen();
    case A_LEARN: return s_learn_busy;
    case A_IQ:    return s_iq_busy && lr_streams > 0;
    }
    return false;
}

/* SUB-GHZ is left: what its leave() does, then the screen manager. */
static void subghz_leave(void)
{
    if (rec_watch_screen_left) rec_watch_screen_left();
    else if (rec_watch_learn_busy()) rec_watch_scan_stop();   /* leave() before the fix */
    ls_mesh_radio_bg_allow(app->radio_screen);
    left = true; left_at = esp_timer_get_time();
}

/* Every wait in the runtime is a turn for everyone else. */
static void others(void)
{
    if (act && act->act == A_CATCH && !caught && s_scan_busy && rr_lease_of(T_WORKER) == 1) {
        caught = true; rec_watch_scan_catch(433920000u);
    }
    if (act && app && !left && running()) subghz_leave();
    if (left) app_turn();
}

/* ---- the FreeRTOS the runtime sees ---- */
static jmp_buf *rr_out;
static int rr_turns;
void rr_delay(TickType_t ticks)
{
    ls_shim_time_advance((int64_t)ticks * 1000);
    if (s_scan_busy && rr_task == T_WORKER) scan_passes++;
    others();
    /* The UI waiting on a WATCH request: the worker answers it. */
    if (rr_task == T_UI && s_watch_pending) {
        rr_task = T_WORKER;
        s_watch_result = rec_watch_enable_owned(s_watch_on); s_watch_pending = false;
        rr_task = T_UI;
    }
    if (!s_fsk_run) s_fsk_exited = true;
}
BaseType_t rr_queue_receive(QueueHandle_t q, void *item, TickType_t ticks)
{
    if (q == s_pending && ticks) {
        ls_shim_time_advance((int64_t)ticks * 1000);
        others();
        if (rr_out && --rr_turns <= 0) longjmp(*rr_out, 1);
    }
    return xQueueReceive(q, item, 0);
}
TaskHandle_t rr_create_task(TaskFunction_t task, const char *name, uint32_t depth, void *arg,
                            UBaseType_t priority, StackType_t *stack, StaticTask_t *tcb, BaseType_t core)
{ (void)task; (void)name; (void)depth; (void)arg; (void)priority; (void)stack; (void)tcb; (void)core;
  return (TaskHandle_t)(uintptr_t)T_POLLER; }
void rr_delete_task(TaskHandle_t task) { (void)task; }
void rr_suspend_task(TaskHandle_t task) { (void)task; }
eTaskState rr_task_state(TaskHandle_t task) { (void)task; return s_fsk_exited ? eSuspended : eRunning; }

/* The worker's loop for this many turns (100 ms each, plus whatever a job
   in it takes). */
static void worker_turns(int turns)
{
    jmp_buf here;
    jmp_buf *outer = rr_out;
    rr_out = &here; rr_turns = turns;
    const uintptr_t was = rr_task;
    rr_task = T_WORKER;
    if (!setjmp(here)) worker(NULL);
    rr_task = was; rr_out = outer;
}

static void setup(const activity_t *a, const other_t *to)
{
    rr_lease_reset();
    static bool once;
    if (!once) {
        once = true;
        s_catalog = calloc(1, sizeof(*s_catalog));
        s_checkpoint = malloc(sizeof(*s_checkpoint));
        static capture_t pool[2];
        s_free = xQueueCreate(2, sizeof(capture_t *)); s_pending = xQueueCreate(2, sizeof(capture_t *));
        for (int i = 0; i < 2; i++) { capture_t *p = &pool[i]; xQueueSend(s_free, &p, 0); }
    }
    memset(&s_status, 0, sizeof(s_status)); s_status.ready = true;
    s_scan_busy = s_learn_busy = s_scan_stop_req = s_catch_armed = false;
    s_scan_min = s_learn_secs = 0; s_scan_n = 0; s_scan_live_n = 0;
    s_watch_pending = s_watch_waiting = s_watch_taken = false;
    s_source = s_scan_source = REC_SOURCE_SX1262;
    s_replay_busy = false; s_replay_id = 0; s_replay_path[0] = 0;
    s_iq_busy = s_iq_pending = false; atomic_store(&s_iq_stop, false);
    s_fsk_run = s_lr_run = s_lrf_run = false; s_fsk_exited = true;
    s_fsk_loaded = true; s_fsk.bitrate = 2400; s_fsk.deviation_hz = 10000; s_fsk.bandwidth_khz = 59;
    s_fsk.preamble_bits = 32;
    s_lr_fsk = a->lr_fsk; s_lr_fsk_loaded = true; lr_fsk = a->lr_fsk;
    s_gate_db = 12; s_on_hit_loaded = 0; s_learn_rewatch = false;
    chip = a->chip; frequency = 433920000u; lr_streams = 0; scan_passes = 0;
    rr_task = T_UI;
    act = a; app = to; left = caught = false; left_at = 0;
    app_asked = app_has = app_gone = false; app_has_at = 0;
    ls_mesh_radio_bg_allow(true);   /* SUB-GHZ is a radio screen */
}

static bool start(const activity_t *a)
{
    switch (a->act) {
    case A_WATCH: return rec_watch_enable(true);
    case A_SWEEP: case A_CATCH: return rec_watch_request_scan(433000000u, 435000000u, 30, 32);
    case A_LEARN: return rec_watch_request_learn(30);
    case A_IQ:    return rec_watch_iq_record(433920000u);
    }
    return false;
}

#define LEAVE_US 1500000   /* the app on screen has the chip within this */

static int failures;
static void verdict(bool ok, const char *what)
{
    if (ok) return;
    char lease[96]; rr_lease_describe(lease, sizeof(lease));
    printf("  FAIL %-28s -> %-20s %s [%s]\n", act->name, app->name, what, lease);
    failures++;
}

LS_CASE(every_lora_activity_gives_the_chip_to_the_next_app_and_the_mesh)
{
    for (int i = 0; i < N_ACTS; i++)
        for (int j = 0; j < N_APPS; j++) {
            setup(&ACTS[i], &APPS[j]);
            const bool started = start(act);
            verdict(started, "did not start");
            if (!started) continue;
            worker_turns(60);   /* starts, runs, SUB-GHZ is left */
            verdict(left, "never had the chip");
            if (!left) continue;
            if (left && !app_has_at) for (int k = 0; k < 20 && !app_has; k++) worker_turns(1);
            verdict(app_has_at && app_has_at - left_at <= LEAVE_US,
                    app->kind == K_BG ? "shows LR2021 IN USE ELSEWHERE" :
                    app->kind == K_HOME ? "mesh still paused" : "never gets the chip");
            if (act->act == A_WATCH || act->act == A_CATCH) {
                /* The yield is visible: WATCH is still on, and says lent. */
                verdict(rec_watch_enabled(), "turned WATCH off instead of lending");
                if (rec_watch_lent) verdict(rec_watch_lent(), "WATCH does not say it is lent");
            }
            /* That app goes to HOME: nobody wants the chip but the mesh. */
            app_leave();
            worker_turns(10);
            verdict(!rr_mesh_paused(), "mesh still paused after HOME");
            if (act->act == A_WATCH || act->act == A_CATCH) {
                /* Back to SUB-GHZ: WATCH takes the chip back by itself. */
                ls_mesh_radio_bg_allow(true);
                worker_turns(10);
                verdict(watch_live() && rr_lease_of(T_WORKER) == 2, "WATCH did not come back");
                if (rec_watch_lent) verdict(!rec_watch_lent(), "WATCH still says lent");
                LS_CHECK(rec_watch_enable(false));
                worker_turns(3);
                verdict(!rr_mesh_paused() && !rr_lease_of(T_WORKER), "WATCH off kept the chip");
            }
            act = NULL; app = NULL;
        }
    printf("  %d of %d activity/app pairs failed\n", failures, N_ACTS * N_APPS);
    LS_EQ_INT(failures, 0);
}

/* The MESH screen opened over a running WATCH and closed again, without
   SUB-GHZ being left: WATCH lends the chip to MeshCore and takes it back. */
LS_CASE(watch_lent_to_the_mesh_screen_comes_back_after_it)
{
    static const other_t NOBODY = { "nobody", K_HOME, true };
    setup(&ACTS[0], &NOBODY);
    LS_CHECK(rec_watch_enable(true));
    worker_turns(2);
    LS_CHECK(watch_live());
    LS_EQ_INT(rr_lease_of(T_WORKER), rec_watch_lent ? 2 : 1);
    /* The MESH screen opens over it and closes again. */
    ls_mesh_radio_front(true);
    worker_turns(2);
    LS_CHECK(!watch_live());
    LS_CHECK(!rr_mesh_paused());
    ls_mesh_radio_front(false);
    worker_turns(2);
    LS_CHECK(watch_live());
    LS_CHECK(rec_watch_enable(false));
    worker_turns(2);
    LS_CHECK(!rr_mesh_paused());
}

/* Reported on the LR2021 board: SUB-GHZ READ, START, and the screen said
   "Mesh would not release the radio". WATCH asked as a background holder,
   which MeshCore's send window (8 s after an advert or a DM) refuses, and
   gave up after a second. Somebody pressing START on the screen outranks
   the mesh, as before lending existed; once the screen is left a WATCH
   that carries on becomes a background holder and lends the chip again. */
LS_CASE(start_on_the_screen_is_not_refused_by_a_mesh_send)
{
    static const other_t NOBODY = { "nobody", K_HOME, true };
    for (int i = 0; i < 2; i++) {
        setup(i ? &ACTS[2] : &ACTS[1], &NOBODY);
        app = NULL;   /* this case leaves the screen itself */
        if (rec_watch_screen_front) rec_watch_screen_front(true);
        rr_mesh_sent();
        const bool on = rec_watch_enable(true);
        worker_turns(2);
        LS_CHECK(on);
        LS_CHECK(watch_live());
        LS_CHECK(!s_fsk_error[0]);
        LS_EQ_INT(rr_lease_of(T_WORKER), 1);
        /* The screen goes (WATCH started from the console carries on):
           it hands over to a background hold that lends. */
        if (rec_watch_screen_left) rec_watch_screen_left();
        ls_mesh_radio_bg_allow(false);
        worker_turns(3);
        LS_CHECK(rr_lease_of(T_WORKER) != 1);
        LS_CHECK(!rr_mesh_paused());
        ls_mesh_radio_bg_allow(true);
        rr_lease_reset();
        worker_turns(3);
        LS_CHECK(watch_live() && rr_lease_of(T_WORKER) == 2);
        LS_CHECK(rec_watch_enable(false));
        worker_turns(2);
        LS_CHECK(!rr_mesh_paused());
    }
}
