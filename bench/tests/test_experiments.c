/* LS_TEST_SOURCES: ls_experiments.c, ls_experiments_builtin.c,
   experiments/exp_carrier.c, experiments/exp_dfm17.c,
   experiments/dfm_decode.c and ls_geo.c against a faked socket and LoRa
   chip: the list, one experiment at a time, start failures and their
   reasons, the `exp` console command, and the readout plumbing */

#include "ls_test.h"

#include "ls_experiments.h"
#include "ls_gps.h"
#include "ls_lora.h"
#include "esp_timer.h"

#include <stdio.h>
#include <string.h>

void ls_shim_time_set(int64_t us);
void ls_shim_time_advance(int64_t us);

/* ------------------------------------------------------------ the board -- */

static const char *s_busy;
static bool s_take_ok = true;
static int s_takes, s_gives, s_wakes;

const char *ls_exp_hw_radio_busy(void) { return s_busy; }
bool ls_exp_hw_radio_take(void) { s_takes++; return s_take_ok; }
void ls_exp_hw_radio_give(void) { s_gives++; }
/* No worker: ls_exp_settle services the request itself, and a test calls
   ls_exp_service where the task would. */
bool ls_exp_hw_wake(void) { s_wakes++; return false; }

/* The LoRa chip CARRIER LEVEL drives. */
static uint32_t s_caps = LS_LORA_CAP_LORA | LS_LORA_CAP_FSK | LS_LORA_CAP_RSSI_INST;
static esp_err_t s_begin_err;
static bool s_fsk;
static ls_fsk_cfg_t s_cfg;
static int s_begins, s_ends, s_rearms, s_fsk_polls;
static float s_rssi = -100.0f;
static esp_err_t s_rssi_err;

uint32_t ls_lora_caps(void) { return s_caps; }
uint32_t ls_lora_fsk_bw_snap(uint32_t hz) { return hz < 14600 ? 14600 : hz; }
esp_err_t ls_lora_fsk_begin(const ls_fsk_cfg_t *cfg)
{
    s_begins++;
    if (s_begin_err != ESP_OK) return s_begin_err;
    s_cfg = *cfg;
    s_fsk = true;
    return ESP_OK;
}
esp_err_t ls_lora_fsk_end(void) { s_ends++; s_fsk = false; return ESP_OK; }
esp_err_t ls_lora_fsk_receive(void) { s_rearms++; return ESP_OK; }
int ls_lora_fsk_poll(uint8_t *buf, size_t size, float *rssi)
{
    (void)buf; (void)size; (void)rssi;
    s_fsk_polls++;
    return 0;
}
/* LR433, built in beside CARRIER LEVEL, opens OOK sessions. */
esp_err_t ls_lora_ook_begin(const ls_ook_cfg_t *cfg) { (void)cfg; return ESP_ERR_NOT_SUPPORTED; }
int ls_lora_ook_poll(uint8_t *buf, size_t size, float *rssi) { (void)buf; (void)size; (void)rssi; return -1; }
esp_err_t ls_lora_ook_end(void) { return ESP_OK; }
bool ls_lora_ook_active(void) { return false; }

esp_err_t ls_lora_rssi_inst(float *dbm)
{
    if (s_rssi_err != ESP_OK) return s_rssi_err;
    *dbm = s_rssi;
    return ESP_OK;
}
/* The others the built-in experiments link against; their own tests drive
   them. */
esp_err_t ls_lora_fsk_retune(uint32_t hz) { (void)hz; return s_fsk ? ESP_OK : ESP_ERR_INVALID_STATE; }
esp_err_t ls_lora_scan_begin(uint32_t lo, uint32_t hi) { (void)lo; (void)hi; return ESP_ERR_NOT_SUPPORTED; }
int ls_lora_scan_pass(float *dbm, int n, bool *done) { (void)dbm; (void)n; if (done) *done = false; return 0; }
esp_err_t ls_lora_scan_end(void) { return ESP_OK; }
/* A LoRa configuration, which a sweep wants to hand the part back to. */
static ls_lora_cfg_t s_lora_cfg;
static bool s_lora_cfg_set;
const ls_lora_cfg_t *ls_lora_cfg(void) { return s_lora_cfg_set ? &s_lora_cfg : NULL; }
void ls_lora_cfg_default(ls_lora_cfg_t *out) { memset(out, 0, sizeof(*out)); out->freq_hz = 910525000u; }
esp_err_t ls_lora_configure(const ls_lora_cfg_t *cfg) { s_lora_cfg = *cfg; s_lora_cfg_set = true; return ESP_OK; }

