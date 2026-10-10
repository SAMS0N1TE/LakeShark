#ifndef TERM_TEST_CONSOLE_H
#define TERM_TEST_CONSOLE_H
#include <stdio.h>
#include <sys/types.h>
#include "esp_err.h"
typedef struct { ssize_t (*write)(void *, const char *, size_t); } cookie_io_functions_t;
FILE *fopencookie(void *cookie, const char *mode, cookie_io_functions_t io);
esp_err_t esp_console_run(const char *line, int *rc);
struct linenoiseCompletions;
void esp_console_get_completion(const char *line, struct linenoiseCompletions *lc);
#endif
