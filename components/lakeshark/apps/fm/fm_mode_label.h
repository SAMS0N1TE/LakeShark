#ifndef FM_MODE_LABEL_H
#define FM_MODE_LABEL_H

#include <stdbool.h>

#include "fm_state.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Short operator-facing name for an FM receiver mode. */
const char *fm_mode_label(fm_mode_t mode);

/* Stable lower-case token used by the console and control-head protocol. */
const char *fm_mode_command_name(fm_mode_t mode);

/* Parse a command token, the legacy "nbfm" alias, or a numeric enum value. */
bool fm_mode_parse(const char *text, fm_mode_t *mode);

#ifdef __cplusplus
}
#endif

#endif