/* Where RADIOSONDE measures range from; test_exp_dfm17.c drives it. */
bool settings_get_home(float *lat, float *lon) { (void)lat; (void)lon; return false; }
void ls_gps_get(ls_gps_state_t *out) { memset(out, 0, sizeof(*out)); }

/* ---------------------------------------------------- fake experiments -- */

typedef struct {
    int starts, stops, polls, configures;
    bool refuse;
    char last_arg[32];
} calls_t;

static calls_t A, B, R;

static bool a_start(char *why, size_t n)
{
    A.starts++;
    if (A.refuse) { snprintf(why, n, "no carrier at all"); return false; }
    return true;
}
static void a_stop(void) { A.stops++; }
static void a_poll(void) { A.polls++; }
static int a_lines(char (*out)[LS_EXP_LINE], int max)
{
    int n = 0;
    if (n < max) snprintf(out[n++], LS_EXP_LINE, "polls %d", A.polls);
    if (n < max) snprintf(out[n++], LS_EXP_LINE, "second line");
    return n;
}
static bool a_configure(int argc, char **argv, char *why, size_t n)
{
    A.configures++;
    if (argc != 1 || !strcmp(argv[0], "bad")) { snprintf(why, n, "wants one good word"); return false; }
    snprintf(A.last_arg, sizeof(A.last_arg), "%s", argv[0]);
    return true;
}

/* Refuses with nothing said. */
static bool b_start(char *why, size_t n) { (void)why; (void)n; B.starts++; return !B.refuse; }
static void b_stop(void) { B.stops++; }

/* Uses no radio. */
static bool r_start(char *why, size_t n) { (void)why; (void)n; R.starts++; return true; }
static void r_stop(void) { R.stops++; }

/* A readout that misbehaves: more lines than asked for, none terminated. */
static int wild_lines(char (*out)[LS_EXP_LINE], int max)
{
    for (int i = 0; i < max; i++) memset(out[i], 'x', LS_EXP_LINE);
    return max + 5;
}
static int negative_lines(char (*out)[LS_EXP_LINE], int max) { (void)out; (void)max; return -3; }

static const ls_experiment_t EXP_A = {
    .id = "alpha", .name = "ALPHA", .sub = "first", .maturity = LS_EXP_TRYING,
    .start = a_start, .stop = a_stop, .poll = a_poll, .lines = a_lines, .configure = a_configure,
};
static const ls_experiment_t EXP_B = {
    .id = "bravo", .name = "BRAVO", .sub = "second", .maturity = LS_EXP_IDEA,
    .start = b_start, .stop = b_stop,
};
static const ls_experiment_t EXP_R = {
    .id = "quiet", .name = "QUIET", .maturity = LS_EXP_WORKS,
    .start = r_start, .stop = r_stop, .no_radio = true,
};
static const ls_experiment_t EXP_WILD = { .id = "wild", .name = "WILD", .lines = wild_lines };
static const ls_experiment_t EXP_NEG = { .id = "neg", .name = "NEG", .lines = negative_lines };

static void fresh(void)
{
    ls_exp_forget();
    memset(&A, 0, sizeof(A));
    memset(&B, 0, sizeof(B));
    memset(&R, 0, sizeof(R));
    s_busy = NULL;
    s_take_ok = true;
    s_takes = s_gives = s_wakes = 0;
    s_caps = LS_LORA_CAP_LORA | LS_LORA_CAP_FSK | LS_LORA_CAP_RSSI_INST;
    s_begin_err = ESP_OK;
    s_fsk = false;
    memset(&s_cfg, 0, sizeof(s_cfg));
    s_begins = s_ends = s_rearms = s_fsk_polls = 0;
    s_rssi = -100.0f;
    s_rssi_err = ESP_OK;
    ls_shim_time_set(1000000);
    ls_exp_register(&EXP_A);
    ls_exp_register(&EXP_B);
    ls_exp_register(&EXP_R);
}

