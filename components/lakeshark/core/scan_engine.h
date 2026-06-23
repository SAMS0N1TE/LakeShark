#ifndef SCAN_ENGINE_H
#define SCAN_ENGINE_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

void scan_engine_init(void);
void scan_engine_start(void);
void scan_engine_stop(void);
bool scan_engine_active(void);

void scan_engine_skip(void);
void scan_engine_set_hang_ms(int ms);
void scan_engine_set_threshold_db(float margin_db);

int   scan_engine_current(void);
int   scan_engine_get_hang_ms(void);
float scan_engine_get_threshold_db(void);

void scan_engine_status(char *buf, size_t n);

#ifdef __cplusplus
}
#endif

#endif
