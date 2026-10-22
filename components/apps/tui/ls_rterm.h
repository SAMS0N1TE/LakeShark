/* REMOTE TERMINAL: the board's screen in a terminal on another machine.
   (TERMINAL, ls_term.h, is the other way round: console commands typed on
   the board.)

   Two ways in, both serving the TUI grid through ls_vt.c:

   - Serial: 'term serial [baud] [ack]' turns the console UART into a framed
     screen feed. host/lsterm/lsterm.py on the PC unwraps it and sends keys
     back as 'term in <hex>' console lines, so the console keeps working
     throughout. With ack, each update ends in an empty frame and the board
     stays at most two updates ahead of the 'term ack' lines coming back.
   - Network: 'term net on' listens on TCP port 2323 (Wi-Fi). A client gives
     the board's terminal password, then gets the screen as plain terminal
     text: `telnet <board> 2323`, or lsterm.py tcp://<board>. Off unless
     switched on; the link is not encrypted.

   Keys and mouse clicks drive the UI exactly as the keyboard and the glass
   do; the board does everything a key does, the terminal only shows it. */

#ifndef LS_RTERM_H
#define LS_RTERM_H

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Where terminal input goes: the UI's own key and tap queues. Set once by
   the UI before ls_rterm_start. Both are called from other tasks. */
void ls_rterm_set_input(void (*key)(int k, char ch), void (*tap)(int col, int row));

/* The console UART, which belongs to main: raw writes that are not split by
   other tasks' output, waiting for the last byte out, and the bit rate. */
void ls_rterm_set_serial(void (*write)(const char *bytes, size_t n), void (*wait_tx)(void),
                         void (*set_baud)(int baud));

/* Start the network listener when it was left on (reads NVS: call from a
   task with an internal-RAM stack). Call after the TUI is running. */
void ls_rterm_start(void);

/* The 'term' console command; main registers it. */
int ls_rterm_console(int argc, char **argv);

/* From the UI task after each present: copies the grid when a terminal is
   watching and its last frame has gone. Cheap when none is. */
void ls_rterm_capture(void);

/* A terminal is attached. */
bool ls_rterm_active(void);

#ifdef __cplusplus
}
#endif

#endif