static int maintenance_passes;
static bool maintain(void) { return ++maintenance_passes < 2; }
LS_CASE(stopped_maintenance_runs_without_starting_or_taking_radio)
{
    fresh(); maintenance_passes = 0;
    static const ls_experiment_t e = { .id = "upkeep", .name = "UPKEEP", .maintenance = maintain };
    ls_exp_register(&e);
    LS_CHECK(ls_exp_service()); LS_CHECK(!ls_exp_service());
    LS_EQ_INT(maintenance_passes, 2); LS_EQ_INT(s_takes, 0); LS_EQ_INT(s_begins, 0);
    LS_CHECK(ls_exp_running() == NULL);
}

static void state_of(const ls_experiment_t *e, char *out)
{
    ls_exp_state_line(e, out, 64);
}

static int run_console(const char *line)
{
    char buf[128];
    snprintf(buf, sizeof(buf), "%s", line);
    char *argv[8];
    int argc = 0;
    for (char *t = strtok(buf, " "); t && argc < 8; t = strtok(NULL, " ")) argv[argc++] = t;
    return ls_exp_console(argc, argv);
}

/* ------------------------------------------------------------- the list -- */

LS_CASE(the_list_holds_each_id_once_in_registration_order)
{
    fresh();
    ls_exp_register(&EXP_A);
    static const ls_experiment_t SAME_ID = { .id = "alpha", .name = "IMPOSTOR" };
    ls_exp_register(&SAME_ID);
    ls_exp_register(NULL);
    LS_EQ_INT(ls_exp_count(), 3);
    LS_CHECK(ls_exp_at(0) == &EXP_A);
    LS_CHECK(ls_exp_at(2) == &EXP_R);
    LS_CHECK(ls_exp_at(3) == NULL);
    LS_CHECK(ls_exp_at(-1) == NULL);
    LS_CHECK(ls_exp_find("bravo") == &EXP_B);
    LS_CHECK(ls_exp_find("charlie") == NULL);
    LS_CHECK(ls_exp_find(NULL) == NULL);
    LS_EQ_STR(ls_exp_maturity_name(LS_EXP_IDEA), "IDEA");
    LS_EQ_STR(ls_exp_maturity_name(LS_EXP_TRYING), "TRYING");
    LS_EQ_STR(ls_exp_maturity_name(LS_EXP_WORKS), "WORKS");
}

LS_CASE(the_list_stops_at_its_capacity)
{
    fresh();
    static char ids[40][8];
    static ls_experiment_t many[40];
    for (int i = 0; i < 40; i++) {
        snprintf(ids[i], sizeof(ids[i]), "e%d", i);
        many[i] = (ls_experiment_t){ .id = ids[i], .name = ids[i] };
        ls_exp_register(&many[i]);
    }
    LS_EQ_INT(ls_exp_count(), LS_EXP_MAX);
}

LS_CASE(the_builtins_register_once_and_carrier_is_among_them)
{
    ls_exp_forget();
    ls_exp_register_builtin();
    ls_exp_register_builtin();
    const ls_experiment_t *c = ls_exp_find("carrier");
    LS_CHECK(c != NULL);
    int seen = 0;
    for (int i = 0; i < ls_exp_count(); i++) if (ls_exp_at(i) == c) seen++;
    LS_EQ_INT(seen, 1);
    LS_EQ_STR(c->name, "CARRIER LEVEL");
    LS_EQ_INT(c->maturity, LS_EXP_WORKS);
    LS_EQ_INT(c->n_opts, 1);
    LS_CHECK(!c->no_radio);
}

/* ------------------------------------------------------------ running -- */

