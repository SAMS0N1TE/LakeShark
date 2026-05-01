#ifndef DIAGLOG_H
#define DIAGLOG_H

#include <stdarg.h>

#ifdef __cplusplus
extern "C" {
#endif

void diaglog_init(void);
void diaglog_write(char level, const char *tag, const char *fmt, ...) __attribute__((format(printf, 3, 4)));
void diaglog_vwrite(char level, const char *tag, const char *fmt, va_list ap);
void diaglog_raw(const char *line, int n);

#ifdef __cplusplus
}
#endif

#endif
