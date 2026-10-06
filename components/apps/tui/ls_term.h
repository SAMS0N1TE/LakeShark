/* TERMINAL: the serial console's commands, typed on the board.

   In the field there is no laptop on the USB port, so `crumb`, `heap` and
   the rest were out of reach exactly when a fault wanted reading. A command
   typed here runs through the same console as the UART (esp_console_run),
   on a worker task whose stdout is captured into the scrollback the screen
   shows. The worker's stack is internal RAM, because commands write flash
   and a PSRAM stack must not; it exists only while TERMINAL is open.

   The session (scrollback, history) is kept apart from the screen so that a
   remote transport can attach to it later. */

#ifndef LS_TERM_H
#define LS_TERM_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define LS_TERM_INPUT_MAX    120    /* the console cuts a line at 127 */
#define LS_TERM_SCROLL_BYTES 8192
#define LS_TERM_HISTORY      16
#define LS_TERM_PROMPT       "lakeshark> "

/* ------------------------------------------------------------ scrollback */

/* What the commands printed, oldest dropped first. */
typedef struct {
    char     buf[LS_TERM_SCROLL_BYTES];
    uint32_t head;                 /* next byte to write          */
    uint32_t used;                 /* bytes held                  */
    uint32_t gen;                  /* changes on every write      */
} ls_term_scroll_t;

void   ls_term_scroll_reset(ls_term_scroll_t *s);
/* Output as a command printed it: CR dropped, a tab is a space, other
   control bytes dropped, and a byte above 0x7E shown as '.', since on this
   grid it would draw as graphics. */
void   ls_term_scroll_put(ls_term_scroll_t *s, const char *data, size_t n);
/* All of it, oldest first, beginning on a whole line once the oldest bytes
   have gone. Returns the length (n includes the NUL). */
size_t ls_term_scroll_copy(const ls_term_scroll_t *s, char *out, size_t n);

/* --------------------------------------------------------------- history */

typedef struct {
    char line[LS_TERM_HISTORY][LS_TERM_INPUT_MAX + 1];
    int  count;                    /* lines held                  */
    int  next;                     /* slot the next one goes in   */
} ls_term_history_t;

/* A blank line, or the same as the newest, is not kept again. */
void        ls_term_history_add(ls_term_history_t *h, const char *line);
/* back = 1 is the newest; NULL past the oldest. */
const char *ls_term_history_get(const ls_term_history_t *h, int back);

/* ---------------------------------------------------- the running session */

/* Start the command runner. False when there is not the internal memory for
   its stack; the screen says so and TERMINAL still shows the scrollback. */
bool     ls_term_start(void);
/* Let the runner go: at once if idle, else when its command returns. */
void     ls_term_stop(void);
bool     ls_term_running(void);
/* Run one line; false while the last one is still running (or no runner). */
bool     ls_term_submit(const char *line);
bool     ls_term_busy(void);
/* Add text to the scrollback, as a command's output would be. */
void     ls_term_out(const char *text);
void     ls_term_clear(void);
/* Changes whenever the scrollback does; cheap enough for every frame. */
uint32_t ls_term_gen(void);
/* The scrollback, as ls_term_scroll_copy. */
size_t   ls_term_text(char *out, size_t n);
/* TAB: the commands starting with the line's first word. With one, out is
   the completed line; with more, list gets them space separated. Returns
   how many matched. */
int      ls_term_complete(const char *line, char *out, size_t n, char *list, size_t list_n);

#ifdef __cplusplus
}
#endif

#endif