LS_CASE(a_start_waits_for_the_worker_and_then_runs_with_the_radio)
{
    fresh();
    char st[64];
    ls_exp_start(&EXP_A);
    LS_CHECK(s_wakes > 0);
    LS_CHECK(ls_exp_busy());
    LS_CHECK(ls_exp_running() == NULL);
    LS_CHECK(ls_exp_wanted() == &EXP_A);
    state_of(&EXP_A, st);
    LS_EQ_STR(st, "starting");
    LS_EQ_INT(A.starts, 0);

    LS_CHECK(ls_exp_service());
    LS_EQ_INT(A.starts, 1);
    LS_EQ_INT(s_takes, 1);
    LS_CHECK(ls_exp_running() == &EXP_A);
    LS_CHECK(!ls_exp_busy());
    LS_EQ_INT(A.polls, 1);
    state_of(&EXP_A, st);
    LS_EQ_STR(st, "running 0s");
    state_of(&EXP_B, st);
    LS_EQ_STR(st, "stopped");

    ls_exp_service();
    ls_exp_service();
    LS_EQ_INT(A.polls, 3);
    ls_shim_time_advance(72 * 1000000LL);
    state_of(&EXP_A, st);
    LS_EQ_STR(st, "running 1m12s");
    ls_shim_time_advance(3600 * 1000000LL);
    state_of(&EXP_A, st);
    LS_EQ_STR(st, "running 1h01m");

    ls_exp_stop();
    state_of(&EXP_A, st);
    LS_EQ_STR(st, "stopping");
    LS_CHECK(!ls_exp_service());
    LS_EQ_INT(A.stops, 1);
    LS_EQ_INT(s_gives, 1);
    LS_CHECK(ls_exp_running() == NULL);
    state_of(&EXP_A, st);
    LS_EQ_STR(st, "stopped");
    /* Nothing to come back for, and polling stopped with it. */
    LS_CHECK(!ls_exp_service());
    LS_EQ_INT(A.polls, 3);
}

LS_CASE(starting_another_stops_the_first_and_keeps_the_socket)
{
    fresh();
    ls_exp_start(&EXP_A);
    ls_exp_service();
    ls_exp_start(&EXP_B);
    ls_exp_service();
    LS_EQ_INT(A.stops, 1);
    LS_EQ_INT(B.starts, 1);
    LS_CHECK(ls_exp_running() == &EXP_B);
    /* Handed straight across: MeshCore never had it in between. */
    LS_EQ_INT(s_takes, 1);
    LS_EQ_INT(s_gives, 0);

    /* Asked twice before the worker looked: only the last ask counts. */
    ls_exp_start(&EXP_A);
    ls_exp_start(&EXP_R);
    ls_exp_service();
    LS_EQ_INT(A.starts, 1);
    LS_EQ_INT(B.stops, 1);
    LS_EQ_INT(R.starts, 1);
    LS_CHECK(ls_exp_running() == &EXP_R);
    /* QUIET uses no radio, so the socket went back. */
    LS_EQ_INT(s_gives, 1);

    ls_exp_stop();
    ls_exp_service();
    LS_EQ_INT(R.stops, 1);
    LS_EQ_INT(s_gives, 1);
    LS_EQ_INT(s_takes, 1);
}

LS_CASE(starting_the_running_one_restarts_it)
{
    fresh();
    char st[64];
    ls_exp_start(&EXP_A);
    ls_exp_service();
    ls_exp_start(&EXP_A);
    state_of(&EXP_A, st);
    LS_EQ_STR(st, "restarting");
    ls_exp_service();
    LS_EQ_INT(A.stops, 1);
    LS_EQ_INT(A.starts, 2);
    LS_CHECK(ls_exp_running() == &EXP_A);
    LS_EQ_INT(s_takes, 1);
    LS_EQ_INT(s_gives, 0);
}

LS_CASE(a_start_that_fails_says_why_and_gives_the_radio_back)
{
    fresh();
    char st[64];
    A.refuse = true;
    ls_exp_start(&EXP_A);
    ls_exp_service();
    LS_CHECK(ls_exp_running() == NULL);
    LS_EQ_INT(s_takes, 1);
    LS_EQ_INT(s_gives, 1);
    state_of(&EXP_A, st);
    LS_EQ_STR(st, "no carrier at all");
    /* Only the one that failed carries the reason. */
    state_of(&EXP_B, st);
    LS_EQ_STR(st, "stopped");

    /* Asking again clears it while the ask waits. */
    A.refuse = false;
    ls_exp_start(&EXP_A);
    state_of(&EXP_A, st);
    LS_EQ_STR(st, "starting");
    ls_exp_service();
    LS_CHECK(ls_exp_running() == &EXP_A);

    /* A refusal with no words gets some. */
    B.refuse = true;
    ls_exp_start(&EXP_B);
    ls_exp_service();
    LS_EQ_INT(A.stops, 1);
    LS_CHECK(ls_exp_running() == NULL);
    state_of(&EXP_B, st);
    LS_EQ_STR(st, "would not start");
    LS_EQ_INT(s_gives, 2);
}

