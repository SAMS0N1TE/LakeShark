#pragma once
#include "esp_err.h"
typedef struct { const char *command, *help, *hint; int (*func)(int, char **); } esp_console_cmd_t;
esp_err_t esp_console_cmd_register(const esp_console_cmd_t *c);
