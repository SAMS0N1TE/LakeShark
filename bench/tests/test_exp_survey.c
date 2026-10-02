/* LS_TEST_SOURCES: experiments/exp_rf.c and experiments/exp_survey.c with
   ls_experiments.c, against a sweep that hears a carrier at 915.2 MHz and
   a fob at 433.92 MHz every other row: the chunk plan on each kind of radio,
   the per-chunk bookkeeping, and BAND SURVEY end to end. */

#include "ls_test.h"

#include "ls_experiments.h"
#include "ls_lora.h"
#include "esp_timer.h"
#include "experiments/exp_rf.h"

#include <stdio.h>
#include <string.h>

void ls_shim_time_set(int64_t us);
void ls_shim_time_advance(int64_t us);

#define SX_CAPS   (LS_LORA_CAP_LORA | LS_LORA_CAP_FSK | LS_LORA_CAP_RSSI_INST)
#define LF_CAPS   (SX_CAPS | LS_LORA_CAP_RX_WIDE)
#define LR21_CAPS (LF_CAPS | LS_LORA_CAP_BAND_1G5_2G5)

/* ---------------------------------------------------------- the board -- */

const char *ls_exp_hw_radio_busy(void) { return NULL; }
bool ls_exp_hw_radio_take(void) { return true; }
void ls_exp_hw_radio_give(void) { }
bool ls_exp_hw_wake(void) { return false; }

static uint32_t s_caps = LR21_CAPS;
static bool s_scanning;
static uint32_t s_lo, s_hi;
static int s_begins, s_rows;
static bool s_fail_pass;

uint32_t ls_lora_caps(void) { return s_caps; }
/* A LoRa configuration, which a sweep wants to hand the part back to. */
static ls_lora_cfg_t s_lora_cfg;
static bool s_lora_cfg_set;
const ls_lora_cfg_t *ls_lora_cfg(void) { return s_lora_cfg_set ? &s_lora_cfg : NULL; }
void ls_lora_cfg_default(ls_lora_cfg_t *out) { memset(out, 0, sizeof(*out)); out->freq_hz = 910525000u; }
esp_err_t ls_lora_configure(const ls_lora_cfg_t *cfg) { s_lora_cfg = *cfg; s_lora_cfg_set = true; return ESP_OK; }
esp_err_t ls_lora_scan_begin(uint32_t lo, uint32_t hi)
{
    if (!s_lora_cfg_set) return ESP_ERR_INVALID_STATE;
    if (hi <= lo || !ls_lora_rx_range_ok(s_caps, lo, hi)) return ESP_ERR_INVALID_ARG;
    s_scanning = true;
    s_lo = lo;
    s_hi = hi;
    s_begins++;
    return ESP_OK;
}
esp_err_t ls_lora_scan_end(void) { s_scanning = false; return ESP_OK; }

static void put(float *dbm, int n, uint32_t hz, float level)
{
    if (hz < s_lo || hz > s_hi) return;
    const int i = (int)(((uint64_t)(hz - s_lo) * (uint64_t)(n - 1) + (s_hi - s_lo) / 2) / (s_hi - s_lo));
    dbm[i] = level;
    /* An emitter spills into the bins either side, weaker. */
    if (i > 0) dbm[i - 1] = level - 12;
    if (i + 1 < n) dbm[i + 1] = level - 12;
}

int ls_lora_scan_pass(float *dbm, int n, bool *done)
{
    if (done) *done = false;
    if (!s_scanning || s_fail_pass) return 0;
    for (int i = 0; i < n; i++) dbm[i] = -105.0f + (float)(i % 3);
    put(dbm, n, 915200000u, -71.0f);
    if (s_rows % 2 == 0) put(dbm, n, 433920000u, -60.0f);
    s_rows++;
    if (done) *done = true;
    return n;
}

/* ------------------------------------------------------- the chunk plan -- */

