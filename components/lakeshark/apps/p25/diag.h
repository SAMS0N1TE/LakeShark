
#ifndef DIAG_H
#define DIAG_H

#include <stdint.h>
#include <stdarg.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#define DIAG_UART_NUM          1
#define DIAG_UART_TX_PIN       32
#define DIAG_UART_RX_PIN       33
#define DIAG_UART_BAUD         115200
#define DIAG_RING_BYTES        4096
#define DIAG_LINE_MAX          512

void diag_init(void);

void diag_line(const char *tag, const char *fmt, ...)
    __attribute__((format(printf, 2, 3)));
void diag_vline(const char *tag, const char *fmt, va_list ap);

void diag_emit_periodic(void);

void diag_count_sync_attempt(int matched_exact, int best_hd);
void diag_count_bch_result(int ok, int ec);
void diag_count_frame(const char *duid_two_chars);

void diag_dump_nid(const char *tag, const int *dibits33, int nac_raw,
                   const char *duid_raw, int ec, int verdict_ok,
                   const char *reason);

float diag_uptime_s(void);

#ifdef __cplusplus
}
#endif

#endif
