/* LINK's REMOTE page in the simulator: the network terminal's switches over
   plain state, no sockets. LSSIM_RTERM_CLIENT=1 shows a phone connected,
   on the network terminal and the web screen both.
   Bench only. */

#include "ls_rterm.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static bool s_on = true, s_kicked, s_web_on = true, s_web_kicked, s_ssh_on = true, s_learn;
static int s_ssh_port = 22, s_keys = 1;
static char s_pass[24] = "k7m2qx9pwa";

void ls_rterm_status(ls_rterm_status_t *o)
{
    memset(o, 0, sizeof(*o));
    o->net_on = s_on;
    o->port = 2323;
    o->client = s_on && !s_kicked && getenv("LSSIM_RTERM_CLIENT");
    o->authed = o->client;
    o->client_ip = 0x2810a8c0;              /* 192.168.16.40 */
    o->web_on = s_web_on;
    o->web_port = 80;
    o->web_named = s_web_on;
    o->web_client = s_web_on && !s_web_kicked && getenv("LSSIM_RTERM_CLIENT");
    o->web_ip = 0x2910a8c0;                 /* 192.168.16.41 */
    snprintf(o->pass, sizeof(o->pass), "%s", s_pass);
    o->ssh_on = s_ssh_on;
    o->ssh_listening = s_ssh_on;
    o->ssh_port = s_ssh_port;
    o->ssh_client = s_ssh_on && !s_kicked && getenv("LSSIM_RTERM_CLIENT");
    o->ssh_authed = o->ssh_client;
    o->ssh_ip = 0x3310a8c0;                 /* 192.168.16.51 */
    o->ssh_keys = s_keys;
    o->ssh_keys_max = 8;
    o->ssh_learn_s = s_learn ? 120 : 0;
    snprintf(o->ssh_fp, sizeof(o->ssh_fp), "SHA256:3q2+7wdGvKfa0E1bUu7Hc6sPz9oLwCk5yXnR4tVj0mA");
}

bool ls_rterm_net_set(bool on) { s_on = on; return true; }

bool ls_rterm_password_set(const char *p)
{
    if (!p) p = "n3wpassw4d";
    else if (strlen(p) < 6 || strlen(p) >= sizeof(s_pass)) return false;
    snprintf(s_pass, sizeof(s_pass), "%s", p);
    return true;
}

void ls_rterm_kick(void) { s_kicked = true; }

bool ls_rterm_web_set(bool on) { s_web_on = on; return true; }

void ls_rterm_web_kick(void) { s_web_kicked = true; }

bool ls_rterm_ssh_set(bool on) { s_ssh_on = on; return true; }

bool ls_rterm_ssh_port_set(int port)
{
    if (port < 1 || port > 65535 || port == 2323) return false;
    s_ssh_port = port;
    return true;
}

void ls_rterm_ssh_learn(bool on) { s_learn = on; }

bool ls_rterm_ssh_keys_clear(void) { s_keys = 0; return true; }

/* No feed in the simulator: the line goes to the console. */
void ls_rterm_viewer_line(const char *line) { printf("%s\n", line); }