LS_CASE(a_busy_socket_refuses_before_start_is_called)
{
    fresh();
    char st[64];
    s_busy = "LoRa Labs owns the radio; turn DIRECT off first";
    ls_exp_start(&EXP_A);
    ls_exp_service();
    LS_EQ_INT(A.starts, 0);
    LS_EQ_INT(s_takes, 0);
    LS_CHECK(ls_exp_running() == NULL);
    state_of(&EXP_A, st);
    LS_EQ_STR(st, "LoRa Labs owns the radio; turn DIRECT off first");

    /* One that uses no radio does not ask. */
    ls_exp_start(&EXP_R);
    ls_exp_service();
    LS_EQ_INT(R.starts, 1);
    LS_CHECK(ls_exp_running() == &EXP_R);

    /* MeshCore holding on is a reason too. */
    s_busy = NULL;
    s_take_ok = false;
    ls_exp_start(&EXP_A);
    ls_exp_service();
    LS_EQ_INT(R.stops, 1);
    LS_EQ_INT(A.starts, 0);
    state_of(&EXP_A, st);
    LS_EQ_STR(st, "MeshCore would not release the LoRa radio");
}

LS_CASE(a_busy_socket_after_a_switch_gives_back_what_was_held)
{
    fresh();
    ls_exp_start(&EXP_A);
    ls_exp_service();
    /* The first one's stop left something running on the chip. */
    s_busy = "An FSK session is using the LoRa chip";
    ls_exp_start(&EXP_B);
    ls_exp_service();
    LS_EQ_INT(A.stops, 1);
    LS_EQ_INT(B.starts, 0);
    LS_EQ_INT(s_takes, 1);
    LS_EQ_INT(s_gives, 1);
    LS_CHECK(ls_exp_running() == NULL);
}

/* ------------------------------------------------------------- readout -- */

LS_CASE(the_readout_is_clamped_and_terminated)
{
    fresh();
    static char out[6][LS_EXP_LINE];
    LS_EQ_INT(ls_exp_read_lines(&EXP_A, out, 6), 2);
    LS_EQ_STR(out[0], "polls 0");
    LS_EQ_STR(out[1], "second line");
    LS_EQ_INT(ls_exp_read_lines(&EXP_A, out, 1), 1);
    LS_EQ_INT(ls_exp_read_lines(&EXP_WILD, out, 4), 4);
    LS_EQ_INT((int)strlen(out[3]), LS_EXP_LINE - 1);
    LS_EQ_INT(ls_exp_read_lines(&EXP_NEG, out, 4), 0);
    LS_EQ_INT(ls_exp_read_lines(&EXP_B, out, 4), 0);
    LS_EQ_INT(ls_exp_read_lines(NULL, out, 4), 0);
    LS_EQ_INT(ls_exp_read_lines(&EXP_A, out, 0), 0);
}

/* ------------------------------------------------------------- console -- */

LS_CASE(the_console_lists_starts_and_stops)
{
    fresh();
    LS_EQ_INT(run_console("exp"), 0);
    LS_EQ_INT(run_console("exp nope"), 1);
    LS_EQ_INT(run_console("exp alpha"), 0);
    LS_EQ_INT(run_console("exp alpha dance"), 1);
    LS_EQ_INT(run_console("exp alpha stop"), 1);

    LS_EQ_INT(run_console("exp alpha start"), 0);
    LS_CHECK(ls_exp_running() == &EXP_A);
    LS_EQ_INT(A.configures, 0);
    LS_EQ_INT(run_console("exp alpha"), 0);

    /* Arguments go to configure first, and a refusal starts nothing. */
    LS_EQ_INT(run_console("exp alpha start 433.92"), 0);
    LS_EQ_INT(A.configures, 1);
    LS_EQ_STR(A.last_arg, "433.92");
    LS_EQ_INT(A.starts, 2);
    LS_EQ_INT(run_console("exp alpha start bad"), 1);
    LS_EQ_INT(A.starts, 2);
    LS_EQ_INT(run_console("exp bravo start 5"), 1);
    LS_EQ_INT(B.starts, 0);

    LS_EQ_INT(run_console("exp bravo start"), 0);
    LS_EQ_INT(A.stops, 2);
    LS_CHECK(ls_exp_running() == &EXP_B);
    LS_EQ_INT(run_console("exp alpha stop"), 1);
    LS_EQ_INT(run_console("exp bravo stop"), 0);
    LS_CHECK(ls_exp_running() == NULL);

    B.refuse = true;
    LS_EQ_INT(run_console("exp bravo start"), 1);
    LS_EQ_INT(run_console("exp stop"), 0);

    LS_EQ_INT(run_console("exp quiet start"), 0);
    LS_EQ_INT(run_console("exp stop"), 0);
    LS_EQ_INT(R.stops, 1);
    LS_CHECK(!ls_exp_busy());
}

