/* Only LR433 is registered; the chip accepts sessions and receives nothing. */
#define ls_lora_caps sensor_fixture_caps
#include "../shims/ls_exp_fake.c"
#undef ls_lora_caps
extern const ls_experiment_t exp_lr433;
void ls_exp_register_builtin(void) { ls_exp_register(&exp_lr433); }

/* This fixture registers only LR433; its screens have no navigation target. */
void ls_compass_set_target(double lat, double lon, const char *name)
{
    (void)lat; (void)lon; (void)name;
}
