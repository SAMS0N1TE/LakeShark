/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef LS_CELL_PERFORMANCE_H
#define LS_CELL_PERFORMANCE_H
#include <stdbool.h>
#ifdef __cplusplus
extern "C" {
#endif
void cell_performance_boot(void);
bool cell_performance_active(void);
void cell_performance_reboot(bool enable);
int cell_performance_command(int argc, char **argv);
#ifdef __cplusplus
}
#endif
#endif
