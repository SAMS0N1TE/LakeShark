/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef LS_RX_PLAN_CONSOLE_H
#define LS_RX_PLAN_CONSOLE_H
#include <stdbool.h>
#include <stddef.h>
#ifdef __cplusplus
extern "C" {
#endif
typedef bool (*ls_rx_plan_write_t)(void *context,const char *line,size_t bytes);
bool ls_rx_plan_export(ls_rx_plan_write_t write,void *context);
int ls_rx_plan_command(int argc,char **argv);
#ifdef __cplusplus
}
#endif
#endif
