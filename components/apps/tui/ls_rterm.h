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
   - SSH: 'term ssh on' listens on port 22 (ls_rterm_ssh.c): the same screen,
     encrypted, its size from the pty request and window-change. A key on
     the list logs in; the terminal password is the fallback.
     The client is asked its window size (telnet NAWS, else where the
     cursor stops); one smaller than the grid, a phone's, gets the part
     that fits and a bar under it to move about (ls_vt_screen_view).
   - Web: 'term web on' serves a page on port 80 (web/screen.html) that
     shows the screen in a phone's or a PC's browser and sends taps and
     keys back, over a WebSocket carrying the USB screen's own frames and
     words ('term web ack z', 'term ack', 'term in <hex>'). The same
     password as the network terminal, asked once: the browser keeps a
     token, which a new password ends. While it is on the board answers
     as lakeshark.local (ls_mdns.h), so a page saved to a phone's home
     screen finds it after the router gives it another address.

   Keys and mouse clicks drive the UI exactly as the keyboard and the glass
   do; the board does everything a key does, the terminal only shows it. */

#ifndef LS_RTERM_H
#define LS_RTERM_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

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

/* 'term size C R' from a viewer: the UI's way to make its grid C x R (a
   screen that is only a terminal can; real glass keeps its own). True when
   the grid will change. Called from the console's or the network's task. */
void ls_rterm_set_resize(bool (*resize)(int cols, int rows));

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

/* For LINK's REMOTE page: what the two ways in are doing, and the network
   terminal's switches. The settings are kept across restarts. */
#define LS_RTERM_WEB_NAME "lakeshark"         /* the web screen's name on the network, .local */

typedef struct {
    bool net_on;                /* the network terminal is switched on */
    bool client, authed;        /* someone is connected; past the password */
    uint32_t client_ip;         /* network byte order */
    int port;
    bool serial_on, serial_z;   /* a PC or the website watches over USB; deflated */
    bool web_on;                /* the web screen is switched on */
    bool web_client;            /* a browser is watching it */
    uint32_t web_ip;            /* that browser's address, network byte order */
    int web_port;
    bool web_named;             /* answering as LS_RTERM_WEB_NAME.local */
    char pass[24];
    /* SSH, ls_rterm_ssh.c */
    bool ssh_on, ssh_listening; /* switched on; bound to its port (it needs the network) */
    bool ssh_client, ssh_authed;
    uint32_t ssh_ip;            /* network byte order */
    int ssh_port;
    int ssh_keys, ssh_keys_max; /* keys that log in without the password: NVS and the SD card's; NVS's room */
    int ssh_learn_s;            /* seconds 'learn' has left, 0 when off */
    char ssh_fp[56];            /* the host key, SHA256:..., once made */
} ls_rterm_status_t;
void ls_rterm_status(ls_rterm_status_t *out);
bool ls_rterm_net_set(bool on);             /* on makes a password when there is none */
bool ls_rterm_password_set(const char *p);  /* 6-23 characters; NULL makes a fresh one */
void ls_rterm_kick(void);                   /* drop the connected clients, telnet and SSH */
bool ls_rterm_web_set(bool on);             /* the web screen; on makes a password when there is none */
void ls_rterm_web_kick(void);               /* drop the browsers signed in to it */

/* SSH. Encrypted, and a client with a key on the list needs no password:
   `ssh -p <port> <board>`, any user name. The password above still works. */
bool ls_rterm_ssh_set(bool on);             /* on makes a password when there is none */
bool ls_rterm_ssh_port_set(int port);       /* 1-65535 but 2323; the client on it is dropped */
void ls_rterm_ssh_learn(bool on);           /* the key offered at the next password login is kept (2 minutes) */
bool ls_rterm_ssh_keys_clear(void);         /* NVS's list; the SD card's file is the card's */

#ifdef __cplusplus
}
#endif

#endif