/* ------------------------------------------------------- CARRIER LEVEL -- */

static const ls_experiment_t *carrier(void)
{
    ls_exp_forget();
    ls_exp_register_builtin();
    return ls_exp_find("carrier");
}

LS_CASE(carrier_takes_one_frequency_in_range)
{
    fresh();
    const ls_experiment_t *c = carrier();
    char why[64];
    char *ok[] = { "433.92" }, *low[] = { "100" }, *high[] = { "1200" }, *junk[] = { "4x" };
    char *two[] = { "433", "434" };
    LS_CHECK(c->configure(1, ok, why, sizeof(why)));
    LS_CHECK(!c->configure(1, low, why, sizeof(why)));
    LS_EQ_STR(why, "one frequency, 150-1100 MHz");
    LS_CHECK(!c->configure(1, high, why, sizeof(why)));
    LS_CHECK(!c->configure(1, junk, why, sizeof(why)));
    LS_CHECK(!c->configure(2, two, why, sizeof(why)));
    LS_NEAR(c->opts[0].num(&c->opts[0]), 433.92, 1e-6);
}

LS_CASE(carrier_opens_a_narrow_fsk_session_and_reads_the_level)
{
    fresh();
    const ls_experiment_t *c = carrier();
    LS_EQ_INT(run_console("exp carrier start 868.3"), 0);
    LS_CHECK(ls_exp_running() == c);
    LS_EQ_INT(s_begins, 1);
    LS_EQ_UINT(s_cfg.freq_hz, 868300000u);
    LS_EQ_UINT(s_cfg.bitrate, 4800);
    LS_EQ_UINT(s_cfg.deviation_hz, 2400);
    LS_EQ_UINT(s_cfg.bandwidth_hz, 14600);
    LS_EQ_UINT(s_cfg.payload_bytes, 8);
    LS_CHECK(s_cfg.sync_word != 0);

    static char out[12][LS_EXP_LINE];
    int n = ls_exp_read_lines(c, out, 12);
    LS_EQ_INT(n, 6);
    LS_EQ_STR(out[0], "FREQ    868.3000 MHz");
    LS_EQ_STR(out[1], "NOW     -100.0 dBm");
    LS_EQ_STR(out[2], "1 SEC   --");

    /* The start read once already. Six readings at -100 in the first half
       second and six at -90 in the second; the last lands one second after
       the start and closes that second. */
    for (int i = 0; i < 5; i++) { ls_exp_service(); ls_shim_time_advance(100000); }
    s_rssi = -90.0f;
    for (int i = 0; i < 5; i++) { ls_exp_service(); ls_shim_time_advance(100000); }
    n = ls_exp_read_lines(c, out, 12);
    LS_EQ_STR(out[2], "1 SEC   --");
    ls_exp_service();
    n = ls_exp_read_lines(c, out, 12);
    LS_EQ_STR(out[1], "NOW      -90.0 dBm");
    LS_EQ_STR(out[2], "1 SEC   avg  -95.0 min -100.0 max  -90.0");
    LS_EQ_STR(out[3], "START   avg  -95.0 min -100.0 max  -90.0");
    LS_EQ_STR(out[4], "-130 [############..................] -30");
    LS_EQ_STR(out[5], "READS   12  errors 0");
    /* The FIFO was looked at as it went. */
    LS_CHECK(s_fsk_polls >= 9);

    /* Out of receive: counted, and listening again. */
    s_rssi_err = ESP_ERR_INVALID_STATE;
    ls_exp_service();
    LS_EQ_INT(s_rearms, 1);
    n = ls_exp_read_lines(c, out, 12);
    LS_EQ_STR(out[5], "READS   12  errors 1");

    LS_EQ_INT(run_console("exp carrier stop"), 0);
    LS_EQ_INT(s_ends, 1);
    LS_CHECK(!s_fsk);
}

