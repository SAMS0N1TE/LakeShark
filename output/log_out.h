/*
 * log_out.h - human-readable event stream over UART.
 *
 * Subscribes to the event bus and prints a one-line ESP_LOG-formatted
 * summary for each event. Used in headless mode (no TUI) when you still
 * want to see what the radio is doing in a normal serial monitor.
 */
#ifndef LOG_OUT_H
#define LOG_OUT_H

#ifdef __cplusplus
extern "C" {
#endif

void log_out_init(void);

#ifdef __cplusplus
}
#endif

#endif
