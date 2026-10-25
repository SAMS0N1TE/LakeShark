/* Inside REMOTE TERMINAL: the SSH way in (ls_rterm_ssh.c) and the parts of
   ls_rterm.c it shares. Not for anything else; ls_rterm.h is the interface.

   One task owns libssh: the listener, the handshake, the session. The
   screen stays with ls_rterm.c's worker, which renders each update into a
   buffer and hands it over; the SSH task sends it and takes it back. Keys
   typed on the client go the other way, through the same input path the
   telnet client uses. */

#ifndef LS_RTERM_SSH_H
#define LS_RTERM_SSH_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "ls_rterm.h"

/* --- ls_rterm.c, for the SSH task ------------------------------------- */

bool rterm_ensure_started(void);       /* the worker and its buffers */
bool rterm_pass_ok(const char *guess); /* the terminal password, in constant time */
bool rterm_pass_ensure(void);          /* make and keep one when there is none */

/* The client asked for a shell: frames start. cols, rows from its pty
   request, 0 when it made none. True when its size must be asked for
   instead (ls_vt_ask_size, sent by the caller). */
bool rterm_ssh_begin(int cols, int rows);
void rterm_ssh_size(int cols, int rows);              /* window-change */
void rterm_ssh_input(const uint8_t *b, size_t n);
bool rterm_ssh_quit(void);                            /* Ctrl-] came: true once */
void rterm_ssh_end(void);
/* An update ready to send, the worker's until rterm_ssh_taken. */
bool rterm_ssh_take(const char **b, size_t *n);
void rterm_ssh_taken(void);

/* --- ls_rterm_ssh.c, for ls_rterm.c ----------------------------------- */

void rterm_ssh_load(void);             /* boot: settings from NVS, and the task when SSH is on */
void rterm_ssh_status(ls_rterm_status_t *o);
void rterm_ssh_kick(void);
int rterm_ssh_console(int argc, char **argv);   /* 'term ssh ...' */

#endif