LS_CASE(carrier_options_retune_a_running_session)
{
    fresh();
    const ls_experiment_t *c = carrier();
    ls_exp_start(c);
    ls_exp_service();
    LS_CHECK(ls_exp_running() == c);
    c->opts[0].set_num(&c->opts[0], 162.025);
    LS_CHECK(ls_exp_busy());
    LS_CHECK(ls_exp_wanted() == c);
    ls_exp_service();
    LS_EQ_INT(s_ends, 1);
    LS_EQ_INT(s_begins, 2);
    LS_EQ_UINT(s_cfg.freq_hz, 162025000u);
    char shown[32];
    c->opts[0].show(&c->opts[0], shown, sizeof(shown));
    LS_EQ_STR(shown, "162.0250 MHz");
    ls_exp_stop();
    ls_exp_service();

    /* Stopped, a change waits for the next start. */
    c->opts[0].set_num(&c->opts[0], 915.0);
    LS_CHECK(!ls_exp_busy());
    LS_EQ_INT(s_begins, 2);
}

LS_CASE(carrier_refuses_a_chip_without_fsk_or_a_session_the_chip_refuses)
{
    fresh();
    const ls_experiment_t *c = carrier();
    char st[64];
    s_caps = LS_LORA_CAP_LORA;
    ls_exp_start(c);
    ls_exp_service();
    LS_CHECK(ls_exp_running() == NULL);
    state_of(c, st);
    LS_EQ_STR(st, "No FSK receiver on this chip");
    LS_EQ_INT(s_begins, 0);

    s_caps = LS_LORA_CAP_LORA | LS_LORA_CAP_FSK | LS_LORA_CAP_RSSI_INST;
    s_begin_err = ESP_ERR_INVALID_ARG;
    ls_exp_start(c);
    ls_exp_service();
    LS_CHECK(ls_exp_running() == NULL);
    state_of(c, st);
    LS_CHECK_MSG(strstr(st, "refused") != NULL, "got [%s]", st);
    LS_EQ_INT(s_takes, s_gives);
}

/* ------------------------------------------------------------ NOTE -- */

LS_CASE(note_text_is_the_readout_under_a_title_with_name_and_time)
{
    fresh();
    char lines[4][LS_EXP_LINE], title[48], body[400];
    int n = ls_exp_read_lines(&EXP_A, lines, 4);
    size_t len = ls_exp_note_text(&EXP_A, "2026-10-06T14:21:09Z", "running 5s",
                                  (const char (*)[LS_EXP_LINE])lines, n, title, sizeof(title), body, sizeof(body));
    LS_EQ_STR(title, "ALPHA 2026-10-06 14:21");
    LS_EQ_STR(body, "> RADIO ALPHA TRYING  running 5s\npolls 0\nsecond line\n");
    LS_EQ_INT((int)len, (int)strlen(body));

    /* No radio: a plain tag. No readout, no clock: said so, the stamp kept whole. */
    len = ls_exp_note_text(&EXP_R, "up 12s", "stopped", NULL, 0, title, sizeof(title), body, sizeof(body));
    LS_EQ_STR(title, "QUIET up 12s");
    LS_EQ_STR(body, "> EXPERIMENT QUIET WORKS  stopped\n(no readout yet)\n");
}

LS_CASE(note_text_stops_at_a_line_boundary_when_the_room_runs_out)
{
    fresh();
    char lines[3][LS_EXP_LINE], title[48], body[64];
    for (int i = 0; i < 3; i++) snprintf(lines[i], LS_EXP_LINE, "line %d of the readout", i);
    ls_exp_note_text(&EXP_A, "", "stopped", (const char (*)[LS_EXP_LINE])lines, 3, title, sizeof(title), body, sizeof(body));
    LS_EQ_STR(title, "ALPHA");
    LS_CHECK(strlen(body) < sizeof(body));
    LS_CHECK(body[strlen(body) - 1] == '\n');
    LS_CHECK(strstr(body, "line 0") != NULL);
    LS_CHECK(strstr(body, "line 2") == NULL);

    /* Nothing to write into, or no experiment: empty, not a crash. */
    LS_EQ_INT((int)ls_exp_note_text(NULL, "", "", NULL, 0, title, sizeof(title), body, sizeof(body)), 0);
    LS_EQ_INT((int)ls_exp_note_text(&EXP_A, "", "", NULL, 0, title, sizeof(title), body, 0), 0);
}
