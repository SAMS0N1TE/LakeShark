/* LS_TEST_SOURCES: none; it runs the lssim this build made on MESH */
/* MESH taps and keys driven through the real screen, router and keyboard.
   The simulator's mesh stub prints every send and save on stderr as
   "mesh-stub ...", so a case can see what a tap would have put on the air. */
#include "ls_test.h"
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#define popen  _popen
#define pclose _pclose
#endif

static char s_out[64 * 1024];

/* Runs lssim on MESH with these options, the grid dumped, and keeps what it
   printed. `last` ends the stub's feed with a private message ("dm" or
   "chan"), or NULL for the plain fixture. */
static void run(const char *opts, const char *last)
{
    s_out[0] = 0;
#ifdef _WIN32
    char env[64];
    snprintf(env, sizeof(env), "LSSIM_MESH_LAST=%s", last ? last : "");
    _putenv(env);
#else
    if (last) setenv("LSSIM_MESH_LAST", last, 1);
    else      unsetenv("LSSIM_MESH_LAST");
#endif
    char cmd[700];
    snprintf(cmd, sizeof(cmd), "%s mesh -d %s 2>&1",
             LSSIM_EXE, opts);
#ifdef _WIN32
    for (char *c = cmd; *c && *c != ' '; c++) if (*c == '/') *c = '\\';
#endif
    FILE *p = popen(cmd, "r");
    LS_CHECK_MSG(p != NULL, "could not run %s", cmd);
    if (!p) return;
    size_t n = 0;
    char line[512];
    while (fgets(line, sizeof(line), p)) {
        const size_t l = strlen(line);
        if (n + l + 1 < sizeof(s_out)) { memcpy(s_out + n, line, l); n += l; }
    }
    s_out[n] = 0;
    const int rc = pclose(p);
    LS_CHECK_MSG(rc == 0, "lssim failed (%d) for: %s", rc, opts);
}

static bool said(const char *what) { return strstr(s_out, what) != NULL; }


static void reply_mode(bool on)
{
#ifdef _WIN32
    _putenv(on ? "LSSIM_MESH_INSPECT=reply" : "LSSIM_MESH_INSPECT=");
#else
    if (on) setenv("LSSIM_MESH_INSPECT", "reply", 1);
    else unsetenv("LSSIM_MESH_INSPECT");
#endif
}
LS_CASE(inspect_refuses_when_disarmed)
{
    run("-x ~2,@right,@enter,+i,+p -o bench/build/inspect-ui.bmp", NULL);
    LS_CHECK(said("NOT PERMITTED"));
    LS_CHECK(said("mesh-stub inspect trace A41F09D7C2B8E350 5"));
}
LS_CASE(portrait_touch_enters_inspector_and_probes)
{
    run("-x ~2,@right,@enter,20:62,@mic,8:61,~8 -o bench/build/inspect-ui.bmp", NULL);
    LS_CHECK(said("WAITING"));
    LS_CHECK(said("mesh-stub inspect trace A41F09D7C2B8E350 1"));
}
LS_CASE(landscape_touch_enters_inspector_and_probes)
{
    run("-l -x ~2,@right,@enter,80:26,@mic,18:26,~8 -o bench/build/inspect-ui.bmp", NULL);
    LS_CHECK(said("WAITING"));
    LS_CHECK(said("mesh-stub inspect trace A41F09D7C2B8E350 1"));
}
LS_CASE(inspector_rejects_a_second_probe)
{
    run("-x ~2,@right,@enter,+i,@mic,+pp -o bench/build/inspect-ui.bmp", NULL);
    LS_CHECK(said("REFUSED"));
    LS_CHECK(said("mesh-stub inspect trace A41F09D7C2B8E350 4"));
    LS_CHECK(said("tag 00000001"));
}
LS_CASE(inspector_timeout_is_visible)
{
    run("-A 1000 -x ~2,@right,@enter,+i,@mic,+p,~32 -o bench/build/inspect-ui.bmp", NULL);
    LS_CHECK(said("TIMED OUT"));
    LS_CHECK(said("PATH TIMED OUT"));
}
LS_CASE(inspector_options_step_in_both_directions)
{
    run("-x ~2,@right,@enter,+i,+o,@right,@left,@right -o bench/build/inspect-ui.bmp", NULL);
    LS_CHECK(said("INSPECT OPTIONS"));
    LS_CHECK(said("35 SEC"));
    LS_CHECK(said("MIN INTERVAL"));
    LS_CHECK(said("OFF"));
}
LS_CASE(trace_and_telemetry_results_remain_visible)
{
    reply_mode(true);
    run("-A 1000 -x ~2,@right,@enter,+i,@mic,+p,~62,+l,~62,+t,~4 -o bench/build/inspect-ui.bmp", NULL);
    reply_mode(false);
    LS_CHECK(said("CH1 VOLT V 4.000"));
    LS_CHECK(said("CH1 TEMP C 22.500"));
    LS_CHECK(said("[41] -3.0dB"));
    LS_CHECK(said("PATH REPLY"));
    LS_CHECK(said("TELEM REPLY"));
    LS_CHECK(said("LOGIN REPLY"));
}