LS_CASE(the_plan_cuts_what_each_radio_reaches_into_chunks_of_50_mhz)
{
    exp_span_t s[64];
    /* An SX1262: 150-960 MHz, 810 MHz in 17 chunks. */
    int n = exp_survey_plan(150000000u, 2500000000u, SX_CAPS, 50000000u, s, 64);
    LS_EQ_INT(n, 17);
    LS_EQ_UINT(s[0].lo_hz, 150000000u);
    LS_EQ_UINT(s[16].hi_hz, 960000000u);
    for (int i = 0; i < n; i++) {
        LS_CHECK(s[i].hi_hz - s[i].lo_hz <= 50000000u);
        LS_EQ_UINT(s[i].lo_hz % 1000000u, 0);              /* whole megahertz */
        if (i) LS_EQ_UINT(s[i].lo_hz, s[i - 1].hi_hz);
    }
    /* 300-470 in four: 42.5 MHz apiece, edges on whole megahertz. */
    n = exp_survey_plan(300000000u, 470000000u, SX_CAPS, 50000000u, s, 64);
    LS_EQ_INT(n, 4);
    LS_EQ_UINT(s[0].hi_hz, 342000000u);
    LS_EQ_UINT(s[1].hi_hz, 385000000u);
    LS_EQ_UINT(s[2].hi_hz, 427000000u);
    LS_EQ_UINT(s[3].hi_hz, 470000000u);
    n = exp_survey_plan(150000000u, 2500000000u, SX_CAPS, 50000000u, s, 64);
    /* An LR2012/LR2022: the LF input to 1100 MHz. */
    n = exp_survey_plan(150000000u, 2500000000u, LF_CAPS, 50000000u, s, 64);
    LS_EQ_INT(n, 19);
    LS_EQ_UINT(s[18].hi_hz, 1100000000u);
    /* An LR2021: and 1500-2500 MHz on the HF input, never the gap. */
    n = exp_survey_plan(150000000u, 2500000000u, LR21_CAPS, 50000000u, s, 64);
    LS_EQ_INT(n, 39);
    LS_EQ_UINT(s[19].lo_hz, 1500000000u);
    LS_EQ_UINT(s[38].hi_hz, 2500000000u);
    for (int i = 0; i < n; i++) LS_CHECK(ls_lora_rx_range_ok(LR21_CAPS, s[i].lo_hz, s[i].hi_hz));
    /* A sliver under a megahertz is dropped; a range across the gap keeps
       both sides. */
    n = exp_survey_plan(1099500000u, 1600000000u, LR21_CAPS, 50000000u, s, 64);
    LS_EQ_INT(n, 2);
    LS_EQ_UINT(s[0].lo_hz, 1500000000u);
    LS_EQ_UINT(s[1].hi_hz, 1600000000u);
    n = exp_survey_plan(1000000000u, 1600000000u, LR21_CAPS, 50000000u, s, 64);
    LS_EQ_INT(n, 4);
    /* Out of reach: nothing; and never more than asked for. */
    LS_EQ_INT(exp_survey_plan(1500000000u, 1600000000u, SX_CAPS, 50000000u, s, 64), 0);
    LS_EQ_INT(exp_survey_plan(150000000u, 2500000000u, LR21_CAPS, 50000000u, s, 5), 5);
    /* One chunk for a band narrower than a chunk. */
    n = exp_survey_plan(902000000u, 928000000u, SX_CAPS, 50000000u, s, 64);
    LS_EQ_INT(n, 1);
    LS_EQ_UINT(s[0].lo_hz, 902000000u);
    LS_EQ_UINT(s[0].hi_hz, 928000000u);
}

/* ------------------------------------------------------- bookkeeping -- */

LS_CASE(a_chunk_keeps_its_floor_and_each_emitter_s_peak_duty_and_times)
{
    exp_survey_chunk_t c;
    exp_survey_chunk_init(&c, (exp_span_t){ 902000000u, 928000000u }, 64);
    LS_EQ_UINT(exp_survey_bin_hz(&c, 0), 902000000u);
    LS_EQ_UINT(exp_survey_bin_hz(&c, 63), 928000000u);
    float row[64];
    exp_survey_event_t ev[8];
    int events = 0;
    for (int r = 0; r < 8; r++) {
        for (int i = 0; i < 64; i++) row[i] = -108.0f + (float)(i % 2);
        row[16] = -70.0f;                                /* always on */
        if (r % 4 == 1) {                                /* one row in four, spilling */
            row[39] = -88.0f; row[40] = -80.0f; row[41] = -90.0f;
        }
        const int ne = exp_survey_row(&c, row, 10.0f, (uint32_t)(r * 10), ev, 8);
        if (r == 0) {
            LS_EQ_INT(ne, 1);
            LS_EQ_UINT(ev[0].hz, exp_survey_bin_hz(&c, 16));
            LS_NEAR(ev[0].dbm, -70.0, 0.01);
        }
        if (r % 4 == 1) {
            /* Three neighbours up together are one event, at the strongest. */
            LS_EQ_INT(ne, 1);
            LS_EQ_UINT(ev[0].hz, exp_survey_bin_hz(&c, 40));
            LS_EQ_UINT(ev[0].t_s, (uint32_t)(r * 10));
        }
        events += ne;
    }
    LS_EQ_INT(events, 3);
    LS_EQ_UINT(c.rows, 8);
    LS_NEAR(c.floor_dbm, -107.5, 0.6);

    exp_survey_emitter_t e[4];
    const int n = exp_survey_top(&c, e, 4);
    LS_EQ_INT(n, 2);                                     /* 39 and 41 are 40's spill */
    LS_EQ_UINT(e[0].hz, exp_survey_bin_hz(&c, 16));
    LS_NEAR(e[0].peak_dbm, -70.0, 0.01);
    LS_NEAR(e[0].duty, 1.0, 0.001);
    LS_EQ_UINT(e[0].first_s, 0);
    LS_EQ_UINT(e[0].last_s, 70);
    LS_EQ_UINT(e[1].hz, exp_survey_bin_hz(&c, 40));
    LS_NEAR(e[1].peak_dbm, -80.0, 0.01);
    LS_NEAR(e[1].duty, 0.25, 0.001);
    LS_EQ_UINT(e[1].first_s, 10);
    LS_EQ_UINT(e[1].last_s, 50);
}

