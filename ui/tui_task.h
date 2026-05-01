/*
 * tui_task.h - TUI task entry point.
 *
 * Spawns the rendering + UART input task on CPU0. Only built when
 * CONFIG_ENABLE_TUI is on.
 */
#ifndef TUI_TASK_H
#define TUI_TASK_H

#ifdef __cplusplus
extern "C" {
#endif

void tui_task_start(void);

#ifdef __cplusplus
}
#endif

#endif
