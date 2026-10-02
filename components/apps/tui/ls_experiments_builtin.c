/* Every experiment built into the firmware: one line each, alphabetical,
   so two branches that each add one meet in a one-line merge. */
#include "ls_experiments.h"

#define EXP(sym) do { extern const ls_experiment_t sym; ls_exp_register(&sym); } while (0)

void ls_exp_register_builtin(void)
{
    EXP(exp_aviation);
    EXP(exp_burst);
    EXP(exp_carrier);
    EXP(exp_dfm17);
    EXP(exp_iridium);
    EXP(exp_loramon);
    EXP(exp_lr433);
    EXP(exp_p25site);
    EXP(exp_pagers);
    EXP(exp_survey);
    EXP(exp_uat);
    EXP(exp_wisun);
    EXP(exp_zwave);
}
