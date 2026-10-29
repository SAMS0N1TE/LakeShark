/* See rx_exp.h. */
#include "rx_exp.h"
#include "ls_rx_head.h"
#include <string.h>

static void *attach(ls_rx_head_t *h, const ls_rx_stream_t *s, char *why, size_t n)
{
    (void)s; (void)why; (void)n;
    const ls_experiment_t *e = h->attaching ? h->attaching->ctx : NULL;
    if (e) ls_exp_start(e);
    return (void *)e;
}

static void detach(void *st)
{
    if (st && ls_exp_wanted() == st) ls_exp_stop();
}

/* Restarting is how an experiment takes a changed setting. */
static void reset(void *st, ls_rx_reset_t why)
{
    if (st && why >= LS_RX_RESET_RATE) ls_exp_start(st);
}

static void process(void *st, const ls_rx_block_t *b) { (void)st; (void)b; }

static void status(const void *st, ls_rx_status_t *out)
{
    out->synced = st && ls_exp_running() == st;
}

static const char EXP_DOC[] = "An EXPERIMENT driving the radio itself; its readout is the panel. "
                              "Test: open it here or under EXPERIMENTS and press START.";

void ls_rx_plugin_from_exp(ls_rx_plugin_t *out, const ls_experiment_t *e)
{
    memset(out, 0, sizeof(*out));
    out->abi = LS_RX_ABI;
    out->id = e->id;
    out->name = e->name;
    out->sub = e->sub;
    out->maturity = e->maturity == LS_EXP_WORKS ? LS_RX_WORKS
                  : e->maturity == LS_EXP_TRYING ? LS_RX_TRYING : LS_RX_IDEA;
    out->needs.rsel_job = -1;
    out->needs.flags = LS_RX_F_OWNS_RADIO | LS_RX_F_EXCLUSIVE |
                       (e->lr2021_only ? LS_RX_F_NEEDS_LR2021 : 0) |
                       (e->maturity != LS_EXP_WORKS ? LS_RX_F_TRYING : 0);
    out->opts = e->opts;
    out->n_opts = e->n_opts;
    out->doc = EXP_DOC;
    out->attach = attach;
    out->detach = detach;
    out->reset = reset;
    out->process = process;
    out->status = status;
    out->ctx = e;
}

const ls_experiment_t *ls_rx_plugin_exp(const ls_rx_plugin_t *p)
{
    return p && p->attach == attach ? p->ctx : NULL;
}
