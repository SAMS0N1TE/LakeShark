/* Whole console lines in one go.

   linenoise reads and echoes the console a byte at a time, and each byte
   through the UART driver and VFS costs about half a millisecond on the
   Nano (a tenth on the T-Display): an app upload, 3,400 lines of base64,
   took seven minutes there. When a line starts and the whole of it is
   already waiting, a tool or a paste sent it, not a typist: read it in one
   call, echo it in one write and run it here. Anything else goes on to
   linenoise a byte at a time, as before. */

#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include "driver/uart.h"
#include "esp_console.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "linenoise/linenoise.h"
#include "ls_textpack.h"

#define UART CONFIG_ESP_CONSOLE_UART_NUM
/* The driver hands bytes over a FIFO (120) at a time, ~10 ms apart at
   115200; a burst has ended once nothing more comes for longer than that.
   A typist's first key waits this long, which nobody notices. */
#define QUIET_MS 15

static char s_buf[512];
static size_t s_len, s_pos;          /* bytes not yet handed to linenoise */
static bool s_at_start = true;       /* linenoise holds no partial line */
static char s_prompt[48];

static bool eol(char c) { return c == '\r' || c == '\n'; }

static void gather(void)
{
    s_len = s_pos = 0;
    if (uart_read_bytes(UART, s_buf, 1, portMAX_DELAY) != 1) return;
    s_len = 1;
    TickType_t last = xTaskGetTickCount();
    while (s_len < sizeof s_buf && !memchr(s_buf, '\n', s_len) && !memchr(s_buf, '\r', s_len)) {
        size_t ready = 0;
        uart_get_buffered_data_len(UART, &ready);
        if (ready) {
            if (ready > sizeof s_buf - s_len) ready = sizeof s_buf - s_len;
            const int n = uart_read_bytes(UART, s_buf + s_len, ready, 0);
            if (n > 0) s_len += n;
            last = xTaskGetTickCount();
        } else if (xTaskGetTickCount() - last > pdMS_TO_TICKS(QUIET_MS)) {
            break;
        } else {
            vTaskDelay(1);
        }
    }
}

/* Runs each complete line at the front of the buffer, as the REPL would. */
static void run_lines(void)
{
    for (;;) {
        size_t end = s_pos;
        while (end < s_len && !eol(s_buf[end])) end++;
        if (end >= s_len) return;                    /* the rest goes to linenoise */
        const bool cr = s_buf[end] == '\r';
        s_buf[end] = 0;
        char *line = s_buf + s_pos;
        s_pos = end + 1;
        if (cr && s_pos < s_len && s_buf[s_pos] == '\n') s_pos++;
        if (!*line) continue;
        const bool quiet = !strncmp(line, "term ev ", 8) || !strcmp(line, "term ack") ||
                           !strncmp(line, "term lat ", 9);
        if (!quiet) printf("%s\n", line);
        int ret;
        const esp_err_t err = esp_console_run(line, &ret);
        if (err == ESP_ERR_NOT_FOUND) printf("Unrecognized command\n");
        else if (err == ESP_OK && ret != ESP_OK) LS_PRINTF(t_console_fast_command_returned_non_zero, "Command returned non-zero error code: 0x%x (%s)\n", ret, esp_err_to_name(ret));
        else if (err != ESP_OK && err != ESP_ERR_INVALID_ARG) printf("Internal error: %s\n", esp_err_to_name(err));
        if (!quiet) { fputs(s_prompt, stdout); fflush(stdout); }
    }
}

static ssize_t fast_read(int fd, void *dst, size_t n)
{
    (void)n;                                         /* linenoise asks for one byte */
    while (s_pos >= s_len) {
        if (!s_at_start) {
            const ssize_t r = read(fd, dst, 1);
            if (r == 1) s_at_start = *(char *)dst == '\n';
            return r;
        }
        gather();
        run_lines();
    }
    char c = s_buf[s_pos++];
    if (c == '\r') c = '\n';                         /* as the VFS does for stdin */
    *(char *)dst = c;
    s_at_start = c == '\n';
    return 1;
}

/* After esp_console_new_repl_uart (its terminal probe reads the plain way). */
void console_fast_install(const char *prompt)
{
    if (linenoiseIsDumbMode()) snprintf(s_prompt, sizeof s_prompt, "%s ", prompt);
    else snprintf(s_prompt, sizeof s_prompt, LOG_COLOR_I "%s " LOG_RESET_COLOR, prompt);
    linenoiseSetReadFunction(fast_read);
}
