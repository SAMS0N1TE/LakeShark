/* EXPERIMENTS through the RX HEAD: an adapted experiment owns the radio,
   takes the stream to itself, starts and stops through the experiments
   worker, and carries its maturity. The worker is stood in for here. */
#include "ls_test.h"
#include "ls_rx_head.h"
#include "rx_panels/rx_exp.h"

#include <string.h>

static const ls_experiment_t *s_wanted, *s_running;
static int s_starts, s_stops;
void ls_exp_start(const ls_experiment_t *e) { s_wanted = s_running = e; s_starts++; }
void ls_exp_stop(void) { s_wanted = s_running = NULL; s_stops++; }
const ls_experiment_t *ls_exp_wanted(void) { return s_wanted; }
const ls_experiment_t *ls_exp_running(void) { return s_running; }

static int lines(char (*out)[LS_EXP_LINE], int max) { (void)out; (void)max; return 0; }
static const ls_experiment_t EXP = {
    .id = "dfm17", .name = "RADIOSONDE", .sub = "DFM-17 balloons, 400-406 MHz",
    .maturity = LS_EXP_TRYING, .lines = lines, .lr2021_only = true,
};
static void noop(void *st, const ls_rx_block_t *b) { (void)st; (void)b; }
static const ls_rx_plugin_t PAGER = { .abi = LS_RX_ABI, .id = "p", .name = "P", .doc = "x",
    .needs = { .inputs = LS_RX_BITS }, .process = noop };

LS_CASE(an_experiment_is_a_plugin_that_owns_the_radio)
{
    ls_rx_plugin_t p;
    ls_rx_plugin_from_exp(&p, &EXP);
    LS_EQ_STR(p.name, "RADIOSONDE");
    LS_EQ_INT(p.maturity, LS_RX_TRYING);
    LS_CHECK(p.needs.flags & LS_RX_F_OWNS_RADIO);
    LS_CHECK(p.needs.flags & LS_RX_F_NEEDS_LR2021);
    LS_CHECK(p.needs.flags & LS_RX_F_TRYING);
    LS_CHECK(ls_rx_plugin_exp(&p) == &EXP);
    LS_CHECK(ls_rx_plugin_exp(&PAGER) == NULL);

    /* A chip stream with nothing the experiment reads: it attaches anyway,
       because it reads the radio itself. */
    ls_rx_head_t h;
    char why[64];
    const ls_rx_stream_t s = { .front = LS_RX_FRONT_NONE, .inputs = LS_RX_BITS };
    ls_rx_head_init(&h, &s);
    LS_EQ_INT(ls_rx_head_attach(&h, &p, why, sizeof(why)), 0);
    LS_EQ_INT(s_starts, 1);
    LS_CHECK(s_running == &EXP);
    ls_rx_status_t st;
    LS_CHECK(ls_rx_head_status(&h, 0, &st));
    LS_CHECK(st.synced);
    /* It keeps the radio to itself. */
    ls_rx_plugin_t other;
    ls_rx_plugin_from_exp(&other, &EXP);
    LS_EQ_INT(ls_rx_head_attach(&h, &other, why, sizeof(why)), -1);
    /* A user reset restarts it, which is how a setting takes effect. */
    ls_rx_head_reset(&h, LS_RX_RESET_USER);
    LS_EQ_INT(s_starts, 2);
    ls_rx_head_detach_all(&h);
    LS_EQ_INT(s_stops, 1);
    LS_CHECK(s_running == NULL);
}
