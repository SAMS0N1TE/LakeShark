/* An EXPERIMENT as an RX HEAD plugin: the adapter (ls_rx_plugin_from_exp)
   the design names for step 7.

   An experiment drives the LoRa chip itself on the experiments worker
   (ls_experiments.h), so its plugin is OWNS_RADIO and EXCLUSIVE: it takes no
   block from the head, attach asks the worker to start it, detach to stop
   it, and its panel is its own lines() readout under an IDEA / TRYING /
   WORKS badge. The descriptor is filled into caller storage, one per
   experiment, and keeps the experiment in its ctx. */
#ifndef RX_EXP_H
#define RX_EXP_H

#include "ls_rx_plugin.h"
#include "../ls_experiments.h"

void ls_rx_plugin_from_exp(ls_rx_plugin_t *out, const ls_experiment_t *e);
/* The experiment behind an adapted plugin, or NULL for any other plugin. */
const ls_experiment_t *ls_rx_plugin_exp(const ls_rx_plugin_t *p);

#endif
