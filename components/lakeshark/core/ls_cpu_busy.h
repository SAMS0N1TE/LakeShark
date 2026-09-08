#ifndef LS_CPU_BUSY_H
#define LS_CPU_BUSY_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Busy percentage per core since the previous call. The first call returns
   false because there is no previous sample to compare against. */
bool ls_cpu_busy(int *core0_pct, int *core1_pct);

/* Pure arithmetic used by the sampler. A zero elapsed interval has no valid
   percentage and returns false. */
bool ls_cpu_busy_from_samples(uint32_t previous_idle,
                              uint32_t current_idle,
                              uint64_t elapsed_us,
                              int *busy_pct);

#ifdef __cplusplus
}
#endif

#endif
