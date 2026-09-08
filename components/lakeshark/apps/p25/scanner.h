#ifndef SCANNER_H
#define SCANNER_H

#include <stdint.h>
#include "radio_endpoint.h"

void scanner_init(void);
uint32_t scanner_run_sweep(ls_radio_session_t *session,
                           const volatile bool *keep_running);

#endif
