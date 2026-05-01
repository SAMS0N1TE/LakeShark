/* app_vhf.c - VHF FM scaffold. Registers in the multi-app framework so the
 * cycle-app key has somewhere to land, but the demodulator is unimplemented. */

#include "app_registry.h"
#include "vhf_demod.h"

static void vhf_on_enter(void) { vhf_demod_init(); }
static void vhf_on_exit(void)  { }

static const app_t VHF_APP = {
    .name         = "VHF",
    .default_freq = 162400000UL,
    .default_rate = 240000,
    .default_gain = 280,
    .on_enter     = vhf_on_enter,
    .on_exit      = vhf_on_exit,
    .on_sample    = vhf_on_sample,
    .draw_main    = NULL,
    .on_key       = NULL,
};

int vhf_app_register(void)
{
    return app_register(&VHF_APP);
}