LS_CASE(a_quiet_chunk_has_no_emitters_and_a_raised_band_moves_the_floor)
{
    exp_survey_chunk_t c;
    exp_survey_chunk_init(&c, (exp_span_t){ 470000000u, 520000000u }, 64);
    float row[64];
    for (int i = 0; i < 64; i++) row[i] = -110.0f;
    for (int r = 0; r < 5; r++) LS_EQ_INT(exp_survey_row(&c, row, 10.0f, (uint32_t)r, NULL, 0), 0);
    exp_survey_emitter_t e[2];
    LS_EQ_INT(exp_survey_top(&c, e, 2), 0);
    LS_NEAR(c.floor_dbm, -110.0, 0.01);
    /* Everything 6 dB up: the floor follows, nothing crosses. */
    for (int i = 0; i < 64; i++) row[i] = -104.0f;
    for (int r = 0; r < 30; r++) exp_survey_row(&c, row, 10.0f, (uint32_t)r, NULL, 0);
    LS_NEAR(c.floor_dbm, -104.0, 0.1);
    LS_EQ_INT(exp_survey_top(&c, e, 2), 0);
}

/* ------------------------------------------------------- BAND SURVEY -- */

static const ls_experiment_t *survey(uint32_t caps)
{
    extern const ls_experiment_t exp_survey;
    ls_exp_forget();
    ls_exp_register(&exp_survey);
    s_caps = caps;
    s_scanning = false;
    s_begins = s_rows = 0;
    s_fail_pass = false;
    s_lora_cfg_set = true;
    ls_shim_time_set(1000000);
    return &exp_survey;
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

static const char *find_line(char (*out)[LS_EXP_LINE], int n, const char *want)
{
    for (int i = 0; i < n; i++) if (strstr(out[i], want)) return out[i];
    return NULL;
}

LS_CASE(band_survey_walks_every_chunk_and_names_what_it_heard)
{
    const ls_experiment_t *e = survey(LR21_CAPS);
    static char out[24][LS_EXP_LINE];
    /* Before a run has a readout of its own: the list it will cover. A
       start that fails clears the last run's. */
    s_caps = 0;
    ls_exp_start(e);
    ls_exp_service();
    LS_CHECK(ls_exp_running() == NULL);
    s_caps = LR21_CAPS;
    int n = ls_exp_read_lines(e, out, 24);
    LS_CHECK(n >= 2);
    LS_CHECK(strstr(out[0], "BANDS 300-470") == out[0]);
    LS_CHECK(find_line(out, n, "2400-2500") != NULL);

    LS_EQ_INT(run_console("exp survey start"), 0);
    LS_CHECK(ls_exp_running() == e);
    LS_EQ_UINT(s_lo, 300000000u);                            /* 150-300 is off to start */
    /* One whole cycle: 38 chunks of six rows. */
    for (int i = 0; i < 38 * 6 + 2; i++) { ls_exp_service(); ls_shim_time_advance(50000); }
    LS_EQ_INT(s_begins, 39);
    ls_shim_time_advance(600000);
    ls_exp_service();
    n = ls_exp_read_lines(e, out, 24);
    for (int i = 0; i < n; i++) LS_CHECK_MSG(strlen(out[i]) <= 48, "line %d is too long: %s", i, out[i]);
    for (int i = 0; i < n; i++) printf("  | %s\n", out[i]);   /* the readout, for the log */
    LS_CHECK(strstr(out[0], "CHUNK 1/38 300-342 MHz") == out[0]);
    LS_CHECK(strstr(out[0], "CYCLE 2") != NULL);
    LS_CHECK(strstr(out[1], "ROWS 232 ") == out[1]);      /* the start polls once too */
    const char *ism = find_line(out, n, "902-928");
    LS_CHECK(ism && strstr(ism, "915.2") && strstr(ism, "/-71/100%"));
    const char *fob = find_line(out, n, "/-60/50%");
    LS_CHECK(fob && strstr(fob, "427-470") == fob && strstr(fob, " 433.8"));
    LS_CHECK(find_line(out, n, "quiet: ") != NULL);
    const char *top = find_line(out, n, "TOP ");
    LS_CHECK(top != NULL);
    /* The strongest first: the fob at -60. */
    LS_CHECK(top && strstr(top + LS_EXP_LINE, "433.8") && strstr(top + LS_EXP_LINE, "   -60   50%"));
    LS_CHECK(find_line(out, n, "LOG ") != NULL);
    LS_EQ_INT(run_console("exp survey stop"), 0);
    LS_CHECK(!s_scanning);
}

LS_CASE(band_survey_takes_a_typed_range_and_goes_back_to_the_list)
{
    const ls_experiment_t *e = survey(SX_CAPS);
    char why[96];
    char *hf[] = { "1500", "1600" }, *one[] = { "915" }, *all[] = { "all" };
    LS_CHECK(!e->configure(2, hf, why, sizeof(why)));
    LS_EQ_STR(why, "1500-1600 MHz is out of this radio's reach");
    LS_CHECK(!e->configure(1, one, why, sizeof(why)));
    LS_EQ_INT(run_console("exp survey start 900 930"), 0);
    LS_EQ_UINT(s_lo, 900000000u);
    LS_EQ_UINT(s_hi, 930000000u);
    for (int i = 0; i < 20; i++) { ls_exp_service(); ls_shim_time_advance(50000); }
    LS_EQ_INT(s_begins, 4);                                  /* one chunk, again and again */
    static char out[24][LS_EXP_LINE];
    ls_exp_read_lines(e, out, 24);
    LS_CHECK(strstr(out[0], "CHUNK 1/1 900-930 MHz") == out[0]);
    ls_exp_stop();
    ls_exp_service();
    LS_CHECK(e->configure(1, all, why, sizeof(why)));
    LS_EQ_INT(run_console("exp survey start"), 0);
    LS_EQ_UINT(s_lo, 300000000u);
    ls_exp_stop();
    ls_exp_service();
}

LS_CASE(band_survey_gives_lora_a_configuration_when_the_mesh_left_none)
{
    const ls_experiment_t *e = survey(LR21_CAPS);
    s_lora_cfg_set = false;
    LS_EQ_INT(run_console("exp survey start"), 0);
    LS_CHECK(ls_exp_running() == e);
    LS_CHECK(s_lora_cfg_set);
    LS_EQ_UINT(s_lora_cfg.freq_hz, 910525000u);
    ls_exp_stop();
    ls_exp_service();
}

LS_CASE(band_survey_options_say_which_bands_this_radio_cannot_reach)
{
    const ls_experiment_t *e = survey(SX_CAPS);
    LS_EQ_INT(e->n_opts, 13);
    LS_CHECK(e->opts[4].why_not(&e->opts[4]) == NULL);      /* 902-928 */
    LS_CHECK(e->opts[6].why_not(&e->opts[6]) != NULL);      /* 960-1100 on an SX1262 */
    LS_CHECK(e->opts[7].why_not(&e->opts[7]) != NULL);
    s_caps = LR21_CAPS;
    LS_CHECK(e->opts[6].why_not(&e->opts[6]) == NULL);
    LS_CHECK(e->opts[10].why_not(&e->opts[10]) == NULL);
    LS_EQ_INT(e->opts[0].get(&e->opts[0]), 0);
    LS_EQ_INT(e->opts[1].get(&e->opts[1]), 1);
    /* Only 902-928 on: one chunk. */
    for (int i = 0; i < 11; i++) e->opts[i].set(&e->opts[i], i == 4);
    LS_EQ_INT(run_console("exp survey start"), 0);
    LS_EQ_UINT(s_lo, 902000000u);
    static char out[24][LS_EXP_LINE];
    ls_exp_service();
    ls_shim_time_advance(600000);
    ls_exp_service();
    ls_exp_read_lines(e, out, 24);
    LS_CHECK(strstr(out[0], "CHUNK 1/1 902-928") == out[0]);
    /* A pass that fails is counted and the chunk begun again. */
    s_fail_pass = true;
    const int begins = s_begins;
    ls_exp_service();
    LS_EQ_INT(s_begins, begins + 1);
    s_fail_pass = false;
    ls_exp_stop();
    ls_exp_service();
    for (int i = 0; i < 11; i++) e->opts[i].set(&e->opts[i], i != 0);
}