LS_CASE(repeater_telemetry_asks_for_a_login_first)
{
    run("-x ~2,@right,@enter,+i,@mic,+t -o bench/build/inspect-ui.bmp", NULL);
    LS_CHECK(said("LOGIN FIRST"));
    LS_CHECK(said("mesh-stub inspect telemetry A41F09D7C2B8E350 6"));
}
LS_CASE(login_defaults_to_a_blank_guest_password)
{
    reply_mode(true);
    run("-A 1000 -x ~2,@right,@enter,+i,@mic,+l,~4 -o bench/build/inspect-ui.bmp", NULL);
    reply_mode(false);
    LS_CHECK(said("mesh-stub login password ''"));
    LS_CHECK(said("mesh-stub inspect login A41F09D7C2B8E350 1"));
    LS_CHECK(said("ROLE guest  PERMS 01  FW 2"));
}
LS_CASE(inspector_header_shows_the_advertised_name_then_the_key)
{
    run("-x ~2,@right,@enter,+i -o bench/build/inspect-ui.bmp", NULL);
    LS_CHECK(said("NORTHFIELD  READY"));
    LS_CHECK(said("A41F09D7C2B8E350  repeater"));
}
LS_CASE(landscape_inspector_header_and_buttons)
{
    run("-l -x ~2,@right,@enter,+i -o bench/build/inspect-ui.bmp", NULL);
    LS_CHECK(said("NORTHFIELD  READY"));
    LS_CHECK(said("LOGIN guest"));
}
LS_CASE(options_offer_login_password_and_answering)
{
    run("-x ~2,@right,@enter,+i,+o -o bench/build/inspect-ui.bmp", NULL);
    LS_CHECK(said("LOGIN PASSWORD"));
    LS_CHECK(said("BLANK (GUEST)"));
}

LS_CASE(repeater_status_shows_uptime_from_the_response)
{
    reply_mode(true);
    run("-A 1000 -x ~2,@right,@enter,+i,@mic,+l,~62,+s,~4 -o bench/build/inspect-ui.bmp", NULL);
    reply_mode(false);
    LS_CHECK(said("UP 123s  AIR 3s"));
    LS_CHECK(said("RX 12  TX 5 packets"));
    LS_CHECK(said("4.000V"));
}

LS_CASE(typed_password_is_sent_with_the_login)
{
    run("-x ~2,@right,@enter,+i,+o,@down,@down,@down,@enter,~2,+secret,@enter,~2,@esc,@mic,+l,~3 -o bench/build/inspect-ui.bmp", NULL);
    LS_CHECK(said("mesh-stub login password 'secret'"));
}
LS_CASE(a_node_can_be_allowed_to_read_our_telemetry_from_inspect_options)
{
    run("-x ~2,@right,@enter,+i,+o,@down,@down,@down,@down,@enter,~2 -o bench/build/inspect-ui.bmp", NULL);
    LS_CHECK(said("ANSWER THIS NODE"));
    LS_CHECK(said("mesh-stub telem-allow A41F09D7C2B8E350 1"));
}
