#ifndef LR433_HISTORY_H
#define LR433_HISTORY_H
#include "sensor_history.h"
#include "../ls_options.h"
/* Snapshots go in caller-owned PSRAM, never on the TUI stack. */
int lr433_history_list(uint8_t order[SH_SENSORS]);
bool lr433_history_copy(int slot, sh_sensor_t *out);
const ls_opt_ctx_t *lr433_history_options(int slot);
bool lr433_history_alert(char *name, size_t n, float *c);
int64_t lr433_history_now(void);
const char *lr433_history_status(void);
void lr433_history_start(void);
void lr433_history_receive(const lr433_msg_t *m);
void lr433_history_flush(void);
bool lr433_history_fahrenheit(void);
void lr433_history_name(const char *name);
bool lr433_history_service(void);
bool lr433_history_owned(const lr433_msg_t *m);
#endif
