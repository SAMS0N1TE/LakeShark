#pragma once
#include <stdbool.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
void ls_cc_capture_clear(void);
int ls_cc_capture_read(int32_t *out,int max,unsigned *dropped);
bool ls_cc_capture_start(void);
void ls_cc_capture_stop(void);
bool ls_cc_capture_poll(uint32_t hz, uint32_t *captures, uint32_t *overflows);
#ifdef __cplusplus
}
#endif
